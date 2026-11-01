/* SPDX-License-Identifier: GPL-2.0-only */
#include <stdbool.h>
#include <stdio.h>
#include <stddef.h>
#include "station.h"
#include "lmac_abi.h"
#include "protocol.h"

int aic_command(void *ctx, aic_debug_exchange exchange, uint16_t id,
                const void *p, size_t n, void *out, size_t expected)
{
    uint8_t response[2048];
    size_t actual = 0;
    if (exchange(ctx, id, p, n, response, sizeof(response), &actual) || actual < expected) {
        fprintf(stderr, "Command 0x%04x failed (response %zu, expected %zu).\n", id, actual, expected);
        return -1;
    }
    if (expected) memcpy(out, response, expected);
    return 0;
}

int aic_station_start(void *ctx, aic_debug_exchange exchange, struct aic_station *s)
{
#define CMD(id, p, out) aic_command(ctx, exchange, id, &(p), sizeof(p), &(out), sizeof(out))
    struct mm_version_cfm version;
    uint8_t reg[4], chip[8];
    aic_put32(reg, AIC_CHIP_REGISTER);
    if (aic_command(ctx, exchange, MM_VERSION_REQ, NULL, 0, &version, sizeof(version)) ||
        aic_command(ctx, exchange, DBG_MEM_READ_REQ, reg, 4, chip, 8) ||
        version.version_lmac != 0x06090101 || aic_get32(chip) != AIC_CHIP_REGISTER ||
        ((aic_get32(chip + 4) >> 16) & 0xff) != 7 || ((aic_get32(chip + 4) >> 25) & 1) != 1)
        return -1;
    struct mm_set_stack_start_req stack = {.is_stack_start = 1, .set_vendor_info = BIT(5)};
    struct mm_set_stack_start_cfm stack_cfm;
    if (CMD(MM_SET_STACK_START_REQ, stack, stack_cfm)) return -1;
    /* Matching D80 userconfig values. Neither power loss compensation nor
     * user offsets are enabled in that file. No increase over Linux defaults. */
    struct mm_set_txpwr_lvl_req power = {.txpwr_lvl_v3 = {
        .enable = 1,
        .pwrlvl_11b_11ag_2g4 = {18,18,18,18,18,18,18,18,16,16,15,15},
        .pwrlvl_11n_11ac_2g4 = {18,18,18,18,16,16,15,15,14,14},
        .pwrlvl_11ax_2g4 = {18,18,18,18,16,16,15,15,14,14,13,13},
        .pwrlvl_11a_5g = {-128,-128,-128,-128,18,18,18,18,16,16,15,15},
        .pwrlvl_11n_11ac_5g = {18,18,18,18,16,16,15,15,14,14},
        .pwrlvl_11ax_5g = {18,18,18,18,16,16,14,14,13,13,12,12}
    }};
    struct mm_set_rf_calib_req rf = {.cal_cfg_24g = 0x0f8f, .cal_cfg_5g = 0x0f0f,
        .param_alpha = 0x0c34c008, .bt_calib_param = 0x264203};
    struct mm_set_rf_calib_cfm rf_cfm;
    struct mm_get_mac_addr_req get = {.get = 1};
    struct mm_get_mac_addr_cfm mac;
    if (aic_command(ctx, exchange, MM_SET_TXPWR_IDX_LVL_REQ, &power, sizeof(power), NULL, 0) ||
        CMD(MM_SET_RF_CALIB_REQ, rf, rf_cfm) || CMD(MM_GET_MAC_ADDR_REQ, get, mac) ||
        aic_command(ctx, exchange, MM_RESET_REQ, NULL, 0, NULL, 0)) return -1;
    memcpy(s->mac, mac.mac_addr, 6);
    if ((s->mac[0] & 1) || !(s->mac[0] | s->mac[1] | s->mac[2] | s->mac[3] | s->mac[4] | s->mac[5]))
        return -1;
    /* First connection deliberately uses legacy OFDM at 20 MHz. Avoids
     * aggregation/reordering until the underlying data path is validated. */
    struct me_config_req config = {.tx_lft = 1000};
    struct me_chan_config_req channels = {0};
    channels.chan5G_cnt = 4;
    for (unsigned i = 0; i < 4; i++)
        channels.chan5G[i] = (struct mac_chan_def){.freq = 5180 + i * 20,
            .band = 1, .tx_power = 18};
    if (!stack_cfm.is_5g_support) return -1;
    struct mm_start_req start = {.uapsd_timeout = 300, .lp_clk_accuracy = 20};
    struct mm_add_if_req add = {.type = VIF_STA};
    struct mm_add_if_cfm vif;
    memcpy(&add.addr, s->mac, 6);
    if (aic_command(ctx, exchange, ME_CONFIG_REQ, &config, sizeof(config), NULL, 0) ||
        aic_command(ctx, exchange, ME_CHAN_CONFIG_REQ, &channels, sizeof(channels), NULL, 0) ||
        aic_command(ctx, exchange, MM_START_REQ, &start, sizeof(start), NULL, 0) ||
        CMD(MM_ADD_IF_REQ, add, vif) || vif.status) return -1;
    s->vif = vif.inst_nbr;
    s->active = true;
    printf("Station interface %u started (20 MHz legacy OFDM).\n", s->vif);
    return 0;
#undef CMD
}

