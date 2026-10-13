/* SPDX-License-Identifier: GPL-3.0-only */
/* BMW DS2 framing helpers. See ds2_codec.h; not an EDIABAS stack. */

#include <string.h>

#include "ds2_codec.h"

uint8_t ds2_checksum(const uint8_t *data, size_t len) {
    uint8_t x = 0;

    if (!data) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        x ^= data[i];
    }
    return x;
}

size_t ds2_frame_len(const uint8_t *buf, size_t len) {
    if (!buf || len < DS2_HEADER_LEN || buf[1] < DS2_MIN_FRAME) {
        return 0;
    }
    return buf[1];
}

bool ds2_frame_ok(const uint8_t *buf, size_t len) {
    if (!buf || len < DS2_MIN_FRAME || ds2_frame_len(buf, len) != len) {
        return false;
    }
    return ds2_checksum(buf, len - 1) == buf[len - 1];
}

size_t ds2_frame_build(uint8_t address, const uint8_t *data, size_t data_len,
                       uint8_t *out, size_t cap) {
    size_t total;

    if (!out || (!data && data_len) || data_len > DS2_MAX_DATA) {
        return 0;
    }
    total = data_len + DS2_MIN_FRAME;
    if (cap < total) {
        return 0;
    }

    out[0] = address;
    out[1] = (uint8_t)total;
    if (data_len) {
        memcpy(&out[DS2_HEADER_LEN], data, data_len);
    }
    out[total - 1] = ds2_checksum(out, total - 1);
    return total;
}

size_t ds2_frame_finish(uint8_t *buf, size_t len, size_t cap) {
    size_t total;

    if (!buf || len < DS2_HEADER_LEN || len > DS2_MAX_FRAME - 1 ||
        cap < len + 1) {
        return 0;
    }
    total = len + 1;
    buf[1] = (uint8_t)total;
    buf[total - 1] = ds2_checksum(buf, total - 1);
    return total;
}

bool ds2_frame_parse(const uint8_t *buf, size_t len, ds2_msg_t *out) {
    if (!out || !ds2_frame_ok(buf, len)) {
        return false;
    }
    out->address = buf[0];
    out->data = &buf[DS2_HEADER_LEN];
    out->data_len = len - DS2_MIN_FRAME;
    return true;
}
