#include "camera.h"
#include "camera_res.h"

#include <errno.h>
#include <stdatomic.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "esp_camera.h"
#include "esp_heap_caps.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "sdkconfig.h"
#include "wifi_mgr.h"

static const char *TAG = "camera";

#define CAM_PORT 82

#if CONFIG_Z1_CAMERA_VFLIP
#define CAMERA_VFLIP true
#else
#define CAMERA_VFLIP false
#endif
#if CONFIG_Z1_CAMERA_HMIRROR
#define CAMERA_HMIRROR true
#else
#define CAMERA_HMIRROR false
#endif
#define SNAP_WAIT_MS 300  // port 80 has one httpd task: a snapshot never holds it long
// About 15 KB per 640x480 frame (stock: about 13 KB); the larger sizes grow
// with the pixel count (1600x1200: roughly 6x), well within the UXGA buffers.
#define JPEG_QUALITY 15
#define LINK_WAIT_TICKS (pdMS_TO_TICKS(100) > 0 ? pdMS_TO_TICKS(100) : 1)  // in 1-tick steps (10 ms at 100 Hz)
#define NVS_NS "open-z1"
#define NVS_KEY_RES "cam_res"  // u8, an index into camera_res_names

static httpd_handle_t s_httpd;
static bool (*s_session_active)(void);
static bool (*s_link_busy)(void);
static SemaphoreHandle_t s_sent;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// One viewer, as stock: the fd that sent start_stream last; -1: none.
static int s_viewer = -1;
// Web UI snapshots. The camera task is the only caller of esp_camera_fb_get,
// so a stream loses no frame. While it streams, it publishes the frame it is
// sending (until it is sent): camera_snapshot copies that one.
// Otherwise camera_snapshot asks, and the camera task hands over a copy of
// the next frame.
static SemaphoreHandle_t s_snap_lock, s_snap_done, s_fb_lock;
static camera_fb_t *s_cur_fb;  // s_fb_lock
static atomic_bool s_snap_want;
static bool s_snap_waiting;  // s_mux
static uint8_t *s_snap_buf;  // s_mux
static size_t s_snap_len;    // s_mux
static bool s_snap_nomem;    // s_mux
static TaskHandle_t s_cam_task;
// The resolution asked for last (camera_res_names); -1: no camera. The camera
// task applies it between frames.
static atomic_int s_res = -1;
static const framesize_t k_sizes[CAMERA_RES_COUNT] = {FRAMESIZE_VGA, FRAMESIZE_HD, FRAMESIZE_UXGA};

static const camera_config_t k_cam = {
    .pin_pwdn = CONFIG_Z1_CAMERA_PWDN_GPIO,
    .pin_reset = CONFIG_Z1_CAMERA_RESET_GPIO,
    .pin_xclk = CONFIG_Z1_CAMERA_XCLK_GPIO,
    .pin_sccb_sda = CONFIG_Z1_CAMERA_SIOD_GPIO,
    .pin_sccb_scl = CONFIG_Z1_CAMERA_SIOC_GPIO,
    .pin_d7 = CONFIG_Z1_CAMERA_Y9_GPIO,
    .pin_d6 = CONFIG_Z1_CAMERA_Y8_GPIO,
    .pin_d5 = CONFIG_Z1_CAMERA_Y7_GPIO,
    .pin_d4 = CONFIG_Z1_CAMERA_Y6_GPIO,
    .pin_d3 = CONFIG_Z1_CAMERA_Y5_GPIO,
    .pin_d2 = CONFIG_Z1_CAMERA_Y4_GPIO,
    .pin_d1 = CONFIG_Z1_CAMERA_Y3_GPIO,
    .pin_d0 = CONFIG_Z1_CAMERA_Y2_GPIO,
    .pin_vsync = CONFIG_Z1_CAMERA_VSYNC_GPIO,
    .pin_href = CONFIG_Z1_CAMERA_HREF_GPIO,
    .pin_pclk = CONFIG_Z1_CAMERA_PCLK_GPIO,
    .xclk_freq_hz = 20000000,  // as stock
    .ledc_timer = LEDC_TIMER_0,
    .ledc_channel = LEDC_CHANNEL_0,
    .pixel_format = PIXFORMAT_JPEG,
    // Initialised for UXGA, as stock does, so the buffers hold any JPEG the
    // sensor produces (VGA-sized ones overflow on detailed scenes); switched
    // to the chosen size (camera_res) right after.
    .frame_size = FRAMESIZE_UXGA,
    .jpeg_quality = JPEG_QUALITY,
    .fb_count = 3,  // as stock
    .fb_location = CAMERA_FB_IN_PSRAM,
    .grab_mode = CAMERA_GRAB_LATEST,  // a slow link gets the newest frame, not a backlog
};

