/* SPDX-License-Identifier: GPL-3.0-only */
#include "ws2812_led.h"
#include <math.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "driver/spi_master.h"
#include "pinout.h"

/*
 * This value scales the brightness of all colors to prevent the LED from
 * being too bright and to reduce power consumption.
 */
#define LED_MAX_BRIGHTNESS 128

// SPI timing bits for WS2812B protocol.
// Each 4 bits of color data are encoded into a 16-bit SPI word.
static const uint16_t timing_bits[16] = {
    0x1111, 0x7111, 0x1711, 0x7711, 0x1171, 0x7171, 0x1771, 0x7771,
    0x1117, 0x7117, 0x1717, 0x7717, 0x1177, 0x7177, 0x1777, 0x7777};

static spi_device_handle_t spi_handle;

typedef struct {
    uint32_t mode_color;
    bool link_solid;
    bool pairing;
    bool pin_live;
    bool error;
} led_cmd_t;

static QueueHandle_t led_cmd_queue;

/* Requested composition (written by callers, snapshotted into the queue). */
static uint32_t g_req_mode_color = LED_COLOR_CYAN;
static bool g_req_link_solid;
static bool g_req_pairing;
static bool g_req_pin_live;
static bool g_req_error;

/* Display composition (owned by the animation task). */
static uint32_t g_mode_color = LED_COLOR_CYAN;
static bool g_link_solid;
static bool g_pairing;
static bool g_pin_live;
static bool g_error;

static led_overlay_t led_resolved_overlay(void) {
    if (g_pairing) {
        return LED_OVERLAY_PAIRING;
    }
    if (g_pin_live) {
        return LED_OVERLAY_PIN_LIVE;
    }
    if (g_error) {
        return LED_OVERLAY_ERROR;
    }
    return LED_OVERLAY_NONE;
}

// SPI buffer: 6 words for color data + 2 for reset pulse
static uint16_t led_buf[8] = {0};

static const char *TAG = "RGB_LED";

static uint32_t scale_color(uint32_t color, uint8_t brightness) {
    uint8_t r = (color >> 16) & 0xFF;
    uint8_t g = (color >> 8) & 0xFF;
    uint8_t b = color & 0xFF;

    r = ((uint16_t)r * brightness) / 255;
    g = ((uint16_t)g * brightness) / 255;
    b = ((uint16_t)b * brightness) / 255;

    return (r << 16) | (g << 8) | b;
}

static void ws2812_write_led(uint32_t rgb) {
    int n = 1; // Start at index 1 to leave room for reset pulse start

    // The WS2812B expects colors in GRB order.
    // Green
    led_buf[n++] = timing_bits[0x0f & (rgb >> 12)];
    led_buf[n++] = timing_bits[0x0f & (rgb >> 8)];
    // Red
    led_buf[n++] = timing_bits[0x0f & (rgb >> 20)];
    led_buf[n++] = timing_bits[0x0f & (rgb >> 16)];
    // Blue
    led_buf[n++] = timing_bits[0x0f & (rgb >> 4)];
    led_buf[n++] = timing_bits[0x0f & (rgb >> 0)];

    spi_transaction_t t = {
        .length = sizeof(led_buf) * 8,
        .tx_buffer = led_buf,
    };

    esp_err_t ret = spi_device_transmit(spi_handle, &t);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "SPI transmission failed");
    }
}

static void led_apply_cmd(const led_cmd_t *cmd) {
    g_mode_color = cmd->mode_color;
    g_link_solid = cmd->link_solid;
    g_pairing = cmd->pairing;
    g_pin_live = cmd->pin_live;
    g_error = cmd->error;
}

static void led_post(void) {
    if (!led_cmd_queue) {
        return;
    }

    led_cmd_t cmd = {
        .mode_color = g_req_mode_color,
        .link_solid = g_req_link_solid,
        .pairing = g_req_pairing,
        .pin_live = g_req_pin_live,
        .error = g_req_error,
    };
    xQueueSend(led_cmd_queue, &cmd, 0);
}

