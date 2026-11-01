/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_ROUTING_H
#define AIC_ROUTING_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
struct aic_route_entry { uint32_t network, mask; unsigned index; bool scoped; };
int aic_routes_parse(const uint8_t *, size_t, struct aic_route_entry *, size_t, size_t *);
/* Exact prefix/scope lookup. Does not accept a covering default route. */
int aic_route_owner(uint32_t, uint32_t, bool, unsigned, unsigned *);
int aic_route_other_tunnel(unsigned, char *, size_t);
bool aic_route_owned(const struct aic_route_entry *, uint32_t, uint32_t, bool, unsigned);
#endif
