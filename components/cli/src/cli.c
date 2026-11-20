#include "cli.h"

#include <dirent.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "cli_args.h"
#include "mbedtls/platform_util.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_app_desc.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "ctrl_server.h"
#include "lpcfw.h"
#include "sdkconfig.h"
#include "storage.h"
#include "wifi_mgr.h"

static const char *TAG = "cli";

#define CLI_LINE_MAX 256
#define CLI_ARGS_MAX 8

static const char *fail_name(wifi_fail_t f)
{
    switch (f) {
    case WIFI_FAIL_NONE: return "none";
    case WIFI_FAIL_PASSWORD: return "password incorrect";
    case WIFI_FAIL_NOT_FOUND: return "network not found";
    default: return "other";
    }
}

static void cmd_wifi(int argc, char **argv)
{
    if (argc == 2 && !strcmp(argv[1], "status")) {
        wifi_mgr_status_t st;
        wifi_mgr_get_status(&st);
        if (!st.configured) {
            printf(st.off ? "wifi: off until reboot (wlan -d)\n" : "wifi: not configured\n");
        } else if (st.connected) {
            printf("wifi: connected to \"%s\", ip %s, rssi %d dBm\n", st.ssid, st.ip, st.rssi);
        } else {
            printf("wifi: connecting to \"%s\", %u failed attempts, last failure: %s (reason %d)\n", st.ssid,
                   st.failed_attempts, fail_name(st.last_fail), st.last_reason);
        }
        char ap[128];
        wifi_mgr_setup_ap_describe(ap, sizeof(ap));
        printf("%s\n", ap);
        return;
    }
    if ((argc == 3 || argc == 4) && !strcmp(argv[1], "set")) {
        const char *password = argc == 4 ? argv[3] : "";
        esp_err_t err = wifi_mgr_set_credentials(argv[2], password);
        if (err == ESP_ERR_INVALID_ARG) {
            printf("wifi: invalid credentials (ssid 1-32 bytes; password empty, 8-63 chars or 64 hex digits)\n");
        } else if (err != ESP_OK) {
            printf("wifi: failed: %s\n", esp_err_to_name(err));
        } else {
            printf("wifi: connecting to \"%s\"\n", argv[2]);
        }
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "scan")) {
        static wifi_mgr_ap_t aps[24];
        size_t n;
        printf("wifi: scanning...\n");
        fflush(stdout);
        esp_err_t err = wifi_mgr_scan(aps, sizeof(aps) / sizeof(aps[0]), &n);
        if (err != ESP_OK) {
            printf("wifi: scan failed: %s\n", esp_err_to_name(err));
            return;
        }
        printf("wifi: %u networks\n", (unsigned)n);
        for (size_t i = 0; i < n; i++) {
            printf("  %4d dBm  ch %2u  %-4s  \"%s\"\n", aps[i].rssi, aps[i].channel, aps[i].open ? "open" : "wpa",
                   aps[i].ssid);
        }
        return;
    }
    if (argc == 2 && !strcmp(argv[1], "setup-ap")) {
        wifi_mgr_setup_ap_force();
        printf("wifi: setup hotspot window opened (closes at the next IP or when the window ends)\n");
        return;
    }
    printf("usage: wifi status | wifi scan | wifi setup-ap | wifi set \"<ssid>\" [\"<password>\"]\n");
}

// Lists a directory on the SD card. Read-only.
static void cmd_sd(int argc, char **argv)
{
    if (!storage_mounted()) {
        printf("sd: no card mounted\n");
        return;
    }
    const char *dir = argc >= 2 ? argv[1] : STORAGE_ROOT;
    if (strncmp(dir, STORAGE_ROOT, strlen(STORAGE_ROOT)) != 0) {
        printf("sd: path must start with %s\n", STORAGE_ROOT);
        return;
    }
    if (!storage_hold()) {
        printf("sd: no card mounted\n");
        return;
    }
    DIR *d = opendir(dir);
    if (!d) {
        storage_release();
        printf("sd: cannot open %s\n", dir);
        return;
    }
    printf("sd: %s%s\n", dir, storage_read_only() ? " (card is read-only for this firmware)" : "");
    struct dirent *e;
    char path[300];
    while ((e = readdir(d)) != NULL) {
        struct stat st;
        snprintf(path, sizeof(path), "%s/%s", dir, e->d_name);
        if (stat(path, &st) != 0) {
            continue;
        }
        if (S_ISDIR(st.st_mode)) {
            printf("  %10s  %s/\n", "", e->d_name);
        } else {
            printf("  %10ld  %s\n", (long)st.st_size, e->d_name);
        }
    }
    closedir(d);
    storage_release();
}

static void dispatch(char *line)
{
    char *argv[CLI_ARGS_MAX];
    int argc = cli_split(line, argv, CLI_ARGS_MAX);
    if (argc < 0) {
        printf("error: unterminated quote or too many arguments\n");
    } else if (argc == 0) {
        return;
    } else if (!strcmp(argv[0], "wifi")) {
        cmd_wifi(argc, argv);
    } else if (!strcmp(argv[0], "ctrl")) {
        ctrl_server_dump();
    } else if (!strcmp(argv[0], "sd")) {
        cmd_sd(argc, argv);
    } else if (argc == 2 && !strcmp(argv[0], "lpc-fw") && !strcmp(argv[1], "cancel")) {
        const char *why = lpcfw_cancel();  // as the app's `upgrade cancel`; the result is logged
        printf("%s", why ? why : "lpc-fw: cancel requested\n");
    } else if (!strcmp(argv[0], "version")) {
        // Ours; the app's `version` gets the stock-shaped value (CONFIG_Z1_APP_ESP_VERSION).
        const esp_app_desc_t *app = esp_app_get_description();
        printf("%s %s (app sees <lpc>.%s)\n", app->project_name, app->version, CONFIG_Z1_APP_ESP_VERSION);
    } else if (!strcmp(argv[0], "help")) {
        printf("commands: help, version, wifi status, wifi scan, wifi setup-ap, wifi set \"<ssid>\" [\"<password>\"], sd [dir], ctrl, lpc-fw cancel\n");
    } else {
        // Not echoed back: a mistyped line may be a password.
        printf("unknown command (try help)\n");
    }
    fflush(stdout);
}

static void cli_task(void *arg)
{
    static char line[CLI_LINE_MAX];
    size_t len = 0;
    bool overflow = false;
    for (;;) {
        uint8_t c;
        if (usb_serial_jtag_read_bytes(&c, 1, portMAX_DELAY) != 1) {
            continue;
        }
        if (c == '\r' || c == '\n') {
            if (overflow) {
                printf("error: line too long\n");
            } else if (len) {
                line[len] = '\0';
                dispatch(line);
            }
            // The line may have held a password; don't leave it for a core dump.
            mbedtls_platform_zeroize(line, sizeof(line));
            len = 0;
            overflow = false;
        } else if (len < CLI_LINE_MAX - 1) {
            line[len++] = (char)c;
        } else {
            overflow = true;
        }
    }
}

esp_err_t cli_start(void)
{
    usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
    esp_err_t err = usb_serial_jtag_driver_install(&cfg);
    if (err != ESP_OK) {
        return err;
    }
    // Route stdout through the driver too, so log output and console replies
    // don't both write the FIFO directly.
    usb_serial_jtag_vfs_use_driver();
    if (xTaskCreate(cli_task, "cli", 4096, NULL, 3, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "console ready on USB (type help)");
    return ESP_OK;
}
