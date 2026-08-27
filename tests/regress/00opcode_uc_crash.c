#include <inttypes.h>
#include <stdio.h>

#include <unicorn/unicorn.h>

#define CODE_ADDRESS UINT64_C(0x1000000)
#define DATA_ADDRESS UINT32_C(0x1000008)
#define MAP_SIZE UINT64_C(0x1000)

int main(void)
{
    const uint8_t code[] = {0x00, 0x00}; /* add byte ptr [eax], al */
    uint32_t eax = DATA_ADDRESS;
    uint32_t eip = 0;
    uint32_t value = 0;
    uc_engine *uc = NULL;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_open failed: %s\n", uc_strerror(err));
        return 1;
    }
    err = uc_mem_map(uc, CODE_ADDRESS, MAP_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_map failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code));
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_write failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_write(uc, UC_X86_REG_EAX, &eax);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_write(EAX) failed: %s\n", uc_strerror(err));
        goto fail;
    }

    err = uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(code), 0, 1);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_emu_start failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_read(uc, DATA_ADDRESS, &value, sizeof(value));
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_read failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_read(uc, UC_X86_REG_EIP, &eip);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_read(EIP) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    if (value != 8 || eip != CODE_ADDRESS + sizeof(code)) {
        fprintf(stderr,
                "unexpected result: value=%" PRIu32 ", EIP=0x%" PRIx32 "\n",
                value, eip);
        goto fail;
    }

    err = uc_close(uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_close failed: %s\n", uc_strerror(err));
        return 1;
    }
    return 0;

fail:
    uc_close(uc);
    return 1;
}
