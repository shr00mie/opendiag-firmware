/* SPDX-License-Identifier: GPL-3.0-only */
#include "usb_cdc.h"
#include <inttypes.h>
#include <stdio.h>
#include "freertos/FreeRTOS.h"
#include "freertos/ringbuf.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_private/periph_ctrl.h"
#include "esp_system.h"
#include "hal/usb_wrap_ll.h"
#include "sdkconfig.h"
#include "soc/rtc_cntl_reg.h"
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"

#define TAG "USB_CDC"

#define TUSB_DESC_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN * 2)
#define USB_CDC_INTERFACE_NUM 2

static volatile bool line_status[USB_CDC_INTERFACE_NUM];
static bool last_dtr[USB_CDC_INTERFACE_NUM];
static bool last_rts[USB_CDC_INTERFACE_NUM];
static usb_cdc_rx_cb_t rx_callback[USB_CDC_INTERFACE_NUM];
static usb_cdc_line_cb_t line_callback;
static usb_cdc_line_coding_cb_t line_coding_callback;
static usb_cdc_break_cb_t break_callback;

/* Data-interface session latch (see usb_cdc_session_latch_enable). */
static bool session_latch_enabled;
static volatile bool session_latched;

enum {
    ITF_NUM_CDC_0 = 0,
    ITF_NUM_CDC_0_DATA,
    ITF_NUM_CDC_1,
    ITF_NUM_CDC_1_DATA,
    ITF_NUM_TOTAL
};

enum {
    EPNUM_CDC_0_NOTIF = 0x81,
    EPNUM_CDC_0_OUT = 0x02,
    EPNUM_CDC_0_IN = 0x82,
    EPNUM_CDC_1_NOTIF = 0x83,
    EPNUM_CDC_1_OUT = 0x04,
    EPNUM_CDC_1_IN = 0x84,
};

static const tusb_desc_device_t cdc_device_descriptor = {
    .bLength = sizeof(cdc_device_descriptor),
    .bDescriptorType = TUSB_DESC_DEVICE,
    .bcdUSB = 0x0200,
    .bDeviceClass = TUSB_CLASS_MISC,
    .bDeviceSubClass = MISC_SUBCLASS_COMMON,
    .bDeviceProtocol = MISC_PROTOCOL_IAD,
    .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor = 0x1209,
    .idProduct = 0xd1a6,
    .bcdDevice = 0x0100,
    .iManufacturer = 0x01,
    .iProduct = 0x02,
    .iSerialNumber = 0x03,
    .bNumConfigurations = 0x01};

/* TUD_CDC_DESCRIPTOR, plus iFunction so hosts name the ports "COM"/"DEBUG".
 * Protocol stays NONE: the data link's grammar is host-selected at runtime. */
#define OPENDIAG_CDC_DESCRIPTOR(_itfnum, _stridx, _ep_notif, _ep_notif_size,   \
                                _epout, _epin, _epsize)                        \
    /* Interface Association */                                                \
    8, TUSB_DESC_INTERFACE_ASSOCIATION, _itfnum, 2, TUSB_CLASS_CDC,            \
        CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE,      \
        _stridx,                                                               \
    /* CDC Control Interface */                                                \
    9, TUSB_DESC_INTERFACE, _itfnum, 0, 1, TUSB_CLASS_CDC,                     \
        CDC_COMM_SUBCLASS_ABSTRACT_CONTROL_MODEL, CDC_COMM_PROTOCOL_NONE,      \
        _stridx,                                                               \
    /* CDC Header */                                                           \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_HEADER, U16_TO_U8S_LE(0x0120),    \
    /* CDC Call Management */                                                  \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_CALL_MANAGEMENT, 0,               \
        (uint8_t)((_itfnum) + 1),                                              \
    /* CDC ACM: line request + send break */                                   \
    4, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_ABSTRACT_CONTROL_MANAGEMENT, 6,   \
    /* CDC Union */                                                            \
    5, TUSB_DESC_CS_INTERFACE, CDC_FUNC_DESC_UNION, _itfnum,                   \
        (uint8_t)((_itfnum) + 1),                                              \
    /* Endpoint Notification */                                                \
    7, TUSB_DESC_ENDPOINT, _ep_notif, TUSB_XFER_INTERRUPT,                     \
        U16_TO_U8S_LE(_ep_notif_size), 1,                                      \
    /* CDC Data Interface */                                                   \
    9, TUSB_DESC_INTERFACE, (uint8_t)((_itfnum) + 1), 0, 2,                    \
        TUSB_CLASS_CDC_DATA, 0, 0, 0,                                          \
    /* Endpoint Out */                                                         \
    7, TUSB_DESC_ENDPOINT, _epout, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0,  \
    /* Endpoint In */                                                          \
    7, TUSB_DESC_ENDPOINT, _epin, TUSB_XFER_BULK, U16_TO_U8S_LE(_epsize), 0

