/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file test_ble_profiles.c
 * @brief The GATT layouts an off-the-shelf OBD-II app goes looking for.
 *
 * An app that does not find the UUIDs it was written against finds nothing at
 * all, so the values themselves are the contract. The other half is where the
 * control characteristic sits: not on the Nordic UART service, or a client
 * that scans for a serial pipe would treat the control plane as one.
 */

#include <string.h>

#include "ble_profiles.h"
#include "td_test.h"

void td_setup(void) {}

/** The 16-byte little-endian form of 6E40xxxx-B5A3-F393-E0A9-E50E24DCCA9E. */
static void nus_bytes(uint8_t lo, uint8_t out[16]) {
    static const uint8_t base[16] = {0x9e, 0xca, 0xdc, 0x24, 0x0e, 0xe5,
                                     0xa9, 0xe0, 0x93, 0xf3, 0xa3, 0xb5,
                                     0x00, 0x00, 0x40, 0x6e};

    memcpy(out, base, 16);
    out[12] = lo;
}

static void assert_nus_uuid(const ble_profile_uuid_t *u, uint8_t lo) {
    uint8_t want[16];

    nus_bytes(lo, want);
    TEST_ASSERT_EQUAL_INT(16, u->len);
    TEST_ASSERT_EQUAL_MEM(want, u->bytes, 16);
}

static const ble_profile_chr_t *find_role(const ble_profile_t *p,
                                          ble_chr_role_t role) {
    for (int i = 0; i < p->num_chrs; i++) {
        if (p->chrs[i].role == role) {
            return &p->chrs[i];
        }
    }
    return NULL;
}

TEST(nus_uses_the_nordic_uuids) {
    const ble_profile_t *p = ble_profile_get(BLE_PROFILE_NUS);

    TEST_ASSERT_NOT_NULL(p);
    assert_nus_uuid(&p->service, 0x01);
    TEST_ASSERT_EQUAL_INT(2, p->num_chrs);

    TEST_ASSERT_EQUAL_INT(BLE_CHR_DATA_RX, p->chrs[0].role);
    assert_nus_uuid(&p->chrs[0].uuid, 0x02);
    TEST_ASSERT(p->chrs[0].props & BLE_CHR_PROP_WRITE);
    TEST_ASSERT(p->chrs[0].props & BLE_CHR_PROP_WRITE_NO_RSP);
    TEST_ASSERT_FALSE(p->chrs[0].props & BLE_CHR_PROP_NOTIFY);

    TEST_ASSERT_EQUAL_INT(BLE_CHR_DATA_TX, p->chrs[1].role);
    assert_nus_uuid(&p->chrs[1].uuid, 0x03);
    TEST_ASSERT(p->chrs[1].props & BLE_CHR_PROP_NOTIFY);
    TEST_ASSERT_FALSE(p->chrs[1].props & BLE_CHR_PROP_WRITE);
}

TEST(ffe0_has_ffe1_rxtx_and_ffe2_write) {
    const ble_profile_t *p = ble_profile_get(BLE_PROFILE_FFE0);
    const ble_profile_chr_t *ffe2;

    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(0xFFE0, ble_profile_uuid16(&p->service));
    TEST_ASSERT_EQUAL_INT(2, p->num_chrs);
    TEST_ASSERT_EQUAL_INT(0xFFE1, ble_profile_uuid16(&p->chrs[0].uuid));
    TEST_ASSERT_EQUAL_INT(BLE_CHR_DATA_RXTX, p->chrs[0].role);
    TEST_ASSERT(ble_chr_is_rx(&p->chrs[0]));
    TEST_ASSERT(ble_chr_is_tx(&p->chrs[0]));
    TEST_ASSERT(p->chrs[0].props & BLE_CHR_PROP_WRITE);
    TEST_ASSERT(p->chrs[0].props & BLE_CHR_PROP_NOTIFY);

    ffe2 = &p->chrs[1];
    TEST_ASSERT_EQUAL_INT(0xFFE2, ble_profile_uuid16(&ffe2->uuid));
    TEST_ASSERT_EQUAL_INT(BLE_CHR_DATA_RX, ffe2->role);
    TEST_ASSERT(ble_chr_is_rx(ffe2));
    TEST_ASSERT_FALSE(ble_chr_is_tx(ffe2));
    TEST_ASSERT(ffe2->props & BLE_CHR_PROP_WRITE);
}

