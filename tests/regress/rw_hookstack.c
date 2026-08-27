#include <unicorn/unicorn.h>

#define CODE_ADDRESS 0x1000000
#define RESULT_ADDRESS (CODE_ADDRESS + 0x40)

typedef struct HookState {
    unsigned int reads;
    unsigned int writes;
} HookState;

static void count_memory_access(uc_engine *uc, uc_mem_type type,
                                uint64_t address, int size, int64_t value,
                                void *user_data)
{
    HookState *state = user_data;

    (void)uc;
    (void)address;
    (void)size;
    (void)value;
    if (type == UC_MEM_READ) {
        state->reads++;
    } else if (type == UC_MEM_WRITE) {
        state->writes++;
    }
}

static bool run_stack_case(uc_engine *uc, uint32_t stack_address,
                           uint32_t value)
{
    uint32_t eax = 0;
    uint32_t esp = stack_address + 4;
    uint32_t result = 0;

    if (uc_mem_map(uc, stack_address, 0x4000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, esp, &value, sizeof(value)) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_EAX, &eax) != UC_ERR_OK ||
        uc_reg_write(uc, UC_X86_REG_ESP, &esp) != UC_ERR_OK ||
        uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + 13, 0, 0) != UC_ERR_OK ||
        uc_reg_read(uc, UC_X86_REG_EAX, &eax) != UC_ERR_OK ||
        uc_mem_read(uc, RESULT_ADDRESS, &result, sizeof(result)) != UC_ERR_OK) {
        return false;
    }
    return eax == value && result == value;
}

int main(void)
{
    const uint8_t code[] = {
        0x8b, 0x04, 0x24,             /* mov eax, [esp] */
        0xa3, 0x40, 0x00, 0x00, 0x01, /* mov [result], eax */
        0xa1, 0x40, 0x00, 0x00, 0x01, /* mov eax, [result] */
    };
    HookState state = {0};
    uc_engine *uc;
    uc_hook hook;

    if (uc_open(UC_ARCH_X86, UC_MODE_32, &uc) != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, CODE_ADDRESS, 0x200000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, CODE_ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
                    count_memory_access, &state, 1, 0) != UC_ERR_OK ||
        !run_stack_case(uc, 0x20d000, 0x0c0c0c0c) ||
        !run_stack_case(uc, 0x30d000, 0xa5a5a5a5)) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return state.reads == 4 && state.writes == 2 ? 0 : 1;
}