static const uint8_t desc_configuration[] = {
    /* Declare the full USB-only draw, including the active radio. */
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, TUSB_DESC_TOTAL_LEN, 0, 500),

    // 1st CDC, the protocol data link: interface number, string index,
    // EP notification, EP notification size, EP data out, EP data in,
    // EP data packet size
    OPENDIAG_CDC_DESCRIPTOR(ITF_NUM_CDC_0, 4, EPNUM_CDC_0_NOTIF, 8,
                            EPNUM_CDC_0_OUT, EPNUM_CDC_0_IN, 64),

    // 2nd CDC, the console and debug shell
    OPENDIAG_CDC_DESCRIPTOR(ITF_NUM_CDC_1, 5, EPNUM_CDC_1_NOTIF, 8,
                            EPNUM_CDC_1_OUT, EPNUM_CDC_1_IN, 64),
};

static char serial_str[13] = "000000000000";

static const char *string_desc_arr[] = {
    (static const char[]){0x09,
                          0x04}, // 0: is supported language is English (0x0409)
    OPENDIAG_MANUFACTURE,        // 1: Manufacturer
    OPENDIAG_PRODUCT,            // 2: Product
    serial_str,                  // 3: Serials,
    "COM",                       // 4: CDC 0 - the protocol data link
    "DEBUG",                     // 5: CDC 1 - console and debug shell
};

static const tinyusb_config_t tusb_cfg = {
    .device_descriptor = &cdc_device_descriptor,
    .string_descriptor = string_desc_arr,
    .string_descriptor_count =
        sizeof(string_desc_arr) / sizeof(string_desc_arr[0]),
    .external_phy = false, // In the most cases you need to use a `false` value
    .configuration_descriptor = desc_configuration,
};

/**
 * @brief Raise the data-interface session latch; notify on the rising edge.
 */
static void session_latch_set(void) {
    bool was = session_latched;
    session_latched = true;
    if (!was && line_callback) {
        line_callback(USB_CDC_INTERFACE_DATA, true);
    }
}

/**
 * @brief Invoked when a line state change event occurs
 *
 * This callback is used to detect when a serial terminal is connected or
 * disconnected. With session latch enabled on the data interface, any
 * line-state change latches the session; DTR+RTS both low does not clear it.
 */
static void tinyusb_cdc_line_state_cb(int itf, cdcacm_event_t *event) {
    bool dtr = event->line_state_changed_data.dtr;
    bool rts = event->line_state_changed_data.rts;
    bool open = dtr && rts;

    if (itf < USB_CDC_INTERFACE_NUM) {
        last_dtr[itf] = dtr;
        last_rts[itf] = rts;
        line_status[itf] = open;
    }

    if (itf == USB_CDC_INTERFACE_DATA && session_latch_enabled) {
        ESP_LOGI(TAG,
                 "CDC%d line state dtr=%d rts=%d (session latch)", itf,
                 (int)dtr, (int)rts);
        session_latch_set();
        return;
    }

    if (open) {
        ESP_LOGI(TAG, "Serial terminal connected on CDC interface %d", itf);
    } else {
        ESP_LOGI(TAG, "Serial terminal disconnected on CDC interface %d", itf);
    }

    if (line_callback) {
        line_callback(itf, open);
    }
}

/**
 * @brief Invoked when the host sets line coding (baud / framing).
 *
 * With session latch enabled on the data interface, a coding change latches
 * the session the same way a line-state change does.
 */
