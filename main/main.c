#include <string.h>

#include "camera.h"
#include "cli.h"
#include "ctrl_server.h"
#include "discovery.h"
#include "esp_app_desc.h"
#include "esp_chip_info.h"

#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "lpc_link.h"
#include "lpcfw.h"
#include "machine.h"
#include "cfg.h"
#include "nvs_flash.h"
#include "health.h"
#include "ota.h"
#include "play.h"
#include "sdlog.h"
#include "stats.h"
#include "storage.h"
#include "webui.h"
#include "wifi_mgr.h"

// This firmware never writes eFuses. Each of these would, from the bootloader
// or the app, so none may ever be enabled (main.c is in every build).
#if CONFIG_SECURE_BOOT || CONFIG_SECURE_BOOT_V2_ENABLED || CONFIG_SECURE_FLASH_ENC_ENABLED || \
    CONFIG_BOOTLOADER_APP_ANTI_ROLLBACK || CONFIG_SECURE_DISABLE_ROM_DL_MODE ||                \
    CONFIG_SECURE_ENABLE_SECURE_ROM_DL_MODE
#error "an option that burns eFuses is enabled"
#endif

static const char *TAG = "z1";
static bool s_ctrl_listening, s_ota_listening;
static volatile uint32_t s_lpc_heard_ms;  // uptime at the LPC's last valid frame (0: boot)

// The status LED, as stock: on at start, then toggled every second (1 s on,
// 1 s off) for as long as the firmware runs.
#if CONFIG_Z1_LED_GPIO >= 0
static void led_toggle(void *arg)
{
    static int level = 1;
    level = !level;
    gpio_set_level(CONFIG_Z1_LED_GPIO, level);
}
#endif

static void led_start(void)
{
#if CONFIG_Z1_LED_GPIO >= 0
    gpio_reset_pin(CONFIG_Z1_LED_GPIO);
    gpio_set_direction(CONFIG_Z1_LED_GPIO, GPIO_MODE_OUTPUT);
    gpio_set_drive_capability(CONFIG_Z1_LED_GPIO, GPIO_DRIVE_CAP_0);  // the weakest is plenty for an LED
    gpio_set_level(CONFIG_Z1_LED_GPIO, 1);
    const esp_timer_create_args_t args = {.callback = led_toggle, .name = "led"};
    esp_timer_handle_t t;
    if (esp_timer_create(&args, &t) != ESP_OK || esp_timer_start_periodic(t, 1000000) != ESP_OK) {
        ESP_LOGE(TAG, "status LED timer failed");
    }
#endif
}

// Text for every app client (as the LPC's console output reaches them).
static void notify_clients(const char *text)
{
    ctrl_server_on_lpc_frame(PTYPE_NORMAL_INFO, (const uint8_t *)text, strlen(text));
}

// As stock: while no valid frame has come from the LPC for 10 s, the app
// clients get this 0x90 text (stock's literal) every 10 s. Any frame counts,
// as on stock, so the LPC's own reset (bootloader, config load: frames all
// along) never trips it.
#define LPC_SILENT_MS 10000
#define LPC_SILENT_ALARM "ALARM: Mainboard did not receive a status response from the CTRL (RX error)\n"

static bool lpc_silent(void)
{
    return (int32_t)((uint32_t)(esp_timer_get_time() / 1000) - s_lpc_heard_ms) >= LPC_SILENT_MS;
}

static void lpc_silence_tick(int64_t now)
{
    static uint32_t last_alarm;  // 0: none in this silence
    // 32 bits: read whole across cores; signed: a frame may land after `now` was taken.
    int32_t quiet = (int32_t)((uint32_t)now - s_lpc_heard_ms);
    if (quiet < LPC_SILENT_MS) {
        if (last_alarm) {
            last_alarm = 0;
            ESP_LOGI(TAG, "LPC talking again");
        }
    } else if (!last_alarm || (uint32_t)now - last_alarm >= LPC_SILENT_MS) {
        last_alarm = (uint32_t)now | 1;  // never 0
        ESP_LOGW(TAG, "LPC silent for %ld s; alarm sent to the app", (long)(quiet / 1000));
        notify_clients(LPC_SILENT_ALARM);
    }
}

