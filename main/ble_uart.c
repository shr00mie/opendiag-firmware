/* SPDX-License-Identifier: GPL-3.0-only */
#include "ble_uart.h"
#include <inttypes.h>
#include <stdio.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "common.h"
#include "host/ble_gatt.h"
#include "host/ble_hs.h"
#include "host/ble_uuid.h"
#include "host/util/util.h"
#include "nimble/nimble_npl.h"
#include "nimble/nimble_port.h"
#include "nvs.h"
#include "nvs_flash.h"
#include "sdkconfig.h"
#include "services/gap/ble_svc_gap.h"
#include "services/gatt/ble_svc_gatt.h"
#include "ws2812_led.h"

#define TAG "BLE_UART"
#define ENABLE_BLE_TRACE 0

#define BLE_GAP_APPEARANCE_GENERIC_TAG 0x0200
#define BLE_GAP_URI_PREFIX_HTTPS 0x17
#define BLE_GAP_LE_ROLE_PERIPHERAL 0x00

/** How long one press of the button keeps the door open. */
#define BLE_PAIRING_WINDOW_MS 60000

extern void ble_store_config_init(void);

static ble_uart_rx_cb_t ble_rx_callback;

/*
 * The characteristics the GATT table registers, flattened. ble_profiles.c is
 * the contract - which services, which UUIDs, which direction - and this is
 * the table built from it, so the access callback can say what a write landed
 * on by looking at the slot it was handed rather than comparing UUIDs.
 *
 * Every data profile writes into the same receive callback and is answered
 * through the same ble_uart_send(): the profiles are only different doors onto
 * one session. The control characteristic is the one slot that is not.
 */
#define BLE_SLOT_CTRL BLE_PROFILE_COUNT /* "profile" index of the control slot */
#define BLE_SLOT_MAX (BLE_PROFILE_COUNT * BLE_PROFILE_MAX_CHRS + 1)

typedef struct {
    const ble_profile_chr_t *def;
    uint8_t profile; /* ble_profile_id_t, or BLE_SLOT_CTRL */
    uint16_t handle; /* Filled in by the stack at registration. */
} ble_slot_t;

static ble_slot_t g_slots[BLE_SLOT_MAX];
static int g_slot_count;

/* One service and its characteristics' UUIDs, in the stack's own type. */
static ble_uuid_any_t g_uuids[(BLE_PROFILE_COUNT + 1) *
                              (1 + BLE_PROFILE_MAX_CHRS)];
static int g_uuid_count;

static struct ble_gatt_chr_def g_chr_defs[BLE_PROFILE_COUNT + 1]
                                         [BLE_PROFILE_MAX_CHRS + 1];
static struct ble_gatt_svc_def g_svc_defs[BLE_PROFILE_COUNT + 2];

/* Connection handle for the current connection */
static uint16_t conn_handle;

/*
 * Which data profile the client is using, so replies go out on the
 * characteristic it subscribed to or last wrote to. Starts on NUS, which is
 * where the adapter always answered before the other layouts existed.
 */
static volatile uint8_t g_active_profile = BLE_PROFILE_NUS;

/* The control slot, for replies. NULL until the table is built. */
static const ble_slot_t *g_ctrl_slot;

/**
 * @brief Largest single GATT write this accepts.
 *
 * Comfortably past any ELM327 command line and past the control plane's
 * longest, so the refusal below is a guard rather than a limit anyone meets.
 */
#define BLE_RX_FLAT_MAX 512

#if ENABLE_BLE_TRACE

#define BLE_TRACE_ENTRIES 512
#define BLE_TRACE_PAYLOAD 48

typedef struct {
    char dir;  /* '<' from the client, '>' to it. */
    bool cont; /* Continues the record before it. */
    uint8_t len;
    uint8_t data[BLE_TRACE_PAYLOAD];
} ble_trace_rec_t;

static ble_trace_rec_t g_trace[BLE_TRACE_ENTRIES];
static uint16_t g_trace_head;  /* Next slot to write. */
static uint16_t g_trace_count; /* Valid slots, capped at BLE_TRACE_ENTRIES. */
static bool g_trace_on = true;

static void ble_trace(char dir, const uint8_t *data, size_t len) {
    bool cont = false;

    if (!g_trace_on || !data || !len) {
        return;
    }

    while (len) {
        size_t n = len < BLE_TRACE_PAYLOAD ? len : BLE_TRACE_PAYLOAD;
        ble_trace_rec_t *r = &g_trace[g_trace_head];

        r->dir = dir;
        r->cont = cont;
        r->len = (uint8_t)n;
        memcpy(r->data, data, n);

        g_trace_head = (uint16_t)((g_trace_head + 1) % BLE_TRACE_ENTRIES);
        if (g_trace_count < BLE_TRACE_ENTRIES) {
            g_trace_count++;
        }

        data += n;
        len -= n;
        cont = true;
    }
}

#else

#define ble_trace(dir, data, len) ((void)0)

#endif /* ENABLE_BLE_TRACE */

/* Deadline in esp_timer microseconds, or 0 when the window was never opened. */
static volatile int64_t g_window_until_us;

/* What the bond store held, last time anybody looked. */
static int g_bond_count;
static bool g_bonds_resolvable;

/* This connection formed or replaced a bond, so the window has done its job. */
static bool g_bond_changed;
static int g_bonds_at_connect;
static uint16_t g_latched_conn = 0xffff;
static bool g_pairing_allowed;

/*
 * The security mode, and the name the adapter answers to. Both survive a
 * reboot, and both are read once before the stack comes up. A mode change
 * applies to the next connection: a link already up keeps the rules it was
 * let in under.
 */
#define BLE_CFG_NAMESPACE "ble"
#define BLE_CFG_KEY_SEC "sec"
#define BLE_CFG_KEY_NAME "name"

static volatile ble_sec_mode_t g_sec_mode = BLE_SEC_STRICT;
static const char *g_gap_name = BLE_GAP_NAME_DEFAULT;

/* The mode the current connection was admitted under. */
static ble_sec_mode_t g_conn_sec_mode = BLE_SEC_STRICT;

static void ble_pair_policy_set_bonds(int count, bool all_resolvable) {
    g_bond_count = count;
    g_bonds_resolvable = all_resolvable;
}

static bool ble_pair_policy_bonds_resolvable(void) {
    return g_bonds_resolvable;
}

static bool ble_pair_policy_window_open(void) {
    return g_window_until_us != 0 && esp_timer_get_time() < g_window_until_us;
}

static void ble_pair_policy_apply_led(void) {
    ws2812_led_set_state(ble_pair_policy_window_open() ? LED_STATE_PAIRING
                                                        : LED_STATE_IDLE);
}

static void ble_pair_policy_open_window(void) {
    g_window_until_us =
        esp_timer_get_time() + (int64_t)BLE_PAIRING_WINDOW_MS * 1000;

    ESP_LOGI(TAG, "pairing window open for %d s", BLE_PAIRING_WINDOW_MS / 1000);
    ble_pair_policy_apply_led();
}

