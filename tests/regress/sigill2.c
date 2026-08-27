#include <unicorn/unicorn.h>

#define ADDRESS 0x2000

int main(void)
{
    const uint8_t code[] = {0x0f, 0x0b}; /* ud2 */
    uc_engine *uc;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_64, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 1);
    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return err == UC_ERR_INSN_INVALID ? 0 : 1;
}
