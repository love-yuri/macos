/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stddef.h>
#include <stdio.h>
#include "scan.h"
#include "lmac_abi.h"
#include "protocol.h"

struct mock {
    unsigned received, removed, scans;
    bool bad_ie, fail_init, wrong_profile;
};

static int exchange(void *context, uint16_t id, const uint8_t *params,
                    size_t length, uint8_t *response, size_t capacity, size_t *actual)
{
    struct mock *mock = context;
    memset(response, 0, capacity);
    *actual = 0;
    switch (id) {
    case MM_VERSION_REQ:
        aic_put32(response, 0x06090101);
        *actual = sizeof(struct mm_version_cfm);
        break;
    case DBG_MEM_READ_REQ:
        assert(length == 4 && aic_get32(params) == AIC_CHIP_REGISTER);
        aic_put32(response, AIC_CHIP_REGISTER);
        aic_put32(response + 4, mock->wrong_profile ? 0xf1078820 : 0xf3078820);
        *actual = 8;
        break;
    case MM_SET_STACK_START_REQ:
        if (mock->fail_init)
            return -1;
        response[0] = 1;
        *actual = sizeof(struct mm_set_stack_start_cfm);
        break;
    case MM_SET_RF_CALIB_REQ:
        *actual = sizeof(struct mm_set_rf_calib_cfm);
        break;
    case MM_GET_MAC_ADDR_REQ:
        response[0] = 2;
        *actual = 6;
        break;
    case MM_ADD_IF_REQ:
        response[1] = 1;
        *actual = 2;
        break;
    case MM_REMOVE_IF_REQ:
        assert(length == 1 && params[0] == 1);
        mock->removed++;
        break;
    case SCANU_START_REQ: {
        assert(length == sizeof(struct scanu_start_req));
        struct scanu_start_req scan;
        memcpy(&scan, params, length);
        assert(scan.vif_idx == 1 && scan.chan_cnt == 36 && scan.ssid_cnt == 0);
        for (unsigned i = 0; i < scan.chan_cnt; i++)
            assert(scan.chan[i].flags & CHAN_NO_IR);
        assert(scan.chan[11].freq == 5180 && scan.chan[11].band == 1);
        mock->scans++;
        break;
    }
    }
    return 0;
}

static int receive(void *context, uint8_t *buffer, size_t capacity, size_t *actual)
{
    struct mock *mock = context;
    assert(capacity >= 128);
    memset(buffer, 0, capacity);
    buffer[2] = 0x11;
    if (mock->received++ == 0) {
        aic_put16(buffer, 63);
        aic_put16(buffer + 4, SCANU_RESULT_IND);
        aic_put16(buffer + 10, 51);
        aic_put16(buffer + 16, 39); /* management frame length */
        aic_put16(buffer + 20, 5180);
        buffer[25] = (uint8_t)-38;
        uint8_t *frame = buffer + 16 + offsetof(struct scanu_result_ind, payload);
        frame[0] = 0x80;
        frame[37] = mock->bad_ie ? 255 : 1;
        frame[38] = 'x';
        *actual = 68;
    } else {
        aic_put16(buffer, 15);
        aic_put16(buffer + 4, SCANU_START_CFM);
        aic_put16(buffer + 10, 3);
        buffer[16] = 1;
        buffer[18] = 1;
        *actual = 20;
    }
    return 1;
}

int main(void)
{
    struct mock mock = {0};
    assert(aic_passive_scan(&mock, exchange, receive) == 0);
    assert(mock.scans == 1 && mock.removed == 1);
    mock = (struct mock){.bad_ie = true};
    assert(aic_passive_scan(&mock, exchange, receive) != 0);
    assert(mock.removed == 1); /* malformed beacon still releases station */
    mock = (struct mock){.fail_init = true};
    assert(aic_passive_scan(&mock, exchange, receive) != 0);
    assert(mock.scans == 0 && mock.removed == 0);
    mock = (struct mock){.wrong_profile = true};
    assert(aic_passive_scan(&mock, exchange, receive) != 0);
    assert(mock.scans == 0 && mock.removed == 0);
    puts("Passive scan tests passed (simulated events; not hardware evidence).");
    return 0;
}
