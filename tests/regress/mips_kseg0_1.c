#include <unicorn/unicorn.h>

#define PHYSICAL_ADDRESS 0x1000
#define KSEG0_ADDRESS 0x80001000
#define KSEG1_ADDRESS 0xa0001000

static bool run_at(uc_engine *uc, uint64_t address)
{
    uint32_t at = 0;

    if (uc_reg_write(uc, UC_MIPS_REG_AT, &at) != UC_ERR_OK ||
        uc_emu_start(uc, address, address + 4, 0, 1) != UC_ERR_OK ||
        uc_reg_read(uc, UC_MIPS_REG_AT, &at) != UC_ERR_OK) {
        return false;
    }
    return at == 0x3456;
}

int main(void)
{
    const uint8_t code[] = {0x56, 0x34, 0x21, 0x34}; /* ori $at, 0x3456 */
    uc_engine *uc;

    if (uc_open(UC_ARCH_MIPS, UC_MODE_MIPS32, &uc) != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, PHYSICAL_ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, PHYSICAL_ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        !run_at(uc, PHYSICAL_ADDRESS) || !run_at(uc, KSEG0_ADDRESS) ||
        !run_at(uc, KSEG1_ADDRESS)) {
        uc_close(uc);
        return 1;
    }

    return uc_close(uc) == UC_ERR_OK ? 0 : 1;
}
