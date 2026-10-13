/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/** RGB packed as 0xRRGGBB. */
#define LED_COLOR_CYAN 0x00FFFF
#define LED_COLOR_GREEN 0x00FF00
#define LED_COLOR_RED 0xFF0000
#define LED_COLOR_AMBER 0xFF6000
#define LED_COLOR_BLUE 0x0000FF
#define LED_COLOR_MAGENTA 0xFF00FF
#define LED_COLOR_WHITE 0xFFFFFF
#define LED_COLOR_OFF 0x000000

typedef enum {
    LED_OVERLAY_NONE = 0,
    LED_OVERLAY_PAIRING,
    LED_OVERLAY_PIN_LIVE,
    LED_OVERLAY_ERROR,
} led_overlay_t;

void ws2812_led_init(void);

/** Base hue for the selected link mode (elm327/slcan/j2534/ediabas/kdcan). */
void ws2812_led_set_mode_color(uint32_t rgb);

/** Solid when USB holds the data link; breathe for BLE or no holder. */
void ws2812_led_set_link_solid(bool solid);

/** Overlay priority: pairing > pin-live > error > none. */
void ws2812_led_set_overlay(led_overlay_t overlay);

void ws2812_led_set_pairing(bool active);
void ws2812_led_set_pin_live(bool active);
void ws2812_led_set_error(bool active);

/** Mode name → default RGB; unknown names map to cyan. */
uint32_t ws2812_led_color_for_mode(const char *name);
