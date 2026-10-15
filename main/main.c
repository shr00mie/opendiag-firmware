/* SPDX-License-Identifier: GPL-3.0-only */
#include <inttypes.h>
#include <stdio.h>
#include <strings.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_chip_info.h"
#include "esp_flash.h"
#include "esp_log.h"
#include "esp_system.h"
#include "esp_task_wdt.h"
#include "ble_uart.h"
#include "button.h"
#include "comm_iface.h"
#include "common.h"
#include "ediabas.h"
#include "elm327_at.h"
#include "kdcan.h"
#include "j2534.h"
#include "link.h"
#include "nvs_flash.h"
#include "pinout.h"
#include "sdkconfig.h"
#include "shell.h"
#include "slcan.h"
#include "tusb_console.h"
#include "usb_cdc.h"
#include "utility.h"
#include "vif.h"
#include "vif_ctrl.h"
#include "ws2812_led.h"

#define TAG "app_main"

/*
 * Port IDs. Only the data links are comm_iface ports: USB CDC 1 carries the
 * console and the debug shell, which are driven through stdio rather than the
 * multiplexer.
 */
comm_port_id_t g_port_usb_cdc0 = COMM_INVALID_PORT_ID;
comm_port_id_t g_port_ble = COMM_INVALID_PORT_ID;

/** Room for a full command line and then some; overflow is dropped, not
 * blocked. */
#define LINK_RX_BUF 256

/* USB CDC 0: the USB data link */
static int port_usb_cdc0_write(const void *buf, uint32_t length) {
    return usb_cdc_tx_write(USB_CDC_INTERFACE_DATA, buf, length);
}

static void port_usb_cdc0_flush(void) {
    usb_cdc_tx_flush(USB_CDC_INTERFACE_DATA);
}

static bool port_usb_cdc0_is_connected(void) {
    return usb_cdc_get_line_status(USB_CDC_INTERFACE_DATA);
}

static void usb_cdc0_rx_callback(const uint8_t *rx_buf, size_t rx_size) {
    comm_port_rx(g_port_usb_cdc0, rx_buf, rx_size);
}

/* BLE UART Wrapper */
static int port_ble_write(const void *buf, uint32_t length) {
    ble_uart_send(buf, length);
    return length; // Assume success
}

static void port_ble_flush(void) {
    // BLE doesn't have explicit flush
}

static bool port_ble_is_connected(void) { return ble_uart_is_connected(); }

static void ble_rx_callback(const uint8_t *rx_buf, size_t rx_size) {
    comm_port_rx(g_port_ble, rx_buf, rx_size);
}

static void comm_ports_setup(void) {
    static const comm_port_ops_t cdc0_ops = {
        .write = port_usb_cdc0_write,
        .flush = port_usb_cdc0_flush,
        .is_connected = port_usb_cdc0_is_connected,
    };
    static const comm_port_ops_t ble_ops = {
        .write = port_ble_write,
        .flush = port_ble_flush,
        .is_connected = port_ble_is_connected,
    };

    g_port_usb_cdc0 = comm_port_register("CDC0", &cdc0_ops, LINK_RX_BUF);
    g_port_ble = comm_port_register("BLE", &ble_ops, LINK_RX_BUF);

    if (g_port_usb_cdc0 == COMM_INVALID_PORT_ID ||
        g_port_ble == COMM_INVALID_PORT_ID) {
        ESP_LOGE(TAG, "Failed to register one or more comm ports");
    }

    /*
     * Connect port drivers to our RX callbacks. USB CDC 1 deliberately gets
     * none: esp_tusb_init_console() puts a VFS reader on that interface, and
     * two consumers of the same CDC would race for every byte.
     */
    usb_cdc_rx_set_callback(USB_CDC_INTERFACE_DATA, usb_cdc0_rx_callback);
    ble_uart_rx_set_callback(ble_rx_callback);
}

/*
 * The one data link follows USB data CDC while a host holds if00 open;
 * otherwise a ready BLE client gets it. Never both. That keeps grammar,
 * settings and claims in one place. Debug CDC never owns the link.
 *
 * The link starts on ELM327. The selected mode is sticky across disconnects;
 * only session state (parser, claims) is rebuilt when a client leaves.
 */

static void link_led_refresh(void) {
    ws2812_led_set_link_solid(vif_link_port() == g_port_usb_cdc0);
}

/**
 * @brief Bind the link to USB-when-active, else BLE-when-ready, else none.
 *
 * Preempts the other transport when policy says so (USB client attaching
 * while BLE held; USB leaving while BLE is ready).
 */
