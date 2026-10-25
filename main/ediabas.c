/* SPDX-License-Identifier: GPL-3.0-only */
/* Deep OBD / CanAdapterElm front-end (E46 DS2): BMW-FAST and 0x00 setup
 * telegrams from the host; F1 F1 identifies the adapter; K-Line drives DS2. */

#include "ediabas.h"

#include <inttypes.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

#include "bus.h"
#include "ds2_codec.h"
#include "ediabas_bmwfast.h"
#include "utility.h"
#include "vif.h"

#define TAG "EDIABAS"

/* Deep OBD custom adapter identity. Version must be >= 0x000F for ConfigTool. */
#define EDIABAS_ADAPTER_TYPE 0x0046u
#define EDIABAS_ADAPTER_VERSION 0x000F

#define EDIABAS_RX_MAX 512
#define EDIABAS_TX_MAX 512
#define EDIABAS_KLINE_TIMEOUT_MS 2000
#define EDIABAS_KLINE_MIN_BAUD 980
#define EDIABAS_KLINE_MAX_BAUD 25000

/* K-Line flags1 (EdCustomAdapterCommon / CanAdapterElm). */
#define KLINEF1_PARITY_MASK 0x07
#define KLINEF1_PARITY_NONE 0x00
#define KLINEF1_PARITY_EVEN 0x01
#define KLINEF1_PARITY_ODD 0x02
#define KLINEF1_USE_LLINE 0x08
#define KLINEF1_SEND_PULSE 0x10
#define KLINEF1_NO_ECHO 0x20
#define KLINEF1_FAST_INIT 0x40
#define KLINEF1_USE_KLINE 0x80

typedef struct {
    uint8_t rx[EDIABAS_RX_MAX];
    size_t rx_len;

    bool escape_mode_send;
    bool escape_mode_rec;
    uint8_t escape_code;
    uint8_t escape_mask;
    bool rec_escape;
    bool send_escape;

    uint8_t kline_flags1;
    uint8_t kline_interbyte;
    uint32_t kline_baud;
    bool kline_open;
} ediabas_ctx_t;

static ediabas_ctx_t g_ed;

/* ------------------------------------------------------------------ *
 * Host I/O (with optional escape stuffing)
 * ------------------------------------------------------------------ */

static void ediabas_host_write_raw(const uint8_t *data, size_t len) {
    if (!data || !len) {
        return;
    }
    log_io_hex(TAG, "app <", data, len);
    if (!g_ed.escape_mode_send) {
        vif_link_write(data, len);
        return;
    }

    uint8_t out[EDIABAS_TX_MAX];
    size_t n = 0;

    for (size_t i = 0; i < len; i++) {
        uint8_t b = data[i];

        if (n + 2 > sizeof(out)) {
            vif_link_write(out, n);
            n = 0;
        }
        if (b == 0x00 || b == g_ed.escape_code) {
            out[n++] = g_ed.escape_code;
            out[n++] = (uint8_t)(b ^ g_ed.escape_mask);
        } else {
            out[n++] = b;
        }
    }
    if (n) {
        vif_link_write(out, n);
    }
}

static void ediabas_host_flush(void) { vif_link_flush(); }

/** Decode one escaped host byte into @p out; false if a second byte is needed. */
static bool ediabas_unescape_byte(uint8_t in, uint8_t *out) {
    if (!g_ed.escape_mode_rec) {
        *out = in;
        return true;
    }
    if (g_ed.rec_escape) {
        g_ed.rec_escape = false;
        *out = (uint8_t)(in ^ g_ed.escape_mask);
        return true;
    }
    if (in == g_ed.escape_code) {
        g_ed.rec_escape = true;
        return false;
    }
    *out = in;
    return true;
}

/* ------------------------------------------------------------------ *
 * Internal telegrams (xx F1 F1 …)
 * ------------------------------------------------------------------ */

static void ediabas_echo_then(const uint8_t *req, size_t req_len,
                              uint8_t *resp, size_t resp_len) {
    ediabas_host_write_raw(req, req_len);
    if (resp && resp_len) {
        ediabas_host_write_raw(resp, resp_len);
    }
    ediabas_host_flush();
}