void aic_station_stop(void *ctx, aic_debug_exchange exchange, struct aic_station *s)
{
    if (!s->active) return;
    struct sm_disconnect_req disconnect = {.reason_code = 3, .vif_idx = s->vif};
    aic_command(ctx, exchange, SM_DISCONNECT_REQ, &disconnect, sizeof(disconnect), NULL, 0);
    struct mm_remove_if_req remove = {.inst_nbr = s->vif};
    aic_command(ctx, exchange, MM_REMOVE_IF_REQ, &remove, sizeof(remove), NULL, 0);
    s->active = false;
}

int aic_bss_parse(const uint8_t *p, size_t n, const char *ssid, struct aic_bss *out)
{
    size_t off = offsetof(struct scanu_result_ind, payload);
    if (n < off || aic_get16(p) > n - off || aic_get16(p) < 36) return -1;
    size_t len = aic_get16(p), target_len = strlen(ssid);
    const uint8_t *f = p + off;
    if ((f[0] & 0xfc) != 0x80 && (f[0] & 0xfc) != 0x50) return 0;
    struct aic_bss b = {.frequency = aic_get16(p + 4), .rssi = (int8_t)p[9]};
    bool match = false;
    memcpy(b.bssid, f + 16, 6);
    for (size_t i = 36; i < len;) {
        if (len - i < 2 || f[i+1] > len - i - 2) return -1;
        size_t k = f[i+1];
        if (f[i] == 0 && k == target_len && !memcmp(f+i+2, ssid, k)) {
            if (k > 32) return -1;
            memcpy(b.ssid, ssid, k); b.ssid_len = k; match = true;
        }
        if (f[i] == 48) {
            if (k + 2 > sizeof(b.rsn)) return -1;
            memcpy(b.rsn, f+i, k+2); b.rsn_len = k+2;
        }
        i += 2 + k;
    }
    if (!match) return 0;
    *out = b;
    return 1;
}

int aic_find_bss(void *ctx, aic_debug_exchange exchange, aic_receive receive,
                 struct aic_station *s, const char *ssid, struct aic_bss *bss)
{
    struct scanu_start_req scan = {.vif_idx = s->vif, .chan_cnt = 4};
    memset(&scan.bssid, 0xff, 6);
    for (unsigned i = 0; i < 4; i++) scan.chan[i] = (struct mac_chan_def){
        .freq = 5180 + i * 20, .band = 1, .flags = CHAN_NO_IR, .tx_power = 18};
    if (aic_command(ctx, exchange, SCANU_START_REQ, &scan, sizeof(scan), NULL, 0)) return -1;
    bool found = false;
    puts("Scanning channels 36-48 for the selected network...");
    for (unsigned i = 0; i < 100; i++) {
        uint8_t buf[8192]; size_t n = 0;
        int rc = receive(ctx, buf, sizeof(buf), &n);
        if (rc < 0) return -1;
        for (size_t k = 0; rc && k < n;) {
            const uint8_t *p = buf + k;
            if (n-k < 4) return -1;
            size_t size = aic_get16(p), frame = 4 + ((size + 3) & ~(size_t)3);
            if (frame > n-k || !size) return -1;
            if ((p[2] & 0x7f) == 0x11) {
                if (size < 12 || aic_get16(p+10) > size-12) return -1;
                if (aic_get16(p+4) == SCANU_RESULT_IND) {
                    struct aic_bss candidate;
                    int parsed = aic_bss_parse(p+16, aic_get16(p+10), ssid, &candidate);
                    if (parsed < 0) return -1;
                    if (parsed && (!found || candidate.rssi > bss->rssi)) { *bss = candidate; found = true; }
                }
                if (aic_get16(p+4) == SCANU_START_CFM) {
                    if (!found) fprintf(stderr, "Selected SSID was not found on channels 36-48.\n");
                    else printf("Selected BSS: %u MHz, RSSI %d, RSN IE %zu bytes.\n", bss->frequency, bss->rssi, bss->rsn_len);
                    return found ? 0 : -1;
                }
            }
            k += frame;
        }
    }
    fprintf(stderr, "Target scan timed out.\n");
    return -1;
}
