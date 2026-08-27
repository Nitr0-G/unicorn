#include <inttypes.h>
#include <stdio.h>

#include <unicorn/unicorn.h>

#define CODE_ADDRESS UINT64_C(0x400)
#define MAP_SIZE UINT64_C(0x1000)

static unsigned int hook_count;

static void count_instruction(uc_engine *uc, uint64_t address, uint32_t size,
                              void *user_data)
{
    (void)uc;
    (void)address;
    (void)size;
    (void)user_data;
    hook_count++;
}

int main(void)
{
    const uint8_t code[] = {0x4c, 0x4c};
    uint32_t pc = 0;
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, &uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_open failed: %s\n", uc_strerror(err));
        return 1;
    }
    err = uc_ctl_set_cpu_model(uc, UC_CPU_M68K_CFV4E);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_ctl_set_cpu_model failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_map(uc, 0, MAP_SIZE, UC_PROT_READ | UC_PROT_EXEC);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_map failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code));
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_write failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_hook_add(uc, &hook, UC_HOOK_CODE, count_instruction, NULL,
                      CODE_ADDRESS, CODE_ADDRESS + 1);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_hook_add failed: %s\n", uc_strerror(err));
        goto fail;
    }

    err = uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(code), 0, 1);
    if (err != UC_ERR_EXCEPTION) {
        fprintf(stderr, "expected UC_ERR_EXCEPTION, got %s\n",
                uc_strerror(err));
        goto fail;
    }
    err = uc_reg_read(uc, UC_M68K_REG_PC, &pc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_read(PC) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    if (pc != CODE_ADDRESS) {
        fprintf(stderr, "expected PC 0x%" PRIx64 ", got 0x%" PRIx32 "\n",
                CODE_ADDRESS, pc);
        goto fail;
    }
    if (hook_count == 0) {
        fprintf(stderr, "bounded code hook was not invoked\n");
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