static void ble_pair_policy_close_window(void) {
    if (g_window_until_us == 0) {
        return;
    }

    g_window_until_us = 0;
    ESP_LOGI(TAG, "pairing window closed");
    ble_pair_policy_apply_led();
}

static ble_adv_state_t ble_pair_policy_adv_state(void) {
    if (g_sec_mode == BLE_SEC_COMPAT) {
        return BLE_ADV_OPEN;
    }

    if (ble_pair_policy_window_open()) {
        return BLE_ADV_PAIRABLE;
    }

    /*
     * Nothing bonded and nobody at the button: there is no phone this could
     * be talking to, so it says nothing.
     */
    if (g_bond_count == 0) {
        return BLE_ADV_SILENT;
    }

    /*
     * A bond with no identity key on file cannot be matched against the
     * private address its owner's phone rotates through. Filtering on it would
     * refuse that phone silently and for ever, which is a far worse failure
     * than letting a stranger reach a link that still refuses to bond.
     */
    if (!g_bonds_resolvable) {
        return BLE_ADV_DEGRADED;
    }

    return BLE_ADV_LOCKED;
}

const char *ble_adv_state_name(ble_adv_state_t state) {
    switch (state) {
    case BLE_ADV_SILENT:
        return "silent (press the button to pair)";
    case BLE_ADV_PAIRABLE:
        return "pairable";
    case BLE_ADV_OPEN:
        return "open to any phone (compat mode)";
    case BLE_ADV_LOCKED:
        return "bonded phones only";
    case BLE_ADV_DEGRADED:
        return "unfiltered (a bond has no identity key)";
    }
    return "?";
}

static void ble_pair_policy_latch(uint16_t conn_handle, int bonds_now) {
    if (conn_handle == g_latched_conn) {
        return;
    }

    g_latched_conn = conn_handle;
    g_bond_changed = false;
    g_bonds_at_connect = bonds_now;
    g_pairing_allowed = ble_pair_policy_window_open();
}

static void ble_pair_policy_unlatch(uint16_t conn_handle) {
    if (conn_handle == g_latched_conn) {
        g_latched_conn = 0xffff;
    }
}

static int ble_pair_policy_bonds_at_connect(void) { return g_bonds_at_connect; }

/*
 * COMPAT has no door to guard: it exists so an app that cannot pair can be
 * brought up, so anyone may connect and a bond is nothing to refuse.
 */
static bool ble_pair_policy_may_bond(void) {
    return g_sec_mode == BLE_SEC_COMPAT || g_pairing_allowed ||
           ble_pair_policy_window_open();
}

static void ble_pair_policy_note_bond_changed(void) { g_bond_changed = true; }

static bool ble_pair_policy_bond_changed(void) { return g_bond_changed; }

/* What the receive side has had to deal with, for the shell. */
static uint32_t g_rx_bytes;
static uint32_t g_rx_write_max;
static uint32_t g_rx_oversize;
static uint32_t g_rx_flatten_err;

static ble_uart_ctrl_cb_t ble_ctrl_callback;
static ble_uart_conn_cb_t ble_conn_callback;

static uint8_t own_addr_type;
static uint8_t addr_val[6] = {0};
static volatile uint8_t connected;

static void adv_apply(void);

/*
 * Three other tasks can change what the radio should be doing: the button
 * task and the shell open the pairing window, and an esp_timer shuts it. None
 * of them may do the work themselves. Stopping advertising and starting it
 * again is only reliable from the host's own event context, ble_gap_wl_set()
 * blocks until the controller acknowledges it, and an esp_timer callback is
 * the last place to be waiting on HCI. So they post, and the host task acts.
 */

#define BLE_JOB_ADV_REFRESH (1u << 0)
#define BLE_JOB_FORGET_BONDS (1u << 1)
#define BLE_JOB_APPLY_NAME (1u << 2)

static volatile uint32_t g_pending_jobs;
static struct ble_npl_event g_job_ev;
static bool g_jobs_ready;

static void ble_job_post(uint32_t job) {
    if (!g_jobs_ready) {
        return;
    }

    g_pending_jobs |= job;
    ble_npl_eventq_put(nimble_port_get_dflt_eventq(), &g_job_ev);
}

/* Shuts the window when its minute is up, so the LED stops saying "open". */
static esp_timer_handle_t g_pair_timer;

/*
 * Catches an adapter that has fallen silent behind our back.
 *
 * NimBLE restarts advertising by itself after some disconnects
 * (CONFIG_BT_NIMBLE_ENABLE_CONN_REATTEMPT) and stops it by itself when a new
 * bond's identity key is written to the controller. Neither tells this file.
 * An adapter that has quietly stopped advertising is indistinguishable from a
 * dead one, and only a power cycle brings it back, so something has to look.
 */
static esp_timer_handle_t g_adv_watchdog;

#define BLE_ADV_WATCHDOG_MS 10000

static ble_adv_state_t g_adv_state = BLE_ADV_SILENT;

static int g_conn_count;
static bool g_session_open;
static volatile uint16_t g_evicted_conn = BLE_HS_CONN_HANDLE_NONE;

/* How many phones are bonded to this adapter. */
int ble_uart_bond_count(void) {
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int num = 0;

    if (ble_store_util_bonded_peers(peers, &num,
                                    sizeof(peers) / sizeof(peers[0])) != 0) {
        return 0;
    }

    return num;
}

/* Counts the bonds that carry the peer's identity key. */
static int count_peer_irks(int obj_type, union ble_store_value *val,
                           void *cookie) {
    (void)obj_type;

    if (val->sec.irk_present) {
        (*(int *)cookie)++;
    }

    return 0; /* keep going; the count is the whole point */
}

static void bond_state_refresh(void) {
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int with_irk = 0;
    int num = 0;

    if (ble_store_util_bonded_peers(peers, &num,
                                    sizeof(peers) / sizeof(peers[0])) != 0) {
        /* The store cannot be read. Claim nothing is bonded rather than
         * filtering on a list we do not have. */
        ble_pair_policy_set_bonds(0, false);
        return;
    }

    ble_store_iterate(BLE_STORE_OBJ_TYPE_PEER_SEC, count_peer_irks, &with_irk);
    ble_pair_policy_set_bonds(num, num > 0 && with_irk >= num);
}

static bool peer_is_bonded(const ble_addr_t *addr) {
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int num = 0;

    if (ble_store_util_bonded_peers(peers, &num,
                                    sizeof(peers) / sizeof(peers[0])) != 0) {
        return false;
    }

    for (int i = 0; i < num; i++) {
        if (ble_addr_cmp(&peers[i], addr) == 0) {
            return true;
        }
    }

    return false;
}

bool ble_uart_pairing_window_open(void) {
    return ble_pair_policy_window_open();
}

ble_adv_state_t ble_uart_adv_state(void) { return g_adv_state; }

void ble_uart_open_pairing_window(void) {
    ble_pair_policy_open_window();

    if (g_pair_timer) {
        esp_timer_stop(g_pair_timer);
        esp_timer_start_once(g_pair_timer,
                             (uint64_t)BLE_PAIRING_WINDOW_MS * 1000);
    }

    ble_job_post(BLE_JOB_ADV_REFRESH);
}

