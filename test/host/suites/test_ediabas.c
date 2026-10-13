/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_ediabas.c
 * @brief Deep OBD wire front-end: detect telegrams and E46 DS2 K-Line path.
 */

#include <string.h>

#include "td_test.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include "ds2_codec.h"
#include "ediabas.h"
#include "ediabas_bmwfast.h"
#include "fake_board.h"
#include "fake_bus.h"
#include "fake_clock.h"
#include "fake_led.h"
#include "fake_port.h"
#include "vif.h"

static comm_port_id_t g_port = COMM_INVALID_PORT_ID;

/* ------------------------------------------------------------------ *
 * Helpers
 * ------------------------------------------------------------------ */

static size_t finish(uint8_t *buf, size_t len, size_t cap) {
    return ediabas_bmwfast_finish(buf, len, cap);
}

/** Feed bytes and return host TX as a binary blob. */
static const uint8_t *ed_ask(const uint8_t *cmd, size_t cmd_len, size_t *out_len) {
    uint8_t buf[64];
    size_t n;

    fake_port_reset();

    TEST_ASSERT_EQUAL_INT(
        ESP_OK, comm_port_rx(fake_port_id(0), cmd, cmd_len));

    while ((n = comm_port_read(g_port, buf, sizeof(buf), 0)) > 0) {
        ediabas_frontend.feed(buf, n);
    }
    ediabas_frontend.poll();

    if (out_len) {
        *out_len = fake_port_len(0);
    }
    return (const uint8_t *)fake_port_text(0);
}

static void assert_mem(const uint8_t *got, size_t got_len, const uint8_t *want,
                       size_t want_len) {
    TEST_ASSERT_EQUAL_INT(want_len, got_len);
    TEST_ASSERT_EQUAL_MEM(want, got, want_len);
}

/* ------------------------------------------------------------------ *
 * Fixture
 * ------------------------------------------------------------------ */

static void ed_harness_init_once(void) {
    static bool done;

    if (done) {
        return;
    }
    done = true;

    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_iface_init());
    fake_port_register_all();
    g_port = fake_port_id(0);
    TEST_ASSERT_MSG(g_port != COMM_INVALID_PORT_ID, "no ediabas port");
}

void td_setup(void) {
    uint8_t drain[64];

    ed_harness_init_once();

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());

    fake_clock_reset();
    fake_led_reset();
    fake_board_reset();
    fake_bus_reset_all();

    while (comm_port_read(g_port, drain, sizeof(drain), 0) > 0) {
        ;
    }
    fake_port_reset();

    idf_stub_set_created_task(xTaskGetCurrentTaskHandle());
    ediabas_register();
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&ediabas_frontend));
    vif_shell_bind();
    vif_link_set_port(g_port);

    TEST_ASSERT_TRUE(ediabas_frontend.start());
}

void td_teardown(void) {
    ediabas_frontend.stop();
    TEST_ASSERT_MSG(idf_stub_lock_balance() == 0, "vif left %d locks held",
                    idf_stub_lock_balance());
}

/* ------------------------------------------------------------------ *
 * BMW-FAST codec
 * ------------------------------------------------------------------ */

TEST(bmwfast_checksum_is_the_byte_sum) {
    static const uint8_t d[] = {0x82, 0xF1, 0xF1, 0xFE, 0xFE};

    TEST_ASSERT_EQUAL_INT(0x60, ediabas_bmwfast_checksum(d, sizeof(d)));
}

TEST(bmwfast_short_frame_length) {
    uint8_t frame[] = {0x82, 0xF1, 0xF1, 0xFE, 0xFE, 0x00};

    frame[5] = ediabas_bmwfast_checksum(frame, 5);
    TEST_ASSERT_EQUAL_INT(6, ediabas_bmwfast_frame_len(frame, 6));
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(frame, 6));
}

TEST(kline_setup_tel2_length) {
    /* tel 0x02, baud_half 4800, flags, 5-byte payload + checksum */
    uint8_t frame[16] = {0x00, 0x02, 0x12, 0xC0, 0x21, 0x00, 0x00, 0x3C,
                         0x00, 0x05, 0x12, 0x05, 0x00, 0x00, 0x00, 0x00};
    size_t n = finish(frame, 15, sizeof(frame));

    TEST_ASSERT_EQUAL_INT(16, n);
    TEST_ASSERT_EQUAL_INT(16, ediabas_bmwfast_frame_len(frame, 16));
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(frame, 16));
}

/* ------------------------------------------------------------------ *
 * Detect / internal telegrams
 * ------------------------------------------------------------------ */

TEST(fe_echoes_and_reports_ignition) {
    uint8_t req[] = {0x82, 0xF1, 0xF1, 0xFE, 0xFE, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[5] = ediabas_bmwfast_checksum(req, 5);
    out = ed_ask(req, sizeof(req), &out_len);

    /* echo (6) + response (6) */
    TEST_ASSERT_EQUAL_INT(12, out_len);
    assert_mem(out, 6, req, 6);
    TEST_ASSERT_EQUAL_INT(0x82, out[6]);
    TEST_ASSERT_EQUAL_INT(0x81, out[10]); /* ignition + invalid mark */
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(out + 6, 6));
}

