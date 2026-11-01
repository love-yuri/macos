/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_SERVICE_H
#define AIC_SERVICE_H
#include "netstack.h"
#include <CoreFoundation/CoreFoundation.h>

/* Paired State: IPv4 and default DNS entities for atomic session publication.
 * No Setup: keys or persistent network preferences are created. */
CFDictionaryRef aic_service_values_create(const char *, const char *, const struct aic_lease *);
#endif
