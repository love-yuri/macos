/* SPDX-License-Identifier: GPL-2.0-only */
/* Native port of the normal-mode D80/U02 loader in
 * drivers/aic8800/aic_load_fw/{aic_compat_8800d80,aicbluetooth}.c.
 * USB protocol: Copyright (C) RivieraWaves 2014-2019.
 * Limited intentionally to the real chip_id=7, MCU0 hardware profile.
 */
#include "firmware.h"
#include "protocol.h"
#include <CommonCrypto/CommonDigest.h>
#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>

struct image {
    const char *name, *sha256;
    uint8_t *data;
    size_t size;
};

struct loader {
    aic_debug_exchange exchange;
    void *context;
};

static int read_word(struct loader *l, uint32_t address, uint32_t *value)
{
    uint8_t request[4], response[8];
    size_t actual = 0;
    aic_put32(request, address);
    if (l->exchange(l->context, 0x400, request, 4, response, 8, &actual) ||
        actual != 8 || aic_get32(response) != address)
        return -1;
    *value = aic_get32(response + 4);
    return 0;
}

static int write_word(struct loader *l, uint32_t address, uint32_t value)
{
    uint8_t request[8], response[8];
    size_t actual = 0;
    aic_put32(request, address);
    aic_put32(request + 4, value);
    if (l->exchange(l->context, 0x402, request, 8, response, 8, &actual))
        return -1;
    /* Linux accepts an empty write confirmation; validate the address when
     * the ROM supplies the documented echo parameters. */
    if (actual != 0 && (actual != 8 || aic_get32(response) != address))
        return -1;
    return 0;
}

static int read_image(const char *directory, struct image *image)
{
    char path[PATH_MAX];
    int count = snprintf(path, sizeof(path), "%s/%s", directory, image->name);
    if (count < 0 || (size_t)count >= sizeof(path))
        return -1;
    FILE *f = fopen(path, "rb");
    if (!f) {
        perror(path);
        return -1;
    }
    int result = -1;
    if (fseek(f, 0, SEEK_END))
        goto done;
    long size = ftell(f);
    if (size <= 0 || size > 1024 * 1024 || fseek(f, 0, SEEK_SET))
        goto done;
    image->size = (size_t)size;
    image->data = malloc(image->size);
    if (!image->data || fread(image->data, 1, image->size, f) != image->size)
        goto done;
    uint8_t hash[CC_SHA256_DIGEST_LENGTH];
    char hex[CC_SHA256_DIGEST_LENGTH * 2 + 1];
    CC_SHA256(image->data, (CC_LONG)image->size, hash);
    for (unsigned i = 0; i < sizeof(hash); i++)
        snprintf(hex + 2 * i, 3, "%02x", hash[i]);
    if (strcmp(hex, image->sha256)) {
        fprintf(stderr, "Firmware checksum mismatch: %s\n", path);
        goto done;
    }
    printf("Verified firmware: %s (%zu bytes)\n", image->name, image->size);
    result = 0;
done:
    fclose(f);
    return result;
}

static int upload(struct loader *l, const struct image *image, uint32_t address)
{
    printf("Uploading %s to RAM 0x%08x...\n", image->name, address);
    for (size_t offset = 0; offset < image->size; offset += 1024) {
        size_t count = image->size - offset;
        if (count > 1024)
            count = 1024;
        /* Linux always sends sizeof(dbg_mem_block_write_req), including
         * zero padding after the final short block. */
        uint8_t request[1032] = {0}, response[4];
        size_t actual = 0;
        aic_put32(request, address + (uint32_t)offset);
        aic_put32(request + 4, (uint32_t)count);
        memcpy(request + 8, image->data + offset, count);
        if (l->exchange(l->context, 0x40b, request, sizeof(request),
                        response, sizeof(response), &actual) ||
            actual != 4 || aic_get32(response) != 0) {
            fprintf(stderr, "RAM upload failed at 0x%08x.\n", address + (uint32_t)offset);
            return -1;
        }
    }
    return 0;
}

static int apply_table(struct loader *l, const struct image *table)
{
    /* Exact pinned table has seven bounded records; all were checksum
     * verified before any write. Type 0 has four info pairs to apply; the
     * remaining pairs describe extension firmware, not register writes.
     */
    const uint32_t btmode[9] = {
        1, UINT32_MAX, 0, 5, 1, 1500000, 1, 0, 0x00006f2f
    };
    size_t offset = 16;
    if (table->size < offset || memcmp(table->data, "AICBT_PT_TAG", 12))
        return -1;
    while (offset < table->size) {
        if (table->size - offset < 24)
            return -1;
        const uint8_t *record = table->data + offset;
        uint32_t type = aic_get32(record + 16), pairs = aic_get32(record + 20);
        if (pairs > (table->size - offset - 24) / 8 || type > 6)
            return -1;
        if (type != 6) {
            uint32_t count = type == 0 ? 4 : pairs;
            if (count > pairs)
                return -1;
            printf("Applying patch table %.16s (%u pairs)\n", (const char *)record, count);
            for (uint32_t i = 0; i < count; i++) {
                uint32_t address = aic_get32(record + 24 + i * 8);
                uint32_t value = aic_get32(record + 28 + i * 8);
                if (type == 3 && i < 9)
                    value = btmode[i];
                if (write_word(l, address, value))
                    return -1;
            }
            if (type == 4)
                usleep(100000);
        }
        offset += 24 + (size_t)pairs * 8;
    }
    return 0;
}

