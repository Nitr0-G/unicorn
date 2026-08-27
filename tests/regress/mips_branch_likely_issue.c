#include <unicorn/unicorn.h>

#define ADDRESS 0x100000
#define DELAY_SLOT_ADDRESS (ADDRESS + 16)

typedef struct HookState {
    unsigned int test_number;
    unsigned int delay_slot_count[2];
} HookState;

static void count_delay_slot(uc_engine *uc, uint64_t address, uint32_t size,
                             void *user_data)
{
    HookState *state = user_data;

    (void)uc;
    (void)size;
    if (address == DELAY_SLOT_ADDRESS && state->test_number < 2) {
        state->delay_slot_count[state->test_number]++;
    }
}

static bool run_case(uc_engine *uc, HookState *state, const uint8_t *code,
                     size_t code_size, uint32_t expected_a0)
{
    uint32_t a0 = UINT32_MAX;

    if (uc_mem_write(uc, ADDRESS, code, code_size) != UC_ERR_OK ||
        uc_emu_start(uc, ADDRESS, ADDRESS + code_size, 0, 0) != UC_ERR_OK ||
        uc_reg_read(uc, UC_MIPS_REG_A0, &a0) != UC_ERR_OK) {
        return false;
    }
    return a0 == expected_a0;
}

int main(void)
{
    const uint8_t taken[] = {
        0x00, 0x00, 0x04, 0x24, /* li $a0, 0 */
        0x01, 0x00, 0x02, 0x24, /* li $v0, 1 */
        0x02, 0x00, 0x03, 0x24, /* li $v1, 2 */
        0x01, 0x00, 0x62, 0x54, /* bnel $v1, $v0, +1 */
        0x21, 0x20, 0x62, 0x00, /* addu $a0, $v1, $v0 */
    };
    const uint8_t not_taken[] = {
        0x00, 0x00, 0x04, 0x24, /* li $a0, 0 */
        0x01, 0x00, 0x02, 0x24, /* li $v0, 1 */
        0x01, 0x00, 0x03, 0x24, /* li $v1, 1 */
        0x01, 0x00, 0x62, 0x54, /* bnel $v1, $v0, +1 */
        0x21, 0x20, 0x62, 0x00, /* addu $a0, $v1, $v0 */
    };
    HookState state = {0};
    uc_engine *uc;
    uc_hook hook;

    if (uc_open(UC_ARCH_MIPS, UC_MODE_MIPS32, &uc) != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_CODE, count_delay_slot, &state, 1, 0) !=
            UC_ERR_OK ||
        !run_case(uc, &state, taken, sizeof(taken), 3)) {
        uc_close(uc);
        return 1;
    }

    state.test_number = 1;
    if (!run_case(uc, &state, not_taken, sizeof(not_taken), 0)) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return state.delay_slot_count[0] == 1 && state.delay_slot_count[1] == 0 ? 0
                                                                            : 1;
}