static void pairing_window_expired(void *arg) {
    (void)arg;
    ble_pair_policy_close_window();
    ble_job_post(BLE_JOB_ADV_REFRESH);
}

static void adv_watchdog_tick(void *arg) {
    (void)arg;

    if (g_conn_count == 0 && !ble_gap_adv_active() &&
        ble_pair_policy_adv_state() != BLE_ADV_SILENT) {
        ESP_LOGW(TAG, "advertising had stopped on its own; restarting");
        ble_job_post(BLE_JOB_ADV_REFRESH);
    }
}

void ble_uart_forget_bonds(void) {
    /* Deliberately asynchronous: unpairing touches the controller's resolving
     * list, which it will not allow while an advertiser is running, so the
     * host task has to stop advertising first. */
    ble_job_post(BLE_JOB_FORGET_BONDS | BLE_JOB_ADV_REFRESH);
}

/* The posted work, on the host task. */
static void ble_job_run(struct ble_npl_event *ev) {
    uint32_t jobs;

    (void)ev;

    jobs = g_pending_jobs;
    g_pending_jobs = 0;

    if (jobs & BLE_JOB_FORGET_BONDS) {
        ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
        int num = 0;

        ble_gap_adv_stop();

        if (ble_store_util_bonded_peers(
                peers, &num, sizeof(peers) / sizeof(peers[0])) == 0) {
            for (int i = 0; i < num; i++) {
                int rc = ble_gap_unpair(&peers[i]);
                if (rc != 0) {
                    /* Drop the record anyway. A bond we cannot fully undo is
                     * still a bond the owner asked us to forget; the identity
                     * key left behind in the controller goes at the next
                     * reboot and grants nothing on its own. */
                    ESP_LOGW(TAG, "unpair failed (rc=%d); deleting the record",
                             rc);
                    ble_store_util_delete_peer(&peers[i]);
                }
            }
            ESP_LOGI(TAG, "forgot %d bonded peer%s", num, num == 1 ? "" : "s");
        }
    }

    if (jobs & BLE_JOB_APPLY_NAME) {
        int rc = ble_svc_gap_device_name_set(g_gap_name);
        if (rc != 0) {
            ESP_LOGW(TAG, "could not rename to %s, rc=%d", g_gap_name, rc);
        }
    }

    bond_state_refresh();
    adv_apply();
}

/*
 * Whether a data write or subscription may be served on this link.
 *
 * The characteristics carry no encryption permission of their own, because
 * that is fixed when the table is registered and the mode is not. So the
 * check is made here, on every access: STRICT wants the link encrypted,
 * COMPAT takes whatever the connection was admitted under.
 *
 * A write-without-response that fails here is dropped without a word - there
 * is no response to carry the error - which is why STRICT asks for security
 * from this side as soon as the link is up instead of waiting to be refused.
 */
static bool link_usable(uint16_t handle) {
    struct ble_gap_conn_desc desc;

    if (g_conn_sec_mode == BLE_SEC_COMPAT) {
        return true;
    }

    return ble_gap_conn_find(handle, &desc) == 0 && desc.sec_state.encrypted;
}

// GATT server access callback function
static int gatt_svr_access_cb(uint16_t conn_handle, uint16_t attr_handle,
                              struct ble_gatt_access_ctxt *ctxt, void *arg) {
    switch (ctxt->op) {
    case BLE_GATT_ACCESS_OP_WRITE_CHR: {
        // ESP_LOGI(TAG, "Characteristic write, conn_handle %d, attr_handle %d",
        // conn_handle, attr_handle);

        /*
         * Belt to the characteristic permissions' braces. Those refuse an
         * unencrypted peer; this refuses an encrypted one that bonded itself
         * without being invited, while its disconnect is still in flight.
         */
        if (conn_handle == g_evicted_conn) {
            ESP_LOGW(TAG, "write from an evicted peer; refused");
            return BLE_ATT_ERR_INSUFFICIENT_AUTHEN;
        }

        const ble_slot_t *slot = arg;

        if (!slot) {
            return BLE_ATT_ERR_UNLIKELY;
        }

        /* Data writes still need the link to have earned it. The control
         * characteristic never gets here without encryption: its own
         * permissions refuse that first, in either mode. */
        if (slot->profile != BLE_SLOT_CTRL && !link_usable(conn_handle)) {
            ESP_LOGW(TAG, "write on an unencrypted link; refused");
            return BLE_ATT_ERR_INSUFFICIENT_ENC;
        }

        /*
         * Flatten the chain. ctxt->om is an mbuf *chain*, and om_data/om_len
         * describe only its first link - so reading them directly delivers
         * the front of a write and silently discards the rest of it.
         */
        static uint8_t rx_flat[BLE_RX_FLAT_MAX];
        uint16_t out_len = 0;
        int rc;

        if (OS_MBUF_PKTLEN(ctxt->om) > sizeof(rx_flat)) {
            g_rx_oversize++;
            ESP_LOGE(TAG, "write of %u bytes is longer than %u; refused",
                     (unsigned)OS_MBUF_PKTLEN(ctxt->om),
                     (unsigned)sizeof(rx_flat));
            return BLE_ATT_ERR_INVALID_ATTR_VALUE_LEN;
        }

        rc = ble_hs_mbuf_to_flat(ctxt->om, rx_flat, sizeof(rx_flat), &out_len);
        if (rc != 0) {
            g_rx_flatten_err++;
            ESP_LOGE(TAG, "cannot flatten a %u byte write; rc=%d",
                     (unsigned)OS_MBUF_PKTLEN(ctxt->om), rc);
            return BLE_ATT_ERR_UNLIKELY;
        }

        g_rx_bytes += out_len;
        if (out_len > g_rx_write_max) {
            g_rx_write_max = out_len;
        }
        ble_trace('<', rx_flat, out_len);

        if (slot->profile == BLE_SLOT_CTRL) {
            if (ble_ctrl_callback) {
                ble_ctrl_callback((const char *)rx_flat, out_len);
            }
            return 0;
        }

        /* Replies follow whichever door the client is using. */
        g_active_profile = slot->profile;

        if (ble_rx_callback) {
            ble_rx_callback(rx_flat, out_len);
        }
        return 0;
    }
    default:
        return BLE_ATT_ERR_UNLIKELY;
    }
}

/*
 * GATT services table.
 *
 * Built from ble_profiles.c at start-up rather than written out, so the
 * contract the host tests assert is the one the stack is given. Four data
 * services and the control service, all primary: an app picks the layout it
 * knows and ignores the rest.
 */
static ble_uuid_t *uuid_make(const ble_profile_uuid_t *u) {
    ble_uuid_any_t *any;

    if (g_uuid_count >= (int)(sizeof(g_uuids) / sizeof(g_uuids[0]))) {
        return NULL;
    }

    any = &g_uuids[g_uuid_count];
    if (ble_uuid_init_from_buf(any, u->bytes, u->len) != 0) {
        return NULL;
    }

    g_uuid_count++;
    return &any->u;
}

