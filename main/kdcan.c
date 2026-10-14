/* SPDX-License-Identifier: GPL-3.0-only */
/* Transparent USB CDC ↔ K-Line pipe for BMWeb-style K+DCAN hosts.
 * Host TX uses BUS_KLINE_FRAME_RAW; baud/parity follow CDC line coding;
 * Send_Break maps to BUS_IOCTL_BREAK_SET. Line coding/break are cached on
 * the TinyUSB task and flushed from the link task before the next write. */

#include "kdcan.h"

#include <string.h>

#include "esp_log.h"

#include "bus.h"
#include "usb_cdc.h"
#include "vif.h"

#define TAG "KDCAN"

#define KDCAN_DEFAULT_BAUD 115200
#define KDCAN_RX_BURST 64
#define KDCAN_RX_BUF 64

typedef struct {
    bool bus_open;
    bool coding_valid;
    bool coding_dirty;
    uint32_t bit_rate;
    uint8_t parity;
    uint8_t data_bits;
    /* Last settings successfully pushed to K-Line (link task only). */
    bool applied_valid;
    uint32_t applied_bit_rate;
    uint8_t applied_parity;
    uint8_t applied_data_bits;
    bool break_dirty;
    bool break_assert;
} kdcan_ctx_t;

static kdcan_ctx_t g_kd;

/* ------------------------------------------------------------------ *
 * CDC line coding / break → K-Line
 * ------------------------------------------------------------------ */

/** CDC ACM parity: 0 none, 1 odd, 2 even (mark/space → none). */
static uint32_t kdcan_parity_from_cdc(uint8_t parity) {
    switch (parity) {
    case 1:
        return BUS_PARITY_ODD;
    case 2:
        return BUS_PARITY_EVEN;
    default:
        return BUS_PARITY_NONE;
    }
}

/** CDC data_bits is 5–8; J2534 only distinguishes 7 vs 8. */
static uint32_t kdcan_data_bits_from_cdc(uint8_t data_bits) {
    return (data_bits == 7) ? BUS_DATA_BITS_7 : BUS_DATA_BITS_8;
}

static char kdcan_parity_letter(uint8_t cdc_parity) {
    switch (cdc_parity) {
    case 1:
        return 'O';
    case 2:
        return 'E';
    default:
        return 'N';
    }
}

/** Effective baud for a stashed CDC bit_rate (0 → default). */
static uint32_t kdcan_effective_rate(uint32_t bit_rate) {
    return bit_rate ? bit_rate : KDCAN_DEFAULT_BAUD;
}

/** Push cached CDC line coding onto K-Line; leave dirty on failure to retry.
 *  Must run on the link task (vif_bus_param_set requires the owner thread). */
static void kdcan_flush_coding(void) {
    int rc_rate = 0;
    int rc_par = 0;
    int rc_bits = 0;
    uint32_t rate;

    if (!g_kd.bus_open || !g_kd.coding_valid || !g_kd.coding_dirty) {
        return;
    }

    rate = kdcan_effective_rate(g_kd.bit_rate);
    if (g_kd.applied_valid && g_kd.applied_bit_rate == rate &&
        g_kd.applied_parity == g_kd.parity &&
        g_kd.applied_data_bits == g_kd.data_bits) {
        g_kd.coding_dirty = false;
        return;
    }

    rc_rate = vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_DATA_RATE,
                                rate);
    rc_par = vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_PARITY,
                               kdcan_parity_from_cdc(g_kd.parity));
    rc_bits = vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_DATA_BITS,
                                kdcan_data_bits_from_cdc(g_kd.data_bits));

    if (rc_rate || rc_par || rc_bits) {
        ESP_LOGW(TAG,
                 "line coding %lu 8%c1 apply failed rc=%d/%d/%d (retry)",
                 (unsigned long)rate, kdcan_parity_letter(g_kd.parity),
                 rc_rate, rc_par, rc_bits);
        return;
    }

    g_kd.coding_dirty = false;
    g_kd.applied_valid = true;
    g_kd.applied_bit_rate = rate;
    g_kd.applied_parity = g_kd.parity;
    g_kd.applied_data_bits = g_kd.data_bits;
    ESP_LOGI(TAG, "K-Line %lu 8%c1 (CDC line coding)", (unsigned long)rate,
             kdcan_parity_letter(g_kd.parity));
}

