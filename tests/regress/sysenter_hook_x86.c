#include <unicorn/unicorn.h>

#define ADDRESS 0x1000000

static unsigned int hook_count;
static uc_err stop_error;

static void stop_on_sysenter(uc_engine *uc, void *user_data)
{
    (void)user_data;
    hook_count++;
    stop_error = uc_emu_stop(uc);
}

int main(void)
{
    const uint8_t code[] = {0x0f, 0x34}; /* sysenter */
    uc_engine *uc;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_INSN, stop_on_sysenter, NULL, 1, 0,
                    UC_X86_INS_SYSENTER) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 0);
    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return err == UC_ERR_OK && stop_error == UC_ERR_OK && hook_count == 1 ? 0
                                                                          : 1;
}
