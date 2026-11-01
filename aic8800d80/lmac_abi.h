/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_MACOS_LMAC_ABI_H
#define AIC_MACOS_LMAC_ABI_H
#include <stdbool.h>
#include <stdint.h>

/* Reuse the real firmware ABI and enumeration values, not a parallel copy.
 * Only lmac_types.h's Linux include dependencies are replaced here.
 * CONFIG_RWNX_TL4 must remain off, as in the D80 USB Linux build.
 */
#define _LMAC_INT_H_
typedef uint8_t u8_l;
typedef int8_t s8_l;
typedef bool bool_l;
typedef uint16_t u16_l;
typedef int16_t s16_l;
typedef uint32_t u32_l;
typedef int32_t s32_l;
typedef uint64_t u64_l;
typedef uint8_t u8;
typedef uint16_t u16;
typedef uint32_t u32;
/* Select the modern declarations in lmac_mac.h, matching the Linux build. */
#define KERNEL_VERSION(a, b, c) (((a) << 16) | ((b) << 8) | (c))
#define LINUX_VERSION_CODE KERNEL_VERSION(6, 16, 0)
#ifndef BIT
#define BIT(n) (UINT32_C(1) << (n))
#endif
#define CONFIG_RWNX_FULLMAC 1
#include "vendor/aic8800/lmac_msg.h"

_Static_assert(sizeof(struct mm_start_req) == 72, "MM_START_REQ ABI");
_Static_assert(sizeof(struct mm_add_if_req) == 10, "MM_ADD_IF_REQ ABI");
_Static_assert(sizeof(struct scanu_start_req) == 376, "SCANU_START_REQ ABI");
#endif