static void tinyusb_cdc_line_coding_cb(int itf, cdcacm_event_t *event) {
    cdc_line_coding_t const *lc =
        event->line_coding_changed_data.p_line_coding;

    if (itf == USB_CDC_INTERFACE_DATA && session_latch_enabled) {
        session_latch_set();
    }

    if (line_coding_callback && lc) {
        line_coding_callback(itf, lc->bit_rate, lc->stop_bits, lc->parity,
                             lc->data_bits);
    }
}

/**
 * @brief Invoked when received new data
 *
 * This callback is triggered when the host sends data to either of the CDC
 * interfaces. The `itf` parameter is used to distinguish between the two
 * interfaces.
 */
static void tinyusb_cdc_rx_cb(int itf, cdcacm_event_t *event) {
    /* initialization */
    size_t rx_size = 0;
    uint8_t rx_buf[64];

    /* read */
    esp_err_t ret = tinyusb_cdcacm_read(itf, rx_buf, sizeof(rx_buf), &rx_size);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "CDC RX read error. ret = %d\n", ret);
        return;
    }

    if (itf == USB_CDC_INTERFACE_DATA && session_latch_enabled && rx_size) {
        session_latch_set();
    }

    if (rx_callback[itf]) {
        rx_callback[itf](rx_buf, rx_size);
    }
}

/**
 * @brief TinyUSB SEND_BREAK (weak symbol). Forwards to the registered callback.
 *
 * duration_ms: 0xFFFF assert until cleared, 0 deassert, else break length.
 */
void tud_cdc_send_break_cb(uint8_t itf, uint16_t duration_ms) {
    if (break_callback) {
        break_callback((int)itf, duration_ms);
    }
}

/**
 * @brief Host unmounted the device: drop line status and clear the latch.
 */
void tud_umount_cb(void) {
    for (int i = 0; i < USB_CDC_INTERFACE_NUM; i++) {
        bool was_open = line_status[i];
        line_status[i] = false;
        last_dtr[i] = false;
        last_rts[i] = false;
        if (i == USB_CDC_INTERFACE_DATA) {
            bool was_latched = session_latched;
            session_latched = false;
            if (line_callback && (was_latched || was_open)) {
                line_callback(i, false);
            }
        } else if (was_open && line_callback) {
            line_callback(i, false);
        }
    }
}

void usb_cdc_set_line_callback(usb_cdc_line_cb_t callback) {
    line_callback = callback;
}

void usb_cdc_set_line_coding_callback(usb_cdc_line_coding_cb_t callback) {
    line_coding_callback = callback;
}

void usb_cdc_set_break_callback(usb_cdc_break_cb_t callback) {
    break_callback = callback;
}

void usb_cdc_rx_set_callback(int itf, usb_cdc_rx_cb_t callback) {
    assert(itf < USB_CDC_INTERFACE_NUM);
    rx_callback[itf] = callback;
}

int usb_cdc_tx_write(int itf, const void *buf, uint32_t length) {
    assert(itf < USB_CDC_INTERFACE_NUM);
    return tinyusb_cdcacm_write_queue(itf, buf, length);
}

void usb_cdc_tx_flush(int itf) {
    assert(itf < USB_CDC_INTERFACE_NUM);
    tinyusb_cdcacm_write_flush(itf, 0);
}

bool usb_cdc_get_line_status(int itf) {
    assert(itf < USB_CDC_INTERFACE_NUM);
    if (itf == USB_CDC_INTERFACE_DATA && session_latch_enabled) {
        return session_latched;
    }
    return line_status[itf];
}

bool usb_cdc_get_line_coding(int itf, uint32_t *bit_rate, uint8_t *stop_bits,
                             uint8_t *parity, uint8_t *data_bits) {
    cdc_line_coding_t coding;

    assert(itf < USB_CDC_INTERFACE_NUM);
    if (!bit_rate || !stop_bits || !parity || !data_bits) {
        return false;
    }
    tud_cdc_n_get_line_coding((uint8_t)itf, &coding);
    *bit_rate = coding.bit_rate;
    *stop_bits = coding.stop_bits;
    *parity = coding.parity;
    *data_bits = coding.data_bits;
    return true;
}

