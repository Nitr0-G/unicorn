#include <unicorn/unicorn.h>

#define ADDRESS 0x1000000

static unsigned int instruction_count;

static void count_instruction(uc_engine *uc, uint64_t address, uint32_t size,
                              void *user_data)
{
    (void)uc;
    (void)address;
    (void)size;
    (void)user_data;
    instruction_count++;
}

int main(void)
{
    const uint8_t code[] = {
        0x33, 0xd2,                         /* xor edx, edx */
        0x8a, 0xd4,                         /* mov dl, ah */
        0x8b, 0xc8,                         /* mov ecx, eax */
        0x81, 0xe1, 0xff, 0x00, 0x00, 0x00, /* and ecx, 0xff */
    };
    uint32_t eax = 0x1db10106;
    uint32_t ebx = 0x7efde000;
    uint32_t ecx = 0x7efde000;
    uint32_t edx = 0x1db1;
    uint32_t eflags = 0x206;
    uc_engine *uc;
    uc_hook hook;

    if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc) != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_EAX, &eax) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_EBX, &ebx) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_ECX, &ecx) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_EDX, &edx) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_EFLAGS, &eflags) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_CODE, count_instruction, NULL, 1, 0) !=
            UC_ERR_OK ||
        uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 0) != UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_ECX, &ecx) != UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_EDX, &edx) != UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_EFLAGS, &eflags) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return ecx == 6 && edx == 1 && eflags == 0x206 && instruction_count == 4
               ? 0
               : 1;
}