static ble_gatt_chr_flags chr_flags(const ble_profile_chr_t *chr) {
    ble_gatt_chr_flags flags = 0;

    if (chr->props & BLE_CHR_PROP_WRITE) {
        flags |= BLE_GATT_CHR_F_WRITE;
    }
    if (chr->props & BLE_CHR_PROP_WRITE_NO_RSP) {
        flags |= BLE_GATT_CHR_F_WRITE_NO_RSP;
    }
    if (chr->props & BLE_CHR_PROP_NOTIFY) {
        flags |= BLE_GATT_CHR_F_NOTIFY;
    }

    /*
     * The control characteristic switches a link's grammar, so it is always
     * encrypted, whatever the mode. The data characteristics are not: their
     * check is link_usable(), made per access. Without a READ_ENC on a
     * notify-only characteristic an unencrypted peer can subscribe, but it
     * has nothing to hear - replies only come from writes, and those are
     * refused.
     */
    if (chr->role == BLE_CHR_CTRL) {
        flags |= BLE_GATT_CHR_F_WRITE_ENC | BLE_GATT_CHR_F_READ_ENC;
    }

    return flags;
}

static int gatt_table_add_chr(struct ble_gatt_chr_def *out,
                              const ble_profile_chr_t *chr, uint8_t profile) {
    ble_slot_t *slot;

    if (g_slot_count >= BLE_SLOT_MAX) {
        return -1;
    }

    out->uuid = uuid_make(&chr->uuid);
    if (!out->uuid) {
        return -1;
    }

    slot = &g_slots[g_slot_count++];
    slot->def = chr;
    slot->profile = profile;
    slot->handle = 0;

    out->access_cb = gatt_svr_access_cb;
    out->arg = slot;
    out->flags = chr_flags(chr);
    out->val_handle = &slot->handle;

    if (profile == BLE_SLOT_CTRL) {
        g_ctrl_slot = slot;
    }
    return 0;
}

static int gatt_table_build(void) {
    const ble_ctrl_service_t *ctrl = ble_profile_ctrl();
    int n = 0;

    g_uuid_count = 0;
    g_slot_count = 0;
    memset(g_chr_defs, 0, sizeof(g_chr_defs));
    memset(g_svc_defs, 0, sizeof(g_svc_defs));

    for (int i = 0; i < BLE_PROFILE_COUNT; i++) {
        const ble_profile_t *p = ble_profile_get((ble_profile_id_t)i);

        g_svc_defs[n].type = BLE_GATT_SVC_TYPE_PRIMARY;
        g_svc_defs[n].uuid = uuid_make(&p->service);
        g_svc_defs[n].characteristics = g_chr_defs[i];
        if (!g_svc_defs[n].uuid) {
            return -1;
        }

        for (int c = 0; c < p->num_chrs; c++) {
            if (gatt_table_add_chr(&g_chr_defs[i][c], &p->chrs[c],
                                   (uint8_t)i) != 0) {
                return -1;
            }
        }
        n++;
    }

    g_svc_defs[n].type = BLE_GATT_SVC_TYPE_PRIMARY;
    g_svc_defs[n].uuid = uuid_make(&ctrl->service);
    g_svc_defs[n].characteristics = g_chr_defs[BLE_PROFILE_COUNT];
    if (!g_svc_defs[n].uuid ||
        gatt_table_add_chr(&g_chr_defs[BLE_PROFILE_COUNT][0], &ctrl->chr,
                           BLE_SLOT_CTRL) != 0) {
        return -1;
    }

    return 0;
}

/** The slot holding the notify characteristic of @p profile, or NULL. */
static const ble_slot_t *slot_for_tx(uint8_t profile) {
    for (int i = 0; i < g_slot_count; i++) {
        if (g_slots[i].profile == profile && ble_chr_is_tx(g_slots[i].def)) {
            return &g_slots[i];
        }
    }
    return NULL;
}

static void nimble_host_task(void *param) {
    ESP_LOGI(TAG, "nimble host task has been started!");

    /* This function won't return until nimble_port_stop() is executed */
    nimble_port_run();

    vTaskDelete(NULL);
}

static int gap_init(void) {
    int rc = 0;

    /* Initialize GAP service */
    ble_svc_gap_init();

    /* Set GAP device name */
    rc = ble_svc_gap_device_name_set(g_gap_name);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device name to %s, error code: %d",
                 g_gap_name, rc);
        return rc;
    }

    /* Set GAP device appearance */
    rc = ble_svc_gap_device_appearance_set(BLE_GAP_APPEARANCE_GENERIC_TAG);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set device appearance, error code: %d", rc);
        return rc;
    }
    return rc;
}

int gatt_svc_init(void) {
    int rc;

    ble_svc_gatt_init();

    rc = gatt_table_build();
    if (rc != 0) {
        return rc;
    }

    rc = ble_gatts_count_cfg(g_svc_defs);
    if (rc != 0) {
        return rc;
    }

    rc = ble_gatts_add_svcs(g_svc_defs);
    if (rc != 0) {
        return rc;
    }

    return 0;
}

inline static void format_addr(char *addr_str, uint8_t addr[]) {
    sprintf(addr_str, "%02X:%02X:%02X:%02X:%02X:%02X", addr[0], addr[1],
            addr[2], addr[3], addr[4], addr[5]);
}

static void print_conn_desc(struct ble_gap_conn_desc *desc) {
    char addr_str[18] = {0};

    ESP_LOGI(TAG, "connection handle: %d", desc->conn_handle);

    format_addr(addr_str, desc->our_id_addr.val);
    ESP_LOGI(TAG, "device id address: type=%d, value=%s",
             desc->our_id_addr.type, addr_str);

    format_addr(addr_str, desc->peer_id_addr.val);
    ESP_LOGI(TAG, "peer id address: type=%d, value=%s", desc->peer_id_addr.type,
             addr_str);

    ESP_LOGI(TAG,
             "conn_itvl=%d, conn_latency=%d, supervision_timeout=%d, "
             "encrypted=%d, authenticated=%d, bonded=%d\n",
             desc->conn_itvl, desc->conn_latency, desc->supervision_timeout,
             desc->sec_state.encrypted, desc->sec_state.authenticated,
             desc->sec_state.bonded);
}

static void conn_params_request(uint16_t handle) {
    /* Interval in 1.25 ms units, timeout in 10 ms units. */
    struct ble_gap_upd_params params = {
        .itvl_min = 12, /* 15 ms - floor */
        .itvl_max = 24, /* 30 ms */
        .latency = 0,   /* an ELM327 session is request/response */
        .supervision_timeout =
            200, /* 2 s: well past 3 x 30 ms, under the 6 s cap */
        .min_ce_len = 0,
        .max_ce_len = 0,
    };

    int rc = ble_gap_update_params(handle, &params);
    if (rc != 0) {
        ESP_LOGW(TAG, "connection parameter update refused, rc=%d", rc);
    }
}

