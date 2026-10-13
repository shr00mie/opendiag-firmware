/* SPDX-License-Identifier: GPL-3.0-only */
#pragma once

#include "vif.h"

/**
 * Deep OBD / CanAdapterElm binary front-end (E46 DS2 v1).
 *
 * Switch with `mode ediabas` (or `AT VIF EDIABAS`). Hosts speak the custom
 * adapter telegrams at 115200 on the data link; see docs/ediabas.md.
 */
extern const vif_frontend_t ediabas_frontend;

/** @brief Make the ediabas grammar available to sessions by name. */
void ediabas_register(void);
