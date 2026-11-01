/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_NETSTACK_H
#define AIC_NETSTACK_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct aic_lease {
    uint8_t ip[4], mask[4], gateway[4], dns[4], server[4];
    uint32_t seconds;
};
struct aic_network;
struct aic_network_ops {
    void *ctx;
    int (*send)(void *, const uint8_t *, size_t);
    int (*lease)(void *, const struct aic_lease *);
    void (*ip)(void *, const uint8_t *, size_t);
    void (*failure)(void *);
};
struct aic_network *aic_network_create(const struct aic_network_ops *, const uint8_t *);
void aic_network_destroy(struct aic_network *);
void aic_network_rx(struct aic_network *, const uint8_t *, size_t);
int aic_network_tx_ip(struct aic_network *, const uint8_t *, size_t);
void aic_network_tick(struct aic_network *, uint64_t);
bool aic_network_ready(struct aic_network *);
const uint8_t *aic_network_test_address(struct aic_network *);
int aic_dns_test_answer(const uint8_t *, size_t, uint8_t *);
uint16_t aic_checksum(const uint8_t *, size_t);
#endif