static bool pins_configured(void)
{
    const int pins[] = {CONFIG_Z1_CAMERA_XCLK_GPIO, CONFIG_Z1_CAMERA_SIOD_GPIO, CONFIG_Z1_CAMERA_SIOC_GPIO,
                        CONFIG_Z1_CAMERA_Y9_GPIO,   CONFIG_Z1_CAMERA_Y8_GPIO,   CONFIG_Z1_CAMERA_Y7_GPIO,
                        CONFIG_Z1_CAMERA_Y6_GPIO,   CONFIG_Z1_CAMERA_Y5_GPIO,   CONFIG_Z1_CAMERA_Y4_GPIO,
                        CONFIG_Z1_CAMERA_Y3_GPIO,   CONFIG_Z1_CAMERA_Y2_GPIO,   CONFIG_Z1_CAMERA_VSYNC_GPIO,
                        CONFIG_Z1_CAMERA_HREF_GPIO, CONFIG_Z1_CAMERA_PCLK_GPIO};
    for (size_t i = 0; i < sizeof(pins) / sizeof(pins[0]); i++) {
        if (pins[i] < 0) {
            return false;
        }
    }
    return true;
}

// Send override for every session: httpd's own send gives up after a partial
// write, which would cut a frame short and desync the stream. A viewer that
// takes nothing for send_wait_timeout fails here and is closed.
static int send_all(httpd_handle_t hd, int fd, const char *buf, size_t len, int flags)
{
    size_t done = 0;
    while (done < len) {
        int w = send(fd, buf + done, len - done, flags);
        if (w < 0) {
            return errno == EAGAIN || errno == EWOULDBLOCK ? HTTPD_SOCK_ERR_TIMEOUT : HTTPD_SOCK_ERR_FAIL;
        }
        done += (size_t)w;
    }
    return (int)done;
}

static esp_err_t on_open(httpd_handle_t hd, int fd)
{
    if (wifi_mgr_on_setup_ap(fd)) {
        return ESP_FAIL;  // the setup hotspot serves only its setup page
    }
    int on = 1;  // httpd sends a WebSocket header and its payload separately
    setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &on, sizeof(on));
    return httpd_sess_set_send_override(hd, fd, send_all);
}

// Every session ends here, whether the viewer closed it, vanished (the app
// reconnecting) or a send failed; httpd's own warnings for that are muted.
static void on_close(httpd_handle_t hd, int fd)
{
    portENTER_CRITICAL(&s_mux);
    bool viewer = s_viewer == fd;
    if (viewer) {
        s_viewer = -1;
    }
    portEXIT_CRITICAL(&s_mux);
    if (viewer) {
        ESP_LOGI(TAG, "viewer dropped");
    }
    close(fd);  // httpd leaves that to close_fn
}

// Runs in the httpd task, as httpd_ws_send_frame_async requires.
static void send_frame(void *arg)
{
    const camera_fb_t *fb = arg;
    httpd_ws_frame_t f = {.final = true, .type = HTTPD_WS_TYPE_BINARY, .payload = fb->buf, .len = fb->len};
    portENTER_CRITICAL(&s_mux);
    int fd = s_viewer;
    portEXIT_CRITICAL(&s_mux);
    if (fd >= 0 && httpd_ws_send_frame_async(s_httpd, fd, &f) != ESP_OK) {
        httpd_sess_trigger_close(s_httpd, fd);
    }
    xSemaphoreGive(s_sent);
}

static bool any_viewer(void)
{
    portENTER_CRITICAL(&s_mux);
    bool any = s_viewer >= 0;
    portEXIT_CRITICAL(&s_mux);
    return any;
}