void usb_cdc_session_latch_enable(bool enable) {
    if (enable == session_latch_enabled) {
        return;
    }

    if (enable) {
        session_latch_enabled = true;
        /* Already-open port stays connected under latch semantics. */
        if (line_status[USB_CDC_INTERFACE_DATA]) {
            session_latched = true;
        }
    } else {
        /*
         * Quiet: do not fire a disconnect edge. Frontend stop() runs this on
         * every client-gone restart; a synthetic disconnect would call
         * vif_link_down() again and loop (kdcan stop ↔ USB release).
         * Status reverts to DTR&&RTS via get_line_status; real unmount still
         * notifies through tud_umount_cb.
         */
        session_latch_enabled = false;
        session_latched = false;
    }
}

void usb_cdc_session_latch_clear(void) {
    /* Quiet clear — same reason as enable(false). Explicit link drop already
     * detaches the port before calling this. */
    session_latched = false;
}

void usb_cdc_session_latch_touch(void) {
    if (!session_latch_enabled) {
        return;
    }
    session_latch_set();
}

void usb_cdc_setup(void) {
    ESP_LOGI(TAG, "USB initialization");

    // Populate serial string with MAC address
    uint8_t base_mac[6] = {0};
    if (esp_efuse_mac_get_default(base_mac) == ESP_OK) {
        snprintf(serial_str, sizeof(serial_str), "%02X%02X%02X%02X%02X%02X",
                 base_mac[0], base_mac[1], base_mac[2], base_mac[3],
                 base_mac[4], base_mac[5]);
        ESP_LOGI(TAG, "USB serial (eFuse MAC): %s", serial_str);
    } else {
        ESP_LOGW(TAG, "Failed to get MAC address for USB serial number");
    }

    ESP_ERROR_CHECK(tinyusb_driver_install(&tusb_cfg));

    ESP_ERROR_CHECK(esp_register_shutdown_handler(usb_cdc_teardown));

    /* CDC 0 is the data link: its receive callback drains the FIFO and hands
     * the bytes to comm_iface. */
    tinyusb_config_cdcacm_t acm_cfg = {.usb_dev = TINYUSB_USBDEV_0,
                                       .cdc_port = TINYUSB_CDC_ACM_0,
                                       .callback_rx = tinyusb_cdc_rx_cb,
                                       .callback_rx_wanted_char = NULL,
                                       .callback_line_state_changed =
                                           tinyusb_cdc_line_state_cb,
                                       .callback_line_coding_changed =
                                           tinyusb_cdc_line_coding_cb};
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));

    /*
     * CDC 1 is the console, and gets NO receive callback.
     *
     * esp_tusb_init_console() puts a VFS reader on this interface, and that
     * reader takes bytes straight out of the TinyUSB FIFO with
     * tud_cdc_n_read_char(). A callback here would run first, drain the same
     * FIFO through tinyusb_cdcacm_read(), and - having nothing to hand the
     * bytes to - discard them. The console would print but never read a key.
     */
    acm_cfg.cdc_port = TINYUSB_CDC_ACM_1;
    acm_cfg.callback_rx = NULL;
    ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm_cfg));

    ESP_LOGI(TAG, "USB initialization finished.");
}

void usb_cdc_teardown(void) {
    ESP_LOGI(TAG, "USB teardown...");

    session_latched = false;
    session_latch_enabled = false;
    for (int i = 0; i < USB_CDC_INTERFACE_NUM; i++) {
        line_status[i] = false;
        last_dtr[i] = false;
        last_rts[i] = false;
    }

    tinyusb_driver_uninstall();

    /*
     * Workaround for `reboot dl` command. The download shutdown path resets USB
     * after TinyUSB teardown: TinyUSB can gate the DWC2 clocks, and
     * `esp_restart()` does not reset the USB peripheral. Leaving that state for
     * ROM can cause USB enumeration failures such as Linux `device descriptor
     * read/64, error -71` even when BOOT + RESET works.
     */
    if (REG_GET_BIT(RTC_CNTL_OPTION1_REG, RTC_CNTL_FORCE_DOWNLOAD_BOOT)) {
        /* TinyUSB gates USB clocks; CPU reset leaves them gated for ROM. */
        PERIPH_RCC_ATOMIC() {
            usb_wrap_ll_enable_bus_clock(true);
            usb_wrap_ll_reset_register();
        }
    }
}
