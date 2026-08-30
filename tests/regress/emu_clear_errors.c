#include <unicorn/unicorn.h>

#define ADDRESS 0x1000

typedef struct UnmappedState {
    unsigned int count;
    uc_err stop_error;
} UnmappedState;

static bool stop_on_unmapped(uc_engine *uc, uc_mem_type type, uint64_t address,
                             int size, int64_t value, void *user_data)
{
    UnmappedState *state = user_data;

    (void)type;
    (void)address;
    (void)size;
    (void)value;
    state->count++;
    state->stop_error = uc_emu_stop(uc);
    return true;
}

int main(void)
{
    /* mov eax, [0]; nop */
    const uint8_t code[] = {0xa1, 0x00, 0x00, 0x00, 0x00, 0x90};
    UnmappedState state = {0};
    uc_engine *uc;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_MEM_UNMAPPED, stop_on_unmapped, &state,
                    1, 0) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_OK || state.count != 1 || state.stop_error != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS + 5, ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_OK || state.count != 1) {
        uc_close(uc);
        return 1;
    }

    return uc_close(uc) == UC_ERR_OK ? 0 : 1;
}