/*
 * The link is fit for ELM traffic: tell main, once per connection.
 *
 * Until now a connect was announced the moment it happened, and the data link
 * was bound to a peer that could not yet write a byte. STRICT waits for the
 * link to be encrypted; COMPAT has nothing to wait for. Either way the
 * transport is only reported as connected from here, so USB is not shut out
 * by a phone that never finishes pairing.
 */
static void ble_link_ready(uint16_t handle) {
    if (g_session_open) {
        return;
    }

    conn_handle = handle;
    connected = 1;
    g_session_open = true;
    if (ble_conn_callback) {
        ble_conn_callback(true);
    }
}

/*
 * NimBLE applies an event-driven model to keep GAP service going
 * gap_event_handler is a callback function registered when calling
 * ble_gap_adv_start API and called when a GAP event arrives
 */
static int gap_event_handler(struct ble_gap_event *event, void *arg) {
    int rc = 0;
    struct ble_gap_conn_desc desc;

    switch (event->type) {
    case BLE_GAP_EVENT_CONNECT:
        ESP_LOGI(TAG, "connection %s; status=%d",
                 event->connect.status == 0 ? "established" : "failed",
                 event->connect.status);

        /* Connection succeeded */
        if (event->connect.status == 0) {
            rc = ble_gap_conn_find(event->connect.conn_handle, &desc);
            if (rc != 0) {
                ESP_LOGE(TAG,
                         "failed to find connection by handle, error code: %d",
                         rc);
                return rc;
            }

            print_conn_desc(&desc);
            g_conn_count++;

            /* Nothing to reset for eviction here: this connection has not
             * been judged yet, and on a bonded peer ENC_CHANGE may already
             * have judged it before this event arrived. The evicted handle is
             * cleared when that connection disconnects, not when a new one
             * turns up. */
            ble_pair_policy_latch(event->connect.conn_handle,
                                  ble_uart_bond_count());

            if (!ble_pair_policy_may_bond() &&
                ble_pair_policy_bonds_resolvable() &&
                !peer_is_bonded(&desc.peer_id_addr)) {
                ESP_LOGW(TAG, "uninvited peer; dropping the link before it "
                              "can pair");
                g_evicted_conn = event->connect.conn_handle;
                ble_gap_terminate(event->connect.conn_handle,
                                  BLE_ERR_REM_USER_CONN_TERM);
                return 0;
            }

            conn_handle = event->connect.conn_handle;
            g_conn_sec_mode = g_sec_mode;
            g_active_profile = BLE_PROFILE_NUS;

            if (g_conn_sec_mode == BLE_SEC_COMPAT) {
                /* No pairing is asked for: an app that cannot pair would be
                 * left staring at a prompt it has no way to answer. A phone
                 * that wants to encrypt still may. */
                conn_params_request(event->connect.conn_handle);
                ble_link_ready(event->connect.conn_handle);
            } else if (!desc.sec_state.encrypted) {
                rc = ble_gap_security_initiate(event->connect.conn_handle);
                if (rc != 0 && rc != BLE_HS_EALREADY) {
                    ESP_LOGW(TAG, "could not start pairing, error code: %d",
                             rc);
                }
                rc = 0;
            }
            /* STRICT: the link is not ready until ENC_CHANGE says so. */
        }
        /* The attempt failed; put the advertisement back. */
        else {
            adv_apply();
        }
        return rc;

    /* Disconnect event */
    case BLE_GAP_EVENT_DISCONNECT:
        /* A connection was terminated, print connection descriptor */
        ESP_LOGI(TAG, "disconnected from peer; reason=%d",
                 event->disconnect.reason);

        connected = 0;
        g_active_profile = BLE_PROFILE_NUS;
        if (g_conn_count > 0) {
            g_conn_count--;
        }

        if (event->disconnect.conn.conn_handle == g_evicted_conn) {
            g_evicted_conn = BLE_HS_CONN_HANDLE_NONE;
        }

        if (g_session_open && ble_conn_callback) {
            ble_conn_callback(false);
        }
        g_session_open = false;

        ble_pair_policy_unlatch(event->disconnect.conn.conn_handle);

        bond_state_refresh();
        adv_apply();
        return rc;

    /* Pairing or key restoration finished, either way */
    case BLE_GAP_EVENT_ENC_CHANGE:
        if (event->enc_change.status == 0) {
            rc = ble_gap_conn_find(event->enc_change.conn_handle, &desc);
            if (rc != 0) {
                return rc;
            }

            ble_pair_policy_latch(event->enc_change.conn_handle,
                                  ble_uart_bond_count());

            if (ble_uart_bond_count() > ble_pair_policy_bonds_at_connect() &&
                !ble_pair_policy_may_bond()) {
                ESP_LOGE(TAG, "uninvited peer bonded before it could be "
                              "refused; dropping it, and its phone will need "
                              "to forget this device");
                g_evicted_conn = event->enc_change.conn_handle;
                if (ble_gap_unpair(&desc.peer_id_addr) != 0) {
                    ble_store_util_delete_peer(&desc.peer_id_addr);
                    ble_gap_terminate(event->enc_change.conn_handle,
                                      BLE_ERR_REM_USER_CONN_TERM);
                }
                bond_state_refresh();
                return 0;
            }

            ESP_LOGI(TAG, "link encrypted; bonded=%d authenticated=%d",
                     desc.sec_state.bonded, desc.sec_state.authenticated);

            if (ble_uart_bond_count() > ble_pair_policy_bonds_at_connect() ||
                ble_pair_policy_bond_changed()) {
                ble_pair_policy_close_window();
                bond_state_refresh();
            }

            conn_params_request(event->enc_change.conn_handle);

            /* Only now is it a session worth binding the ELM327 link to. */
            ble_link_ready(event->enc_change.conn_handle);
        } else {
            ESP_LOGW(TAG, "link not encrypted; status=0x%03x",
                     event->enc_change.status);
            if (g_conn_sec_mode == BLE_SEC_COMPAT) {
                /* Compat already opened the session without encryption. A
                 * phone that still holds an old bond often tears the link
                 * itself (MIC / key missing). Forget OpenDIAG on the phone. */
                ESP_LOGW(TAG, "compat: encryption failed; if the phone drops "
                              "the link, forget this device in its Bluetooth "
                              "settings and reconnect");
            }
        }
        return 0;

    /*
     * The peer wants to pair again on a link it is already bonded on.
     *
     * Which happens for an ordinary reason: somebody removed this device in
     * the phone's Bluetooth settings and is adding it back. The stored bond
     * is then a key the peer no longer holds, and keeping it would refuse
     * that phone forever with no way back that does not involve reflashing.
     * Drop ours and let the new pairing proceed.
     */
    case BLE_GAP_EVENT_REPEAT_PAIRING:
        rc = ble_gap_conn_find(event->repeat_pairing.conn_handle, &desc);
        if (rc != 0) {
            return rc;
        }

        ble_pair_policy_latch(event->repeat_pairing.conn_handle,
                              ble_uart_bond_count());

        if (!ble_pair_policy_may_bond()) {
            ESP_LOGW(TAG, "re-pairing refused: press the button first");
            return BLE_GAP_REPEAT_PAIRING_IGNORE;
        }

        ESP_LOGI(TAG, "peer is re-pairing; dropping the stored bond");
        ble_pair_policy_note_bond_changed();
        ble_store_util_delete_peer(&desc.peer_id_addr);
        return BLE_GAP_REPEAT_PAIRING_RETRY;

    /* Connection parameters update event */
    case BLE_GAP_EVENT_CONN_UPDATE:
        ESP_LOGI(TAG, "connection updated; status=%d",
                 event->conn_update.status);

        rc = ble_gap_conn_find(event->conn_update.conn_handle, &desc);
        if (rc != 0) {
            ESP_LOGE(TAG, "failed to find connection by handle, error code: %d",
                     rc);
            return rc;
        }
        print_conn_desc(&desc);
        return rc;

    /* A client turned notifications on or off: that is the door it chose. */
    case BLE_GAP_EVENT_SUBSCRIBE:
        for (int i = 0; i < g_slot_count; i++) {
            if (g_slots[i].handle == event->subscribe.attr_handle &&
                g_slots[i].profile != BLE_SLOT_CTRL &&
                ble_chr_is_tx(g_slots[i].def) && event->subscribe.cur_notify) {
                g_active_profile = g_slots[i].profile;
            }
        }
        return 0;

    case BLE_GAP_EVENT_ADV_COMPLETE:
        ESP_LOGI(TAG, "advertising ended; reason=%d",
                 event->adv_complete.reason);
        adv_apply();
        return 0;
    }

    return rc;
}