TEST(fd_reports_adapter_type_and_version) {
    uint8_t req[] = {0x82, 0xF1, 0xF1, 0xFD, 0xFD, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[5] = ediabas_bmwfast_checksum(req, 5);
    out = ed_ask(req, sizeof(req), &out_len);

    TEST_ASSERT_EQUAL_INT(6 + 9, out_len);
    assert_mem(out, 6, req, 6);
    TEST_ASSERT_EQUAL_INT(0x85, out[6]);
    TEST_ASSERT_EQUAL_INT(0x00, out[10]); /* type hi */
    TEST_ASSERT_EQUAL_INT(0x46, out[11]); /* type lo = 0x0046 */
    TEST_ASSERT_EQUAL_INT(0x00, out[12]); /* ver hi */
    TEST_ASSERT_EQUAL_INT(0x0F, out[13]); /* ver lo = 0x000F */
    TEST_ASSERT_TRUE(ediabas_bmwfast_frame_ok(out + 6, 9));
}

TEST(escape_mode_configure_round_trip) {
    /* Write escape: mode (send|rec)=3, code=FF, mask=80, all XOR 55 */
    uint8_t req[] = {0x84, 0xF1, 0xF1, 0x06, 0x00, 0x00, 0x00, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[4] = (uint8_t)(0x03 ^ 0x55);
    req[5] = (uint8_t)(0xFF ^ 0x55);
    req[6] = (uint8_t)(0x80 ^ 0x55);
    req[7] = ediabas_bmwfast_checksum(req, 7);

    out = ed_ask(req, sizeof(req), &out_len);
    TEST_ASSERT_EQUAL_INT(8 + 8, out_len);
    assert_mem(out, 8, req, 8);
    TEST_ASSERT_EQUAL_INT(req[4], out[12]);
    TEST_ASSERT_EQUAL_INT(req[5], out[13]);
    TEST_ASSERT_EQUAL_INT(req[6], out[14]);
}

TEST(fb_returns_serial_bytes) {
    uint8_t req[] = {0x82, 0xF1, 0xF1, 0xFB, 0xFB, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[5] = ediabas_bmwfast_checksum(req, 5);
    out = ed_ask(req, sizeof(req), &out_len);

    TEST_ASSERT_EQUAL_INT(6 + 13, out_len);
    TEST_ASSERT_EQUAL_INT(0x89, out[6]);
    TEST_ASSERT_EQUAL_MEM("OpenDIAG", out + 10, 8);
}

TEST(fc_reports_battery_voltage_tenths) {
    uint8_t req[] = {0x82, 0xF1, 0xF1, 0xFC, 0xFC, 0x00};
    size_t out_len = 0;
    const uint8_t *out;

    req[5] = ediabas_bmwfast_checksum(req, 5);
    out = ed_ask(req, sizeof(req), &out_len);

    TEST_ASSERT_EQUAL_INT(12, out_len);
    /* fake board default vbatt is 12600 mV → 126 tenths */
    TEST_ASSERT_EQUAL_INT(126, out[10]);
}

/* ------------------------------------------------------------------ *
 * DS2 K-Line path
 * ------------------------------------------------------------------ */

TEST(ds2_setup_sends_on_kline_at_9600_even_parity) {
    uint8_t frame[8];
    uint8_t reply_frame[8];
    uint8_t setup[24];
    size_t n;
    size_t rn;
    size_t setup_len;
    size_t out_len = 0;
    size_t sent_len = 0;
    const uint8_t *sent;
    const uint8_t *out;
    const uint8_t data[] = {0x00};

    n = ds2_frame_build(0x12, data, sizeof(data), frame, sizeof(frame));
    TEST_ASSERT_EQUAL_INT(4, n);

    rn = ds2_frame_build(0x12, (const uint8_t *)"\xA0\x00", 2, reply_frame,
                         sizeof(reply_frame));
    TEST_ASSERT_TRUE(rn >= 5);
    fake_bus_stage_response(FAKE_BUS_KLINE, reply_frame, rn, 5);

    /* tel2: baud_half=4800 (9600), flags1=NO_ECHO|EVEN=0x21 */
    setup[0] = 0x00;
    setup[1] = 0x02;
    setup[2] = 0x12;
    setup[3] = 0xC0;
    setup[4] = 0x21;
    setup[5] = 0x00;
    setup[6] = 0x00;
    setup[7] = 0x3C;
    setup[8] = 0x00;
    setup[9] = (uint8_t)n;
    memcpy(&setup[10], frame, n);
    setup_len = finish(setup, 10 + n, sizeof(setup));

    out = ed_ask(setup, setup_len, &out_len);

    TEST_ASSERT_EQUAL_INT(9600, (int)fake_bus_kline_baud());
    {
        uint32_t parity = 0;
        uint32_t mode = 0;

        TEST_ASSERT_EQUAL_INT(
            0, vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_KLINE, BUS_P_PARITY,
                                 &parity));
        TEST_ASSERT_EQUAL_INT(BUS_PARITY_EVEN, (int)parity);
        TEST_ASSERT_EQUAL_INT(
            0, vif_bus_param_get(VIF_OWNER_LINK, VIF_BUS_KLINE,
                                 BUS_P_KLINE_FRAME_MODE, &mode));
        TEST_ASSERT_EQUAL_INT(BUS_KLINE_FRAME_DS2, (int)mode);
    }

    TEST_ASSERT_EQUAL_INT(1, fake_bus_sent_count(FAKE_BUS_KLINE));
    sent = fake_bus_sent(FAKE_BUS_KLINE, 0, &sent_len);
    TEST_ASSERT_EQUAL_INT(n, sent_len);
    TEST_ASSERT_EQUAL_MEM(frame, sent, n);

    TEST_ASSERT_EQUAL_INT(rn, out_len);
    TEST_ASSERT_EQUAL_MEM(reply_frame, out, rn);
}

TEST(frontend_is_registered_as_ediabas) {
    TEST_ASSERT_NOT_NULL(vif_frontend_find("ediabas"));
    TEST_ASSERT_EQUAL_STRING("ediabas", vif_frontend_find("ediabas")->name);
}