static void kdcan_flush_break(void) {
    uint32_t assert;
    int rc;

    if (!g_kd.bus_open || !g_kd.break_dirty) {
        return;
    }

    assert = g_kd.break_assert ? 1u : 0u;
    rc = vif_bus_ioctl(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_IOCTL_BREAK_SET,
                       &assert, NULL);
    if (rc != 0) {
        ESP_LOGW(TAG, "break %s failed rc=%d (retry)",
                 assert ? "assert" : "deassert", rc);
        return;
    }

    g_kd.break_dirty = false;
    ESP_LOGI(TAG, "K-Line break %s", assert ? "assert" : "deassert");
}

/** Apply any CDC side-effects that must land before the next TX/RX. */
static void kdcan_flush_pending(void) {
    kdcan_flush_coding();
    kdcan_flush_break();
}

static void kdcan_stash_line_coding(uint32_t bit_rate, uint8_t parity,
                                    uint8_t data_bits) {
    uint32_t rate = kdcan_effective_rate(bit_rate);

    g_kd.bit_rate = bit_rate;
    g_kd.parity = parity;
    g_kd.data_bits = data_bits;
    g_kd.coding_valid = true;

    /* Host often reasserts the active settings; skip a no-op UART reconfig. */
    if (g_kd.applied_valid && g_kd.applied_bit_rate == rate &&
        g_kd.applied_parity == parity &&
        g_kd.applied_data_bits == data_bits) {
        g_kd.coding_dirty = false;
        return;
    }

    g_kd.coding_dirty = true;
}

static void kdcan_line_coding_cb(int itf, uint32_t bit_rate, uint8_t stop_bits,
                                 uint8_t parity, uint8_t data_bits) {
    (void)stop_bits;
    if (itf != USB_CDC_INTERFACE_DATA) {
        return;
    }
    /* Stash only: TinyUSB is not the link task, so vif_bus_param_set would
     * return VIF_ERR_NO_CLAIM. feed/poll applies on the owner thread. */
    kdcan_stash_line_coding(bit_rate, parity, data_bits);
    if (g_kd.coding_dirty) {
        ESP_LOGI(TAG, "CDC line coding %lu 8%c1",
                 (unsigned long)kdcan_effective_rate(bit_rate),
                 kdcan_parity_letter(parity));
    }
}

static void kdcan_break_cb(int itf, uint16_t duration_ms) {
    if (itf != USB_CDC_INTERFACE_DATA) {
        return;
    }
    /* 0 = deassert; 0xFFFF or any nonzero = assert (host bit-bangs timing). */
    g_kd.break_assert = (duration_ms != 0);
    g_kd.break_dirty = true;
    ESP_LOGI(TAG, "CDC break duration=%u", (unsigned)duration_ms);
    /* Same as line coding: apply from feed/poll on the link task. */
}

static void kdcan_break_clear(void) {
    uint32_t off = 0;

    g_kd.break_assert = false;
    g_kd.break_dirty = false;
    if (!g_kd.bus_open) {
        return;
    }
    (void)vif_bus_ioctl(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_IOCTL_BREAK_SET, &off,
                        NULL);
}

/* ------------------------------------------------------------------ *
 * Bus setup
 * ------------------------------------------------------------------ */

static esp_err_t kdcan_bus_ensure(void) {
    if (g_kd.bus_open) {
        return ESP_OK;
    }
    if (vif_bus_open(VIF_OWNER_LINK, VIF_BUS_KLINE, NULL) != ESP_OK) {
        return ESP_FAIL;
    }

    /*
     * Defaults match BMWeb's initial KDCAN open (115200 8N1). If the host
     * already SET_LINE_CODING (cached), flush_coding overrides these next.
     */
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_DATA_RATE,
                      KDCAN_DEFAULT_BAUD);
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_PARITY,
                      BUS_PARITY_NONE);
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_DATA_BITS,
                      BUS_DATA_BITS_8);
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_KLINE_FRAME_MODE,
                      BUS_KLINE_FRAME_RAW);
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_CHECKSUM_TX, 0);
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_CHECKSUM_RX, 0);
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_K_LINE_ONLY, 1);

    g_kd.bus_open = true;
    /* Defaults are live until CDC coding overrides them. */
    g_kd.applied_valid = true;
    g_kd.applied_bit_rate = KDCAN_DEFAULT_BAUD;
    g_kd.applied_parity = 0; /* CDC none */
    g_kd.applied_data_bits = 8;
    kdcan_flush_pending();
    return ESP_OK;
}

