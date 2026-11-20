#include "camera.h"

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
#define JPEG_QUALITY 15  // about 15 KB per 640x480 frame (stock: about 13 KB)

static httpd_handle_t s_httpd;
static bool (*s_session_active)(void);
static SemaphoreHandle_t s_sent;
static portMUX_TYPE s_mux = portMUX_INITIALIZER_UNLOCKED;
// One viewer, as stock: the fd that sent start_stream last; -1: none.
static int s_viewer = -1;
// Web UI snapshots. The camera task is the only caller of esp_camera_fb_get,
// so a stream loses no frame. While it streams, it publishes the frame it is
// sending (and holds it until its pause ends): camera_snapshot copies that one.
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
    // to VGA right after.
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

static void camera_task(void *arg)
{
    for (;;) {
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
        xSemaphoreTake(s_fb_lock, portMAX_DELAY);
        s_cur_fb = fb;
        xSemaphoreGive(s_fb_lock);
        int64_t t0 = esp_timer_get_time();
        if (httpd_queue_work(s_httpd, send_frame, fb) == ESP_OK) {
            xSemaphoreTake(s_sent, portMAX_DELAY);
        }
        // The app link has priority: idle as long as the frame took to send, so
        // the stream uses at most half of whatever the uplink carries and a
        // reply never queues behind a backlog of video.
        vTaskDelay(pdMS_TO_TICKS((esp_timer_get_time() - t0) / 1000) + 1);
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

esp_err_t camera_start(bool (*session_active)(void))
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
    sensor_t *sensor = esp_camera_sensor_get();
    if (sensor->set_framesize(sensor, FRAMESIZE_VGA) != 0) {
        ESP_LOGW(TAG, "could not switch the sensor to 640x480");
    }
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
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "ws://<ip>:%d/ws_video ready (one viewer, the newest wins)", CAM_PORT);
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