// A copy of fb for a waiting camera_snapshot.
static void hand_over(const camera_fb_t *fb)
{
    uint8_t *copy = heap_caps_malloc_prefer(fb->len, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
    if (copy) {
        memcpy(copy, fb->buf, fb->len);
    }
    portENTER_CRITICAL(&s_mux);
    if (s_snap_waiting && !s_snap_buf) {
        s_snap_buf = copy;
        s_snap_len = fb->len;
        s_snap_nomem = !copy;
        copy = NULL;
    }
    portEXIT_CRITICAL(&s_mux);
    free(copy);  // nobody waits any more
    xSemaphoreGive(s_snap_done);
}

// Runs in the camera task with no frame out (s_cur_fb is NULL), so neither
// the viewer nor a snapshot ever gets a frame of the old size cut short.
static void apply_res(int res)
{
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor->set_framesize(sensor, k_sizes[res]) != 0) {
        ESP_LOGW(TAG, "could not switch the sensor to %s", camera_res_names[res]);
        return;
    }
    // The buffers may hold frames from before the switch, and the first one
    // after it can be torn.
    for (int i = 0; i < k_cam.fb_count; i++) {
        camera_fb_t *fb = esp_camera_fb_get();
        if (fb) {
            esp_camera_fb_return(fb);
        }
    }
    ESP_LOGI(TAG, "resolution %s", camera_res_names[res]);
}

// The app link has priority: a frame goes out only while the app server has
// nothing in flight (no queued reply or status, no file transfer). Waits up to
// LINK_WAIT_TICKS for that; false: drop this frame. While the link stays busy
// (a long file transfer, a slow client) one frame still goes out every
// BUSY_FRAME_US, so the picture slows down instead of freezing. A reply that
// comes up while a frame is being sent waits for that one frame at most.
#define BUSY_FRAME_US 1000000
static bool link_free(void)
{
    static int64_t last_sent;
    for (int i = 0; s_link_busy && s_link_busy(); i++) {
        if (i >= LINK_WAIT_TICKS) {
            if (esp_timer_get_time() - last_sent < BUSY_FRAME_US) {
                return false;
            }
            break;
        }
        vTaskDelay(1);
    }
    last_sent = esp_timer_get_time();
    return true;
}

static void camera_task(void *arg)
{
    int applied = atomic_load(&s_res);  // camera_start set it
    for (;;) {
        int want = atomic_load(&s_res);
        if (want != applied) {
            apply_res(want);
            applied = want;
        }
        bool stream = s_session_active() && any_viewer();
        if (!stream && !atomic_load(&s_snap_want)) {
            ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(100));  // camera_snapshot wakes it at once
            continue;
        }
        camera_fb_t *fb = esp_camera_fb_get();
        if (!fb) {
            continue;
        }
        if (atomic_exchange(&s_snap_want, false)) {
            hand_over(fb);
        }
        if (!stream) {
            esp_camera_fb_return(fb);
            continue;
        }
        if (!link_free()) {
            esp_camera_fb_return(fb);  // CAMERA_GRAB_LATEST: the next one is fresh, no backlog
            continue;
        }
        xSemaphoreTake(s_fb_lock, portMAX_DELAY);
        s_cur_fb = fb;
        xSemaphoreGive(s_fb_lock);
        if (httpd_queue_work(s_httpd, send_frame, fb) == ESP_OK) {
            xSemaphoreTake(s_sent, portMAX_DELAY);
        }
        vTaskDelay(1);  // let the app server's task in before the next frame
        xSemaphoreTake(s_fb_lock, portMAX_DELAY);
        s_cur_fb = NULL;
        xSemaphoreGive(s_fb_lock);
        esp_camera_fb_return(fb);
    }
}

