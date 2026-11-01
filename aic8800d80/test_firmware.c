/* SPDX-License-Identifier: GPL-2.0-only */
#include <assert.h>
#include <stdio.h>
#include <stdlib.h>
#include "firmware.h"
#include "protocol.h"

struct rom {
    uint32_t chip;
    unsigned blocks, writes, started;
    int fail_block;
    uint8_t *ram;
};

static int exchange(void *context, uint16_t id, const uint8_t *params,
                     size_t length, uint8_t *out, size_t capacity, size_t *actual)
{
    struct rom *rom = context;
    uint32_t address = aic_get32(params);
    if (id == 0x400) {
        assert(length == 4 && capacity >= 8);
        uint32_t value = 0;
        if (address == AIC_CHIP_REGISTER)
            value = rom->chip;
        else {
            assert(address >= 0x100000 && address <= 0x23fffc);
            value = aic_get32(rom->ram + address - 0x100000);
        }
        aic_put32(out, address);
        aic_put32(out + 4, value);
        *actual = 8;
    } else if (id == 0x402) {
        assert(length == 8 && capacity >= 8);
        if (address >= 0x100000 && address <= 0x23fffc)
            memcpy(rom->ram + address - 0x100000, params + 4, 4);
        rom->writes++;
        memcpy(out, params, 8);
        *actual = 8;
    } else if (id == 0x40b) {
        assert(length == 1032 && capacity >= 4);
        if (rom->fail_block)
            return -1;
        uint32_t count = aic_get32(params + 4);
        assert(count > 0 && count <= 1024);
        assert(address >= 0x100000 && count <= 0x240000 - address);
        memcpy(rom->ram + address - 0x100000, params + 8, count);
        for (uint32_t i = count; i < 1024; i++)
            assert(params[8 + i] == 0);
        rom->blocks++;
        memset(out, 0, 4);
        *actual = 4;
    } else {
        assert(id == 0x40d && length == 8);
        assert(address == 0x120000 && aic_get32(params + 4) == 1);
        assert(rom->blocks > 350 && rom->writes > 100);
        rom->started++;
    }
    return 0;
}

int main(void)
{
    struct rom rom = {.chip = 0xf3078820, .ram = calloc(1, 0x140000)};
    assert(rom.ram);
    assert(aic_firmware_boot("firmware", exchange, &rom) == 0);
    assert(rom.started == 1);
    /* Confirm the entire uploaded main image, including its short final
     * block, matches the real file byte-for-byte after patch configuration.
     */
    FILE *f = fopen("firmware/fmacfw_8800d80_u02.bin", "rb");
    assert(f);
    unsigned offset = 0;
    int byte;
    while ((byte = fgetc(f)) != EOF)
        assert(rom.ram[0x20000 + offset++] == (uint8_t)byte);
    assert(!ferror(f) && offset == 358072);
    fclose(f);
    assert(aic_get32(rom.ram + 0x77c00) == 0x48435450);
    assert(aic_get32(rom.ram + 0x77c04) == 0x17d318);
    assert(aic_get32(rom.ram + 0x77c0c) == 3);
    assert(aic_get32(rom.ram + 0x7d318) == 0x177158 + 0xb4);

    rom.chip = 0xf1078820; /* MCU1 must be refused before the first write. */
    rom.blocks = rom.writes = rom.started = 0;
    assert(aic_firmware_boot("firmware", exchange, &rom) != 0);
    assert(!rom.blocks && !rom.writes && !rom.started);

    rom.chip = 0xf3078820;
    rom.fail_block = 1;
    assert(aic_firmware_boot("firmware", exchange, &rom) != 0);
    assert(!rom.writes && !rom.started);
    free(rom.ram);
    puts("Firmware integration tests passed (simulated ROM; not hardware evidence).");
    return 0;
}
