#include <unicorn/unicorn.h>

#define ADDRESS 0x1000

int main(void)
{
    /* vadd.f32 s0, s1, s2 */
    const uint8_t code[] = {0x81, 0x0a, 0x30, 0xee};
    uint32_t s0 = 0;
    uint32_t s1 = 0x3f800000;
    uint32_t s2 = 0x40000000;
    uint32_t cpacr = 0;
    uint32_t fpexc = 1U << 30;
    uc_engine *uc;
    uc_err err;

    err = uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_A15) != UC_ERR_OK ||
        uc_reg_read(uc, UC_ARM_REG_C1_C0_2, &cpacr) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }
    cpacr |= 0xfU << 20;
    if (uc_reg_write(uc, UC_ARM_REG_C1_C0_2, &cpacr) != UC_ERR_OK ||
        uc_reg_write(uc, UC_ARM_REG_FPEXC, &fpexc) != UC_ERR_OK ||
        uc_reg_write(uc, UC_ARM_REG_S0, &s0) != UC_ERR_OK ||
        uc_reg_write(uc, UC_ARM_REG_S1, &s1) != UC_ERR_OK ||
        uc_reg_write(uc, UC_ARM_REG_S2, &s2) != UC_ERR_OK ||
        uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 1) != UC_ERR_OK ||
        uc_reg_read(uc, UC_ARM_REG_S0, &s0) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return s0 == 0x40400000 ? 0 : 1;
}
