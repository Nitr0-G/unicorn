#include <unicorn/unicorn.h>

#define ADDRESS 0x100000

int main(void)
{
    const uint8_t code[] = {
        0x02, 0xbc, 0x00, 0x00, /* be 0 */
        0x01, 0x00, 0x00, 0x00, /* nop (delay slot) */
    };
    uint32_t pc = UINT32_MAX;
    uint32_t psr = 1U << 22; /* ICC.Z */
    uc_engine *uc;
    uc_err err;

    err = uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_reg_write(uc, UC_SPARC_REG_PSR, &psr) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_FETCH_UNMAPPED ||
        uc_reg_read(uc, UC_SPARC_REG_PC, &pc) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return pc == 0 ? 0 : 1;
}
