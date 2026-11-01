/* SPDX-License-Identifier: GPL-2.0-only */
/* Native scan bring-up using the real FullMAC ABI and initialization order
 * from rwnx_main.c/rwnx_msg_tx.c. Passive scan only at this stage.
 * Protocol definitions: Copyright (C) RivieraWaves 2011-2019.
 */
#include "scan.h"
#include "lmac_abi.h"
#include "protocol.h"
#include <stddef.h>
#include <stdio.h>

static int command(void *context, aic_debug_exchange exchange, uint16_t id,
                    const void *parameters, size_t size, void *confirmation, size_t expected)
{
    uint8_t response[2048];
    size_t actual = 0;
    printf("Runtime command 0x%04x (%zu bytes)...\n", id, size);
    if (exchange(context, id, parameters, size, response, sizeof(response), &actual))
        return -1;
    if (actual < expected) {
        fprintf(stderr, "Short response for 0x%04x: %zu < %zu\n", id, actual, expected);
        return -1;
    }
    if (expected)
        memcpy(confirmation, response, expected);
    return 0;
}

static int print_result(const uint8_t *p, size_t size, uint8_t seen[64][6], unsigned *count)
{
    const size_t offset = offsetof(struct scanu_result_ind, payload);
    if (size < offset)
        return -1;
    size_t length = aic_get16(p);
    if (length < 36 || length > size - offset)
        return -1;
    const uint8_t *frame = p + offset;
    if ((frame[0] & 0xfc) != 0x80 && (frame[0] & 0xfc) != 0x50)
        return 0;
    for (unsigned i = 0; i < *count; i++)
        if (memcmp(seen[i], frame + 16, 6) == 0)
            return 0;
    for (size_t i = 36; i < length;) {
        if (length - i < 2 || frame[i + 1] > length - i - 2)
            return -1;
        size_t n = frame[i + 1];
        if (frame[i] == 0) {
            if (n > 32)
                return -1;
            printf("Wi-Fi: \"");
            for (size_t j = 0; j < n; j++) {
                unsigned char ch = frame[i + 2 + j];
                if (ch >= 0x20 && ch != 0x7f && ch != '"' && ch != '\\')
                    putchar(ch);
                else
                    printf("\\x%02x", ch);
            }
            printf("\" frequency=%u MHz RSSI=%d dBm\n", aic_get16(p + 4), (int8_t)p[9]);
            if (*count < 64) {
                memcpy(seen[*count], frame + 16, 6);
                (*count)++;
            }
            return 0;
        }
        i += 2 + n;
    }
    return 0;
}