static int sram_pointer(uint32_t value, size_t bytes)
{
    return !(value & 3) && value >= 0x100000 && value < 0x240000 &&
           bytes <= 0x240000 - value;
}

static int configure_patch(struct loader *l)
{
    uint32_t config, patch, version, buffer;
    if (read_word(l, 0x120198, &config) || read_word(l, 0x1201a0, &patch) ||
        read_word(l, 0x12001c, &version) || read_word(l, 0x1201a4, &buffer))
        return -1;
    printf("Firmware version=0x%08x config=0x%08x patch=0x%08x buffer=0x%08x\n",
        version, config, patch, buffer);
    if (version != 0x06090101 || !sram_pointer(config, 0x18c) ||
        !sram_pointer(patch, 64) || !sram_pointer(buffer, 24)) {
        fprintf(stderr, "Unexpected firmware layout; refusing patch writes.\n");
        return -1;
    }
    if (write_word(l, patch, 0x48435450) || write_word(l, patch + 8, 0x50544348) ||
        write_word(l, patch + 4, buffer) || write_word(l, patch + 12, 3))
        return -1;
    const uint32_t pairs[3][2] = {
        {0x00b4, 0xf3010001}, /* USE_5G */
        {0x0170, 0x0001000a}, /* default RX aggregation count */
        {0x0188, 1}           /* USER_EXT_FLAGS_DEFAULT_D80 */
    };
    for (unsigned i = 0; i < 3; i++) {
        if (write_word(l, buffer + i * 8, config + pairs[i][0]) ||
            write_word(l, buffer + i * 8 + 4, pairs[i][1]))
            return -1;
    }
    /* aic_patch_t.block_size[] starts at byte offset 48. */
    for (unsigned i = 0; i < 4; i++)
        if (write_word(l, patch + 48 + i * 4, 0))
            return -1;
    return 0;
}

int aic_firmware_boot(const char *directory, aic_debug_exchange exchange, void *context)
{
    struct image images[] = {
        {"fw_adid_8800d80_u02.bin", "a526cbd02fcdc495f049f3ad6b5933cb08cd984b16790c716a060d582fee1a56", NULL, 0},
        {"fw_patch_8800d80_u02.bin", "f0e2f5bbc17bc327ca7f1574ff55370dfd863d931514347bb4abc18a74f6218f", NULL, 0},
        {"fw_patch_8800d80_u02_ext0.bin", "ca738f5ea5aca6021c4f48b60ff35e835dfdaf072a8490e3ba8ba0a42440e3e5", NULL, 0},
        {"fw_patch_table_8800d80_u02.bin", "9decb77435b7e9713e33e32da483d683b7329ed93b672b2d1b134031d7da5f67", NULL, 0},
        {"fmacfw_8800d80_u02.bin", "23f2bed2ac0e8f7bda8f36df7defd30d9b928c97867f6f0f01f28d89dd78d670", NULL, 0}
    };
    int result = -1;
    for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); i++)
        if (read_image(directory, &images[i]))
            goto done;
    if (!exchange) {
        result = 0;
        goto done;
    }
    struct loader l = {exchange, context};
    uint32_t chip;
    if (read_word(&l, AIC_CHIP_REGISTER, &chip) ||
        ((chip >> 16) & 0xff) != 7 || ((chip >> 25) & 1) != 1) {
        fprintf(stderr, "Loader requires D80 chip_id=7 / chip_mcu_id=0; no firmware written.\n");
        goto done;
    }
    /* The pinned main-branch system_config_8800d80() has no MCU0 writes
     * when CONFIG_PMIC_SETTING=n. Patch info supplies the actual load
     * addresses, which differ from defaults in the header. */
    const uint8_t *info = images[3].data + 40;
    uint32_t adid = aic_get32(info + 4), patch = aic_get32(info + 12);
    uint32_t extension_id = aic_get32(info + 40), extension = aic_get32(info + 44);
    if (adid != 0x201940 || patch != 0x1e0000 || extension_id != 0 || extension != 0x20b43c)
        goto done;
    if (upload(&l, &images[0], adid) || upload(&l, &images[1], patch) ||
        upload(&l, &images[2], extension) || apply_table(&l, &images[3]) ||
        upload(&l, &images[4], 0x120000) || configure_patch(&l))
        goto done;
    uint8_t start[8];
    aic_put32(start, 0x120000);
    aic_put32(start + 4, 1); /* HOST_START_APP_AUTO, not flash OTA */
    puts("Starting RAM firmware at 0x00120000...");
    if (exchange(context, 0x40d, start, sizeof(start), NULL, 0, NULL))
        goto done;
    result = 0;
done:
    for (unsigned i = 0; i < sizeof(images) / sizeof(images[0]); i++)
        free(images[i].data);
    return result;
}
