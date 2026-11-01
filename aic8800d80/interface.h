/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_INTERFACE_H
#define AIC_INTERFACE_H
#include "netstack.h"
struct aic_interface;
struct aic_interface *aic_interface_open(void);
int aic_interface_configure(struct aic_interface *, const struct aic_lease *);
int aic_interface_route(struct aic_interface *, const struct aic_lease *);
const char *aic_interface_name(struct aic_interface *);
/* 1 after configd elects our IPv4 service and installs its default DNS. */
int aic_interface_service_ready(struct aic_interface *);
/* Address: scoped link probe; NULL: ordinary system DNS and route probe. */
int aic_interface_check_start(struct aic_interface *, const uint8_t *);
int aic_interface_check_poll(struct aic_interface *);
int aic_interface_fd(struct aic_interface *);
int aic_interface_write(struct aic_interface *, const uint8_t *, size_t);
int aic_interface_read(struct aic_interface *, uint8_t *, size_t);
void aic_interface_close(struct aic_interface *);
#endif