static void link_apply_policy(void) {
    comm_port_id_t want = COMM_INVALID_PORT_ID;
    comm_port_id_t held = vif_link_port();

    if (port_usb_cdc0_is_connected()) {
        want = g_port_usb_cdc0;
    } else if (ble_uart_is_connected()) {
        want = g_port_ble;
    }

    if (held == want) {
        link_led_refresh();
        return;
    }

    if (held != COMM_INVALID_PORT_ID) {
        vif_link_down();
        vif_link_set_port(COMM_INVALID_PORT_ID);
    }

    if (want != COMM_INVALID_PORT_ID) {
        vif_link_set_port(want);
        ESP_LOGI(TAG, "link: %s",
                 want == g_port_ble ? "ble" : "usb");
    }
    link_led_refresh();
}

/* ------------------------------------------------------------------ *
 * link.h: the manual override
 * ------------------------------------------------------------------ */

static comm_port_id_t transport_port(link_transport_t t) {
    switch (t) {
    case LINK_USB:
        return g_port_usb_cdc0;
    case LINK_BLE:
        return g_port_ble;
    default:
        return COMM_INVALID_PORT_ID;
    }
}

link_transport_t link_holder(void) {
    comm_port_id_t held = vif_link_port();

    if (held == COMM_INVALID_PORT_ID) {
        return LINK_NONE;
    }

    return (held == g_port_ble) ? LINK_BLE : LINK_USB;
}

const char *link_transport_name(link_transport_t t) {
    switch (t) {
    case LINK_USB:
        return "usb";
    case LINK_BLE:
        return "ble";
    default:
        return "none";
    }
}

bool link_transport_connected(link_transport_t t) {
    switch (t) {
    case LINK_USB:
        return port_usb_cdc0_is_connected();
    case LINK_BLE:
        return ble_uart_is_connected();
    default:
        return false;
    }
}

void link_set(link_transport_t t) {
    comm_port_id_t want = transport_port(t);
    comm_port_id_t held = vif_link_port();

    if (held == want) {
        return;
    }

    if (held != COMM_INVALID_PORT_ID) {
        vif_link_down();
        vif_link_set_port(COMM_INVALID_PORT_ID);
    }

    if (want != COMM_INVALID_PORT_ID) {
        /* Named outright — override automatic USB/BLE policy. */
        vif_link_set_port(want);
        ESP_LOGI(TAG, "link: %s by request", link_transport_name(t));
        link_led_refresh();
        return;
    }

    /* Explicit drop: clear a kdcan-style CDC session latch so DTR-idle
     * cannot keep the data port "connected" after the user let go. */
    usb_cdc_session_latch_clear();
    ESP_LOGI(TAG, "link: let go by request");
    link_apply_policy();
}

/**
 * @brief A host opened or closed the USB data link.
 *
 * Connect never reverts the sticky grammar: the shell may set the mode
 * before the tool that needs it opens the port (slcand cannot ask for
 * SLCAN itself).
 */
static void usb_line_state_changed(int itf, bool connected) {
    (void)connected;
    if (itf != USB_CDC_INTERFACE_DATA) {
        return;
    }
    link_apply_policy();
}

/**
 * @brief The front panel button was held down.
 *
 * Opens the BLE pairing window. A device with no screen and no keypad cannot
 * prove who is asking to connect to it any other way: the one thing a
 * stranger driving past cannot do is stand at the car and press a button, so
 * that press is what a new phone needs before it is allowed to bond.
 */
/** Short tap cycles the sticky link mode in registration order. */
static void button_tapped(void) {
    const char *now = vif_link_frontend_name();
    const vif_frontend_t *next = NULL;
    size_t i;

    for (i = 0; (next = vif_frontend_at(i)) != NULL; i++) {
        if (now && strcasecmp(next->name, now) == 0) {
            next = vif_frontend_at(i + 1);
            break;
        }
    }

    if (!next) {
        next = vif_frontend_at(0);
    }
    if (!next) {
        return;
    }

    ESP_LOGI(TAG, "button: mode %s", next->name);
    vif_link_select_frontend(next);
}

static void button_held(void) { ble_uart_open_pairing_window(); }

/**
 * @brief The front panel button was held down for a long time.
 *
 * Forgets every phone. The way back when a phone and this adapter disagree
 * about what key they share - which the phone alone can cause, by being told
 * to forget the adapter - and the way to hand a unit on to somebody else.
 *
 * The pairing window opened on the way past the shorter threshold, and is
 * left open on purpose: with nothing bonded, pairing again is the only thing
 * anybody would want to do next.
 */
static void button_held_long(void) { ble_uart_forget_bonds(); }

/**
 * "Ready" is the transport's call: encrypted in STRICT, any link in COMPAT.
 * A phone that is merely connected is not reported. USB data, when active,
 * still outranks BLE.
 */
static void ble_conn_changed(bool connected) {
    (void)connected;
    link_apply_policy();
}

/**
 * @brief One control command written to the BLE control characteristic.
 *
 * There is one link and this acts on it. A phone that has the link gets the
 * answers it expects; one that was turned away can still ask what is running
 * and who holds what, which is the only way it would find out.
 */