static int adv_set_payload(ble_adv_state_t state) {
    struct ble_hs_adv_fields adv_fields = {0};
    struct ble_hs_adv_fields rsp_fields = {0};
    const char *name = ble_svc_gap_device_name();
    bool named_in_adv = (state != BLE_ADV_LOCKED);
    ble_uuid16_t svc16[BLE_PROFILE_COUNT];
    uint16_t raw16[BLE_PROFILE_COUNT];
    ble_uuid_any_t nus;
    size_t n16 = ble_profile_adv_uuid16s(raw16, BLE_PROFILE_COUNT);
    int rc;

    for (size_t i = 0; i < n16; i++) {
        svc16[i] = (ble_uuid16_t)BLE_UUID16_INIT(raw16[i]);
    }

    adv_fields.flags = BLE_HS_ADV_F_DISC_GEN | BLE_HS_ADV_F_BREDR_UNSUP;

    if (named_in_adv) {
        adv_fields.name = (uint8_t *)name;
        adv_fields.name_len = strlen(name);
        adv_fields.name_is_complete = 1;
    }

    adv_fields.tx_pwr_lvl = BLE_HS_ADV_TX_PWR_LVL_AUTO;
    adv_fields.tx_pwr_lvl_is_present = 1;

    adv_fields.appearance = BLE_GAP_APPEARANCE_GENERIC_TAG;
    adv_fields.appearance_is_present = 1;

    adv_fields.le_role = BLE_GAP_LE_ROLE_PERIPHERAL;
    adv_fields.le_role_is_present = 1;

    /*
     * The short UUIDs apps scan for. A 31 byte advertisement is tight: flags,
     * the name, power, appearance and role leave exactly eight bytes, which is
     * these three. If a longer name ever tips it over, the transmit power is
     * what goes - nothing scans for that.
     */
    adv_fields.uuids16 = svc16;
    adv_fields.num_uuids16 = (uint8_t)n16;
    adv_fields.uuids16_is_complete = 1;

    rc = ble_gap_adv_set_fields(&adv_fields);
    if (rc == BLE_HS_EMSGSIZE) {
        adv_fields.tx_pwr_lvl_is_present = 0;
        rc = ble_gap_adv_set_fields(&adv_fields);
    }
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set advertising data, error code: %d", rc);
        return rc;
    }

    if (!named_in_adv) {
        rsp_fields.name = (uint8_t *)name;
        rsp_fields.name_len = strlen(name);
        rsp_fields.name_is_complete = 1;
    }

    /*
     * The Nordic UART service, which does not fit alongside the rest. It was
     * advertised nowhere at first, which left an app that scans by service -
     * as an iOS app must, to scan in the background at all - unable to find
     * this adapter.
     */
    if (ble_profile_get(BLE_PROFILE_NUS)->service.len == 16 &&
        ble_uuid_init_from_buf(&nus,
                               ble_profile_get(BLE_PROFILE_NUS)->service.bytes,
                               16) == 0) {
        rsp_fields.uuids128 = &nus.u128;
        rsp_fields.num_uuids128 = 1;
        rsp_fields.uuids128_is_complete = 1;
    }

    rc = ble_gap_adv_rsp_set_fields(&rsp_fields);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to set scan response data, error code: %d", rc);
    }

    return rc;
}

static int adv_start_with(ble_adv_state_t state) {
    struct ble_gap_adv_params adv_params = {0};

    adv_params.conn_mode = BLE_GAP_CONN_MODE_UND;
    adv_params.disc_mode = BLE_GAP_DISC_MODE_GEN;

    adv_params.itvl_min = BLE_GAP_ADV_ITVL_MS(500);
    adv_params.itvl_max = BLE_GAP_ADV_ITVL_MS(510);

    /* Scan requests as well as connection requests, so the name in the scan
     * response is withheld from everybody but a bonded phone. */
    adv_params.filter_policy = (state == BLE_ADV_LOCKED)
                                   ? BLE_HCI_ADV_FILT_BOTH
                                   : BLE_HCI_ADV_FILT_NONE;

    return ble_gap_adv_start(own_addr_type, NULL, BLE_HS_FOREVER, &adv_params,
                             gap_event_handler, NULL);
}

