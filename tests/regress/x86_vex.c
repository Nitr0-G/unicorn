#include <string.h>

#include <unicorn/unicorn.h>

#define CODE_ADDRESS 0x1000
#define DATA_ADDRESS 0x2000

static bool test_vmovdqu_xmm(void)
{
    const uint8_t code[] = {0xc5, 0xfa, 0x6f, 0x07};
    const uint8_t expected[16] = {
        0xad, 0xfa, 0x5c, 0x6d, 0x45, 0x4a, 0x93, 0x40,
        0xd2, 0x00, 0xde, 0x02, 0x89, 0xe8, 0x94, 0x40,
    };
    uint8_t xmm0[16] = {0};
    uint32_t edi = DATA_ADDRESS;
    uc_engine *uc;

    if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc) != UC_ERR_OK) {
        return false;
    }
    if (uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL) != UC_ERR_OK ||
        uc_mem_map(uc, CODE_ADDRESS, 0x2000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_mem_write(uc, DATA_ADDRESS, expected, sizeof(expected)) !=
            UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_EDI, &edi) != UC_ERR_OK ||
        uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(code), 0, 1) !=
            UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_XMM0, xmm0) != UC_ERR_OK) {
        uc_close(uc);
        return false;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return false;
    }
    return memcmp(xmm0, expected, sizeof(expected)) == 0;
}

static bool test_vmovdqu_ymm(void)
{
    const uint8_t code[] = {0xc5, 0xfe, 0x6f, 0x09};
    const uint8_t expected[32] = {
        0x00, 0x01, 0x02, 0x03, 0x04, 0x05, 0x06, 0x07, 0x08, 0x09, 0x0a,
        0x0b, 0x0c, 0x0d, 0x0e, 0x0f, 0x10, 0x11, 0x12, 0x13, 0x14, 0x15,
        0x16, 0x17, 0x18, 0x19, 0x1a, 0x1b, 0x1c, 0x1d, 0x1e, 0x1f,
    };
    uint8_t ymm1[32] = {0};
    uint64_t rcx = DATA_ADDRESS;
    uc_engine *uc;

    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK) {
        return false;
    }
    if (uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL) != UC_ERR_OK ||
        uc_mem_map(uc, CODE_ADDRESS, 0x2000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_mem_write(uc, DATA_ADDRESS, expected, sizeof(expected)) !=
            UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_RCX, &rcx) != UC_ERR_OK ||
        uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(code), 0, 1) !=
            UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_YMM1, ymm1) != UC_ERR_OK) {
        uc_close(uc);
        return false;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return false;
    }
    return memcmp(ymm1, expected, sizeof(expected)) == 0;
}

int main(void)
{
    return test_vmovdqu_xmm() && test_vmovdqu_ymm() ? 0 : 1;
}