static void ble_ctrl_command(const char *cmd, size_t len) {
    char out[VIF_CTRL_REPLY_MAX];

    vif_ctrl_exec(cmd, len, out, sizeof(out));
    ble_uart_ctrl_reply(out);
}

static void links_setup(void) {
    elm327_register();
    slcan_register();
    j2534_register();
    ediabas_register();
    kdcan_register();

    if (vif_link_start(&elm327_frontend) != ESP_OK) {
        ESP_LOGE(TAG, "failed to start the data link");
        return;
    }

    ws2812_led_set_mode_color(ws2812_led_color_for_mode("elm327"));

    /*
     * USB data CDC outranks BLE while a host holds if00; otherwise BLE when
     * ready. Both edges are reported by the transports, so nothing polls.
     */
    usb_cdc_set_line_callback(usb_line_state_changed);
    ble_uart_set_conn_callback(ble_conn_changed);

    /* A host that opened the port before the callback existed reports no
     * edge, so policy runs once here rather than waiting for an edge. */
    link_apply_policy();

    /* Tap cycles modes; hold opens BLE pairing; long-hold forgets bonds. */
    button_set_click_callback(button_tapped);
    button_set_hold_callback(button_held);
    button_set_long_hold_callback(button_held_long);

    /* The control plane for a BLE client. USB reaches it through the shell. */
    ble_uart_ctrl_set_callback(ble_ctrl_command);
}

/**
 * @brief Application main entry point
 */
void app_main(void) {
    esp_err_t err;
    /*
     * NVS flash initialization
     * Dependency of BLE stack to store configurations and board voltage senses
     * calibration data
     */
    err = nvs_flash_init();
    if (err == ESP_ERR_NVS_NO_FREE_PAGES ||
        err == ESP_ERR_NVS_NEW_VERSION_FOUND) {
        ESP_ERROR_CHECK(nvs_flash_erase());
        err = nvs_flash_init();
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "failed to initialize nvs flash, error code: %d ", err);
        abort();
    }

    /*
     * Vehicle interface first: it owns the board - gpio, adc, pwm - and every
     * bus, and a session cannot be opened before it is up.
     */
    err = vif_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize the vehicle interface: %s",
                 esp_err_to_name(err));
        abort();
    }

    /* Print chip information */
    esp_chip_info_t chip_info;
    uint32_t flash_size;
    esp_chip_info(&chip_info);
    printf("This is %s chip with %d CPU core(s), %s%s%s%s, ", CONFIG_IDF_TARGET,
           chip_info.cores,
           (chip_info.features & CHIP_FEATURE_WIFI_BGN) ? "WiFi/" : "",
           (chip_info.features & CHIP_FEATURE_BT) ? "BT" : "",
           (chip_info.features & CHIP_FEATURE_BLE) ? "BLE" : "",
           (chip_info.features & CHIP_FEATURE_IEEE802154)
               ? ", 802.15.4 (Zigbee/Thread)"
               : "");

    unsigned major_rev = chip_info.revision / 100;
    unsigned minor_rev = chip_info.revision % 100;
    printf("silicon revision v%d.%d, ", major_rev, minor_rev);
    if (esp_flash_get_size(NULL, &flash_size) != ESP_OK) {
        printf("Get flash size failed");
        abort();
    }

    printf("%" PRIu32 "MB %s flash\n", flash_size / (uint32_t)(1024 * 1024),
           (chip_info.features & CHIP_FEATURE_EMB_FLASH) ? "embedded"
                                                         : "external");

    printf("Minimum free heap size: %" PRIu32 " bytes\n",
           esp_get_minimum_free_heap_size());

    /* The front panel button. Its handler is installed with the transports'. */
    button_init();

    /* Setup WS2812 status led */
    ws2812_led_init();
    ws2812_led_set_mode_color(LED_COLOR_CYAN);
    ws2812_led_set_link_solid(false);

    /*
     * Bring the multiplexer and its ports up before the transports, so that a
     * BLE central cannot connect and write before the RX callbacks exist.
     */
    err = comm_iface_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Failed to initialize comm_iface: %s",
                 esp_err_to_name(err));
    }
    comm_ports_setup();

    /* setup devices */
    usb_cdc_setup();
    ble_uart_setup();

    /*
     * Hand the console to USB CDC 1. Everything up to here has gone to UART0,
     * which is what it is for now: early debug, before USB exists. From this
     * point the shell and every ESP_LOG line come out on the second CDC
     * interface, which is where a user can actually reach them.
     */
    err = esp_tusb_init_console(USB_CDC_INTERFACE_SHELL);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "Console stays on UART0: %s", esp_err_to_name(err));
    }

    /* The data link, on whichever of USB CDC 0 and BLE has a client. */
    links_setup();

    /* start shell */
    shell_loop();

    /* shouldn't be here */
    printf("Shell loop returned. Restarting now.\n");
    fflush(stdout);
    esp_restart();
}
