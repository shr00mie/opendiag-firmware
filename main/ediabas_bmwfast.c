/* SPDX-License-Identifier: GPL-3.0-only */
#include "ediabas_bmwfast.h"

uint8_t ediabas_bmwfast_checksum(const uint8_t *data, size_t len) {
    uint8_t sum = 0;

    if (!data) {
        return 0;
    }
    for (size_t i = 0; i < len; i++) {
        sum = (uint8_t)(sum + data[i]);
    }
    return sum;
}

size_t ediabas_bmwfast_frame_len(const uint8_t *buf, size_t len) {
    size_t tel_len;

    if (!buf || len < 4) {
        return 0;
    }

    if (buf[0] == 0x00) {
        switch (buf[1]) {
        case 0x00: /* K-Line telegram 1 */
            if (len < 8) {
                return 0;
            }
            tel_len = ((size_t)buf[6] << 8) + buf[7] + 9u;
            break;
        case 0x01: /* CAN telegram 1 (stub in v1) */
            if (len < 10) {
                return 0;
            }
            tel_len = ((size_t)buf[8] << 8) + buf[9] + 11u;
            break;
        case 0x02: /* K-Line telegram 2 */
            if (len < 10) {
                return 0;
            }
            tel_len = ((size_t)buf[8] << 8) + buf[9] + 11u;
            break;
        case 0x03: /* CAN telegram 2 (stub in v1) */
            if (len < 13) {
                return 0;
            }
            tel_len = ((size_t)buf[11] << 8) + buf[12] + 14u;
            break;
        default:
            return (size_t)-1;
        }
        return tel_len;
    }

    /* Standard BMW-FAST */
    tel_len = buf[0] & 0x3Fu;
    if (tel_len == 0) {
        if (buf[3] == 0) {
            if (len < 6) {
                return 0;
            }
            tel_len = ((size_t)buf[4] << 8) + buf[5] + 7u;
        } else {
            tel_len = (size_t)buf[3] + 5u;
        }
    } else {
        tel_len += 4u;
    }
    return tel_len;
}

bool ediabas_bmwfast_frame_ok(const uint8_t *buf, size_t len) {
    size_t want;

    if (!buf || len < 4) {
        return false;
    }
    want = ediabas_bmwfast_frame_len(buf, len);
    if (want == 0 || want == (size_t)-1 || want != len) {
        return false;
    }
    return ediabas_bmwfast_checksum(buf, len - 1) == buf[len - 1];
}

size_t ediabas_bmwfast_finish(uint8_t *buf, size_t len, size_t cap) {
    if (!buf || len + 1 > cap) {
        return 0;
    }
    buf[len] = ediabas_bmwfast_checksum(buf, len);
    return len + 1;
}
