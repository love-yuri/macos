/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_AUTH_H
#define AIC_AUTH_H
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
struct aic_auth;
struct aic_auth_ops {
    void *ctx;
    int (*key)(void *, bool, unsigned, const uint8_t *, size_t);
    int (*send)(void *, const uint8_t *, const uint8_t *, size_t);
    void (*complete)(void *);
    void (*failed)(void *);
};
int aic_auth_validate(const uint8_t *, size_t);
struct aic_auth *aic_auth_create(const struct aic_auth_ops *, const uint8_t *, const uint8_t *,
                                const char *, const char *, const uint8_t *, size_t);
const uint8_t *aic_auth_ie(struct aic_auth *, size_t *);
void aic_auth_associated(struct aic_auth *);
int aic_auth_rx(struct aic_auth *, const uint8_t *, const uint8_t *, size_t, int);
void aic_auth_destroy(struct aic_auth *);
#endif
