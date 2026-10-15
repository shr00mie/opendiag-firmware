/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include <stdbool.h>
#include <stdint.h>

/**
 * @file elm327_user_can.h
 * @brief USER1/USER2 (ELM protocols B/C) option decode — stock PP 2C/2E bits.
 *
 * Kept free of the AT parser so host tests and bus bring-up share one policy:
 * format none and ISO 15765 open CAN; J1939 format is refused until that
 * stack exists. Brand-specific recipes (tester IDs, CEA) are client AT, not
 * baked in here.
 */

/** PP 2C / 2E bits 2..0 — data format. */
typedef enum {
    ELM327_USER_FMT_NONE = 0,   /**< Raw CAN (e.g. AT MA). */
    ELM327_USER_FMT_ISOTP = 1,  /**< ISO 15765-4 framing. */
    ELM327_USER_FMT_J1939 = 2,  /**< Not implemented yet. */
} elm327_user_fmt_t;

static inline elm327_user_fmt_t elm327_user_fmt(uint8_t opts) {
    return (elm327_user_fmt_t)(opts & 0x07u);
}

/** b7: 1 = 11-bit ID, 0 = 29-bit. */
static inline bool elm327_user_opts_11bit(uint8_t opts) {
    return (opts & 0x80u) != 0;
}

/** b6: 1 = variable DLC, 0 = fixed 8. */
static inline bool elm327_user_opts_var_dlc(uint8_t opts) {
    return (opts & 0x40u) != 0;
}

/**
 * @brief Whether USER1/USER2 may open the CAN driver for these PP values.
 *
 * Divisor must be 0x01..0x40. Format none and ISO-TP are supported; J1939
 * and reserved format codes are not.
 */
static inline bool elm327_user_can_openable(uint8_t opts, uint8_t div) {
    elm327_user_fmt_t fmt;

    if (div < 0x01u || div > 0x40u) {
        return false;
    }
    fmt = elm327_user_fmt(opts);
    return fmt == ELM327_USER_FMT_NONE || fmt == ELM327_USER_FMT_ISOTP;
}

/** Baud from PP 2D/2F: rate_kbps = 500 / div. */
static inline uint32_t elm327_user_bitrate(uint8_t div) {
    return 500000u / (uint32_t)div;
}

/** Default request ID when the client has not set AT SH. */
static inline uint32_t elm327_user_default_header(uint8_t opts) {
    return elm327_user_opts_11bit(opts) ? 0x7DFu : 0x18DB33F1u;
}
