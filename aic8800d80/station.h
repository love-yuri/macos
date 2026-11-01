/* SPDX-License-Identifier: GPL-2.0-only */
#ifndef AIC_STATION_H
#define AIC_STATION_H
#include <stdbool.h>
#include "scan.h"
struct aic_station {
    uint8_t mac[6], vif;
    bool active;
};
struct aic_bss {
    uint8_t bssid[6], ssid[32], ssid_len, rsn[256];
    size_t rsn_len;
    uint16_t frequency;
    int8_t rssi;
};
int aic_command(void *, aic_debug_exchange, uint16_t, const void *, size_t, void *, size_t);
int aic_station_start(void *, aic_debug_exchange, struct aic_station *);
void aic_station_stop(void *, aic_debug_exchange, struct aic_station *);
int aic_find_bss(void *, aic_debug_exchange, aic_receive, struct aic_station *,
                 const char *, struct aic_bss *);
int aic_bss_parse(const uint8_t *, size_t, const char *, struct aic_bss *);
#endif
