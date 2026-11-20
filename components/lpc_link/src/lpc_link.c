#include "lpc_link.h"

#include <stdatomic.h>

#include "health.h"
#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "sdkconfig.h"

static const char *TAG = "lpc_link";

#define UART CONFIG_Z1_LPC_UART_NUM

static SemaphoreHandle_t s_tx_lock;
static mk_frame_cb_t s_cb;
static void *s_ctx;
static atomic_bool s_console_blocked;

void lpc_link_console_block(bool on)
{
    atomic_store(&s_console_blocked, on);
}

esp_err_t lpc_link_send(uint8_t type, const void *payload, size_t n)
{
    if (!s_tx_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    if (n > (type == PTYPE_FIRM_DATA ? LPC_FIRM_TX_PAYLOAD : LPC_MAX_TX_PAYLOAD)) {
        return ESP_ERR_INVALID_SIZE;
    }
    if ((type == PTYPE_CTRL_SINGLE || type == PTYPE_CTRL_MULTI) && atomic_load(&s_console_blocked)) {
        return ESP_ERR_INVALID_STATE;
    }
    uint8_t frame[LPC_FIRM_TX_PAYLOAD + MK_FRAME_OVERHEAD];
    size_t len = mk_frame_encode(type, payload, n, frame, sizeof(frame));
    xSemaphoreTake(s_tx_lock, portMAX_DELAY);
    int w = uart_write_bytes(UART, frame, len);
    xSemaphoreGive(s_tx_lock);
    return w == (int)len ? ESP_OK : ESP_FAIL;
}

void lpc_link_keepalive(void)
{
    lpc_link_send(PTYPE_CTRL_SINGLE, "?", 1);
}

static void rx_task(void *arg)
{
    static mk_parser_t parser;
    static uint8_t frame_buf[LPC_MAX_RX_PAYLOAD + MK_FRAME_OVERHEAD];
    uint8_t chunk[256];
    mk_parser_init(&parser, frame_buf, sizeof(frame_buf), MK_LOSSY);
    health_watch(HEALTH_LPC, true);
    for (;;) {
        health_beat(HEALTH_LPC);
        int n = uart_read_bytes(UART, chunk, sizeof(chunk), pdMS_TO_TICKS(50));
        if (n > 0) {
            mk_parser_feed(&parser, chunk, (size_t)n, s_cb, s_ctx);
        }
    }
}



// Pins that would take down the console, the flash or the PSRAM if used here.
static bool pin_reserved(int gpio)
{
    if (gpio == 19 || gpio == 20) {  // USB-Serial/JTAG console
        return true;
    }
    if (gpio >= 26 && gpio <= 32) {  // SPI flash / PSRAM
        return true;
    }
#if CONFIG_SPIRAM_MODE_OCT
    if (gpio >= 33 && gpio <= 37) {  // octal PSRAM data lines
        return true;
    }
#endif
    return false;
}

static esp_err_t check_pin(int gpio, bool output)
{
    bool valid = output ? GPIO_IS_VALID_OUTPUT_GPIO(gpio) : GPIO_IS_VALID_GPIO(gpio);
    if (!valid || pin_reserved(gpio)) {
        ESP_LOGE(TAG, "GPIO %d cannot be used for the LPC link", gpio);
        return ESP_ERR_INVALID_ARG;
    }
    if (gpio == 0 || gpio == 3 || gpio == 45 || gpio == 46) {
        ESP_LOGW(TAG, "GPIO %d is a strapping pin; make sure the LPC side cannot hold it at boot", gpio);
    }
    return ESP_OK;
}

static esp_err_t start_uart(void)
{
    if (CONFIG_Z1_LPC_UART_TX_GPIO < 0 || CONFIG_Z1_LPC_UART_RX_GPIO < 0) {
        ESP_LOGW(TAG, "LPC UART pins not configured (menuconfig > Z1 Mainboard); link disabled");
        return ESP_ERR_INVALID_STATE;
    }
    esp_err_t err = check_pin(CONFIG_Z1_LPC_UART_TX_GPIO, true);
    if (err == ESP_OK) {
        err = check_pin(CONFIG_Z1_LPC_UART_RX_GPIO, false);
    }
    if (err != ESP_OK) {
        return err;
    }

    const uart_config_t cfg = {
        .baud_rate = CONFIG_Z1_LPC_UART_BAUD,
        .data_bits = UART_DATA_8_BITS,
        .parity = UART_PARITY_DISABLE,
        .stop_bits = UART_STOP_BITS_1,
        .flow_ctrl = UART_HW_FLOWCTRL_DISABLE,
        .source_clk = UART_SCLK_DEFAULT,
    };
    err = uart_driver_install(UART, 4096, 4096, 0, NULL, 0);
    if (err == ESP_OK) {
        err = uart_param_config(UART, &cfg);
    }
    if (err == ESP_OK) {
        err = uart_set_pin(UART, CONFIG_Z1_LPC_UART_TX_GPIO, CONFIG_Z1_LPC_UART_RX_GPIO, UART_PIN_NO_CHANGE,
                           UART_PIN_NO_CHANGE);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "UART%d setup failed: %s", UART, esp_err_to_name(err));
        uart_driver_delete(UART);
        return err;
    }
    if (xTaskCreate(rx_task, "lpc_rx", 4096, NULL, 10, NULL) != pdPASS) {
        uart_driver_delete(UART);
        return ESP_ERR_NO_MEM;
    }
    ESP_LOGI(TAG, "link up on UART%d (tx %d, rx %d) at %d baud", UART, CONFIG_Z1_LPC_UART_TX_GPIO,
             CONFIG_Z1_LPC_UART_RX_GPIO, CONFIG_Z1_LPC_UART_BAUD);
    return ESP_OK;
}

esp_err_t lpc_link_start(mk_frame_cb_t cb, void *ctx)
{
    if (!cb) {
        return ESP_ERR_INVALID_ARG;
    }
    if (s_tx_lock) {
        return ESP_ERR_INVALID_STATE;
    }
    s_cb = cb;
    s_ctx = ctx;
    s_tx_lock = xSemaphoreCreateMutex();
    if (!s_tx_lock) {
        return ESP_ERR_NO_MEM;
    }
    esp_err_t err = start_uart();
    if (err != ESP_OK) {
        vSemaphoreDelete(s_tx_lock);
        s_tx_lock = NULL;
        return err;
    }
    return err;
}
