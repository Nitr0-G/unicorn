#include <stdbool.h>
#include <stdio.h>
#include <unicorn/unicorn.h>

#define CODE_ADDRESS UINT64_C(0x1000000)
#define LOOP_ADDRESS (CODE_ADDRESS + UINT64_C(4))

typedef struct BlockState {
    unsigned int count;
    bool failed;
} BlockState;

static void hook_block(uc_engine *uc, uint64_t address, uint32_t size,
                       void *user_data)
{
    const uint64_t expected_addresses[] = {
        CODE_ADDRESS,
        LOOP_ADDRESS,
        LOOP_ADDRESS,
    };
    BlockState *state = user_data;

    if (state->count >= 3 || address != expected_addresses[state->count] ||
        size != 3) {
        state->failed = true;
        uc_emu_stop(uc);
        return;
    }
    state->count++;
    if (state->count == 3) {
        if (uc_emu_stop(uc) != UC_ERR_OK) {
            state->failed = true;
        }
    }
}
int main(void)
{
    const uint8_t code[] = {
        0x90,       /* nop */
        0xeb, 0x01, /* jmp LOOP_ADDRESS */
        0x90,       /* unreachable nop */
        0x90,       /* nop */
        0xeb, 0xfd, /* jmp LOOP_ADDRESS */
    };
    BlockState state = {0};
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_open failed: %s\n", uc_strerror(err));
        return 1;
    }
    err = uc_mem_map(uc, CODE_ADDRESS, 0x1000, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_map failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code));
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_write failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_hook_add(uc, &hook, UC_HOOK_BLOCK, hook_block, &state, 1, 0);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_hook_add failed: %s\n", uc_strerror(err));
        goto fail;
    }

    err = uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_emu_start failed: %s\n", uc_strerror(err));
        goto fail;
    }
    if (state.failed || state.count != 3) {
        fprintf(stderr, "unexpected block trace: count=%u failed=%d\n",
                state.count, state.failed);
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
