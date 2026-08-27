#include <inttypes.h>
#include <stdio.h>

#include <unicorn/unicorn.h>

#define CODE_ADDRESS UINT64_C(0x1000000)
#define INITIAL_EFLAGS UINT32_C(0x00000206)
#define POPPED_EFLAGS UINT32_C(0x00247ed7)

int main(void)
{
    const uint8_t code[] = {
        0x9c,                         /* pushfd */
        0x68, 0xff, 0xfe, 0xff, 0xff, /* push 0xfffffeff */
        0x9d,                         /* popfd */
        0x9c,                         /* pushfd */
        0x58,                         /* pop eax */
        0x9d,                         /* popfd */
    };
    uint32_t eax = 0;
    uint32_t eflags = INITIAL_EFLAGS;
    uint32_t esp = (uint32_t)CODE_ADDRESS + 0x800;
    uint32_t final_esp = 0;
    uc_engine *uc = NULL;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_open failed: %s\n", uc_strerror(err));
        return 1;
    }
    err = uc_ctl_set_cpu_model(uc, UC_CPU_X86_QEMU64);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_ctl_set_cpu_model failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_map(uc, CODE_ADDRESS, 0x1000, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_map failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code));
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_write failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_write(uc, UC_X86_REG_ESP, &esp);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_write(ESP) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_write(uc, UC_X86_REG_EFLAGS, &eflags);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_write(EFLAGS) failed: %s\n", uc_strerror(err));
        goto fail;
    }

    err = uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_emu_start failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_read(uc, UC_X86_REG_EAX, &eax);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_read(EAX) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_read(uc, UC_X86_REG_EFLAGS, &eflags);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_read(EFLAGS) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_reg_read(uc, UC_X86_REG_ESP, &final_esp);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_reg_read(ESP) failed: %s\n", uc_strerror(err));
        goto fail;
    }
    if (eax != POPPED_EFLAGS || eflags != INITIAL_EFLAGS || final_esp != esp) {
        fprintf(stderr,
                "unexpected flags state: EAX=0x%08" PRIx32
                " EFLAGS=0x%08" PRIx32 " ESP=0x%08" PRIx32 "\n",
                eax, eflags, final_esp);
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