// The periodic chores, on absolute deadlines: the LPC keepalive (every 500 ms;
// the LPC alarms after 10 s without a frame; a gap over 1 s is logged), the
// LAN announcement (DISCOVERY_PERIOD_MS), the statistics and the CPU load (1 s; an NVS commit
// every 10 minutes is the one slow call, and the keepalive catches up right
// after it) and the silent-LPC check. Ticks of a part that is not started yet
// do nothing.
#define HOUSE_TICK_MS 100

static void housekeeping_task(void *arg)
{
    int64_t next_keepalive = 0, next_announce = 0, next_stats = 0, last_keepalive = 0;
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        int64_t now = esp_timer_get_time() / 1000;
        if (now >= next_keepalive) {
            lpc_link_keepalive();  // refused while the LPC's bootloader is served
            // Meanwhile the app still follows the update in the status.
            static char stand_in[MACHINE_REPORT_MAX + 8];
            if (lpcfw_status_stand_in(stand_in, sizeof(stand_in))) {
                ctrl_server_on_lpc_frame(PTYPE_STATUS_RES, (const uint8_t *)stand_in, strlen(stand_in));
            }
            if (last_keepalive && now - last_keepalive > 1000) {
                ESP_LOGW(TAG, "keepalive late: %lld ms since the last one", (long long)(now - last_keepalive));
            }
            last_keepalive = now;
            next_keepalive = now - (now - next_keepalive) % 500 + 500;  // keep the 500 ms grid
        }
        if (now >= next_announce) {
            discovery_tick();
            next_announce = now + DISCOVERY_PERIOD_MS;
        }
        if (now >= next_stats) {
            stats_tick();
            health_cpu_tick();
            next_stats = next_stats ? next_stats + 1000 : now + 1000;
        }
        lpc_silence_tick(now);
        vTaskDelayUntil(&wake, pdMS_TO_TICKS(HOUSE_TICK_MS));
    }
}

// An update must not restart the board under a job or an upload.
static const char *ota_busy(void)
{
    if (play_job_busy()) {
        return "a job is running";
    }
    if (ctrl_server_uploading()) {
        return "an upload is running";
    }
    if (lpcfw_busy()) {
        return "the motion controller is being updated";
    }
    return NULL;
}

// Our own restart (an OTA rollback) waits for these.
static bool ota_hold(void)
{
    return play_job_serving() || lpcfw_busy();
}

// A new image is good once the app can reach it again, and so can the next update.
static bool ota_healthy(void)
{
    wifi_mgr_status_t st;
    wifi_mgr_get_status(&st);
    return st.connected && s_ctrl_listening && s_ota_listening && health_recent(3000);
}

static void on_lpc_frame(uint8_t type, const uint8_t *payload, size_t n, void *ctx)
{
    s_lpc_heard_ms = (uint32_t)(esp_timer_get_time() / 1000);
    if (type == PTYPE_FIRM_START || type == PTYPE_FACTORY_START || type == PTYPE_CONFIG_START) {
        // The LPC is booting: a job it was playing is over, even without the
        // PLAY_CAN a reset never sends. Frees the card, writes and OTA.
        play_job_lpc_reset();
    }
    ctrl_server_on_lpc_frame(type, payload, n);
    lpcfw_on_lpc_frame(type, payload, n);  // the bootloader's FIRM_* frames, and the end of a transfer
    if (type >= PTYPE_FIRM_START && type <= PTYPE_FIRM_CAN) {
        return;
    }
    if (type >= PTYPE_CONFIG_START && type <= PTYPE_CONFIG_CAN) {
        cfg_on_lpc_frame(type, payload, n);
        return;
    }
    if (type >= PTYPE_PLAY_START && type <= PTYPE_GOTO_LINES) {
        play_job_on_lpc_frame(type, payload, n);
        return;
    }
    switch (type) {
    case PTYPE_FACTORY_START:
        // Factory data: the LPC then keeps its EEPROM values (Kernel::read_Factroy_SD).
        lpc_link_send(PTYPE_FACTORY_CAN, NULL, 0);
        ESP_LOGI(TAG, "LPC factory data request declined (it keeps its EEPROM values)");
        break;
    case PTYPE_STATUS_RES:
        machine_on_status((const char *)payload, n);
        ESP_LOGD(TAG, "status: %.*s", (int)n, (const char *)payload);
        break;
    case PTYPE_NORMAL_INFO:
        ESP_LOGD(TAG, "info: %.*s", (int)n, (const char *)payload);
        break;
    case PTYPE_FIRM_VER:
        ESP_LOGD(TAG, "lpc firmware: %.*s", (int)n, (const char *)payload);
        break;
    default:
        ESP_LOGD(TAG, "frame 0x%02X, %u bytes", type, (unsigned)n);
        break;
    }
}

