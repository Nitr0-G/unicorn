#include <unicorn/unicorn.h>

#define ADDRESS 0x1000

static unsigned int invalid_instruction_count;

static bool count_invalid_instruction(uc_engine *uc, void *user_data)
{
    (void)uc;
    (void)user_data;
    invalid_instruction_count++;
    return false;
}

int main(void)
{
    const uint8_t code[] = {0x0f, 0x0b}; /* ud2 */
    uc_engine *uc;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_64, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_INSN_INVALID, count_invalid_instruction,
                    NULL, 1, 0) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 1);
    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return err == UC_ERR_INSN_INVALID && invalid_instruction_count == 1 ? 0 : 1;
}
