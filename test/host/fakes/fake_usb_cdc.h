/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file fake_usb_cdc.h
 * @brief Host-test double for usb_cdc.h (no TinyUSB).
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "usb_cdc.h"

void fake_usb_cdc_reset(void);

/** Simulate SET_CONTROL_LINE_STATE on @p itf. */
void fake_usb_cdc_line_state(int itf, bool dtr, bool rts);

/** Simulate SET_LINE_CODING on @p itf. */
void fake_usb_cdc_line_coding(int itf, uint32_t bit_rate, uint8_t stop_bits,
                              uint8_t parity, uint8_t data_bits);

/** Simulate SEND_BREAK on @p itf. */
void fake_usb_cdc_send_break(int itf, uint16_t duration_ms);

/** Simulate host RX toward the device (also latches when enabled). */
void fake_usb_cdc_host_rx(int itf, const uint8_t *data, size_t len);

/** Simulate USB unmount (clears latch). */
void fake_usb_cdc_umount(void);

int fake_usb_cdc_line_events(void);
bool fake_usb_cdc_last_connected(void);
uint16_t fake_usb_cdc_last_break_ms(void);
