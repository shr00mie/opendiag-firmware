/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file ds2_codec.h
 * @brief BMW DS2 message framing helpers (E36/E38/E39/E46/E53 era K-Line).
 *
 * This is framing and checksum arithmetic only. It is NOT an EDIABAS stack:
 * there is no job interpreter, no SGBD/PRG/GRP description handling, no
 * address or service tables, and no session or timing logic. It exists so the
 * K-Line driver can end a DS2 message on its own length byte and so a host
 * can build and check the bytes a DS2 tester exchanges.
 *
 * A DS2 message is:
 *
 *   +---------+--------+------------------+----------+
 *   | address | length | data (0..n bytes)| checksum |
 *   +---------+--------+------------------+----------+
 *
 *  - address:  the ECU address on a request, or the tester/ECU address echoed
 *              on a reply (for example 0x12 for the DME).
 *  - length:   the length of the WHOLE message in bytes, address, length and
 *              checksum included. A message with no data is therefore 3.
 *  - data:     the service byte and its parameters.
 *  - checksum: the XOR of every preceding byte (address, length and data).
 *              This is the usual BMW DS2 convention; it is not the 8 bit sum
 *              ISO 9141-2/14230 use.
 *
 * Physical layer notes, which this file does not apply: DS2 normally runs at
 * 9600 baud, 8 data bits, even parity, 1 stop bit (BUS_P_PARITY and
 * BUS_P_DATA_RATE on the K-Line bus). Faster BMW rates exist on later
 * systems; the driver's hardware ceiling is KLINE_BAUD_HW_MAX.
 */

#pragma once

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/** @brief Address and length bytes. */
#define DS2_HEADER_LEN 2

/** @brief Address, length and checksum with no data: the shortest message. */
#define DS2_MIN_FRAME 3

/** @brief The length byte is eight bits, and counts the whole message. */
#define DS2_MAX_FRAME 255

/** @brief Most data bytes one message can carry. */
#define DS2_MAX_DATA (DS2_MAX_FRAME - DS2_MIN_FRAME)

/** @brief A message split into its parts. @c data points into the input. */
typedef struct {
    uint8_t address;
    const uint8_t *data;
    size_t data_len;
} ds2_msg_t;

/** @brief The XOR of @p len bytes. NULL or zero length gives 0. */
uint8_t ds2_checksum(const uint8_t *data, size_t len);

/**
 * @brief How long the message starting at @p buf is, from its length byte.
 *
 * @return The total on-wire length including the checksum, or 0 when it is not
 *         yet known (fewer than two bytes) or cannot be valid (a length byte
 *         below DS2_MIN_FRAME).
 */
size_t ds2_frame_len(const uint8_t *buf, size_t len);

/**
 * @brief Is @p buf exactly one complete message with a matching checksum?
 *
 * The length byte must equal @p len and the last byte must be the XOR of all
 * the others.
 */
bool ds2_frame_ok(const uint8_t *buf, size_t len);

/**
 * @brief Build a message: address, length, @p data, checksum.
 *
 * @param out  Destination.
 * @param cap  Room in @p out.
 * @return Total length written, or 0 when the data would not fit in a message
 *         (more than DS2_MAX_DATA), @p out is too small, or @p data is NULL
 *         with a non-zero @p data_len.
 */
size_t ds2_frame_build(uint8_t address, const uint8_t *data, size_t data_len,
                       uint8_t *out, size_t cap);

/**
 * @brief Overwrite the length byte of @p buf and append the checksum.
 *
 * For a caller that has laid down address, a placeholder length and data in
 * @p buf and wants it finished in place. @p len is the number of bytes
 * present (address, length, data), and @p cap must have room for one more.
 *
 * @return The finished length, or 0 on a bad argument.
 */
size_t ds2_frame_finish(uint8_t *buf, size_t len, size_t cap);

/** @brief Validate @p buf and split it into @p out. */
bool ds2_frame_parse(const uint8_t *buf, size_t len, ds2_msg_t *out);
