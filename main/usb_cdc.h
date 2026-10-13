/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include "common.h"

/* The one USB data link: a protocol front-end lives here. */
#define USB_CDC_INTERFACE_DATA 0

/*
 * The console. The IDF console and the debug shell are redirected onto it,
 * so it is not a comm_iface port and takes no RX callback.
 */
#define USB_CDC_INTERFACE_SHELL 1

/*
 * Callback when a host opened or closed one of the CDC interfaces.
 *
 * With session latch disabled, connected follows DTR&&RTS.
 * With latch enabled on the data interface, connected rises when the latch
 * sets and falls only when the latch is cleared (not when DTR+RTS go low).
 */
typedef void (*usb_cdc_line_cb_t)(int itf, bool connected);
typedef void (*usb_cdc_rx_cb_t)(const uint8_t *rx_buf, size_t rx_size);

/* Host SET_LINE_CODING. stop_bits/parity/data_bits match cdc_line_coding_t. */
typedef void (*usb_cdc_line_coding_cb_t)(int itf, uint32_t bit_rate,
                                        uint8_t stop_bits, uint8_t parity,
                                        uint8_t data_bits);

/*
 * Host SEND_BREAK. duration_ms: 0xFFFF = assert until cleared, 0 = deassert,
 * other values = break for that many milliseconds.
 */
typedef void (*usb_cdc_break_cb_t)(int itf, uint16_t duration_ms);

void usb_cdc_setup(void);
void usb_cdc_teardown(void);
void usb_cdc_rx_set_callback(int itf, usb_cdc_rx_cb_t callback);
void usb_cdc_set_line_callback(usb_cdc_line_cb_t callback);
void usb_cdc_set_line_coding_callback(usb_cdc_line_coding_cb_t callback);
void usb_cdc_set_break_callback(usb_cdc_break_cb_t callback);
int usb_cdc_tx_write(int itf, const void *buf, uint32_t length);
void usb_cdc_tx_flush(int itf);
bool usb_cdc_get_line_status(int itf);

/**
 * @brief Read TinyUSB's last SET_LINE_CODING for @p itf.
 * @return true when coding was available (bit_rate may still be 0).
 */
bool usb_cdc_get_line_coding(int itf, uint32_t *bit_rate, uint8_t *stop_bits,
                             uint8_t *parity, uint8_t *data_bits);

/*
 * Session latch for the data interface (CDC0). Used by front-ends whose hosts
 * drop DTR/RTS during an active session (e.g. some KD tools): once latched,
 * usb_cdc_get_line_status(DATA) stays true until clear — not when both control
 * lines go low.
 *
 * Latch sets on any line-state change, any line-coding change, or an explicit
 * usb_cdc_session_latch_touch() (typically from RX). Clear on unmount, mode
 * leave, or link drop via usb_cdc_session_latch_clear(). Clearing / disabling
 * is quiet (no disconnect callback) so a frontend restart cannot re-enter
 * vif_link_down through a synthetic USB drop.
 */
void usb_cdc_session_latch_enable(bool enable);
void usb_cdc_session_latch_clear(void);
void usb_cdc_session_latch_touch(void);
