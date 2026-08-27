#include <unicorn/unicorn.h>

#define ADDRESS 0x1000000

int main(void)
{
    const uint8_t code[] = {
        0x80, 0x05, 0xff, 0xff, 0xff, 0xff, 0x30, 0xeb, 0xf7, 0x30,
    };
    uint8_t immediate = UINT8_MAX;
    uint64_t rip = UINT64_MAX;
    uc_engine *uc;

    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x200000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 20) != UC_ERR_OK ||
        uc_mem_read(uc, ADDRESS + 6, &immediate, sizeof(immediate)) !=
            UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_RIP, &rip) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return immediate == 0 && rip == ADDRESS ? 0 : 1;
}
