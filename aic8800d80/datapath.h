/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_DATAPATH_H
#define AIC_DATAPATH_H
#include <stddef.h>
#include <stdint.h>
typedef void (*aic_ethernet_rx)(void *, const uint8_t *, size_t, int);
size_t aic_tx_encode(uint8_t *, size_t, const uint8_t *, size_t, uint8_t, uint8_t, uint32_t);
/* Includes USB aggregation decoding, 802.11/SNAP conversion and validation. */
int aic_rx_decode(const uint8_t *, size_t, uint8_t, aic_ethernet_rx, void *);
struct aic_rx_stream { uint8_t bytes[131072]; size_t used; };
/* USB completions can split an aggregated firmware frame. */
int aic_rx_feed(struct aic_rx_stream *, const uint8_t *, size_t, uint8_t, aic_ethernet_rx, void *);
#endif
