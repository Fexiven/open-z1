#include "storage.h"

#include <stdatomic.h>

#include "debounce.h"

#include "driver/gpio.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "sdkconfig.h"
#include "sdmmc_cmd.h"
#include "driver/sdmmc_host.h"

static const char *TAG = "storage";

static _Atomic(sdmmc_card_t *) s_card;    // mounted and open for new files
static sdmmc_card_t *volatile s_pending;  // removed, unmounted once its last hold ends
static atomic_int s_holds;

bool storage_hold(void)
{
    atomic_fetch_add(&s_holds, 1);
    if (atomic_load(&s_card)) {
        return true;
    }
    atomic_fetch_sub(&s_holds, 1);
    return false;
}

void storage_release(void)
{
    atomic_fetch_sub(&s_holds, 1);
}

bool storage_mounted(void)
{
    return atomic_load(&s_card) != NULL;
}

bool storage_read_only(void)
{
#if CONFIG_Z1_SD_READ_ONLY
    return true;
#else
    return false;
#endif
}

static sdmmc_host_t s_host = SDMMC_HOST_DEFAULT();

static esp_err_t mount(void)
{
    const esp_vfs_fat_mount_config_t mount = {
        // Never: a card that fails to mount may still hold a machine's only
        // copy of its config and jobs.
        .format_if_mount_failed = false,
        .max_files = 8,
        .allocation_unit_size = 16 * 1024,
    };
    sdmmc_card_t *card = NULL;
    // 4-bit, on the board's own pull-ups as stock (D2 is GPIO 45, the VDD_SPI strap).
    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = CONFIG_Z1_SD_MMC_CLK_GPIO;
    slot.cmd = CONFIG_Z1_SD_MMC_CMD_GPIO;
    slot.d0 = CONFIG_Z1_SD_MMC_D0_GPIO;
    slot.d1 = CONFIG_Z1_SD_MMC_D1_GPIO;
    slot.d2 = CONFIG_Z1_SD_MMC_D2_GPIO;
    slot.d3 = CONFIG_Z1_SD_MMC_D3_GPIO;
    esp_err_t err = esp_vfs_fat_sdmmc_mount(STORAGE_ROOT, &s_host, &slot, &mount, &card);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "SD card not mounted: %s (not inserted, or not FAT; it was left untouched)",
                 esp_err_to_name(err));
        return err;
    }
    atomic_store(&s_card, card);
    ESP_LOGI(TAG, "SD card \"%s\", %llu MB, mounted at %s%s", card->cid.name,
             (unsigned long long)card->csd.capacity * card->csd.sector_size / (1024 * 1024), STORAGE_ROOT,
             storage_read_only() ? " (read-only)" : "");
    return ESP_OK;
}

static bool card_present(void)
{
    return gpio_get_level(CONFIG_Z1_SD_CD_GPIO) == 0;  // low = card in the slot
}

// As stock's TF card monitor: mount when a card goes in, unmount when it
// comes out. A pulled card refuses new files at once, but is only unmounted
// once every open file and folder on it is closed (storage_release); those
// see I/O errors and end their transfers.
static atomic_uint s_used_mb, s_total_mb;

void storage_space_mb(uint32_t *used, uint32_t *total)
{
    *used = atomic_load(&s_used_mb);
    *total = atomic_load(&s_total_mb);
}

static void refresh_space(void)
{
    uint64_t total = 0, free = 0;
    if (storage_hold()) {
        if (esp_vfs_fat_info(STORAGE_ROOT, &total, &free) != ESP_OK) {
            total = free = 0;
        }
        storage_release();
    }
    atomic_store(&s_total_mb, (unsigned)(total >> 20));
    atomic_store(&s_used_mb, (unsigned)((total - free) >> 20));
}

static void monitor_task(void *arg)
{
    debounce_t cd;  // a switch that bounces, or a card pushed half in, is not a change
    debounce_init(&cd, card_present());
    for (unsigned tick = 0;; tick++) {
        vTaskDelay(pdMS_TO_TICKS(500));
        if (tick % 10 == 0) {
            refresh_space();  // FatFs caches the free count after the first scan
        }
        bool present = debounce_feed(&cd, card_present());
        if (!present && atomic_load(&s_card)) {
            s_pending = atomic_exchange(&s_card, NULL);
            ESP_LOGW(TAG, "SD card removed; unmounted once its open files are closed");
        }
        if (s_pending && atomic_load(&s_holds) == 0) {
            esp_vfs_fat_sdcard_unmount(STORAGE_ROOT, s_pending);
            s_pending = NULL;
            ESP_LOGW(TAG, "SD card unmounted");
        }
        if (present && !atomic_load(&s_card) && !s_pending) {
            vTaskDelay(pdMS_TO_TICKS(300));  // let the card settle in the slot
            if (card_present()) {
                mount();
            }
        }
    }
}

esp_err_t storage_start(void)
{
    static bool started;
    if (started) {
        return ESP_ERR_INVALID_STATE;
    }
    started = true;
    s_host.slot = SDMMC_HOST_SLOT_1;
    s_host.max_freq_khz = CONFIG_Z1_SD_MMC_FREQ_KHZ;
    const gpio_config_t cd = {
        .pin_bit_mask = 1ULL << CONFIG_Z1_SD_CD_GPIO,
        .mode = GPIO_MODE_INPUT,
        .pull_up_en = GPIO_PULLUP_ENABLE,
    };
    gpio_config(&cd);
    if (xTaskCreate(monitor_task, "sd_monitor", 3072, NULL, 2, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    if (!card_present()) {
        ESP_LOGW(TAG, "no SD card in the slot; it is mounted when inserted");
        return ESP_ERR_NOT_FOUND;
    }
    return mount();
}
