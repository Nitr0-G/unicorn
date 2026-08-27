#include <unicorn/unicorn.h>

#define ADDRESS 0x100000
#define STOP_ADDRESS (ADDRESS + 8)

static uc_err stop_error;

static void stop_at_instruction(uc_engine *uc, uint64_t address, uint32_t size,
                                void *user_data)
{
    (void)size;
    (void)user_data;
    if (address == STOP_ADDRESS) {
        stop_error = uc_emu_stop(uc);
    }
}

int main(void)
{
    const uint8_t code[16] = {0};
    uint32_t pc = 0;
    uc_engine *uc;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_MIPS, UC_MODE_MIPS32, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_CODE, stop_at_instruction, NULL, 1, 0) !=
            UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_OK || stop_error != UC_ERR_OK ||
        uc_reg_read(uc, UC_MIPS_REG_PC, &pc) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return pc == STOP_ADDRESS ? 0 : 1;
}