TEST(fff0_notifies_on_fff1_and_writes_on_fff2) {
    const ble_profile_t *p = ble_profile_get(BLE_PROFILE_FFF0);
    const ble_profile_chr_t *rx;
    const ble_profile_chr_t *tx;

    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(0xFFF0, ble_profile_uuid16(&p->service));

    rx = find_role(p, BLE_CHR_DATA_RX);
    tx = find_role(p, BLE_CHR_DATA_TX);
    TEST_ASSERT_NOT_NULL(rx);
    TEST_ASSERT_NOT_NULL(tx);
    /* WgSoft polarity: notify FFF1, write FFF2. */
    TEST_ASSERT_EQUAL_INT(0xFFF2, ble_profile_uuid16(&rx->uuid));
    TEST_ASSERT_EQUAL_INT(0xFFF1, ble_profile_uuid16(&tx->uuid));
    TEST_ASSERT(rx->props & BLE_CHR_PROP_WRITE);
    TEST_ASSERT(tx->props & BLE_CHR_PROP_NOTIFY);
}

TEST(profile_18f0_writes_on_2af1_and_notifies_on_2af0) {
    const ble_profile_t *p = ble_profile_get(BLE_PROFILE_18F0);
    const ble_profile_chr_t *rx;
    const ble_profile_chr_t *tx;

    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_INT(0x18F0, ble_profile_uuid16(&p->service));

    rx = find_role(p, BLE_CHR_DATA_RX);
    tx = find_role(p, BLE_CHR_DATA_TX);
    TEST_ASSERT_NOT_NULL(rx);
    TEST_ASSERT_NOT_NULL(tx);
    TEST_ASSERT_EQUAL_INT(0x2AF1, ble_profile_uuid16(&rx->uuid));
    TEST_ASSERT_EQUAL_INT(0x2AF0, ble_profile_uuid16(&tx->uuid));
    TEST_ASSERT(rx->props & BLE_CHR_PROP_WRITE);
    TEST_ASSERT(tx->props & BLE_CHR_PROP_NOTIFY);
}

TEST(sixteen_bit_uuids_go_over_the_air_little_endian) {
    const ble_profile_t *p = ble_profile_get(BLE_PROFILE_FFE0);

    TEST_ASSERT_EQUAL_INT(2, p->service.len);
    TEST_ASSERT_EQUAL_HEX32(0xE0, p->service.bytes[0]);
    TEST_ASSERT_EQUAL_HEX32(0xFF, p->service.bytes[1]);
}

TEST(every_data_profile_has_a_way_in_and_a_way_out) {
    for (int i = 0; i < BLE_PROFILE_COUNT; i++) {
        const ble_profile_t *p = ble_profile_get((ble_profile_id_t)i);
        bool rx = false;
        bool tx = false;

        TEST_ASSERT_NOT_NULL(p);
        TEST_ASSERT_EQUAL_INT(i, p->id);
        TEST_ASSERT(p->num_chrs > 0 && p->num_chrs <= BLE_PROFILE_MAX_CHRS);

        for (int c = 0; c < p->num_chrs; c++) {
            rx |= ble_chr_is_rx(&p->chrs[c]);
            tx |= ble_chr_is_tx(&p->chrs[c]);
            /* The control plane has no business in a data profile. */
            TEST_ASSERT(p->chrs[c].role != BLE_CHR_CTRL);
        }
        TEST_ASSERT(rx);
        TEST_ASSERT(tx);
    }

    TEST_ASSERT_NULL(ble_profile_get(BLE_PROFILE_COUNT));
}

TEST(no_uuid_is_claimed_twice) {
    const ble_ctrl_service_t *ctrl = ble_profile_ctrl();

    for (int i = 0; i < BLE_PROFILE_COUNT; i++) {
        const ble_profile_t *a = ble_profile_get((ble_profile_id_t)i);

        TEST_ASSERT_FALSE(ble_profile_uuid_equal(&a->service, &ctrl->service));
        TEST_ASSERT_FALSE(ble_profile_uuid_equal(&a->service, &ctrl->chr.uuid));

        for (int j = 0; j < BLE_PROFILE_COUNT; j++) {
            const ble_profile_t *b = ble_profile_get((ble_profile_id_t)j);

            if (i != j) {
                TEST_ASSERT_FALSE(
                    ble_profile_uuid_equal(&a->service, &b->service));
            }
        }

        for (int c = 0; c < a->num_chrs; c++) {
            TEST_ASSERT_FALSE(
                ble_profile_uuid_equal(&a->chrs[c].uuid, &ctrl->chr.uuid));
            TEST_ASSERT_FALSE(
                ble_profile_uuid_equal(&a->chrs[c].uuid, &ctrl->service));
        }
    }
}

