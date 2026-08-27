/*
 * rep movsb regression
 *
 * Copyright(c) 2015 Chris Eagle
 *
 * This program is free software; you can redistribute it and/or
 * modify it under the terms of the GNU General Public License
 * version 2 as published by the Free Software Foundation.
 */

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#include <unicorn/unicorn.h>

#define CODE_ADDRESS UINT64_C(0x100000)
#define SOURCE_ADDRESS UINT32_C(0x200000)
#define DESTINATION_ADDRESS UINT32_C(0x201000)
#define COPY_SIZE 20

typedef struct WriteState {
    const uint8_t *expected;
    size_t count;
    bool failed;
} WriteState;

static void hook_mem_write(uc_engine *uc, uc_mem_type type, uint64_t address,
                           int size, int64_t value, void *user_data)
{
    WriteState *state = user_data;

    if (type != UC_MEM_WRITE || state->count >= COPY_SIZE ||
        address != DESTINATION_ADDRESS + state->count || size != 1 ||
        (uint8_t)value != state->expected[state->count]) {
        state->failed = true;
        uc_emu_stop(uc);
        return;
    }
    state->count++;
}
int main(void)
{
    const uint8_t code[] = {
        0xbe, 0x00, 0x00, 0x20, 0x00, /* mov esi, 0x200000 */
        0xbf, 0x00, 0x10, 0x20, 0x00, /* mov edi, 0x201000 */
        0xb9, 0x14, 0x00, 0x00, 0x00, /* mov ecx, 20 */
        0xf3, 0xa4,                   /* rep movsb */
    };
    uint8_t source[COPY_SIZE];
    uint8_t destination[COPY_SIZE];
    uint32_t esi = 0;
    uint32_t edi = 0;
    uint32_t ecx = 0;
    WriteState state = {source, 0, false};
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_err err;
    size_t i;

    for (i = 0; i < COPY_SIZE; i++) {
        source[i] = (uint8_t)(0x40 + i);
    }

    err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_open failed: %s\n", uc_strerror(err));
        return 1;
    }
    err = uc_mem_map(uc, CODE_ADDRESS, 0x1000, UC_PROT_READ | UC_PROT_EXEC);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_map(code) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_map(uc, SOURCE_ADDRESS, 0x2000, UC_PROT_READ | UC_PROT_WRITE);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_map(data) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code));
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_write(code) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_write(uc, SOURCE_ADDRESS, source, sizeof(source));
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_write(source) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err =
        uc_hook_add(uc, &hook, UC_HOOK_MEM_WRITE, hook_mem_write, &state, 1, 0);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_hook_add failed: %s\n", uc_strerror(err));
        goto fail;
    }

    err = uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_emu_start failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err =
        uc_mem_read(uc, DESTINATION_ADDRESS, destination, sizeof(destination));
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_read failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_read(uc, UC_X86_REG_ESI, &esi);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_read(ESI) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_read(uc, UC_X86_REG_EDI, &edi);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_read(EDI) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_read(uc, UC_X86_REG_ECX, &ecx);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_read(ECX) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    if (state.failed || state.count != COPY_SIZE ||
        memcmp(destination, source, sizeof(source)) != 0 ||
        esi != SOURCE_ADDRESS + COPY_SIZE ||
        edi != DESTINATION_ADDRESS + COPY_SIZE || ecx != 0) {
        fprintf(stderr,
                "unexpected REP result: hooks=%zu failed=%d "
                "ESI=0x%x EDI=0x%x ECX=%u\n",
                state.count, state.failed, esi, edi, ecx);
        goto fail;
    }

    err = uc_close(uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_close failed: %s\n", uc_strerror(err));
        return 1;
    }
    return 0;

fail:
    uc_close(uc);
    return 1;
}
