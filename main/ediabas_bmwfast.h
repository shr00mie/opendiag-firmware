/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file ediabas_bmwfast.h
 * @brief BMW-FAST UART envelope helpers for the Deep OBD / CanAdapterElm wire
 *        protocol (checksum and framed length).
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

/** @brief 8-bit sum of @p len bytes starting at @p data. */
uint8_t ediabas_bmwfast_checksum(const uint8_t *data, size_t len);

/**
 * @brief How many bytes a BMW-FAST (or Deep OBD setup) frame needs in total.
 *
 * @return Total length including checksum, or 0 if more header bytes are
 *         required before the length is known, or (size_t)-1 if invalid.
 */
size_t ediabas_bmwfast_frame_len(const uint8_t *buf, size_t len);

/** @brief True when @p buf is one complete frame with a matching checksum. */
bool ediabas_bmwfast_frame_ok(const uint8_t *buf, size_t len);

/**
 * @brief Append the BMW-FAST checksum to @p buf of @p len payload bytes.
 *
 * @return New length (len + 1), or 0 if @p cap is too small.
 */
size_t ediabas_bmwfast_finish(uint8_t *buf, size_t len, size_t cap);