void app_main(void)
{
    esp_err_t err = sdlog_start();  // first, so the card log has the whole boot
    const esp_app_desc_t *app = esp_app_get_description();
    esp_chip_info_t chip;
    esp_chip_info(&chip);
    ESP_LOGI(TAG, "open-z1 %s (IDF %s), %d cores, chip rev %d.%d", app->version, app->idf_ver,
             chip.cores, chip.revision / 100, chip.revision % 100);

    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD log failed to start: %s", esp_err_to_name(err));
    }
    led_start();

    err = nvs_flash_init();
    if (err != ESP_OK) {
        // Never erase: a Z1's NVS holds the serial number and factory data.
        ESP_LOGE(TAG, "NVS init failed (%s); continuing without it", esp_err_to_name(err));
    } else {
        nvs_stats_t st;
        if (nvs_get_stats(NULL, &st) == ESP_OK) {
            ESP_LOGI(TAG, "NVS: %u of %u entries used", (unsigned)st.used_entries, (unsigned)st.total_entries);
        }
        if ((err = stats_start(play_job_running)) != ESP_OK) {
            ESP_LOGE(TAG, "statistics failed to start: %s", esp_err_to_name(err));
        }
    }

    err = storage_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card unavailable: %s", esp_err_to_name(err));
    }

    // The LPC asks for its config only at its own boot and waits about 2 s
    // (Kernel::read_Factroy_SD), so its link comes right after the card, before
    // Wi-Fi and the rest. Frames for the control server before it exists are
    // dropped (it relays only to connected clients anyway).
    err = cfg_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "config service failed to start: %s", esp_err_to_name(err));
    }
    err = play_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "playback failed to start: %s", esp_err_to_name(err));
    }
    play_set_notify(notify_clients);
    // Before the LPC link: its bootloader may ask for an update right away.
    lpcfw_set_notify(notify_clients);
    err = lpcfw_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LPC firmware update failed to start: %s", esp_err_to_name(err));
    }
    err = lpc_link_start(on_lpc_frame, NULL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "LPC link failed to start: %s", esp_err_to_name(err));
    }
    // Priority 5, as the keepalive task had. The higher ones (the LPC link 10,
    // the app server 6) mostly wait on the UART, sockets and the card.
    if (xTaskCreate(housekeeping_task, "house", 4096, NULL, 5, NULL) != pdPASS) {
        ESP_LOGE(TAG, "housekeeping failed to start: the LPC will alarm without its keepalive");
    }

    // Wi-Fi first: the console calls into it, so it must be fully started
    // before the console task exists.
    err = wifi_mgr_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Wi-Fi failed to start: %s", esp_err_to_name(err));
    }
    err = cli_start();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "console failed to start: %s", esp_err_to_name(err));
    }
    err = ctrl_server_start();
    s_ctrl_listening = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "control server failed to start: %s", esp_err_to_name(err));
    }

    err = camera_start(ctrl_server_has_clients);
    if (err == ESP_ERR_NOT_SUPPORTED) {
        ESP_LOGI(TAG, "camera off on this board/build");
    } else if (err != ESP_OK) {
        ESP_LOGE(TAG, "camera failed to start: %s", esp_err_to_name(err));
    }

    err = discovery_start(CTRL_PORT, ctrl_server_has_clients);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "discovery failed to start: %s", esp_err_to_name(err));
    }

    err = ota_start(ota_busy);
    s_ota_listening = err == ESP_OK;
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "OTA server failed to start: %s", esp_err_to_name(err));
    }
    if (s_ota_listening && (err = webui_start(lpc_silent)) != ESP_OK) {
        ESP_LOGE(TAG, "web UI failed to start: %s", esp_err_to_name(err));
    }
    ota_confirm(ota_healthy, ota_hold);  // never restart under a job or an LPC update
    ESP_LOGI(TAG, "internal RAM free %u (largest block %u), PSRAM free %u",
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM));
}
