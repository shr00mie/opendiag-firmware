/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file fake_led.h
 * @brief Test double for main/ws2812_led.c.
 *
 * Only the composition matters here: vif drives pin-live, mode select drives
 * colour, and that is the behaviour under test, not the SPI timing of a WS2812.
 */

#pragma once

#include "ws2812_led.h"

void fake_led_reset(void);

uint32_t fake_led_mode_color(void);
bool fake_led_link_solid(void);
bool fake_led_pairing(void);
bool fake_led_pin_live(void);
bool fake_led_error(void);

/** Resolved overlay after the last setter call (pairing > pin-live > error). */
led_overlay_t fake_led_overlay(void);

/** Times any composition field actually changed. */
int fake_led_change_count(void);
