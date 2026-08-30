#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <unicorn/unicorn.h>

#define API_FUZZ_BASE UINT64_C(0x1000000)
#define API_FUZZ_RECOVERY UINT64_C(0x2000000)
#define API_FUZZ_PAGE_SIZE 0x1000
#define API_FUZZ_PAGE_COUNT 8
#define API_FUZZ_MAX_INPUT_SIZE 4096
#define API_FUZZ_MAX_OPERATIONS 256
#define API_FUZZ_MAX_EMULATION_STEPS 64
#define API_FUZZ_MAX_X86_INSN_SIZE 15

typedef struct ApiFuzzInput {
    const uint8_t *data;
    size_t size;
    size_t offset;
} ApiFuzzInput;

typedef struct ApiFuzzState {
    uc_context *context;
    uint64_t nested_address;
    uint8_t code_action;
    uint8_t memory_action;
    size_t instruction_budget;
    bool context_valid;
    bool nested_active;
} ApiFuzzState;

typedef struct ApiFuzzNestedCheck {
    ApiFuzzState *state;
    uint64_t outer_address;
    uint64_t inner_address;
    unsigned int nested_count;
} ApiFuzzNestedCheck;

static uint8_t fuzz_read_u8(ApiFuzzInput *input)
{
    if (input->offset == input->size) {
        return 0;
    }
    return input->data[input->offset++];
}

static uint64_t fuzz_page(uint8_t index)
{
    return API_FUZZ_BASE +
           (uint64_t)(index % API_FUZZ_PAGE_COUNT) * API_FUZZ_PAGE_SIZE;
}

static uint32_t fuzz_permissions(uint8_t value)
{
    return value & UC_PROT_ALL;
}

static bool fuzz_is_x86_string_opcode(uint8_t opcode)
{
    return (opcode >= 0x6c && opcode <= 0x6f) ||
           (opcode >= 0xa4 && opcode <= 0xa7) ||
           (opcode >= 0xaa && opcode <= 0xaf);
}

static bool fuzz_is_rep_string(uc_engine *uc, uint64_t address, uint32_t size)
{
    uint8_t code[API_FUZZ_MAX_X86_INSN_SIZE];
    bool has_rep = false;
    unsigned int i;

    if (size == 0 || size > sizeof(code) ||
        uc_mem_read(uc, address, code, size) != UC_ERR_OK) {
        return false;
    }

    for (i = 0; i < size; i++) {
        switch (code[i]) {
        case 0xf2:
        case 0xf3:
            has_rep = true;
            continue;
        case 0x26:
        case 0x2e:
        case 0x36:
        case 0x3e:
        case 0x64:
        case 0x65:
        case 0x66:
        case 0x67:
            continue;
        default:
            if (code[i] >= 0x40 && code[i] <= 0x4f) {
                continue;
            }
            return has_rep && fuzz_is_x86_string_opcode(code[i]);
        }
    }
    return false;
}

static void fuzz_budget_hook(uc_engine *uc, uint64_t address, uint32_t size,
                             void *user_data)
{
    ApiFuzzState *state = user_data;
    uint64_t repeat_count;

    if (state->instruction_budget == 0) {
        (void)uc_emu_stop(uc);
        return;
    }
    state->instruction_budget--;

    if (fuzz_is_rep_string(uc, address, size) &&
        uc_reg_read(uc, UC_X86_REG_RCX, &repeat_count) == UC_ERR_OK &&
        repeat_count > API_FUZZ_MAX_EMULATION_STEPS) {
        repeat_count = API_FUZZ_MAX_EMULATION_STEPS;
        (void)uc_reg_write(uc, UC_X86_REG_RCX, &repeat_count);
    }
}

static uc_err fuzz_emu_start(uc_engine *uc, ApiFuzzState *state,
                             uint64_t begin, uint64_t until, size_t count)
{
    size_t previous_budget = state->instruction_budget;
    uc_err err;

    state->instruction_budget = count;
    err = uc_emu_start(uc, begin, until, 0, count);
    state->instruction_budget = previous_budget;
    return err;
}

