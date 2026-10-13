/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "vif.h"

/**
 * Transparent K+DCAN UART front-end for BMWeb and similar hosts.
 *
 * Switch with `mode kdcan` (or `AT VIF KDCAN`). USB data CDC bytes map to
 * K-Line RAW; line coding and send-break drive baud/parity and break.
 * See docs/kdcan.md.
 */
extern const vif_frontend_t kdcan_frontend;

/** @brief Make the kdcan grammar available to sessions by name. */
void kdcan_register(void);
