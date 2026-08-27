#include <unicorn/unicorn.h>

#define ADDRESS 0x1000000
#define ITERATIONS 2

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
    /* rep stosd */
    const uint8_t code[] = {0xf3, 0xab};
    const uint32_t value = 0xbaadbabe;
    uint32_t destination = ADDRESS + 0x300;
    uint32_t count = ITERATIONS;
    uint32_t result[ITERATIONS] = {0};
    uc_engine *uc;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x200000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_EAX, &value) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_EDI, &destination) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_ECX, &count) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_CODE, count_instruction, NULL, 1, 0) !=
            UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_ECX, &count) != UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_EDI, &destination) != UC_ERR_OK ||
        uc_mem_read(uc, ADDRESS + 0x300, result, sizeof(result)) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_close(uc);
    if (err != UC_ERR_OK) {
        return 1;
    }

    return hook_count == ITERATIONS + 1 && count == 0 &&
                   destination == ADDRESS + 0x300 + sizeof(result) &&
                   result[0] == value && result[1] == value
               ? 0
               : 1;
}
