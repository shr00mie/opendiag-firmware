/* SPDX-License-Identifier: GPL-3.0-only */
/**
 * @file kline.h
 * @brief ISO 9141-2 and ISO 14230-4 (KWP 2000) K-Line driver.
 *
 * The whole interface is the bus_ops_t below. Everything a caller can ask
 * of this bus - transfers, the fifteen timing parameters ISO 14230-2 and
 * J2534 between them define, both initialisation sequences, the periodic
 * message that holds a session open - goes through the vtable in bus.h,
 * so neither vif.c nor a protocol front-end has to know which bus it is
 * driving. The parameter and IOCTL identifiers are J2534's, so the Pass-Thru
 * layer will pass SET_CONFIG and PassThruIoctl straight through.
 *
 * The message grammar - checksums, message length, what the key bytes meant -
 * lives in kline_codec.h, where it is testable off target. This file is the
 * UART, the timing, and the initialisation sequences.
 *
 * A single task owns the peripheral. It frames received bytes into messages
 * continuously, so the inter-byte gap that ends an ISO 9141-2 message is
 * measured where it happens rather than inferred later, and so a link that
 * would otherwise time out at P3max can be held open while nobody is asking
 * for anything.
 *
 * Clause references are to ISO/DIS 14230-2 unless noted.
 */

#pragma once

#include "bus.h"
#include "kline_codec.h"

/**
 * @brief The K-Line bus, as vif.c and the front-ends see it.
 *
 * Close timeout retains worker resources and rejects I/O; retry close later.
 *
 * Protocol specific values that travel through the generic interface:
 *
 *  - bus_link_t::variant holds a kline_variant_t.
 *  - BUS_IOCTL_ASSUME_LINK takes a pointer to one.
 *  - BUS_P_DATA_RATE accepts 1200 to KLINE_BAUD_HW_MAX (115200). ISO 14230-2
 *    clause 5.1.5.2.2 stops at 10400; the rates above it are there because
 *    AT IB and J2534 both offer them for the manufacturer specific systems
 *    that use them, BMW's among them.
 *  - BUS_P_KLINE_FRAME_MODE may be BUS_KLINE_FRAME_DS2: messages are then
 *    framed by the DS2 length byte and checked and generated with the DS2 XOR
 *    checksum (ds2_codec.h). Parity and baud are separate parameters; DS2 is
 *    normally 9600 baud 8E1. BUS_KLINE_FRAME_RAW delivers each UART byte as
 *    its own message and publishes physical TX echo to the receive queue;
 *    BUS_IOCTL_BREAK_SET asserts or releases a GPIO break on the TX pin.
 *  - BUS_P_K_LINE_BRIDGE_78 requests joining OBD pin 7 to pin 8 (BMW
 *    DS2/ADS). It is a software hook only: the stock board has no pin 8
 *    K-Line driver, so the request is stored and reported, nothing is driven,
 *    and a warning is logged. It needs a hardware rework (see pinout.h).
 */
extern const bus_ops_t kline_bus_ops;
