/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_ds2_codec.c
 * @brief BMW DS2 framing helpers, off target. Framing only, not EDIABAS.
 */

#include <string.h>

#include "ds2_codec.h"
#include "td_test.h"

TEST(checksum_is_the_xor_of_every_byte) {
    /* DME identification request: address 12, length 4, service 00. */
    const uint8_t req[] = {0x12, 0x04, 0x00};

    TEST_ASSERT_EQUAL_INT(0x16, ds2_checksum(req, sizeof(req)));
    TEST_ASSERT_EQUAL_INT(0, ds2_checksum(NULL, 4));
    TEST_ASSERT_EQUAL_INT(0, ds2_checksum(req, 0));
    /* Not the ISO 14230 sum: 0x12 + 0x04 + 0x00 is 0x16 here only by luck. */
    const uint8_t two[] = {0xFF, 0x01};
    TEST_ASSERT_EQUAL_INT(0xFE, ds2_checksum(two, sizeof(two)));
}

TEST(a_known_request_validates) {
    const uint8_t req[] = {0x12, 0x04, 0x00, 0x16};

    TEST_ASSERT_EQUAL_INT(4, ds2_frame_len(req, sizeof(req)));
    TEST_ASSERT_TRUE(ds2_frame_ok(req, sizeof(req)));
}

TEST(length_is_unknown_until_the_length_byte_arrives) {
    const uint8_t req[] = {0x12, 0x04, 0x00, 0x16};

    TEST_ASSERT_EQUAL_INT(0, ds2_frame_len(req, 0));
    TEST_ASSERT_EQUAL_INT(0, ds2_frame_len(req, 1));
    TEST_ASSERT_EQUAL_INT(4, ds2_frame_len(req, 2));
    TEST_ASSERT_EQUAL_INT(0, ds2_frame_len(NULL, 4));
}

TEST(a_length_below_the_minimum_is_never_valid) {
    for (unsigned v = 0; v < DS2_MIN_FRAME; v++) {
        const uint8_t buf[] = {0x12, (uint8_t)v, 0x00};

        TEST_ASSERT_EQUAL_INT(0, ds2_frame_len(buf, sizeof(buf)));
        TEST_ASSERT_FALSE(ds2_frame_ok(buf, sizeof(buf)));
    }
}

TEST(a_frame_with_no_data_is_three_bytes) {
    uint8_t out[8];
    size_t n = ds2_frame_build(0x12, NULL, 0, out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(3, n);
    TEST_ASSERT_EQUAL_INT(0x12, out[0]);
    TEST_ASSERT_EQUAL_INT(3, out[1]);
    TEST_ASSERT_EQUAL_INT(0x12 ^ 0x03, out[2]);
    TEST_ASSERT_TRUE(ds2_frame_ok(out, n));
}

TEST(build_matches_the_known_request) {
    const uint8_t data[] = {0x00};
    const uint8_t want[] = {0x12, 0x04, 0x00, 0x16};
    uint8_t out[8];
    size_t n = ds2_frame_build(0x12, data, sizeof(data), out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(sizeof(want), n);
    TEST_ASSERT_EQUAL_MEM(want, out, n);
}

TEST(build_refuses_what_does_not_fit) {
    uint8_t data[DS2_MAX_DATA + 1] = {0};
    uint8_t out[DS2_MAX_FRAME + 1];

    TEST_ASSERT_EQUAL_INT(0, ds2_frame_build(1, data, sizeof(data), out,
                                             sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, ds2_frame_build(1, data, 2, out, 4));
    TEST_ASSERT_EQUAL_INT(0, ds2_frame_build(1, NULL, 2, out, sizeof(out)));
    TEST_ASSERT_EQUAL_INT(0, ds2_frame_build(1, data, 0, NULL, 8));
}

TEST(the_largest_frame_is_255_bytes) {
    uint8_t data[DS2_MAX_DATA];
    uint8_t out[DS2_MAX_FRAME];

    memset(data, 0xA5, sizeof(data));
    size_t n = ds2_frame_build(0x80, data, sizeof(data), out, sizeof(out));

    TEST_ASSERT_EQUAL_INT(DS2_MAX_FRAME, n);
    TEST_ASSERT_EQUAL_INT(255, out[1]);
    TEST_ASSERT_TRUE(ds2_frame_ok(out, n));
}

TEST(finish_fixes_length_and_appends_the_checksum_in_place) {
    uint8_t buf[8] = {0x12, 0xEE, 0x00};
    size_t n = ds2_frame_finish(buf, 3, sizeof(buf));

    TEST_ASSERT_EQUAL_INT(4, n);
    TEST_ASSERT_EQUAL_INT(4, buf[1]);
    TEST_ASSERT_EQUAL_INT(0x16, buf[3]);
    TEST_ASSERT_TRUE(ds2_frame_ok(buf, n));

    TEST_ASSERT_EQUAL_INT(0, ds2_frame_finish(buf, 3, 3));
    TEST_ASSERT_EQUAL_INT(0, ds2_frame_finish(buf, 1, sizeof(buf)));
    TEST_ASSERT_EQUAL_INT(0, ds2_frame_finish(NULL, 3, sizeof(buf)));
}

TEST(validation_rejects_bad_checksums_and_lengths) {
    uint8_t req[] = {0x12, 0x04, 0x00, 0x16};

    req[3] ^= 0x01;
    TEST_ASSERT_FALSE(ds2_frame_ok(req, sizeof(req)));
    req[3] ^= 0x01;
    /* Truncated and over-long against the length byte. */
    TEST_ASSERT_FALSE(ds2_frame_ok(req, 3));
    uint8_t longer[5] = {0x12, 0x04, 0x00, 0x16, 0x00};
    TEST_ASSERT_FALSE(ds2_frame_ok(longer, sizeof(longer)));
    TEST_ASSERT_FALSE(ds2_frame_ok(NULL, 4));
    TEST_ASSERT_FALSE(ds2_frame_ok(req, 0));
}

TEST(parse_splits_a_valid_frame) {
    const uint8_t data[] = {0xA0, 0x01, 0x02};
    uint8_t buf[16];
    ds2_msg_t m;
    size_t n = ds2_frame_build(0x00, data, sizeof(data), buf, sizeof(buf));

    TEST_ASSERT_TRUE(ds2_frame_parse(buf, n, &m));
    TEST_ASSERT_EQUAL_INT(0x00, m.address);
    TEST_ASSERT_EQUAL_INT(sizeof(data), m.data_len);
    TEST_ASSERT_EQUAL_MEM(data, m.data, sizeof(data));

    buf[n - 1] ^= 0xFF;
    TEST_ASSERT_FALSE(ds2_frame_parse(buf, n, &m));
    TEST_ASSERT_FALSE(ds2_frame_parse(buf, n, NULL));
}

TEST(every_built_frame_round_trips) {
    uint8_t data[DS2_MAX_DATA];
    uint8_t out[DS2_MAX_FRAME];

    for (size_t i = 0; i < sizeof(data); i++)
        data[i] = (uint8_t)(i * 7 + 3);
    for (size_t n = 0; n <= DS2_MAX_DATA; n++) {
        size_t total = ds2_frame_build(0x44, data, n, out, sizeof(out));
        ds2_msg_t m;

        TEST_ASSERT_EQUAL_INT(n + DS2_MIN_FRAME, total);
        TEST_ASSERT_EQUAL_INT(total, ds2_frame_len(out, total));
        TEST_ASSERT_TRUE(ds2_frame_parse(out, total, &m));
        TEST_ASSERT_EQUAL_INT(n, m.data_len);
    }
}
