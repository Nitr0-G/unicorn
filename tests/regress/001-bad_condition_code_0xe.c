#include <unicorn/unicorn.h>

#define ADDRESS 0x2000

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
    const uint8_t code[] = {
        0x56, 0xe8, 0x46, 0x46, 0x80, 0xf6, 0x8c, 0x56, 0xff,
        0xbf, 0xcd, 0x90, 0xda, 0xa0, 0xed, 0xe8, 0x46, 0x43,
        0x45, 0xe5, 0x80, 0x90, 0x44, 0x46, 0x04,
    };
    uint32_t pc = 0;
    uc_engine *uc;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_WRITE | UC_PROT_EXEC) !=
            UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_CODE, count_instruction, NULL, ADDRESS,
                    ADDRESS + 1) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_WRITE_UNMAPPED ||
        uc_reg_read(uc, UC_ARM_REG_PC, &pc) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }
    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return pc == ADDRESS + 4 && hook_count == 1 ? 0 : 1;
}
