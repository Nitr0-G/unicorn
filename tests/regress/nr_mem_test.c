#include <unicorn/unicorn.h>

#define CODE_ADDRESS 0x100000
#define READABLE_ADDRESS 0x300000
#define WRITE_ONLY_ADDRESS 0x400000

static unsigned int protection_fault_count;

static bool reject_protected_read(uc_engine *uc, uc_mem_type type,
                                  uint64_t address, int size, int64_t value,
                                  void *user_data)
{
    (void)uc;
    (void)value;
    (void)user_data;
    if (type == UC_MEM_READ_PROT && address == WRITE_ONLY_ADDRESS &&
        size == 4) {
        protection_fault_count++;
    }
    return false;
}

int main(void)
{
    /* mov ebx, [0x300000]; mov eax, [0x400000] */
    const uint8_t code[] = {
        0x8b, 0x1d, 0x00, 0x00, 0x30, 0x00, 0xa1, 0x00, 0x00, 0x40, 0x00,
    };
    const uint32_t readable_value = 0x41414141;
    const uint32_t protected_value = 0x42424242;
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uc_engine *uc;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, CODE_ADDRESS, 0x1000, UC_PROT_READ | UC_PROT_EXEC) !=
            UC_ERR_OK ||
        uc_mem_map(uc, READABLE_ADDRESS, 0x1000,
                   UC_PROT_READ | UC_PROT_WRITE) != UC_ERR_OK ||
        uc_mem_map(uc, WRITE_ONLY_ADDRESS, 0x1000, UC_PROT_WRITE) !=
            UC_ERR_OK ||
        uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_mem_write(uc, READABLE_ADDRESS, &readable_value,
                     sizeof(readable_value)) != UC_ERR_OK ||
        uc_mem_write(uc, WRITE_ONLY_ADDRESS, &protected_value,
                     sizeof(protected_value)) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_MEM_READ_PROT, reject_protected_read,
                    NULL, 1, 0) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_READ_PROT ||
        uc_reg_read(uc, UC_X86_REG_EAX, &eax) != UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_EBX, &ebx) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return protection_fault_count == 1 && eax == 0 && ebx == readable_value ? 0
                                                                            : 1;
}