/** Pull TinyUSB's current coding in case SET_LINE_CODING beat our callback. */
static void kdcan_sync_host_coding(void) {
    uint32_t bit_rate = 0;
    uint8_t stop_bits = 0;
    uint8_t parity = 0;
    uint8_t data_bits = 0;

    if (!usb_cdc_get_line_coding(USB_CDC_INTERFACE_DATA, &bit_rate, &stop_bits,
                                 &parity, &data_bits)) {
        return;
    }
    if (!bit_rate) {
        return;
    }
    kdcan_stash_line_coding(bit_rate, parity, data_bits);
    kdcan_flush_coding();
}

/* ------------------------------------------------------------------ *
 * Frontend vtable
 * ------------------------------------------------------------------ */

static bool kdcan_fe_start(void) {
    memset(&g_kd, 0, sizeof(g_kd));

    /*
     * Session latch: BMWeb idles with DTR+RTS low; that must not drop the
     * USB data link. Latch APIs live in usb_cdc.h / usb_cdc.c.
     */
    usb_cdc_session_latch_enable(true);
    usb_cdc_session_latch_touch();
    usb_cdc_set_line_coding_callback(kdcan_line_coding_cb);
    usb_cdc_set_break_callback(kdcan_break_cb);

    if (kdcan_bus_ensure() != ESP_OK) {
        ESP_LOGW(TAG, "K-Line open failed at start; will retry on feed");
    } else {
        kdcan_sync_host_coding();
    }

    ESP_LOGI(TAG, "kdcan mode (raw K-Line UART), default %u 8N1",
             KDCAN_DEFAULT_BAUD);
    return true;
}

static void kdcan_fe_stop(void) {
    usb_cdc_set_line_coding_callback(NULL);
    usb_cdc_set_break_callback(NULL);
    usb_cdc_session_latch_enable(false);
    usb_cdc_session_latch_clear();

    kdcan_break_clear();
    if (g_kd.bus_open) {
        (void)vif_bus_release_all(VIF_OWNER_LINK);
        g_kd.bus_open = false;
    }
    memset(&g_kd, 0, sizeof(g_kd));
}

static void kdcan_fe_feed(const uint8_t *data, size_t len) {
    bus_msg_t msg;
    int rc;

    if (!data || !len) {
        return;
    }

    usb_cdc_session_latch_touch();

    if (kdcan_bus_ensure() != ESP_OK) {
        ESP_LOGW(TAG, "K-Line open failed");
        return;
    }

    /* Baud/parity/break must match the host before this telegram hits the wire. */
    kdcan_flush_pending();

    bus_msg_tx(&msg, data, len, 0);
    rc = vif_bus_send(VIF_OWNER_LINK, VIF_BUS_KLINE, &msg, BUS_TX_NO_CHECKSUM);
    if (rc != 0) {
        ESP_LOGW(TAG, "K-Line send failed: %d", rc);
    }
}

static void kdcan_fe_poll(void) {
    uint8_t buf[KDCAN_RX_BUF];
    bus_msg_t msg;
    int forwarded = 0;
    bool wrote = false;

    if (!g_kd.bus_open && !vif_bus_is_open(VIF_OWNER_LINK, VIF_BUS_KLINE)) {
        return;
    }
    if (!g_kd.bus_open) {
        g_kd.bus_open = true;
    }

    kdcan_flush_pending();

    while (forwarded < KDCAN_RX_BURST) {
        int rc;

        bus_msg_init(&msg, buf, sizeof(buf));
        rc = vif_bus_recv(VIF_OWNER_LINK, VIF_BUS_KLINE, &msg, 0);
        if (rc <= 0) {
            break;
        }
        if (msg.len) {
            vif_link_write(msg.data, msg.len);
            wrote = true;
        }
        forwarded++;
    }

    if (wrote) {
        vif_link_flush();
    }
}

const vif_frontend_t kdcan_frontend = {
    .name = "kdcan",
    .start = kdcan_fe_start,
    .feed = kdcan_fe_feed,
    .poll = kdcan_fe_poll,
    .stop = kdcan_fe_stop,
};

void kdcan_register(void) {
    if (vif_frontend_register(&kdcan_frontend) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register the kdcan front-end");
    }
}