TEST(control_is_not_on_the_nordic_uart_service) {
    const ble_ctrl_service_t *ctrl = ble_profile_ctrl();
    const ble_profile_t *nus = ble_profile_get(BLE_PROFILE_NUS);
    uint8_t nordic_base[16];

    TEST_ASSERT_NOT_NULL(ctrl);
    TEST_ASSERT_EQUAL_INT(BLE_CHR_CTRL, ctrl->chr.role);
    TEST_ASSERT(ctrl->chr.props & BLE_CHR_PROP_WRITE);
    TEST_ASSERT(ctrl->chr.props & BLE_CHR_PROP_NOTIFY);

    /* Its service is not the UART service... */
    TEST_ASSERT_FALSE(ble_profile_uuid_equal(&ctrl->service, &nus->service));
    TEST_ASSERT_EQUAL_INT(16, ctrl->service.len);

    /* ...nor a sibling of it on the Nordic base, which the old 6E400004
     * characteristic was. Bytes 0-11 and 13-15 are what the base is. */
    nus_bytes(0, nordic_base);
    TEST_ASSERT(memcmp(ctrl->service.bytes, nordic_base, 12) != 0);
    TEST_ASSERT(memcmp(ctrl->chr.uuid.bytes, nordic_base, 12) != 0);
    TEST_ASSERT(memcmp(ctrl->service.bytes + 13, nordic_base + 13, 3) != 0);

    /* And no characteristic of the UART service plays the control role. */
    TEST_ASSERT_NULL(find_role(nus, BLE_CHR_CTRL));
}

TEST(control_characteristic_sits_in_the_control_service_alone) {
    const ble_ctrl_service_t *ctrl = ble_profile_ctrl();

    TEST_ASSERT_EQUAL_INT(16, ctrl->chr.uuid.len);
    /* Same vendor base as its service, differing only in the 16-bit slot. */
    TEST_ASSERT_EQUAL_MEM(ctrl->service.bytes, ctrl->chr.uuid.bytes, 12);
    TEST_ASSERT_EQUAL_MEM(ctrl->service.bytes + 13, ctrl->chr.uuid.bytes + 13,
                          3);
    TEST_ASSERT(ctrl->service.bytes[12] != ctrl->chr.uuid.bytes[12]);
}

TEST(only_sixteen_bit_services_are_listed_for_the_advertisement) {
    uint16_t uuids[8];
    size_t n = ble_profile_adv_uuid16s(uuids, 8);

    TEST_ASSERT_EQUAL_INT(3, (int)n);
    TEST_ASSERT_EQUAL_INT(0xFFE0, uuids[0]);
    TEST_ASSERT_EQUAL_INT(0xFFF0, uuids[1]);
    TEST_ASSERT_EQUAL_INT(0x18F0, uuids[2]);

    /* Never overruns what it was given. */
    TEST_ASSERT_EQUAL_INT(2, (int)ble_profile_adv_uuid16s(uuids, 2));
}

TEST(gap_names_are_the_two_allowed_and_match_without_case) {
    TEST_ASSERT_EQUAL_STRING("OpenDIAG", BLE_GAP_NAME_DEFAULT);
    TEST_ASSERT_EQUAL_STRING("OpenDIAG", ble_profile_gap_name_find("opendiag"));
    TEST_ASSERT_EQUAL_STRING("OBDII", ble_profile_gap_name_find("obdii"));
    TEST_ASSERT_NULL(ble_profile_gap_name_find("VEEPEAK"));
    TEST_ASSERT_NULL(ble_profile_gap_name_find(""));
    TEST_ASSERT_NULL(ble_profile_gap_name_find(NULL));
    TEST_ASSERT_NOT_NULL(ble_profile_gap_names()[0]);
    TEST_ASSERT_NOT_NULL(ble_profile_gap_names()[1]);
    TEST_ASSERT_NULL(ble_profile_gap_names()[2]);
}

TEST(security_modes_have_names) {
    TEST_ASSERT_EQUAL_STRING("strict", ble_sec_mode_name(BLE_SEC_STRICT));
    TEST_ASSERT_EQUAL_STRING("compat", ble_sec_mode_name(BLE_SEC_COMPAT));
    /* The default is the closed one. */
    TEST_ASSERT_EQUAL_INT(0, BLE_SEC_STRICT);
}
