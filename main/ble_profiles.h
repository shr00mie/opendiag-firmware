/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/**
 * @file ble_profiles.h
 * @brief The GATT layouts the BLE adapter presents, as plain data.
 *
 * Off-the-shelf OBD-II apps do not discover a serial pipe, they look for one
 * of a handful of well-known vendor layouts and use the first they recognise.
 * This is that list, kept free of NimBLE so the host tests can assert it:
 * ble_uart.c reads the tables and builds the stack's own structures from them.
 *
 * All data layouts end up at the same place - the ELM327 front-end behind
 * ble_uart_send() and the receive callback. The control characteristic is not
 * one of them: it lives in a service of its own, so it is reachable whatever
 * a data layout is carrying, and a client that scans for a serial service
 * never mistakes it for one.
 *
 * UUID bytes are in the order they go over the air (little endian), which is
 * the order NimBLE's ble_uuid_init_from_buf() takes them in.
 */

/** @brief Longest characteristic list any profile carries. */
#define BLE_PROFILE_MAX_CHRS 2

/** @brief What a characteristic is for. */
typedef enum {
    /** Requests in: the central writes ELM327 command lines here. */
    BLE_CHR_DATA_RX,
    /** Replies out: the central subscribes and is notified. */
    BLE_CHR_DATA_TX,
    /** Both directions on one characteristic, as FFE1 does. */
    BLE_CHR_DATA_RXTX,
    /** The OpenDIAG control plane. Never part of a data profile. */
    BLE_CHR_CTRL,
} ble_chr_role_t;

/** @name Characteristic properties */
/** @{ */
#define BLE_CHR_PROP_WRITE (1u << 0)
#define BLE_CHR_PROP_WRITE_NO_RSP (1u << 1)
#define BLE_CHR_PROP_NOTIFY (1u << 2)
/** @} */

/** @brief A 16-bit or 128-bit UUID. */
typedef struct {
    uint8_t len;      /**< 2 or 16: the number of valid bytes in @c bytes. */
    uint8_t bytes[16]; /**< Little endian, as transmitted. */
} ble_profile_uuid_t;

typedef struct {
    ble_profile_uuid_t uuid;
    ble_chr_role_t role;
    uint8_t props; /**< BLE_CHR_PROP_* */
} ble_profile_chr_t;

typedef enum {
    BLE_PROFILE_NUS,  /**< Nordic UART Service. The original, and the default. */
    BLE_PROFILE_FFE0, /**< FFE0 / FFE1: Vgate and most HM-10 style dongles. */
    BLE_PROFILE_FFF0, /**< FFF0 / FFF1 / FFF2: Veepeak. */
    BLE_PROFILE_18F0, /**< 18F0 / 2AF1 / 2AF0. */
    BLE_PROFILE_COUNT,
} ble_profile_id_t;

typedef struct {
    ble_profile_id_t id;
    const char *name;
    ble_profile_uuid_t service;
    uint8_t num_chrs;
    ble_profile_chr_t chrs[BLE_PROFILE_MAX_CHRS];
} ble_profile_t;

/** @brief The control service and its one characteristic. */
typedef struct {
    ble_profile_uuid_t service;
    ble_profile_chr_t chr;
} ble_ctrl_service_t;

/**
 * @brief Who may use the data characteristics.
 *
 * STRICT is what the adapter ships with: an encrypted, bonded link, and only
 * ever one a button press invited. COMPAT is for bringing up an app that
 * cannot pair - it opens the data characteristics to an unencrypted link.
 * The control characteristic needs encryption in both.
 */
typedef enum {
    BLE_SEC_STRICT,
    BLE_SEC_COMPAT,
} ble_sec_mode_t;

/** @brief The profile for @p id, or NULL when out of range. */
const ble_profile_t *ble_profile_get(ble_profile_id_t id);

/** @brief The control service, outside every data profile. */
const ble_ctrl_service_t *ble_profile_ctrl(void);

/** @brief Whether two UUIDs are the same (length and bytes). */
bool ble_profile_uuid_equal(const ble_profile_uuid_t *a,
                            const ble_profile_uuid_t *b);

/** @brief The 16-bit value of a 2 byte UUID, or 0 for any other length. */
uint16_t ble_profile_uuid16(const ble_profile_uuid_t *u);

/** @brief Whether the characteristic carries data in the receive direction. */
bool ble_chr_is_rx(const ble_profile_chr_t *chr);

/** @brief Whether the characteristic carries data in the notify direction. */
bool ble_chr_is_tx(const ble_profile_chr_t *chr);

/**
 * @brief The 16-bit data service UUIDs, for the advertisement.
 *
 * @param[out] out Receives the service UUIDs, in profile order.
 * @param max      Room in @p out.
 * @return How many were stored. NUS is 128-bit and is never among them.
 */
size_t ble_profile_adv_uuid16s(uint16_t *out, size_t max);

/** @brief The text for a security mode: "strict" or "compat". */
const char *ble_sec_mode_name(ble_sec_mode_t mode);

/** @name GAP names
 *
 * Some apps filter scan results on the name. The adapter answers to its own,
 * or to the generic one those apps look for.
 */
/** @{ */
#define BLE_GAP_NAME_DEFAULT "OpenDIAG"

/** @brief The names the shell accepts, NULL terminated. */
const char *const *ble_profile_gap_names(void);

/** @brief The canonical spelling of @p name, or NULL when it is not allowed.
 *  Matching ignores case. */
const char *ble_profile_gap_name_find(const char *name);
/** @} */
