#include <unicorn/unicorn.h>

#define ADDRESS 0x100000
#define DELAY_SLOT_ADDRESS (ADDRESS + 12)

static unsigned int delay_slot_hook_count;
static unsigned int loop_count;

static void count_instructions(uc_engine *uc, uint64_t address, uint32_t size,
                               void *user_data)
{
    (void)uc;
    (void)size;
    (void)user_data;
    if (address == ADDRESS + 4) {
        loop_count++;
    } else if (address == DELAY_SLOT_ADDRESS) {
        delay_slot_hook_count++;
    }
}

int main(void)
{
    const uint8_t code[] = {
        0x02, 0x00, 0x04, 0x24, /* li $a0, 2 */
        0x00, 0x00, 0x00, 0x00, /* nop */
        0xfe, 0xff, 0x80, 0x14, /* bnez $a0, -2 */
        0xff, 0xff, 0x84, 0x24, /* addiu $a0, -1 */
    };
    uint32_t a0 = 0;
    uc_engine *uc;
    uc_hook hook;
    uc_err err;

    err = uc_open(UC_ARCH_MIPS, UC_MODE_MIPS32, &uc);
    if (err != UC_ERR_OK) {
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, code, sizeof(code)) != UC_ERR_OK ||
        uc_hook_add(uc, &hook, UC_HOOK_CODE, count_instructions, NULL, 1, 0) !=
            UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    err = uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code), 0, 0);
    if (err != UC_ERR_OK || uc_reg_read(uc, UC_MIPS_REG_A0, &a0) != UC_ERR_OK) {
        uc_close(uc);
        return 1;
    }

    if (uc_close(uc) != UC_ERR_OK) {
        return 1;
    }
    return a0 == UINT32_MAX && loop_count == 3 && delay_slot_hook_count == 3
               ? 0
               : 1;
}
