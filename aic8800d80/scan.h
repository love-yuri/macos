/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_MACOS_SCAN_H
#define AIC_MACOS_SCAN_H
#include "firmware.h"
typedef int (*aic_receive)(void *context, uint8_t *buffer, size_t capacity, size_t *actual);
int aic_passive_scan(void *context, aic_debug_exchange exchange, aic_receive receive);
#endif
