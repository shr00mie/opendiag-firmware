/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file fake_led.c
 * @brief Test double for main/ws2812_led.c.
 */

#include "fake_led.h"

#include <strings.h>

static uint32_t g_mode_color = LED_COLOR_CYAN;
static bool g_link_solid;
static bool g_pairing;
static bool g_pin_live;
static bool g_error;
static int g_changes;

static void note_change(bool changed) {
    if (changed) {
        g_changes++;
    }
}

void fake_led_reset(void) {
    g_mode_color = LED_COLOR_CYAN;
    g_link_solid = false;
    g_pairing = false;
    g_pin_live = false;
    g_error = false;
    g_changes = 0;
}

uint32_t fake_led_mode_color(void) { return g_mode_color; }

bool fake_led_link_solid(void) { return g_link_solid; }

bool fake_led_pairing(void) { return g_pairing; }

bool fake_led_pin_live(void) { return g_pin_live; }

bool fake_led_error(void) { return g_error; }

led_overlay_t fake_led_overlay(void) {
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

int fake_led_change_count(void) { return g_changes; }

/* ------------------------------------------------------------------ *
 * ws2812_led.h implementation
 * ------------------------------------------------------------------ */

void ws2812_led_init(void) { fake_led_reset(); }

void ws2812_led_set_mode_color(uint32_t rgb) {
    note_change(rgb != g_mode_color);
    g_mode_color = rgb;
}

void ws2812_led_set_link_solid(bool solid) {
    note_change(solid != g_link_solid);
    g_link_solid = solid;
}

void ws2812_led_set_overlay(led_overlay_t overlay) {
    bool pairing = overlay == LED_OVERLAY_PAIRING;
    bool pin_live = overlay == LED_OVERLAY_PIN_LIVE;
    bool error = overlay == LED_OVERLAY_ERROR;

    note_change(pairing != g_pairing || pin_live != g_pin_live ||
                error != g_error);
    g_pairing = pairing;
    g_pin_live = pin_live;
    g_error = error;
}

void ws2812_led_set_pairing(bool active) {
    note_change(active != g_pairing);
    g_pairing = active;
}

void ws2812_led_set_pin_live(bool active) {
    note_change(active != g_pin_live);
    g_pin_live = active;
}

void ws2812_led_set_error(bool active) {
    note_change(active != g_error);
    g_error = active;
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