static void led_animation_task(void *pvParameters) {
    led_cmd_t cmd;

    (void)pvParameters;

    for (;;) {
        if (xQueueReceive(led_cmd_queue, &cmd, 0) == pdPASS) {
            led_apply_cmd(&cmd);
        }

        switch (led_resolved_overlay()) {
        case LED_OVERLAY_PAIRING:
            /* Fast blue blink at 2.5Hz. */
            ws2812_write_led(scale_color(LED_COLOR_BLUE, LED_MAX_BRIGHTNESS));
            vTaskDelay(pdMS_TO_TICKS(200));
            ws2812_write_led(LED_COLOR_OFF);
            vTaskDelay(pdMS_TO_TICKS(200));
            break;

        case LED_OVERLAY_PIN_LIVE:
            /* Pulsing amber at 1Hz. */
            ws2812_write_led(scale_color(LED_COLOR_AMBER, LED_MAX_BRIGHTNESS));
            vTaskDelay(pdMS_TO_TICKS(750));
            ws2812_write_led(scale_color(LED_COLOR_AMBER, LED_MAX_BRIGHTNESS / 8));
            vTaskDelay(pdMS_TO_TICKS(250));
            break;

        case LED_OVERLAY_ERROR:
            /* Flashing red at 2Hz. */
            ws2812_write_led(scale_color(LED_COLOR_RED, LED_MAX_BRIGHTNESS));
            vTaskDelay(pdMS_TO_TICKS(250));
            ws2812_write_led(LED_COLOR_OFF);
            vTaskDelay(pdMS_TO_TICKS(250));
            break;

        case LED_OVERLAY_NONE:
        default:
            if (g_link_solid) {
                ws2812_write_led(
                    scale_color(g_mode_color, LED_MAX_BRIGHTNESS));
                vTaskDelay(pdMS_TO_TICKS(250));
            } else {
                const uint32_t BREATHING_PERIOD_MS = 3000;
                uint32_t time_ms = xTaskGetTickCount() * portTICK_PERIOD_MS;
                float angle = (float)(time_ms % BREATHING_PERIOD_MS) /
                              (float)BREATHING_PERIOD_MS * 2.0f * M_PI;
                float brightness_factor = (sinf(angle) + 1.0f) / 2.0f;
                uint8_t brightness =
                    (uint8_t)(brightness_factor * LED_MAX_BRIGHTNESS);

                ws2812_write_led(scale_color(g_mode_color, brightness));
                vTaskDelay(pdMS_TO_TICKS(20));
            }
            break;
        }
    }
}

void ws2812_led_init(void) {
    spi_bus_config_t buscfg = {
        .mosi_io_num = PIN_WS2812_LED,
        .miso_io_num = -1,
        .sclk_io_num = -1,
        .quadwp_io_num = -1,
        .quadhd_io_num = -1,
        .max_transfer_sz = sizeof(led_buf),
    };

    spi_device_interface_config_t devcfg = {
        .command_bits = 0,
        .address_bits = 0,
        .dummy_bits = 0,
        .clock_speed_hz = 3200000, // 3.2 MHz clock for WS2812
        .mode = 0,
        .spics_io_num = -1,
        .queue_size = 1,
        .flags = SPI_DEVICE_TXBIT_LSBFIRST,
    };

    ESP_ERROR_CHECK(spi_bus_initialize(SPI2_HOST, &buscfg, SPI_DMA_CH_AUTO));
    ESP_ERROR_CHECK(spi_bus_add_device(SPI2_HOST, &devcfg, &spi_handle));

    led_cmd_queue = xQueueCreate(5, sizeof(led_cmd_t));
    if (led_cmd_queue == NULL) {
        ESP_LOGE(TAG, "Failed to create LED command queue");
        return;
    }

    g_req_mode_color = LED_COLOR_CYAN;
    g_req_link_solid = false;
    g_req_pairing = false;
    g_req_pin_live = false;
    g_req_error = false;
    led_post();

    xTaskCreate(led_animation_task, "led_animation_task", 2048, NULL, 5, NULL);

    ESP_LOGI(TAG, "Status RGB LED initialized");
}

void ws2812_led_set_mode_color(uint32_t rgb) {
    g_req_mode_color = rgb;
    led_post();
}

void ws2812_led_set_link_solid(bool solid) {
    g_req_link_solid = solid;
    led_post();
}

void ws2812_led_set_overlay(led_overlay_t overlay) {
    g_req_pairing = overlay == LED_OVERLAY_PAIRING;
    g_req_pin_live = overlay == LED_OVERLAY_PIN_LIVE;
    g_req_error = overlay == LED_OVERLAY_ERROR;
    led_post();
}

void ws2812_led_set_pairing(bool active) {
    g_req_pairing = active;
    led_post();
}

void ws2812_led_set_pin_live(bool active) {
    g_req_pin_live = active;
    led_post();
}

void ws2812_led_set_error(bool active) {
    g_req_error = active;
    led_post();
}

uint32_t ws2812_led_color_for_mode(const char *name) {
    if (!name) {
        return LED_COLOR_CYAN;
    }
    if (strcasecmp(name, "elm327") == 0) {
        return LED_COLOR_CYAN;
    }
    if (strcasecmp(name, "slcan") == 0) {
        return LED_COLOR_GREEN;
    }
    if (strcasecmp(name, "j2534") == 0) {
        return LED_COLOR_MAGENTA;
    }
    if (strcasecmp(name, "ediabas") == 0) {
        return LED_COLOR_WHITE;
    }
    if (strcasecmp(name, "kdcan") == 0) {
        return LED_COLOR_BLUE;
    }
    return LED_COLOR_CYAN;
}
