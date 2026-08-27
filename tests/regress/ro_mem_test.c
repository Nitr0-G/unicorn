/*
 * Non-writable memory test case
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

#define CODE_ADDRESS UINT64_C(0x400000)
#define STACK_ADDRESS UINT64_C(0x500000)
#define STACK_SIZE UINT64_C(0x5000)
#define STACK_TOP (STACK_ADDRESS + STACK_SIZE)
#define UNALIGNED_WRITE_ADDRESS (CODE_ADDRESS + UINT64_C(0x25))
#define ALIGNED_WRITE_ADDRESS (CODE_ADDRESS + UINT64_C(0x2c))

typedef struct WriteFaultState {
    unsigned int stack_maps;
    unsigned int protected_writes;
    bool failed;
} WriteFaultState;

static bool hook_mem_invalid(uc_engine *uc, uc_mem_type type, uint64_t address,
                             int size, int64_t value, void *user_data)
{
    const uint64_t protected_addresses[] = {
        UNALIGNED_WRITE_ADDRESS,
        ALIGNED_WRITE_ADDRESS,
    };
    const uint32_t protected_values[] = {
        UINT32_C(0x12345678),
        UINT32_C(0x87654321),
    };
    WriteFaultState *state = user_data;
    uc_err err;

    if (type == UC_MEM_WRITE_UNMAPPED) {
        if (state->stack_maps != 0 || address != STACK_TOP - 4 || size != 4 ||
            (uint32_t)value != CODE_ADDRESS + 0x21) {
            state->failed = true;
            return false;
        }
        err = uc_mem_map(uc, STACK_ADDRESS, STACK_SIZE,
                         UC_PROT_READ | UC_PROT_WRITE);
        if (err != UC_ERR_OK) {
            state->failed = true;
            return false;
        }
        state->stack_maps++;
        return true;
    }

    if (type == UC_MEM_WRITE_PROT) {
        if (state->protected_writes >= 2 ||
            address != protected_addresses[state->protected_writes] ||
            size != 4 ||
            (uint32_t)value != protected_values[state->protected_writes]) {
            state->failed = true;
        }
        state->protected_writes++;
        return false;
    }

    state->failed = true;
    return false;
}
int main(void)
{
    const uint8_t code[] = {
        0xeb, 0x1a,                         /* jmp bottom */
        0x58,                               /* pop eax */
        0x83, 0xc0, 0x04,                   /* add eax, 4 */
        0x83, 0xe0, 0xfc,                   /* and eax, -4 */
        0x83, 0xc0, 0x01,                   /* add eax, 1 */
        0xc7, 0x00, 0x78, 0x56, 0x34, 0x12, /* unaligned write */
        0x83, 0xc0, 0x07,                   /* add eax, 7 */
        0xc7, 0x00, 0x21, 0x43, 0x65, 0x87, /* aligned write */
        0x90,                               /* nop */
        0xe8, 0xe1, 0xff, 0xff, 0xff,       /* call top */
        'x',  'x',  'x',  'x',  'A',  'A',  'A', 'A',
        'x',  'x',  'x',  'B',  'B',  'B',  'B',
    };
    const uint8_t expected_stack[] = {0x21, 0x00, 0x40, 0x00};
    const uint8_t expected_a[] = {'A', 'A', 'A', 'A'};
    const uint8_t expected_b[] = {'B', 'B', 'B', 'B'};
    uint8_t actual[4];
    uint32_t esp = (uint32_t)STACK_TOP;
    uint32_t eax = (uint32_t)ALIGNED_WRITE_ADDRESS;
    WriteFaultState state = {0};
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_err err;

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
    err = uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code));
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_write(code) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_write(uc, UC_X86_REG_ESP, &esp);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_write(ESP) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_hook_add(uc, &hook,
                      UC_HOOK_MEM_WRITE_UNMAPPED | UC_HOOK_MEM_WRITE_PROT,
                      hook_mem_invalid, &state, 1, 0);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_hook_add failed: %s\n", uc_strerror(err));
        goto fail;
    }

    err = uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(code), 0, 10);
    if (err != UC_ERR_WRITE_PROT) {
        fprintf(stderr, "first write expected UC_ERR_WRITE_PROT, got %s\n",
                uc_strerror(err));
        goto fail;
    }
    if (state.failed || state.stack_maps != 1 || state.protected_writes != 1) {
        fprintf(stderr,
                "unexpected first fault state: maps=%u protected=%u "
                "failed=%d\n",
                state.stack_maps, state.protected_writes, state.failed);
        goto fail;
    }

    err = uc_reg_write(uc, UC_X86_REG_EAX, &eax);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_write(EAX) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_emu_start(uc, CODE_ADDRESS + 0x15, CODE_ADDRESS + sizeof(code), 0,
                       1);
    if (err != UC_ERR_WRITE_PROT) {
        fprintf(stderr, "second write expected UC_ERR_WRITE_PROT, got %s\n",
                uc_strerror(err));
        goto fail;
    }
    if (state.failed || state.stack_maps != 1 || state.protected_writes != 2) {
        fprintf(stderr,
                "unexpected second fault state: maps=%u protected=%u "
                "failed=%d\n",
                state.stack_maps, state.protected_writes, state.failed);
        goto fail;
    }

    err = uc_mem_read(uc, STACK_TOP - 4, actual, sizeof(actual));
    if (err != UC_ERR_OK || memcmp(actual, expected_stack, sizeof(actual))) {
        fprintf(stderr, "stack return address is incorrect\n");
        goto fail;
    }
    err = uc_mem_read(uc, UNALIGNED_WRITE_ADDRESS, actual, sizeof(actual));
    if (err != UC_ERR_OK || memcmp(actual, expected_a, sizeof(actual))) {
        fprintf(stderr, "unaligned protected write changed memory\n");
        goto fail;
    }
    err = uc_mem_read(uc, ALIGNED_WRITE_ADDRESS, actual, sizeof(actual));
    if (err != UC_ERR_OK || memcmp(actual, expected_b, sizeof(actual))) {
        fprintf(stderr, "aligned protected write changed memory\n");
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