int aic_passive_scan(void *context, aic_debug_exchange exchange, aic_receive receive)
{
    struct mm_set_stack_start_req stack = {
        .is_stack_start = 1, .set_vendor_info = BIT(5)
    };
    struct mm_set_stack_start_cfm stack_cfm;
    struct mm_set_rf_calib_req calibration = {
        .cal_cfg_24g = 0x0f8f, .cal_cfg_5g = 0x0f0f,
        .param_alpha = 0x0c34c008, .bt_calib_param = 0x264203
    };
    struct mm_set_rf_calib_cfm calibration_cfm;
    struct mm_get_mac_addr_req get_mac = {.get = 1};
    struct mm_get_mac_addr_cfm mac;
    struct mm_add_if_cfm vif = {0};
    bool have_vif = false;
    int result = -1;
    struct mm_version_cfm version;
    uint8_t chip_request[4], chip_response[8];
    aic_put32(chip_request, AIC_CHIP_REGISTER);
    if (command(context, exchange, MM_VERSION_REQ, NULL, 0, &version, sizeof(version)) ||
        command(context, exchange, DBG_MEM_READ_REQ, chip_request, sizeof(chip_request),
                chip_response, sizeof(chip_response)))
        goto done;
    uint32_t chip = aic_get32(chip_response + 4);
    if (version.version_lmac != 0x06090101 ||
        aic_get32(chip_response) != AIC_CHIP_REGISTER ||
        ((chip >> 16) & 0xff) != 7 || ((chip >> 25) & 1) != 1) {
        fprintf(stderr, "Passive scan requires the validated D80 MCU0 / 6.9.1.1 profile.\n");
        goto done;
    }
    if (command(context, exchange, MM_SET_STACK_START_REQ, &stack, sizeof(stack),
                &stack_cfm, sizeof(stack_cfm)) ||
        command(context, exchange, MM_SET_RF_CALIB_REQ, &calibration, sizeof(calibration),
                &calibration_cfm, sizeof(calibration_cfm)) ||
        command(context, exchange, MM_GET_MAC_ADDR_REQ, &get_mac, sizeof(get_mac), &mac, sizeof(mac)) ||
        command(context, exchange, MM_RESET_REQ, NULL, 0, NULL, 0))
        goto done;
    /* Legacy capabilities suffice for a passive receive test. No HT/VHT/HE
     * feature is claimed. TX power tables and connection support are a
     * separate implementation stage. */
    struct me_config_req config = {.tx_lft = 1000};
    struct me_chan_config_req channels = {0};
    channels.chan2G4_cnt = 11;
    for (unsigned i = 0; i < channels.chan2G4_cnt; i++) {
        channels.chan2G4[i].freq = (uint16_t)(2412 + i * 5);
        channels.chan2G4[i].band = 0;
        channels.chan2G4[i].flags = CHAN_NO_IR;
    }
    /* The normal 5GHz channels in rwnx_main.c, excluding its extra PHY
     * measurement channels. Scan passively, including DFS channels.
     */
    const uint16_t frequencies[] = {
        5180,5200,5220,5240,5260,5280,5300,5320,
        5500,5520,5540,5560,5580,5600,5620,5640,5660,5680,5700,5720,
        5745,5765,5785,5805,5825
    };
    if (stack_cfm.is_5g_support) {
        channels.chan5G_cnt = sizeof(frequencies) / sizeof(frequencies[0]);
        for (unsigned i = 0; i < channels.chan5G_cnt; i++) {
            channels.chan5G[i].freq = frequencies[i];
            channels.chan5G[i].band = 1;
            channels.chan5G[i].flags = CHAN_NO_IR;
        }
    }
    struct mm_start_req start = {.uapsd_timeout = 300, .lp_clk_accuracy = 20};
    struct mm_add_if_req add = {.type = VIF_STA};
    memcpy(&add.addr, mac.mac_addr, 6);
    if (command(context, exchange, ME_CONFIG_REQ, &config, sizeof(config), NULL, 0) ||
        command(context, exchange, ME_CHAN_CONFIG_REQ, &channels, sizeof(channels), NULL, 0) ||
        command(context, exchange, MM_START_REQ, &start, sizeof(start), NULL, 0) ||
        command(context, exchange, MM_ADD_IF_REQ, &add, sizeof(add), &vif, sizeof(vif)))
        goto done;
    if (vif.status) {
        fprintf(stderr, "MM_ADD_IF failed with status %u.\n", vif.status);
        goto done;
    }
    have_vif = true;
    struct scanu_start_req scan = {0};
    memcpy(scan.chan, channels.chan2G4, 11 * sizeof(struct mac_chan_def));
    memcpy(scan.chan + 11, channels.chan5G,
        channels.chan5G_cnt * sizeof(struct mac_chan_def));
    memset(&scan.bssid, 0xff, sizeof(scan.bssid));
    scan.vif_idx = vif.inst_nbr;
    scan.chan_cnt = 11 + channels.chan5G_cnt;
    if (command(context, exchange, SCANU_START_REQ, &scan, sizeof(scan), NULL, 0))
        goto done;
    printf("Listening for passive scan results: 11 channels in 2.4GHz, %u channels in 5GHz...\n",
        channels.chan5G_cnt);
    uint8_t seen[64][6];
    unsigned count = 0;
    for (unsigned tries = 0; tries < 80; tries++) {
        uint8_t buffer[4096];
        size_t size = 0;
        int received = receive(context, buffer, sizeof(buffer), &size);
        if (received < 0)
            goto done;
        if (received == 0)
            continue;
        for (size_t offset = 0; offset < size;) {
            if (size - offset < 4)
                goto done;
            const uint8_t *p = buffer + offset;
            size_t payload = aic_get16(p);
            size_t frame_size = 4 + ((payload + 3) & ~(size_t)3);
            if (payload > size - offset - 4 || frame_size > size - offset || !payload)
                goto done;
            if ((p[2] & 0x7f) == 0x11) {
                if (payload < 12 || aic_get16(p + 10) > payload - 12)
                    goto done;
                uint16_t id = aic_get16(p + 4), length = aic_get16(p + 10);
                if (id == SCANU_RESULT_IND && print_result(p + 16, length, seen, &count))
                    goto done;
                if (id == SCANU_START_CFM) {
                    if (length < sizeof(struct scanu_start_cfm) || p[17])
                        goto done;
                    printf("Passive scan completed: %u unique access points. "
                           "Use connect to join a network.\n", count);
                    result = 0;
                    goto done;
                }
            }
            offset += frame_size;
        }
    }
    fprintf(stderr, "Passive scan completion timed out.\n");
done:
    if (have_vif) {
        struct mm_remove_if_req remove = {.inst_nbr = vif.inst_nbr};
        command(context, exchange, MM_REMOVE_IF_REQ, &remove, sizeof(remove), NULL, 0);
    }
    return result;
}
