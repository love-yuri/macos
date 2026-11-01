/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_CREDENTIALS_H
#define AIC_CREDENTIALS_H
#include <stddef.h>
int aic_read_password(const char *, char *, size_t);
/* Explicit root-only storage for unattended boot; never falls back to UI. */
int aic_read_password_file(const char *, const char *, char *, size_t);
int aic_save_password(const char *, const char *);
static inline void aic_clear(void *p, size_t n)
{
    volatile unsigned char *q=p;
    while(n--) *q++=0;
}
#endif
