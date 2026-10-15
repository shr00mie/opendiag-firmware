/* SPDX-License-Identifier: GPL-3.0-only */
#include "ble_profiles.h"
#include <string.h>
#include <strings.h>

#define UUID16(v) {.len = 2, .bytes = {(uint8_t)((v) & 0xff), (uint8_t)((v) >> 8)}}

/* The Nordic UART base, B5A3-F393-E0A9-E50E24DCCA9E, with its 16-bit slot
 * filled in. */
#define NUS_UUID(lo)                                                           \
    {.len = 16,                                                                \
     .bytes = {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5, 0xa9, 0xe0, 0x93, 0xf3,    \
               0xa3, 0xb5, (lo), 0x00, 0x40, 0x6e}}

/*
 * The OpenDIAG vendor base, 6F70-656E-6469-61672D637472: the ASCII of
 * "opendiag-ctr". Deliberately not the Nordic base, so nothing that scans for
 * a UART service can come to this one by accident.
 */
#define CTRL_UUID(lo)                                                          \
    {.len = 16,                                                                \
     .bytes = {0x72, 0x74, 0x63, 0x2d, 0x67, 0x61, 0x69, 0x64, 0x6e, 0x65,    \
               0x70, 0x6f, (lo), 0x6c, 0x1a, 0x7d}}

#define RX_PROPS (BLE_CHR_PROP_WRITE | BLE_CHR_PROP_WRITE_NO_RSP)

static const ble_profile_t g_profiles[BLE_PROFILE_COUNT] = {
    {
        .id = BLE_PROFILE_NUS,
        .name = "nus",
        /* {6E400001-B5A3-F393-E0A9-E50E24DCCA9E} */
        .service = NUS_UUID(0x01),
        .num_chrs = 2,
        .chrs =
            {
                {NUS_UUID(0x02), BLE_CHR_DATA_RX, RX_PROPS},
                {NUS_UUID(0x03), BLE_CHR_DATA_TX, BLE_CHR_PROP_NOTIFY},
            },
    },
    {
        /* Deep OBD: notify FFE1, write FFE2. Carly uses FFE1 for both;
         * FFE1 keeps write+notify so that layout still matches. */
        .id = BLE_PROFILE_FFE0,
        .name = "ffe0",
        .service = UUID16(0xFFE0),
        .num_chrs = 2,
        .chrs =
            {
                {UUID16(0xFFE1), BLE_CHR_DATA_RXTX,
                 RX_PROPS | BLE_CHR_PROP_NOTIFY},
                {UUID16(0xFFE2), BLE_CHR_DATA_RX, RX_PROPS},
            },
    },
    {
        /* WgSoft / common FFF0 clones: notify FFF1, write FFF2. */
        .id = BLE_PROFILE_FFF0,
        .name = "fff0",
        .service = UUID16(0xFFF0),
        .num_chrs = 2,
        .chrs =
            {
                {UUID16(0xFFF1), BLE_CHR_DATA_TX, BLE_CHR_PROP_NOTIFY},
                {UUID16(0xFFF2), BLE_CHR_DATA_RX, RX_PROPS},
            },
    },
    {
        .id = BLE_PROFILE_18F0,
        .name = "18f0",
        .service = UUID16(0x18F0),
        .num_chrs = 2,
        .chrs =
            {
                {UUID16(0x2AF1), BLE_CHR_DATA_RX, RX_PROPS},
                {UUID16(0x2AF0), BLE_CHR_DATA_TX, BLE_CHR_PROP_NOTIFY},
            },
    },
};

/* {7D1A6C01-6F70-656E-6469-61672D637472}, characteristic 7D1A6C02-... */
static const ble_ctrl_service_t g_ctrl = {
    .service = CTRL_UUID(0x01),
    .chr = {CTRL_UUID(0x02), BLE_CHR_CTRL,
            RX_PROPS | BLE_CHR_PROP_NOTIFY},
};

static const char *const g_gap_names[] = {BLE_GAP_NAME_DEFAULT, "OBDII", NULL};

const ble_profile_t *ble_profile_get(ble_profile_id_t id) {
    if ((unsigned)id >= BLE_PROFILE_COUNT) {
        return NULL;
    }
    return &g_profiles[id];
}

const ble_ctrl_service_t *ble_profile_ctrl(void) { return &g_ctrl; }

bool ble_profile_uuid_equal(const ble_profile_uuid_t *a,
                            const ble_profile_uuid_t *b) {
    return a && b && a->len == b->len && memcmp(a->bytes, b->bytes, a->len) == 0;
}

uint16_t ble_profile_uuid16(const ble_profile_uuid_t *u) {
    if (!u || u->len != 2) {
        return 0;
    }
    return (uint16_t)(u->bytes[0] | (u->bytes[1] << 8));
}

bool ble_chr_is_rx(const ble_profile_chr_t *chr) {
    return chr && (chr->role == BLE_CHR_DATA_RX ||
                   chr->role == BLE_CHR_DATA_RXTX);
}

bool ble_chr_is_tx(const ble_profile_chr_t *chr) {
    return chr && (chr->role == BLE_CHR_DATA_TX ||
                   chr->role == BLE_CHR_DATA_RXTX);
}

size_t ble_profile_adv_uuid16s(uint16_t *out, size_t max) {
    size_t n = 0;

    for (int i = 0; i < BLE_PROFILE_COUNT && n < max; i++) {
        if (g_profiles[i].service.len == 2) {
            out[n++] = ble_profile_uuid16(&g_profiles[i].service);
        }
    }
    return n;
}

const char *ble_sec_mode_name(ble_sec_mode_t mode) {
    return mode == BLE_SEC_COMPAT ? "compat" : "strict";
}

const char *const *ble_profile_gap_names(void) { return g_gap_names; }

const char *ble_profile_gap_name_find(const char *name) {
    if (!name) {
        return NULL;
    }

    for (int i = 0; g_gap_names[i]; i++) {
        if (strcasecmp(name, g_gap_names[i]) == 0) {
            return g_gap_names[i];
        }
    }
    return NULL;
}