static esp_err_t ws_handler(httpd_req_t *req)
{
    if (req->method == HTTP_GET) {
        return ESP_OK;  // handshake only (the app's camera probe closes right after)
    }
    uint8_t buf[32];
    httpd_ws_frame_t f = {.payload = buf};
    if (httpd_ws_recv_frame(req, &f, 0) != ESP_OK || f.len > sizeof(buf) ||
        (f.len && httpd_ws_recv_frame(req, &f, f.len) != ESP_OK)) {
        return ESP_FAIL;  // nothing the app sends is this long
    }
    if (f.type != HTTPD_WS_TYPE_TEXT || f.len != 12 || memcmp(buf, "start_stream", 12) != 0) {
        return ESP_OK;
    }
    // One viewer, as stock: the newest start_stream wins. The one it replaces
    // gets stock's text and is closed (stock strings: "vlive preempted", "The
    // video stream channel is already occupied."). The app reconnects its
    // camera socket often, so a leftover never blocks it.
    int fd = httpd_req_to_sockfd(req);
    portENTER_CRITICAL(&s_mux);
    int old = s_viewer;
    s_viewer = fd;
    portEXIT_CRITICAL(&s_mux);
    if (old >= 0 && old != fd) {
        // Stock's preemption notice (its strings: ns, rsp, reason, message).
        static const char k_busy[] = "{\"ns\":\"vlive\",\"rsp\":\"preempted\",\"reason\":\"live\","
                                     "\"message\":\"The video stream channel is already occupied.\"}";
        httpd_ws_frame_t t = {.final = true, .type = HTTPD_WS_TYPE_TEXT, .payload = (uint8_t *)k_busy,
                              .len = sizeof(k_busy) - 1};
        httpd_ws_send_frame_async(s_httpd, old, &t);  // this handler runs in the httpd task
        httpd_sess_trigger_close(s_httpd, old);
        ESP_LOGI(TAG, "new viewer; the previous one is told the channel is occupied and closed");
    }
    return ESP_OK;
}

#ifdef CONFIG_SPIRAM
#define HAVE_PSRAM 1
#else
#define HAVE_PSRAM 0  // the frame buffers live in PSRAM
#endif

esp_err_t camera_start(bool (*session_active)(void), bool (*link_busy)(void))
{
    if (!pins_configured()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!HAVE_PSRAM) {
        ESP_LOGW(TAG, "camera needs PSRAM, which this build does not enable");
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (s_httpd || !session_active) {
        return ESP_ERR_INVALID_STATE;
    }
    s_sent = xSemaphoreCreateBinary();
    s_snap_done = xSemaphoreCreateBinary();
    s_snap_lock = xSemaphoreCreateMutex();
    s_fb_lock = xSemaphoreCreateMutex();
    if (!s_sent || !s_snap_done || !s_snap_lock || !s_fb_lock) {
        return ESP_ERR_NO_MEM;
    }
    // Like stock: the sensor is set up once at boot and keeps running.
    esp_err_t err = esp_camera_init(&k_cam);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "no camera: %s", esp_err_to_name(err));
        return err;
    }
    uint8_t res = CAMERA_RES_DEFAULT;
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        if (nvs_get_u8(h, NVS_KEY_RES, &res) != ESP_OK || res >= CAMERA_RES_COUNT) {
            res = CAMERA_RES_DEFAULT;
        }
        nvs_close(h);
    }
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor->set_framesize(sensor, k_sizes[res]) != 0) {
        ESP_LOGW(TAG, "could not switch the sensor to %s", camera_res_names[res]);
    }
    atomic_store(&s_res, res);
    // Flips relative to the state the driver read back after init (the OV3660
    // init table may already set one). The Z1 flips vertically only, as stock.
    int vflip = sensor->status.vflip ^ (CAMERA_VFLIP ? 1 : 0);
    int hmirror = sensor->status.hmirror ^ (CAMERA_HMIRROR ? 1 : 0);
    if ((CAMERA_VFLIP && sensor->set_vflip(sensor, vflip) != 0) ||
        (CAMERA_HMIRROR && sensor->set_hmirror(sensor, hmirror) != 0)) {
        ESP_LOGE(TAG, "could not flip the picture");
    } else if (CAMERA_VFLIP || CAMERA_HMIRROR) {
        ESP_LOGI(TAG, "picture flipped (vflip %d, hmirror %d)", vflip, hmirror);
    }
    s_session_active = session_active;
    s_link_busy = link_busy;
    httpd_config_t cfg = HTTPD_DEFAULT_CONFIG();
    cfg.server_port = CAM_PORT;
    cfg.ctrl_port = 32000 + CAM_PORT;               // must differ from any other httpd instance
    cfg.max_open_sockets = 3;  // the viewer, its successor, and the app's camera probe
    cfg.open_fn = on_open;
    cfg.close_fn = on_close;
    // "Failed to send WS payload" / "error in recv : 104" on every viewer that
    // goes away without a close frame; on_close logs one line instead. The
    // levels are per tag, so they apply to the OTA server too (ota.c logs its own).
    esp_log_level_set("httpd_ws", ESP_LOG_ERROR);
    esp_log_level_set("httpd_txrx", ESP_LOG_ERROR);
    err = httpd_start(&s_httpd, &cfg);
    if (err != ESP_OK) {
        esp_camera_deinit();
        return err;
    }
    const httpd_uri_t ws = {.uri = "/ws_video", .method = HTTP_GET, .handler = ws_handler, .is_websocket = true};
    httpd_register_uri_handler(s_httpd, &ws);
    if (xTaskCreate(camera_task, "camera", 4096, NULL, 4, &s_cam_task) != pdPASS) {
        httpd_stop(s_httpd);
        s_httpd = NULL;
        esp_camera_deinit();
        atomic_store(&s_res, -1);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ws://<ip>:%d/ws_video ready (one viewer, the newest wins), %s", CAM_PORT,
             camera_res_names[res]);
    return ESP_OK;
}

