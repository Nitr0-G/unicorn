#include <unicorn/unicorn.h>

#define ADDRESS 0x1000000

int main(void)
{
    /* mulx rsp, rsp, rdx */
    const uint8_t code[] = {0xc4, 0xe2, 0xdb, 0xf6, 0xe2};
    uint64_t rdx = 3;
    uc_engine *uc;

    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK) {
        return 1;
    }
    if (uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL) != UC_ERR_OK ||
        uc_mem_map(uc, ADDRESS, 0x200000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_RDX, &rdx) != UC_ERR_OK ||
        uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 1) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    return uc_close(uc) == UC_ERR_OK ? 0 : 1;
}
