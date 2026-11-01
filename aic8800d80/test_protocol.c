/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include "protocol.h"

int main(void)
{
    uint8_t cbw[31];
    char hex[63];
    const char *expected[] = {
        "555342438765432100000000000010fd0000000000000000000000000000f3",
        "555342438765432100000000000010fd0000000000000000000000000000f2"
    };
    for (unsigned i = 0; i < 2; i++) {
        aic_switch_cbw(cbw, i == 0 ? 0xf3 : 0xf2);
        for (unsigned j = 0; j < sizeof(cbw); j++)
            snprintf(hex + j * 2, 3, "%02x", cbw[j]);
        assert(strcmp(hex, expected[i]) == 0);
    }
    uint8_t csw[13] = {0x55, 0x53, 0x42, 0x53, 0x87, 0x65, 0x43, 0x21};
    assert(aic_valid_csw(csw, sizeof(csw)));
    assert(!aic_valid_csw(csw, 12));
    csw[4]++;
    assert(!aic_valid_csw(csw, sizeof(csw)));
    csw[4]--;
    csw[12] = 1;
    assert(!aic_valid_csw(csw, sizeof(csw)));

    uint8_t request[20];
    const uint8_t expected_request[20] = {
        0x10, 0, 0x11, 0, 0, 0, 0, 0,
        0, 4, 1, 0, 100, 0, 4, 0, 0, 0, 0x50, 0x40
    };
    aic_chip_read_request(request);
    assert(memcmp(request, expected_request, sizeof(request)) == 0);
    uint8_t generic[1048];
    assert(aic_debug_request(generic, sizeof(generic), 0x400, request + 16, 4) == 20);
    assert(memcmp(generic, expected_request, sizeof(request)) == 0);
    assert(aic_debug_request(generic, 19, 0x400, request + 16, 4) == 0);
    assert(aic_debug_request(generic, sizeof(generic), 0x400, request, 1033) == 0);

    /* Synthetic confirmation, NOT captured hardware evidence. */
    uint8_t response[24] = {
        20, 0, 0x11, 0, 1, 4, 100, 0, 1, 0, 8, 0,
        0, 0, 0, 0, 0, 0, 0x50, 0x40, 0, 0, 7, 0
    };
    uint32_t value = 0;
    for (size_t n = 1; n < sizeof(response); n++)
        assert(aic_parse_chip_response(response, n, &value) == -1);
    assert(aic_parse_chip_response(response, sizeof(response), &value) == 1);
    assert(value == 0x00070000);
    uint8_t confirmation[8];
    size_t actual = 0;
    assert(aic_parse_debug_cfm(response, 24, 0x401, confirmation, 8, &actual) == 1);
    assert(actual == 8 && aic_get32(confirmation) == AIC_CHIP_REGISTER);
    assert(aic_parse_debug_cfm(response, 24, 0x401, confirmation, 7, &actual) == -1);
    response[6] = 99;
    assert(aic_parse_chip_response(response, sizeof(response), &value) == 1);
    response[6] = 100;
    response[10] = 255;
    assert(aic_parse_chip_response(response, sizeof(response), &value) == -1);
    response[10] = 8;
    response[4] = 3;
    assert(aic_parse_chip_response(response, sizeof(response), &value) == 0);
    uint8_t aggregate[48];
    memcpy(aggregate, response, 24);
    response[4] = 1;
    memcpy(aggregate + 24, response, 24);
    assert(aic_parse_chip_response(aggregate, sizeof(aggregate), &value) == 1);
    /* Captured from this real 88M80 after native firmware startup. The
     * dummy source/destination and pattern words are not protocol IDs. */
    const uint8_t runtime_version[] = {
        0x28,0,0x11,0, 5,0,0xbd,0xea,0xa0,0x17,0x1c,0,0xb4,0x82,0x65,0xff,
        1,1,9,6,0xfb,0xfd,2,0,0x47,0x40,1,0,0x11,0x41,0xe2,0x5e,
        0,0,2,1,0xd7,0x77,0xe8,1,0x20,0,4,0
    };
    uint8_t version[28];
    assert(aic_parse_debug_cfm(runtime_version, sizeof(runtime_version), 5,
        version, sizeof(version), &actual) == 1);
    assert(actual == 28 && aic_get32(version) == 0x06090101);
    puts("Protocol tests passed (fixtures only; hardware tested separately).");
    return 0;
}