static bool ediabas_handle_internal(uint8_t *buf, size_t len) {
    uint8_t resp[32];
    size_t resp_len;
    uint8_t cmd;

    if (len == 5 && buf[0] == 0x81 && buf[1] == 0x00 && buf[2] == 0x00) {
        /* Legacy baud/CAN-mode EEPROM poke: acknowledge with CAN mode off. */
        memcpy(resp, buf, len);
        resp[3] = (uint8_t)~0x00; /* ~can_mode, mode off */
        resp_len = ediabas_bmwfast_finish(resp, 4, sizeof(resp));
        ediabas_echo_then(buf, len, resp, resp_len);
        return true;
    }

    if (len < 6 || buf[1] != 0xF1 || buf[2] != 0xF1) {
        return false;
    }

    cmd = (uint8_t)(buf[3] & 0x7Fu);

    /* Echo the request first (Deep OBD detect requires it). */
    ediabas_host_write_raw(buf, len);

    if (len == 6 && cmd == 0x00) {
        /* Block size: report default 0x0F. */
        memcpy(resp, buf, 6);
        resp[4] = 0x0F;
        resp_len = ediabas_bmwfast_finish(resp, 5, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (len == 6 && cmd == 0x01) {
        memcpy(resp, buf, 6);
        resp[4] = 0x0A; /* sep time */
        resp_len = ediabas_bmwfast_finish(resp, 5, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (len == 6 && cmd == 0x02) {
        /* CAN mode: always report off for E46 DS2 v1. */
        memcpy(resp, buf, 6);
        resp[4] = 0x00;
        resp_len = ediabas_bmwfast_finish(resp, 5, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (len >= 6 && cmd == 0x04) {
        /* BT pin: none. */
        resp[0] = 0x81;
        resp[1] = 0xF1;
        resp[2] = 0xF1;
        resp[3] = buf[3];
        resp_len = ediabas_bmwfast_finish(resp, 4, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (len >= 6 && cmd == 0x05) {
        resp[0] = 0x81;
        resp[1] = 0xF1;
        resp[2] = 0xF1;
        resp[3] = buf[3];
        resp_len = ediabas_bmwfast_finish(resp, 4, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (len == 8 && cmd == 0x06) {
        if ((buf[3] & 0x80) == 0) {
            if (buf[4] == 0x00 || buf[5] == 0x00 || buf[6] == 0x00) {
                g_ed.escape_mode_send = false;
                g_ed.escape_mode_rec = false;
                g_ed.escape_code = 0xFF;
                g_ed.escape_mask = 0x80;
            } else {
                uint8_t mode_val = (uint8_t)(buf[4] ^ 0x55);

                g_ed.escape_mode_send = (mode_val & 0x01) != 0;
                g_ed.escape_mode_rec = (mode_val & 0x02) != 0;
                g_ed.escape_code = (uint8_t)(buf[5] ^ 0x55);
                g_ed.escape_mask = (uint8_t)(buf[6] ^ 0x55);
            }
            g_ed.rec_escape = false;
            g_ed.send_escape = false;
        }
        {
            uint8_t mode_val = 0;

            if (g_ed.escape_mode_send) {
                mode_val |= 0x01;
            }
            if (g_ed.escape_mode_rec) {
                mode_val |= 0x02;
            }
            memcpy(resp, buf, 8);
            resp[4] = (uint8_t)(mode_val ^ 0x55);
            resp[5] = (uint8_t)(g_ed.escape_code ^ 0x55);
            resp[6] = (uint8_t)(g_ed.escape_mask ^ 0x55);
            resp_len = ediabas_bmwfast_finish(resp, 7, sizeof(resp));
            ediabas_host_write_raw(resp, resp_len);
        }
    } else if (len == 6 && buf[3] == 0xFA && buf[4] == 0xFA) {
        resp[0] = 0x83;
        resp[1] = 0xF1;
        resp[2] = 0xF1;
        resp[3] = 0xFA;
        resp[4] = 0x00; /* CAN off, clamp invalid */
        resp[5] = 0x00;
        resp_len = ediabas_bmwfast_finish(resp, 6, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (len == 6 && buf[3] == 0xFB && buf[4] == 0xFB) {
        static const uint8_t serial[8] = {'O', 'p', 'e', 'n', 'D', 'I', 'A',
                                          'G'};

        resp[0] = 0x89;
        resp[1] = 0xF1;
        resp[2] = 0xF1;
        resp[3] = 0xFB;
        memcpy(&resp[4], serial, 8);
        resp_len = ediabas_bmwfast_finish(resp, 12, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (len == 6 && buf[3] == 0xFC && buf[4] == 0xFC) {
        int32_t mv = vif_vbatt_mv();
        uint8_t v10;

        if (mv < 0) {
            mv = 0;
        }
        /* Voltage * 10, same scaling as CanAdapterElm ADC path. */
        v10 = (uint8_t)((mv + 50) / 100);
        memcpy(resp, buf, 6);
        resp[4] = v10;
        resp_len = ediabas_bmwfast_finish(resp, 5, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (len == 6 && buf[3] == 0xFD && buf[4] == 0xFD) {
        resp[0] = 0x85;
        resp[1] = 0xF1;
        resp[2] = 0xF1;
        resp[3] = 0xFD;
        resp[4] = (uint8_t)(EDIABAS_ADAPTER_TYPE >> 8);
        resp[5] = (uint8_t)(EDIABAS_ADAPTER_TYPE & 0xFF);
        resp[6] = (uint8_t)(EDIABAS_ADAPTER_VERSION >> 8);
        resp[7] = (uint8_t)(EDIABAS_ADAPTER_VERSION & 0xFF);
        resp_len = ediabas_bmwfast_finish(resp, 8, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (len == 6 && buf[3] == 0xFE && buf[4] == 0xFE) {
        memcpy(resp, buf, 6);
        /* Bit0 ignition (assume on when powered); bit7 invalid mark. */
        resp[4] = 0x81;
        resp_len = ediabas_bmwfast_finish(resp, 5, sizeof(resp));
        ediabas_host_write_raw(resp, resp_len);
    } else if (buf[3] == 0xFF && buf[4] == 0xFF) {
        /* Soft session reset (no MCU reboot). */
        g_ed.escape_mode_send = false;
        g_ed.escape_mode_rec = false;
        g_ed.escape_code = 0xFF;
        g_ed.escape_mask = 0x80;
        g_ed.rec_escape = false;
        g_ed.kline_flags1 = 0;
        if (g_ed.kline_open) {
            (void)vif_bus_release_all(VIF_OWNER_LINK);
            g_ed.kline_open = false;
        }
    }

    ediabas_host_flush();
    return true;
}

/* ------------------------------------------------------------------ *
 * K-Line / DS2
 * ------------------------------------------------------------------ */

static uint32_t ediabas_parity_from_flags(uint8_t flags1) {
    switch (flags1 & KLINEF1_PARITY_MASK) {
    case KLINEF1_PARITY_EVEN:
        return BUS_PARITY_EVEN;
    case KLINEF1_PARITY_ODD:
        return BUS_PARITY_ODD;
    default:
        return BUS_PARITY_NONE;
    }
}

static esp_err_t ediabas_kline_ensure(uint32_t baud, uint8_t flags1,
                                      uint8_t interbyte) {
    bool k_only = (flags1 & KLINEF1_USE_LLINE) == 0;

    if (!g_ed.kline_open) {
        if (vif_bus_open(VIF_OWNER_LINK, VIF_BUS_KLINE, NULL) != ESP_OK) {
            return ESP_FAIL;
        }
        g_ed.kline_open = true;
    }

    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_DATA_RATE, baud);
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_PARITY,
                      ediabas_parity_from_flags(flags1));
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_KLINE_FRAME_MODE,
                      BUS_KLINE_FRAME_DS2);
    /* Host builds DS2 including XOR; we verify RX checksums. */
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_CHECKSUM_TX, 0);
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_CHECKSUM_RX, 1);
    vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_K_LINE_ONLY,
                      k_only ? 1 : 0);
    if (interbyte) {
        vif_bus_param_set(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_P4_MIN,
                          (uint32_t)interbyte);
    }

    g_ed.kline_baud = baud;
    g_ed.kline_flags1 = flags1;
    g_ed.kline_interbyte = interbyte;
    return ESP_OK;
}

static void ediabas_kline_xfer(const uint8_t *payload, size_t payload_len,
                               uint8_t flags1) {
    bus_msg_t msg;
    uint8_t rxbuf[DS2_MAX_FRAME];
    TickType_t deadline;
    int rc;

    /* PIC echoes the extracted payload (not the 0x00 setup wrapper). */
    if ((flags1 & KLINEF1_NO_ECHO) == 0 && payload_len) {
        ediabas_host_write_raw(payload, payload_len);
        ediabas_host_flush();
    }

    if ((flags1 & KLINEF1_SEND_PULSE) != 0) {
        if (payload_len >= 3 && payload[1] > 0) {
            /* Bit-bang pulse is not mapped to a stock IOCTL; E46 DS2 usually
             * does not need it. Apply config only. */
            ESP_LOGW(TAG, "pulse pattern ignored (E46 DS2 v1)");
        }
        return;
    }

    if ((flags1 & KLINEF1_FAST_INIT) != 0) {
        bus_init_t init = {0};

        init.address = 0x33;
        (void)vif_bus_ioctl(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_IOCTL_FAST_INIT,
                            &init, NULL);
    }

    if (payload_len == 0) {
        return;
    }

    log_io_hex(TAG, "bus tx", payload, payload_len);
    bus_msg_tx(&msg, payload, payload_len, 0);
    rc = vif_bus_send(VIF_OWNER_LINK, VIF_BUS_KLINE, &msg, BUS_TX_CLEAR_RX_QUEUE);
    if (rc != 0) {
        ESP_LOGW(TAG, "K-Line send failed: %d", rc);
        return;
    }

    bus_msg_init(&msg, rxbuf, sizeof(rxbuf));
    deadline = xTaskGetTickCount() + pdMS_TO_TICKS(EDIABAS_KLINE_TIMEOUT_MS);
    while ((int32_t)(deadline - xTaskGetTickCount()) > 0) {
        rc = vif_bus_recv(VIF_OWNER_LINK, VIF_BUS_KLINE, &msg, pdMS_TO_TICKS(20));
        if (rc > 0 && msg.len > 0) {
            log_io_hex(TAG, "bus rx", msg.data, msg.len);
            ediabas_host_write_raw(msg.data, msg.len);
            ediabas_host_flush();
            return;
        }
        if (rc < 0 && rc != BUS_ERR_TIMEOUT) {
            break;
        }
    }
}

static void ediabas_handle_kline_setup(const uint8_t *buf, size_t len) {
    uint8_t tel = buf[1];
    uint32_t baud_half;
    uint32_t baud;
    uint8_t flags1;
    uint8_t interbyte;
    uint16_t payload_len;
    const uint8_t *payload;

    if (tel == 0x00) {
        baud_half = ((uint32_t)buf[2] << 8) | buf[3];
        flags1 = buf[4];
        interbyte = buf[5];
        payload_len = (uint16_t)(((uint16_t)buf[6] << 8) | buf[7]);
        payload = &buf[8];
    } else if (tel == 0x02) {
        baud_half = ((uint32_t)buf[2] << 8) | buf[3];
        flags1 = buf[4];
        interbyte = buf[6];
        payload_len = (uint16_t)(((uint16_t)buf[8] << 8) | buf[9]);
        payload = &buf[10];
        (void)buf[5]; /* flags2: KWP1281 ignored in v1 */
        (void)buf[7];
    } else {
        return;
    }

    if ((size_t)(payload - buf) + payload_len + 1u > len) {
        return;
    }

    baud = baud_half << 1;
    if (baud == 0) {
        /* BMW-FAST 115200 K-Line — deferred past E46 DS2 v1. */
        ESP_LOGW(TAG, "BMW-FAST 115200 K-Line not in E46 DS2 v1");
        return;
    }
    if (baud == 2) {
        /* Auto-baud: treat as 9600 for DS2. */
        baud = 9600;
    }
    if (baud < EDIABAS_KLINE_MIN_BAUD || baud > EDIABAS_KLINE_MAX_BAUD) {
        ESP_LOGW(TAG, "K-Line baud %" PRIu32 " out of range", baud);
        return;
    }

    if (ediabas_kline_ensure(baud, flags1, interbyte) != ESP_OK) {
        ESP_LOGW(TAG, "failed to open K-Line");
        return;
    }

    ediabas_kline_xfer(payload, payload_len, flags1);
}

static void ediabas_handle_frame(uint8_t *buf, size_t len) {
    if (!ediabas_bmwfast_frame_ok(buf, len)) {
        ESP_LOGW(TAG, "bad frame len=%u", (unsigned)len);
        return;
    }

    if (ediabas_handle_internal(buf, len)) {
        return;
    }

    if (buf[0] == 0x00 && (buf[1] == 0x00 || buf[1] == 0x02)) {
        ediabas_handle_kline_setup(buf, len);
        return;
    }

    if (buf[0] == 0x00) {
        /* CAN setup telegrams: acknowledge by ignoring (E46 DS2 v1). */
        ESP_LOGW(TAG, "CAN setup tel 0x%02X ignored (E46 DS2 v1)", buf[1]);
        return;
    }

    /* Raw BMW-FAST at 115200 without a prior setup — not used for DS2. */
    ESP_LOGD(TAG, "unhandled BMW-FAST frame");
}

/* ------------------------------------------------------------------ *
 * Frontend vtable
 * ------------------------------------------------------------------ */

static bool ediabas_fe_start(void) {
    memset(&g_ed, 0, sizeof(g_ed));
    g_ed.escape_code = 0xFF;
    g_ed.escape_mask = 0x80;
    ESP_LOGI(TAG, "ediabas mode (E46 DS2), type=0x%04X ver=0x%04X",
             EDIABAS_ADAPTER_TYPE, EDIABAS_ADAPTER_VERSION);
    return true;
}

static void ediabas_fe_stop(void) {
    if (g_ed.kline_open) {
        (void)vif_bus_release_all(VIF_OWNER_LINK);
        g_ed.kline_open = false;
    }
    memset(&g_ed, 0, sizeof(g_ed));
    g_ed.escape_code = 0xFF;
    g_ed.escape_mask = 0x80;
}

static void ediabas_fe_feed(const uint8_t *data, size_t len) {
    for (size_t i = 0; i < len; i++) {
        uint8_t b;
        size_t want;

        if (!ediabas_unescape_byte(data[i], &b)) {
            continue;
        }
        if (g_ed.rx_len >= sizeof(g_ed.rx)) {
            g_ed.rx_len = 0;
            continue;
        }
        g_ed.rx[g_ed.rx_len++] = b;

        want = ediabas_bmwfast_frame_len(g_ed.rx, g_ed.rx_len);
        if (want == (size_t)-1) {
            g_ed.rx_len = 0;
            continue;
        }
        if (want == 0 || g_ed.rx_len < want) {
            continue;
        }
        if (g_ed.rx_len > want) {
            /* Should not happen with a well-behaved host; resync. */
            g_ed.rx_len = 0;
            continue;
        }
        log_io_hex(TAG, "app >", g_ed.rx, g_ed.rx_len);
        ediabas_handle_frame(g_ed.rx, g_ed.rx_len);
        g_ed.rx_len = 0;
    }
}

static void ediabas_fe_poll(void) {
    /* K-Line replies are collected synchronously in the setup path. */
}

const vif_frontend_t ediabas_frontend = {
    .name = "ediabas",
    .start = ediabas_fe_start,
    .feed = ediabas_fe_feed,
    .poll = ediabas_fe_poll,
    .stop = ediabas_fe_stop,
};

void ediabas_register(void) {
    if (vif_frontend_register(&ediabas_frontend) != ESP_OK) {
        ESP_LOGE(TAG, "Failed to register the ediabas front-end");
    }
}