esp_err_t camera_snapshot(uint8_t **jpeg, size_t *len)
{
    *jpeg = NULL;
    *len = 0;
    if (!s_snap_lock || !s_httpd || !s_cam_task) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (xSemaphoreTake(s_snap_lock, 0) != pdTRUE) {
        return ESP_ERR_INVALID_STATE;  // one at a time
    }
    esp_err_t err = ESP_ERR_NOT_FOUND;
    if (xSemaphoreTake(s_fb_lock, pdMS_TO_TICKS(SNAP_WAIT_MS)) == pdTRUE) {
        if (s_cur_fb) {  // streaming: the frame the viewer just got
            *jpeg = heap_caps_malloc_prefer(s_cur_fb->len, 2, MALLOC_CAP_SPIRAM, MALLOC_CAP_DEFAULT);
            if (*jpeg) {
                memcpy(*jpeg, s_cur_fb->buf, s_cur_fb->len);
                *len = s_cur_fb->len;
            }
            err = *jpeg ? ESP_OK : ESP_ERR_NO_MEM;
        }
        xSemaphoreGive(s_fb_lock);
    }
    if (err != ESP_ERR_NOT_FOUND) {
        xSemaphoreGive(s_snap_lock);
        return err;
    }
    xSemaphoreTake(s_snap_done, 0);  // a late hand-over from a timed-out request
    portENTER_CRITICAL(&s_mux);
    s_snap_waiting = true;
    s_snap_nomem = false;
    portEXIT_CRITICAL(&s_mux);
    atomic_store(&s_snap_want, true);
    xTaskNotifyGive(s_cam_task);
    xSemaphoreTake(s_snap_done, pdMS_TO_TICKS(SNAP_WAIT_MS));
    atomic_store(&s_snap_want, false);
    portENTER_CRITICAL(&s_mux);
    s_snap_waiting = false;
    *jpeg = s_snap_buf;
    *len = s_snap_len;
    bool nomem = s_snap_nomem;
    s_snap_buf = NULL;
    portEXIT_CRITICAL(&s_mux);
    xSemaphoreGive(s_snap_lock);
    return *jpeg ? ESP_OK : nomem ? ESP_ERR_NO_MEM : ESP_ERR_TIMEOUT;
}

int camera_res(void)
{
    return s_cam_task ? atomic_load(&s_res) : -1;
}

esp_err_t camera_set_res(int res)
{
    if (!s_cam_task) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (res < 0 || res >= CAMERA_RES_COUNT) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_exchange(&s_res, res) == res) {
        return ESP_OK;  // no flash write for nothing
    }
    xTaskNotifyGive(s_cam_task);
    nvs_handle_t h;
    esp_err_t err = nvs_open(NVS_NS, NVS_READWRITE, &h);
    if (err == ESP_OK) {
        err = nvs_set_u8(h, NVS_KEY_RES, (uint8_t)res);
        if (err == ESP_OK) {
            err = nvs_commit(h);
        }
        nvs_close(h);
    }
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "resolution not saved: %s", esp_err_to_name(err));
    }
    return err;
}
