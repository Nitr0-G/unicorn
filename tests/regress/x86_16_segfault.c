#include <unicorn/unicorn.h>

#define ADDRESS (100 * 1024)

int main(void)
{
    const uint8_t code[] = {0x90};
    uint32_t eip = 0;
    uc_engine *uc;
    uc_err err;

    if (uc_open(UC_ARCH_X86, UC_MODE_16, &uc) != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 1);
    if (err != UC_ERR_FETCH_UNMAPPED ||
        uc_reg_read(uc, UC_X86_REG_EIP, &eip) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }
    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return eip == (ADDRESS & UINT16_MAX) ? 0 : 1;
}
