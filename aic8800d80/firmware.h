/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_MACOS_FIRMWARE_H
#define AIC_MACOS_FIRMWARE_H
#include <stddef.h>
#include <stdint.h>

typedef int (*aic_debug_exchange)(void *context, uint16_t request,
    const uint8_t *parameters, size_t length, uint8_t *confirmation,
    size_t capacity, size_t *actual);

/* NULL exchange validates the pinned main-branch firmware without USB access.
 * Otherwise, only the identified D80 chip_id=7 / MCU0 profile may be booted.
 */
int aic_firmware_boot(const char *directory, aic_debug_exchange exchange, void *context);
#endif
