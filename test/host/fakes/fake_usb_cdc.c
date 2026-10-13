/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file fake_usb_cdc.c
 * @brief In-memory usb_cdc for host tests (mirrors latch / coding / break).
 */

#include "fake_usb_cdc.h"

#include <string.h>

#define USB_CDC_INTERFACE_NUM 2

static volatile bool line_status[USB_CDC_INTERFACE_NUM];
static bool last_dtr[USB_CDC_INTERFACE_NUM];
static bool last_rts[USB_CDC_INTERFACE_NUM];
static usb_cdc_line_cb_t line_callback;
static usb_cdc_line_coding_cb_t line_coding_callback;
static usb_cdc_break_cb_t break_callback;
static usb_cdc_rx_cb_t rx_callback[USB_CDC_INTERFACE_NUM];
static bool session_latch_enabled;
static volatile bool session_latched;
static int line_events;
static bool last_connected;
static uint16_t last_break_ms;
static uint32_t last_bit_rate[USB_CDC_INTERFACE_NUM];
static uint8_t last_stop_bits[USB_CDC_INTERFACE_NUM];
static uint8_t last_parity[USB_CDC_INTERFACE_NUM];
static uint8_t last_data_bits[USB_CDC_INTERFACE_NUM];
static bool have_line_coding[USB_CDC_INTERFACE_NUM];

static void session_latch_set(void) {
    bool was = session_latched;
    session_latched = true;
    if (!was && line_callback) {
        line_callback(USB_CDC_INTERFACE_DATA, true);
        line_events++;
        last_connected = true;
    }
}

void fake_usb_cdc_reset(void) {
    memset((void *)line_status, 0, sizeof(line_status));
    memset(last_dtr, 0, sizeof(last_dtr));
    memset(last_rts, 0, sizeof(last_rts));
    memset(rx_callback, 0, sizeof(rx_callback));
    line_callback = NULL;
    line_coding_callback = NULL;
    break_callback = NULL;
    session_latch_enabled = false;
    session_latched = false;
    line_events = 0;
    last_connected = false;
    last_break_ms = 0;
    memset(last_bit_rate, 0, sizeof(last_bit_rate));
    memset(last_stop_bits, 0, sizeof(last_stop_bits));
    memset(last_parity, 0, sizeof(last_parity));
    memset(last_data_bits, 0, sizeof(last_data_bits));
    memset(have_line_coding, 0, sizeof(have_line_coding));
}

void usb_cdc_set_line_callback(usb_cdc_line_cb_t callback) {
    line_callback = callback;
}

void usb_cdc_set_line_coding_callback(usb_cdc_line_coding_cb_t callback) {
    line_coding_callback = callback;
}

void usb_cdc_set_break_callback(usb_cdc_break_cb_t callback) {
    break_callback = callback;
}

void usb_cdc_rx_set_callback(int itf, usb_cdc_rx_cb_t callback) {
    if (itf >= 0 && itf < USB_CDC_INTERFACE_NUM) {
        rx_callback[itf] = callback;
    }
}

int usb_cdc_tx_write(int itf, const void *buf, uint32_t length) {
    (void)itf;
    (void)buf;
    return (int)length;
}

void usb_cdc_tx_flush(int itf) { (void)itf; }

bool usb_cdc_get_line_status(int itf) {
    if (itf < 0 || itf >= USB_CDC_INTERFACE_NUM) {
        return false;
    }
    if (itf == USB_CDC_INTERFACE_DATA && session_latch_enabled) {
        return session_latched;
    }
    return line_status[itf];
}

bool usb_cdc_get_line_coding(int itf, uint32_t *bit_rate, uint8_t *stop_bits,
                             uint8_t *parity, uint8_t *data_bits) {
    if (itf < 0 || itf >= USB_CDC_INTERFACE_NUM || !bit_rate || !stop_bits ||
        !parity || !data_bits || !have_line_coding[itf]) {
        return false;
    }
    *bit_rate = last_bit_rate[itf];
    *stop_bits = last_stop_bits[itf];
    *parity = last_parity[itf];
    *data_bits = last_data_bits[itf];
    return true;
}

void usb_cdc_session_latch_enable(bool enable) {
    if (enable == session_latch_enabled) {
        return;
    }
    if (enable) {
        session_latch_enabled = true;
        if (line_status[USB_CDC_INTERFACE_DATA]) {
            session_latched = true;
        }
    } else {
        /* Quiet — mirrors firmware: no disconnect edge on latch teardown. */
        session_latch_enabled = false;
        session_latched = false;
    }
}

void usb_cdc_session_latch_clear(void) {
    session_latched = false;
}

void usb_cdc_session_latch_touch(void) {
    if (!session_latch_enabled) {
        return;
    }
    session_latch_set();
}

void usb_cdc_setup(void) {}
void usb_cdc_teardown(void) {
    session_latched = false;
    session_latch_enabled = false;
}

void fake_usb_cdc_line_state(int itf, bool dtr, bool rts) {
    bool open = dtr && rts;

    if (itf < 0 || itf >= USB_CDC_INTERFACE_NUM) {
        return;
    }
    last_dtr[itf] = dtr;
    last_rts[itf] = rts;
    line_status[itf] = open;

    if (itf == USB_CDC_INTERFACE_DATA && session_latch_enabled) {
        session_latch_set();
        return;
    }

    if (line_callback) {
        line_callback(itf, open);
        line_events++;
        last_connected = open;
    }
}

void fake_usb_cdc_line_coding(int itf, uint32_t bit_rate, uint8_t stop_bits,
                              uint8_t parity, uint8_t data_bits) {
    if (itf >= 0 && itf < USB_CDC_INTERFACE_NUM) {
        last_bit_rate[itf] = bit_rate;
        last_stop_bits[itf] = stop_bits;
        last_parity[itf] = parity;
        last_data_bits[itf] = data_bits;
        have_line_coding[itf] = true;
    }
    if (itf == USB_CDC_INTERFACE_DATA && session_latch_enabled) {
        session_latch_set();
    }
    if (line_coding_callback) {
        line_coding_callback(itf, bit_rate, stop_bits, parity, data_bits);
    }
}

void fake_usb_cdc_send_break(int itf, uint16_t duration_ms) {
    last_break_ms = duration_ms;
    if (break_callback) {
        break_callback(itf, duration_ms);
    }
}

void fake_usb_cdc_host_rx(int itf, const uint8_t *data, size_t len) {
    if (itf == USB_CDC_INTERFACE_DATA && session_latch_enabled && len) {
        session_latch_set();
    }
    if (itf >= 0 && itf < USB_CDC_INTERFACE_NUM && rx_callback[itf] && data &&
        len) {
        rx_callback[itf](data, len);
    }
}

void fake_usb_cdc_umount(void) {
    for (int i = 0; i < USB_CDC_INTERFACE_NUM; i++) {
        bool was_open = line_status[i];
        line_status[i] = false;
        last_dtr[i] = false;
        last_rts[i] = false;
        if (i == USB_CDC_INTERFACE_DATA) {
            bool was_latched = session_latched;
            session_latched = false;
            if (line_callback && (was_latched || was_open)) {
                line_callback(i, false);
                line_events++;
                last_connected = false;
            }
        } else if (was_open && line_callback) {
            line_callback(i, false);
            line_events++;
        }
    }
}

int fake_usb_cdc_line_events(void) { return line_events; }
bool fake_usb_cdc_last_connected(void) { return last_connected; }
uint16_t fake_usb_cdc_last_break_ms(void) { return last_break_ms; }
