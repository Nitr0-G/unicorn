#include <unicorn/unicorn.h>

#define ADDRESS 0x1000000

static unsigned int hook_count;

static void count_instruction(uc_engine *uc, uint64_t address, uint32_t size,
                              void *user_data)
{
    (void)uc;
    (void)address;
    (void)size;
    (void)user_data;
    hook_count++;
}

int main(void)
{
    const uint8_t code[] = "00000000000000000000000000AA";
    uint32_t pc = 0;
    uc_engine *uc;
    uc_hook hook;

    if (uc_open(UC_ARCH_MIPS, UC_MODE_MIPS32, &uc) != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x200000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code) - 1) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_CODE, count_instruction, NULL, ADDRESS,
                    ADDRESS + 1) != UC_ERR_OK ||
        uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code) - 1, 0, 100) !=
            UC_ERR_OK ||
        uc_reg_read(uc, UC_MIPS_REG_PC, &pc) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return pc == ADDRESS + 24 && hook_count == 1 ? 0 : 1;
}