static void fuzz_code_hook(uc_engine *uc, uint64_t address, uint32_t size,
                           void *user_data)
{
    static const uint8_t smc_code[] = {0x90, 0x90, 0xeb, 0xfc};
    ApiFuzzState *state = user_data;

    switch (state->code_action % 5) {
    case 0:
        (void)uc_emu_stop(uc);
        break;
    case 1:
        if (!state->nested_active) {
            state->nested_active = true;
            (void)fuzz_emu_start(uc, state, state->nested_address,
                                 state->nested_address + 2, 2);
            state->nested_active = false;
        }
        break;
    case 2:
        (void)uc_ctl(uc, UC_CTL_TB_FLUSH);
        break;
    case 3:
        if (state->context_valid) {
            (void)uc_context_restore(uc, state->context);
        }
        break;
    case 4:
        (void)uc_mem_write(uc, address, smc_code,
                           size < sizeof(smc_code) ? size : sizeof(smc_code));
        break;
    }
}

static void fuzz_memory_hook(uc_engine *uc, uc_mem_type type, uint64_t address,
                             int size, int64_t value, void *user_data)
{
    ApiFuzzState *state = user_data;
    uint64_t page = address & ~(uint64_t)(API_FUZZ_PAGE_SIZE - 1);

    switch (state->memory_action % 3) {
    case 0:
        (void)uc_mem_unmap(uc, page, API_FUZZ_PAGE_SIZE);
        break;
    case 1:
        (void)uc_mem_protect(uc, page, API_FUZZ_PAGE_SIZE, UC_PROT_READ);
        break;
    case 2:
        (void)uc_emu_stop(uc);
        break;
    }
}

static void fuzz_require_ok(uc_err err)
{
    if (err != UC_ERR_OK) {
        abort();
    }
}

