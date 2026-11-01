/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_CONNECT_H
#define AIC_CONNECT_H
#include <stdbool.h>
#include "scan.h"
struct aic_connection_io {
    void *context;
    aic_debug_exchange exchange;
    aic_receive receive, receive_data;
    int (*send_data)(void *, const uint8_t *, size_t);
};
int aic_connect(const struct aic_connection_io *, const char *, bool, bool, unsigned, const char *);
#endif
