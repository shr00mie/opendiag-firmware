/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_kdcan.c
 * @brief K+DCAN UART front-end: raw pipe, line coding, break, CDC latch.
 */

#include <string.h>

#include "td_test.h"

#include "freertos/semphr.h"
#include "freertos/task.h"

#include "fake_board.h"
#include "fake_bus.h"
#include "fake_clock.h"
#include "fake_led.h"
#include "fake_port.h"
#include "fake_usb_cdc.h"
#include "kdcan.h"
#include "usb_cdc.h"
#include "vif.h"

static comm_port_id_t g_port = COMM_INVALID_PORT_ID;

static void kd_harness_init_once(void) {
    static bool done;

    if (done) {
        return;
    }
    done = true;

    TEST_ASSERT_EQUAL_INT(ESP_OK, comm_iface_init());
    fake_port_register_all();
    g_port = fake_port_id(0);
    TEST_ASSERT_MSG(g_port != COMM_INVALID_PORT_ID, "no kdcan port");
}

void td_setup(void) {
    uint8_t drain[64];

    kd_harness_init_once();

    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_init());

    fake_clock_reset();
    fake_led_reset();
    fake_board_reset();
    fake_bus_reset_all();
    fake_usb_cdc_reset();

    while (comm_port_read(g_port, drain, sizeof(drain), 0) > 0) {
        ;
    }
    fake_port_reset();

    idf_stub_set_created_task(xTaskGetCurrentTaskHandle());
    kdcan_register();
    TEST_ASSERT_EQUAL_INT(ESP_OK, vif_link_start(&kdcan_frontend));
    vif_link_service();
    vif_shell_bind();
    vif_link_set_port(g_port);

    TEST_ASSERT_TRUE(kdcan_frontend.start());
}

void td_teardown(void) {
    kdcan_frontend.stop();
    TEST_ASSERT_MSG(idf_stub_lock_balance() == 0, "vif left %d locks held",
                    idf_stub_lock_balance());
}

/* ------------------------------------------------------------------ *
 * Defaults and byte pipe
 * ------------------------------------------------------------------ */

TEST(kdcan_start_opens_raw_kline_at_115200_8n1) {
    TEST_ASSERT_EQUAL_STRING("kdcan", vif_link_frontend_name());
    TEST_ASSERT_TRUE(fake_bus_is_up(FAKE_BUS_KLINE));
    TEST_ASSERT_EQUAL_INT(115200, (int)fake_bus_kline_baud());
    TEST_ASSERT_EQUAL_INT(BUS_PARITY_NONE, (int)fake_bus_kline_parity());
    TEST_ASSERT_EQUAL_INT(BUS_KLINE_FRAME_RAW, (int)fake_bus_kline_frame_mode());
}

TEST(kdcan_feed_sends_host_bytes_on_kline) {
    static const uint8_t req[] = {0x12, 0x04, 0x00, 0x16};
    uint8_t chunk[16];
    size_t n;

    fake_port_reset();
    TEST_ASSERT_EQUAL_INT(ESP_OK,
                          comm_port_rx(fake_port_id(0), req, sizeof(req)));

    while ((n = comm_port_read(g_port, chunk, sizeof(chunk), 0)) > 0) {
        kdcan_frontend.feed(chunk, n);
    }

    TEST_ASSERT_EQUAL_INT(1, fake_bus_sent_count(FAKE_BUS_KLINE));
    {
        size_t len = 0;
        const uint8_t *tx = fake_bus_sent(FAKE_BUS_KLINE, 0, &len);
        TEST_ASSERT_EQUAL_INT(sizeof(req), len);
        TEST_ASSERT_EQUAL_MEM(req, tx, sizeof(req));
    }
}

TEST(kdcan_poll_forwards_kline_rx_to_host) {
    static const uint8_t reply[] = {0x12, 0x05, 0xa0, 0x01, 0xb6};
    size_t host_len = 0;

    fake_bus_stage_stale(FAKE_BUS_KLINE, reply, sizeof(reply));
    fake_port_reset();

    kdcan_frontend.poll();

    host_len = fake_port_len(0);
    TEST_ASSERT_EQUAL_INT(sizeof(reply), host_len);
    TEST_ASSERT_EQUAL_MEM(reply, fake_port_text(0), sizeof(reply));
}

/* ------------------------------------------------------------------ *
 * Line coding and break
 * ------------------------------------------------------------------ */

TEST(kdcan_line_coding_updates_baud_and_parity) {
    /* CDC parity 2 = even. */
    fake_usb_cdc_line_coding(USB_CDC_INTERFACE_DATA, 9600, 0, 2, 8);

    TEST_ASSERT_EQUAL_INT(9600, (int)fake_bus_kline_baud());
    TEST_ASSERT_EQUAL_INT(BUS_PARITY_EVEN, (int)fake_bus_kline_parity());
}

TEST(kdcan_break_maps_to_ioctl) {
    fake_usb_cdc_send_break(USB_CDC_INTERFACE_DATA, 0xFFFF);
    TEST_ASSERT_EQUAL_INT(1, (int)fake_bus_kline_break());

    fake_usb_cdc_send_break(USB_CDC_INTERFACE_DATA, 0);
    TEST_ASSERT_EQUAL_INT(0, (int)fake_bus_kline_break());
}

/* ------------------------------------------------------------------ *
 * Session latch
 * ------------------------------------------------------------------ */

TEST(kdcan_latch_survives_dtr_rts_idle) {
    int before;

    /* Already latched by start()'s touch. */
    TEST_ASSERT_TRUE(usb_cdc_get_line_status(USB_CDC_INTERFACE_DATA));

    before = fake_usb_cdc_line_events();
    /* BMWeb K-line idle: both lines low must not drop the session. */
    fake_usb_cdc_line_state(USB_CDC_INTERFACE_DATA, false, false);

    TEST_ASSERT_TRUE(usb_cdc_get_line_status(USB_CDC_INTERFACE_DATA));
    /* No disconnect event while latch holds. */
    TEST_ASSERT_EQUAL_INT(before, fake_usb_cdc_line_events());
}

TEST(kdcan_stop_clears_latch) {
    TEST_ASSERT_TRUE(usb_cdc_get_line_status(USB_CDC_INTERFACE_DATA));

    kdcan_frontend.stop();

    TEST_ASSERT_FALSE(usb_cdc_get_line_status(USB_CDC_INTERFACE_DATA));
}

TEST(kdcan_umount_clears_latch) {
    TEST_ASSERT_TRUE(usb_cdc_get_line_status(USB_CDC_INTERFACE_DATA));

    fake_usb_cdc_umount();

    TEST_ASSERT_FALSE(usb_cdc_get_line_status(USB_CDC_INTERFACE_DATA));
}