static void fuzz_nested_budget_hook(uc_engine *uc, uint64_t address,
                                    uint32_t size, void *user_data)
{
    ApiFuzzNestedCheck *check = user_data;

    if (address != check->outer_address || check->nested_count != 0 ||
        size != 3 || check->state->instruction_budget != 1) {
        abort();
    }
    check->nested_count++;
    fuzz_require_ok(fuzz_emu_start(uc, check->state, check->inner_address,
                                   check->inner_address + 3, 1));
    if (check->state->instruction_budget != 1) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    static const uint8_t recovery_code[] = {0x90, 0x90};
    static const uint8_t outer_code[] = {0x48, 0xff, 0xc0, 0x48, 0xff, 0xc0};
    static const uint8_t inner_code[] = {0x48, 0xff, 0xc3};
    uint8_t read_buffer[32];
    ApiFuzzInput input = {data, size, 0};
    ApiFuzzState state = {0};
    ApiFuzzNestedCheck nested_check = {
        &state, API_FUZZ_RECOVERY, API_FUZZ_RECOVERY + 0x100, 0,
    };
    uc_hook code_hook = 0;
    uc_hook budget_hook = 0;
    uc_hook memory_hook = 0;
    uc_hook nested_hook = 0;
    uc_engine *uc = NULL;
    uc_context *initial_context = NULL;
    unsigned int operation;
    uint64_t rax = 0;
    uint64_t rbx = 0;
    uint64_t rip;

    if (size > API_FUZZ_MAX_INPUT_SIZE) {
        return 0;
    }

    fuzz_require_ok(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    fuzz_require_ok(
        uc_mem_map(uc, API_FUZZ_RECOVERY, API_FUZZ_PAGE_SIZE, UC_PROT_ALL));
    fuzz_require_ok(uc_mem_write(uc, API_FUZZ_RECOVERY, recovery_code,
                                 sizeof(recovery_code)));
    fuzz_require_ok(uc_context_alloc(uc, &initial_context));
    fuzz_require_ok(uc_context_save(uc, initial_context));
    fuzz_require_ok(uc_context_alloc(uc, &state.context));
    fuzz_require_ok(uc_context_save(uc, state.context));
    state.context_valid = true;
    state.nested_address = API_FUZZ_RECOVERY;
    fuzz_require_ok(uc_hook_add(uc, &budget_hook, UC_HOOK_CODE,
                                fuzz_budget_hook, &state, 1, 0));

    for (operation = 0;
         input.offset < input.size && operation < API_FUZZ_MAX_OPERATIONS;
         operation++) {
        uint8_t command = fuzz_read_u8(&input);
        uint64_t page = fuzz_page(fuzz_read_u8(&input));
        uint8_t argument = fuzz_read_u8(&input);

        switch (command % 13) {
        case 0:
            (void)uc_mem_map(uc, page, API_FUZZ_PAGE_SIZE,
                             fuzz_permissions(argument));
            break;
        case 1:
            (void)uc_mem_unmap(uc, page, API_FUZZ_PAGE_SIZE);
            break;
        case 2:
            (void)uc_mem_protect(uc, page, API_FUZZ_PAGE_SIZE,
                                 fuzz_permissions(argument));
            break;
        case 3: {
            size_t remaining = input.size - input.offset;
            size_t write_size = argument % (sizeof(read_buffer) + 1);

            if (write_size > remaining) {
                write_size = remaining;
            }
            (void)uc_mem_write(uc, page, input.data + input.offset, write_size);
            input.offset += write_size;
            break;
        }
        case 4:
            (void)uc_mem_read(uc, page, read_buffer,
                              argument % (sizeof(read_buffer) + 1));
            break;
        case 5:
            (void)fuzz_emu_start(uc, &state, page,
                                 page + API_FUZZ_PAGE_SIZE,
                                 1 + argument % API_FUZZ_MAX_EMULATION_STEPS);
            break;
        case 6:
            if (uc_context_save(uc, state.context) == UC_ERR_OK) {
                state.context_valid = true;
            }
            break;
        case 7:
            if (state.context_valid) {
                (void)uc_context_restore(uc, state.context);
            }
            break;
        case 8:
            state.code_action = argument;
            if (code_hook == 0) {
                (void)uc_hook_add(uc, &code_hook, UC_HOOK_CODE, fuzz_code_hook,
                                  &state, API_FUZZ_BASE, API_FUZZ_RECOVERY - 1);
            }
            break;
        case 9:
            if (code_hook != 0) {
                (void)uc_hook_del(uc, code_hook);
                code_hook = 0;
            }
            break;
        case 10:
            state.memory_action = argument;
            if (memory_hook == 0) {
                (void)uc_hook_add(uc, &memory_hook,
                                  UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
                                  fuzz_memory_hook, &state, API_FUZZ_BASE,
                                  API_FUZZ_RECOVERY - 1);
            }
            break;
        case 11:
            if (memory_hook != 0) {
                (void)uc_hook_del(uc, memory_hook);
                memory_hook = 0;
            }
            break;
        case 12: {
            uc_mem_region *regions = NULL;
            uint32_t count = 0;

            if (uc_mem_regions(uc, &regions, &count) == UC_ERR_OK) {
                uc_free(regions);
            }
            break;
        }
        }
    }

    if (code_hook != 0) {
        fuzz_require_ok(uc_hook_del(uc, code_hook));
    }
    if (memory_hook != 0) {
        fuzz_require_ok(uc_hook_del(uc, memory_hook));
    }
    fuzz_require_ok(uc_context_restore(uc, initial_context));
    fuzz_require_ok(
        uc_mem_protect(uc, API_FUZZ_RECOVERY, API_FUZZ_PAGE_SIZE, UC_PROT_ALL));
    fuzz_require_ok(uc_mem_write(uc, nested_check.outer_address, outer_code,
                                 sizeof(outer_code)));
    fuzz_require_ok(uc_mem_write(uc, nested_check.inner_address, inner_code,
                                 sizeof(inner_code)));
    fuzz_require_ok(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    fuzz_require_ok(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));
    fuzz_require_ok(uc_hook_add(uc, &nested_hook, UC_HOOK_CODE,
                                fuzz_nested_budget_hook, &nested_check,
                                nested_check.outer_address,
                                nested_check.outer_address));
    fuzz_require_ok(fuzz_emu_start(
        uc, &state, nested_check.outer_address,
        nested_check.outer_address + sizeof(outer_code), 2));
    fuzz_require_ok(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    fuzz_require_ok(uc_reg_read(uc, UC_X86_REG_RBX, &rbx));
    fuzz_require_ok(uc_reg_read(uc, UC_X86_REG_RIP, &rip));
    if (nested_check.nested_count != 1 || rax != 2 || rbx != 1 ||
        rip != nested_check.outer_address + sizeof(outer_code)) {
        abort();
    }
    fuzz_require_ok(uc_hook_del(uc, nested_hook));
    fuzz_require_ok(uc_hook_del(uc, budget_hook));
    fuzz_require_ok(uc_context_free(state.context));
    fuzz_require_ok(uc_context_free(initial_context));
    fuzz_require_ok(uc_close(uc));
    return 0;
}