static void adv_apply(void) {
    ble_adv_state_t state = ble_pair_policy_adv_state();
    ble_addr_t peers[MYNEWT_VAL(BLE_STORE_MAX_BONDS)];
    int num = 0;
    int rc;

    /* One client at a time on one vehicle bus. A second connectable
     * advertisement would be a second session on the same wires. */
    if (g_conn_count != 0) {
        return;
    }

    /* Always stop first. The controller will not let the accept list be
     * touched while an advertiser might be using it - it answers Command
     * Disallowed - and restarting is the only way to change the filter policy
     * in any case. EALREADY here just means it was not running. */
    rc = ble_gap_adv_stop();
    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGW(TAG, "could not stop advertising, rc=%d", rc);
    }

    if (state == BLE_ADV_SILENT) {
        g_adv_state = state;
        ESP_LOGI(TAG, "nothing bonded and the window is shut; off the air "
                      "until the button is pressed");
        ble_pair_policy_apply_led();
        return;
    }

    if (state == BLE_ADV_LOCKED) {
        /* Identity addresses, not the addresses phones connect from: the
         * controller resolves the private address first and matches the list
         * against what it resolved to. */
        if (ble_store_util_bonded_peers(
                peers, &num, sizeof(peers) / sizeof(peers[0])) != 0) {
            state = BLE_ADV_DEGRADED;
        } else if ((rc = ble_gap_wl_set(peers, (uint8_t)num)) != 0) {
            ESP_LOGW(TAG,
                     "accept list refused (rc=%d); advertising to "
                     "everyone instead",
                     rc);
            state = BLE_ADV_DEGRADED;
        }
    }

    if (adv_set_payload(state) != 0) {
        return;
    }

    rc = adv_start_with(state);
    if (rc != 0 && rc != BLE_HS_EALREADY && state == BLE_ADV_LOCKED) {
        ESP_LOGW(TAG, "filtered advertising failed (rc=%d); falling back", rc);
        state = BLE_ADV_DEGRADED;
        if (adv_set_payload(state) == 0) {
            rc = adv_start_with(state);
        }
    }

    if (rc != 0 && rc != BLE_HS_EALREADY) {
        ESP_LOGE(TAG, "not advertising at all; rc=%d", rc);
        return;
    }

    g_adv_state = state;
    ESP_LOGI(TAG, "advertising: %s", ble_adv_state_name(state));
    ble_pair_policy_apply_led();
}

static void adv_init(void) {
    int rc = 0;
    char addr_str[18] = {0};

    rc = ble_hs_util_ensure_addr(0);
    if (rc != 0) {
        ESP_LOGE(TAG, "device does not have any available bt address!");
        return;
    }

    rc = ble_hs_id_infer_auto(0, &own_addr_type);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to infer address type, error code: %d", rc);
        return;
    }

    rc = ble_hs_id_copy_addr(own_addr_type, addr_val, NULL);
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to copy device address, error code: %d", rc);
        return;
    }
    format_addr(addr_str, addr_val);
    ESP_LOGI(TAG, "device address: %s", addr_str);

    /* Bonds restored from NVS are what decides who may connect, so they have
     * to be counted before the first advertisement goes out. */
    bond_state_refresh();
    adv_apply();

    /* Stopped first because the host resyncs after a controller reset, and
     * adv_init() runs again each time. */
    if (g_adv_watchdog) {
        esp_timer_stop(g_adv_watchdog);
        esp_timer_start_periodic(g_adv_watchdog,
                                 (uint64_t)BLE_ADV_WATCHDOG_MS * 1000);
    }
}

static void on_stack_reset(int reason) {
    ESP_LOGI(TAG, "nimble stack reset, reset reason: %d", reason);
}

static void on_stack_sync(void) { adv_init(); }

/*
 * Compat exists for apps that cannot pair. Keep the stack from offering a
 * bond in that mode so a phone is not nudged into encrypting a link that is
 * meant to stay open. Strict still bonds as before.
 *
 * Applied at host init (after NVS load) and whenever the mode changes.
 */
static void ble_sm_apply_mode(ble_sec_mode_t mode) {
    if (mode == BLE_SEC_COMPAT) {
        ble_hs_cfg.sm_bonding = 0;
        ble_hs_cfg.sm_our_key_dist = 0;
        ble_hs_cfg.sm_their_key_dist = 0;
    } else {
        ble_hs_cfg.sm_bonding = 1;
        ble_hs_cfg.sm_our_key_dist =
            BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
        ble_hs_cfg.sm_their_key_dist =
            BLE_SM_PAIR_KEY_DIST_ENC | BLE_SM_PAIR_KEY_DIST_ID;
    }
}

static void nimble_host_config_init(void) {
    /* Set host callbacks */
    ble_hs_cfg.reset_cb = on_stack_reset;
    ble_hs_cfg.sync_cb = on_stack_sync;
    ble_hs_cfg.store_status_cb = ble_store_util_status_rr;

    ble_hs_cfg.sm_io_cap = BLE_HS_IO_NO_INPUT_OUTPUT;
    ble_hs_cfg.sm_mitm = 0;
    ble_hs_cfg.sm_sc = 1;
    ble_sm_apply_mode(g_sec_mode);

    ble_store_config_init();
}

void ble_uart_rx_set_callback(ble_uart_rx_cb_t callback) {
    ble_rx_callback = callback;
}

bool ble_uart_is_connected(void) { return connected != 0; }

#define BLE_NOTIFY_RETRY_MS 400

/* Notifications abandoned after that, and the bytes lost with them. */
static uint32_t g_notify_dropped;
static uint32_t g_notify_retries;

static void ble_notify_chunked(uint16_t handle, const char *buffer,
                               size_t size) {
    uint16_t mtu;
    size_t chunk;

    if (!connected || !handle || !buffer || !size) {
        return;
    }

    mtu = ble_att_mtu(conn_handle);

    /* Before the exchange the stack reports the default 23. Three bytes go to
     * the notification header. */
    chunk = (mtu > 3) ? (size_t)(mtu - 3) : 20;

    while (size) {
        size_t n = size < chunk ? size : chunk;
        int64_t deadline =
            esp_timer_get_time() + (int64_t)BLE_NOTIFY_RETRY_MS * 1000;
        bool sent = false;

        while (!sent) {
            struct os_mbuf *om = ble_hs_mbuf_from_flat(buffer, n);
            int rc;

            if (om) {
                /* The mbuf is consumed either way, so there is nothing to
                 * free on the error path. */
                rc = ble_gatts_notify_custom(conn_handle, handle, om);
                if (rc == 0) {
                    sent = true;
                    break;
                }
            } else {
                rc = BLE_HS_ENOMEM;
            }

            if (!connected) {
                return; /* nobody left to tell */
            }

            if (esp_timer_get_time() >= deadline) {
                g_notify_dropped++;
                ESP_LOGE(TAG,
                         "notification abandoned after %d ms (rc=%d); "
                         "%u bytes lost, the link is now out of step",
                         BLE_NOTIFY_RETRY_MS, rc, (unsigned)size);
                return;
            }

            g_notify_retries++;
            vTaskDelay(pdMS_TO_TICKS(2));
        }

        buffer += n;
        size -= n;
    }
}

void ble_uart_get_notify_stats(uint32_t *dropped, uint32_t *retries) {
    if (dropped) {
        *dropped = g_notify_dropped;
    }
    if (retries) {
        *retries = g_notify_retries;
    }
}

void ble_uart_get_rx_stats(uint32_t *bytes, uint32_t *widest,
                           uint32_t *refused) {
    if (bytes) {
        *bytes = g_rx_bytes;
    }
    if (widest) {
        *widest = g_rx_write_max;
    }
    if (refused) {
        *refused = g_rx_oversize + g_rx_flatten_err;
    }
}

void ble_uart_send(const char *buffer, size_t size) {
    const ble_slot_t *tx = slot_for_tx(g_active_profile);

    ble_trace('>', (const uint8_t *)buffer, size);
    ble_notify_chunked(tx ? tx->handle : 0, buffer, size);
}

void ble_uart_ctrl_reply(const char *text) {
    if (text) {
        ble_notify_chunked(g_ctrl_slot ? g_ctrl_slot->handle : 0, text,
                           strlen(text));
    }
}

