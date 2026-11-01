/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_MACOS_PROTOCOL_H
#define AIC_MACOS_PROTOCOL_H

#include <stddef.h>
#include <stdint.h>
#include <string.h>

/* Wire format from aic_load_fw/aicbluetooth_cmds.{c,h}.
 * Original protocol implementation: Copyright (C) RivieraWaves 2014-2019.
 * This implementation uses explicit byte offsets, without Linux structures.
 */
#define AIC_CHIP_REGISTER UINT32_C(0x40500000)

static inline void aic_put16(uint8_t *p, uint16_t v)
{
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static inline void aic_put32(uint8_t *p, uint32_t v)
{
    for (unsigned i = 0; i < 4; i++)
        p[i] = (uint8_t)(v >> (8 * i));
}

static inline uint16_t aic_get16(const uint8_t *p)
{
    return (uint16_t)(p[0] | ((uint16_t)p[1] << 8));
}

static inline uint32_t aic_get32(const uint8_t *p)
{
    return p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

/* Exactly the 31-byte CBWs in usb_modeswitch/1111_1111. */
static inline void aic_switch_cbw(uint8_t out[31], uint8_t command)
{
    const uint8_t prefix[16] = {
        0x55, 0x53, 0x42, 0x43, 0x87, 0x65, 0x43, 0x21,
        0, 0, 0, 0, 0, 0, 0x10, 0xfd
    };
    memset(out, 0, 31);
    memcpy(out, prefix, sizeof(prefix));
    out[30] = command;
}

static inline int aic_valid_csw(const uint8_t *p, size_t length)
{
    const uint8_t signature_tag[8] = {
        0x55, 0x53, 0x42, 0x53, 0x87, 0x65, 0x43, 0x21
    };
    return length == 13 && memcmp(p, signature_tag, 8) == 0 &&
           aic_get32(p + 8) == 0 && p[12] == 0;
}

static inline void aic_chip_read_request(uint8_t out[20])
{
    const uint8_t request[20] = {
        0x10, 0, 0x11, 0,  /* USB length excludes its own 4-byte header */
        0, 0, 0, 0,        /* TX dummy word */
        0, 0x04,           /* DBG_MEM_READ_REQ = TASK_DBG << 10 */
        1, 0,              /* destination: TASK_DBG */
        100, 0,            /* source: DRV_TASK_ID */
        4, 0,              /* request parameter length */
        0, 0, 0x50, 0x40  /* 0x40500000, little endian */
    };
    memcpy(out, request, sizeof(request));
}

static inline size_t aic_debug_request(uint8_t *out, size_t capacity,
                                       uint16_t id, const uint8_t *params,
                                       size_t length)
{
    if (length > 1032 || capacity < length + 16)
        return 0;
    memset(out, 0, length + 16);
    aic_put16(out, (uint16_t)(length + 12));
    out[2] = 0x11;
    aic_put16(out + 8, id);
    aic_put16(out + 10, id >> 10);
    aic_put16(out + 12, 100);
    aic_put16(out + 14, (uint16_t)length);
    if (length)
        memcpy(out + 16, params, length);
    return length + 16;
}

static inline int aic_parse_debug_cfm(const uint8_t *p, size_t length,
                                     uint16_t id, uint8_t *params,
                                     size_t capacity, size_t *actual)
{
    while (length != 0) {
        if (length < 4)
            return -1;
        size_t payload = aic_get16(p);
        size_t frame = 4 + ((payload + 3) & ~(size_t)3);
        if (payload == 0 || payload > length - 4 || frame > length)
            return -1;
        if ((p[2] & 0x7f) == 0x11) {
            if (payload < 12 || aic_get16(p + 10) > payload - 12)
                return -1;
            if (aic_get16(p + 4) == id) {
                size_t count = aic_get16(p + 10);
                /* ipc_e2a_msg calls destination/source "dummy" fields.
                 * Real runtime firmware leaves them uninitialized; Linux
                 * matches the message ID and does not inspect these words.
                 */
                if (count > capacity)
                    return -1;
                if (count)
                    memcpy(params, p + 16, count);
                *actual = count;
                return 1;
            }
        }
        p += frame;
        length -= frame;
    }
    return 0;
}

/* RX has a pattern word AFTER its message header; TX's dummy is BEFORE it.
 * Return 1 for a matching confirmation, 0 for other messages, -1 if malformed.
 * Iterate 4-byte-aligned messages as aic_load_fw/aic_txrxif.c does.
 */
static inline int aic_parse_chip_response(const uint8_t *p, size_t length,
                                          uint32_t *value)
{
    uint8_t params[8];
    size_t actual = 0;
    int result = aic_parse_debug_cfm(p, length, 0x401, params, sizeof(params), &actual);
    if (result != 1)
        return result;
    if (actual != 8 || aic_get32(params) != AIC_CHIP_REGISTER)
        return -1;
    *value = aic_get32(params + 4);
    return 1;
}

#endif