void ble_uart_ctrl_set_callback(ble_uart_ctrl_cb_t cb) {
    ble_ctrl_callback = cb;
}

void ble_uart_set_conn_callback(ble_uart_conn_cb_t cb) {
    ble_conn_callback = cb;
}

/* What a reboot should find: the mode and the name last asked for. */
static void ble_config_load(void) {
    nvs_handle_t h;
    uint8_t sec = BLE_SEC_STRICT;
    char name[16];
    size_t len = sizeof(name);

    /* A unit that was never configured has no namespace yet: the defaults
     * stand, and STRICT is the default. */
    if (nvs_open(BLE_CFG_NAMESPACE, NVS_READONLY, &h) != ESP_OK) {
        return;
    }

    if (nvs_get_u8(h, BLE_CFG_KEY_SEC, &sec) == ESP_OK &&
        sec == BLE_SEC_COMPAT) {
        g_sec_mode = BLE_SEC_COMPAT;
    }

    if (nvs_get_str(h, BLE_CFG_KEY_NAME, name, &len) == ESP_OK) {
        const char *known = ble_profile_gap_name_find(name);

        if (known) {
            g_gap_name = known;
        }
    }

    nvs_close(h);
    ESP_LOGI(TAG, "security: %s, name: %s", ble_sec_mode_name(g_sec_mode),
             g_gap_name);
}

static esp_err_t ble_config_save(const char *key, uint8_t sec,
                                 const char *name) {
    nvs_handle_t h;
    esp_err_t err = nvs_open(BLE_CFG_NAMESPACE, NVS_READWRITE, &h);

    if (err != ESP_OK) {
        return err;
    }

    err = name ? nvs_set_str(h, key, name) : nvs_set_u8(h, key, sec);
    if (err == ESP_OK) {
        err = nvs_commit(h);
    }

    nvs_close(h);
    return err;
}

ble_sec_mode_t ble_uart_security_mode(void) { return g_sec_mode; }

esp_err_t ble_uart_set_security_mode(ble_sec_mode_t mode) {
    esp_err_t err;

    if (mode != BLE_SEC_STRICT && mode != BLE_SEC_COMPAT) {
        return ESP_ERR_INVALID_ARG;
    }

    g_sec_mode = mode;
    ble_sm_apply_mode(mode);
    ESP_LOGI(TAG, "security mode: %s", ble_sec_mode_name(mode));

    /* Compat is a different advertisement: one that needs no button. */
    ble_job_post(BLE_JOB_ADV_REFRESH);

    err = ble_config_save(BLE_CFG_KEY_SEC, (uint8_t)mode, NULL);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "security mode not saved: %s", esp_err_to_name(err));
    }
    return err;
}

const char *ble_uart_gap_name(void) { return g_gap_name; }

esp_err_t ble_uart_set_gap_name(const char *name) {
    const char *known = ble_profile_gap_name_find(name);
    esp_err_t err;

    if (!known) {
        return ESP_ERR_INVALID_ARG;
    }

    g_gap_name = known;
    ble_job_post(BLE_JOB_APPLY_NAME | BLE_JOB_ADV_REFRESH);

    err = ble_config_save(BLE_CFG_KEY_NAME, 0, known);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "name not saved: %s", esp_err_to_name(err));
    }
    return err;
}

void ble_uart_setup(void) {
    esp_err_t ret;
    int rc;

    ble_config_load();

    /* NimBLE stack initialization */
    ret = nimble_port_init();
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nimble stack, error code: %d ",
                 ret);
        return;
    }

    /* NimBLE too noisy - reduce to warning */
    esp_log_level_set("NimBLE", ESP_LOG_WARN);

    /* GAP service initialization */
    rc = gap_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GAP service, error code: %d", rc);
        return;
    }

    /* GATT server initialization */
    rc = gatt_svc_init();
    if (rc != 0) {
        ESP_LOGE(TAG, "failed to initialize GATT server, error code: %d", rc);
        return;
    }

    /* The pairing window's own clock. */
    const esp_timer_create_args_t pair_timer_args = {
        .callback = pairing_window_expired,
        .name = "ble_pair_window",
    };
    if (esp_timer_create(&pair_timer_args, &g_pair_timer) != ESP_OK) {
        ESP_LOGW(TAG, "no pairing window timer; the door will stay open");
        g_pair_timer = NULL;
    }

    const esp_timer_create_args_t watchdog_args = {
        .callback = adv_watchdog_tick,
        .name = "ble_adv_watchdog",
    };
    if (esp_timer_create(&watchdog_args, &g_adv_watchdog) != ESP_OK) {
        ESP_LOGW(TAG, "no advertising watchdog");
        g_adv_watchdog = NULL;
    }

    /* Everything that changes what the radio is doing runs through here, on
     * the host task, whichever task asked for it. */
    ble_npl_event_init(&g_job_ev, ble_job_run, NULL);
    g_jobs_ready = true;

    nimble_host_config_init();

    /* Start NimBLE host task thread */
    xTaskCreate(nimble_host_task, "NimBLE Host", 4 * 1024, NULL, 5, NULL);
}

#if ENABLE_BLE_TRACE

/*
 * Print the traced conversation, oldest first.
 *
 * "<" is a write from the client, ">" is a notification to it, and any other
 * tag is a marker somebody left with ble_uart_trace_note(). Control
 * characters are shown escaped, because the whole question on this link is
 * usually where the carriage returns and the prompts fell.
 */
void ble_uart_print_trace(void) {
    uint16_t pos;

    if (g_trace_count == 0) {
        printf("ble-trace empty\n");
        return;
    }

    pos = (uint16_t)((g_trace_head + BLE_TRACE_ENTRIES - g_trace_count) %
                     BLE_TRACE_ENTRIES);

    printf("ble-trace %u records\n", (unsigned)g_trace_count);

    for (uint16_t i = 0; i < g_trace_count; i++) {
        const ble_trace_rec_t *r = &g_trace[pos];

        if (!r->cont) {
            if (i) {
                printf("\n");
            }
            printf("  %c ", r->dir);
        }

        for (uint8_t k = 0; k < r->len; k++) {
            uint8_t b = r->data[k];

            if (b == '\r') {
                printf("<CR>");
            } else if (b == '\n') {
                printf("<LF>");
            } else if (b >= 0x20 && b < 0x7F) {
                printf("%c", b);
            } else {
                printf("<%02X>", b);
            }
        }

        pos = (uint16_t)((pos + 1) % BLE_TRACE_ENTRIES);
    }

    printf("\n");
}

void ble_uart_trace_note(char tag, const char *text, size_t len) {
    ble_trace(tag, (const uint8_t *)text, len);
}

void ble_uart_trace_reset(void) {
    g_trace_head = 0;
    g_trace_count = 0;
}

#else

void ble_uart_print_trace(void) {
    printf("ble-trace not built in; enable ENABLE_BLE_TRACE\n");
}

#endif /* ENABLE_BLE_TRACE */
