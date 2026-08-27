#include <unicorn/unicorn.h>

#include <stdio.h>

#define ADDRESS 0x10000
#define TEST_ITERATIONS 32
#define TEST_TIMEOUT (UC_SECOND_SCALE * 5)

static bool check_error(const char *operation, uc_err error)
{
    if (error == UC_ERR_OK) {
        return true;
    }
    fprintf(stderr, "%s failed with %u: %s\n", operation, (unsigned)error,
            uc_strerror(error));
    return false;
}

static bool run_arm(unsigned int iteration)
{
    const uint8_t code[] = {
        0x37, 0x00, 0xa0, 0xe3, 0x03, 0x10, 0x42, 0xe0,
    };
    uint32_t r0 = 0;
    uint32_t r1 = 0;
    uint32_t r2 = 0x6789;
    uint32_t r3 = 0x3333;
    size_t timed_out = 0;
    uc_engine *uc;

    if (!check_error("ARM uc_open", uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc))) {
        return false;
    }
    if (!check_error("ARM uc_mem_map",
                     uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL)) ||
        !check_error("ARM uc_mem_write",
                     uc_mem_write(uc, ADDRESS, code, sizeof(code))) ||
        !check_error("ARM uc_reg_write(R2)",
                     uc_reg_write(uc, UC_ARM_REG_R2, &r2)) ||
        !check_error("ARM uc_reg_write(R3)",
                     uc_reg_write(uc, UC_ARM_REG_R3, &r3)) ||
        !check_error("ARM uc_emu_start",
                     uc_emu_start(uc, ADDRESS, ADDRESS + sizeof(code),
                                  TEST_TIMEOUT, 0)) ||
        !check_error("ARM uc_query(UC_QUERY_TIMEOUT)",
                     uc_query(uc, UC_QUERY_TIMEOUT, &timed_out)) ||
        !check_error("ARM uc_reg_read(R0)",
                     uc_reg_read(uc, UC_ARM_REG_R0, &r0)) ||
        !check_error("ARM uc_reg_read(R1)",
                     uc_reg_read(uc, UC_ARM_REG_R1, &r1))) {
        uc_close(uc);
        return false;
    }

    if (timed_out != 0) {
        fprintf(stderr, "ARM iteration %u timed out\n", iteration + 1);
        uc_close(uc);
        return false;
    }
    if (r0 != 0x37 || r1 != r2 - r3) {
        fprintf(stderr,
                "unexpected ARM result at iteration %u: R0=0x%x R1=0x%x\n",
                iteration + 1, r0, r1);
        uc_close(uc);
        return false;
    }
    return check_error("ARM uc_close", uc_close(uc));
}

static bool run_thumb(unsigned int iteration)
{
    const uint8_t code[] = {0x83, 0xb0};
    uint32_t sp = 0x1234;
    size_t timed_out = 0;
    uc_engine *uc;

    if (!check_error("Thumb uc_open",
                     uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc))) {
        return false;
    }
    if (!check_error("Thumb uc_mem_map",
                     uc_mem_map(uc, ADDRESS, 0x1000, UC_PROT_ALL)) ||
        !check_error("Thumb uc_mem_write",
                     uc_mem_write(uc, ADDRESS, code, sizeof(code))) ||
        !check_error("Thumb uc_reg_write(SP)",
                     uc_reg_write(uc, UC_ARM_REG_SP, &sp)) ||
        !check_error("Thumb uc_emu_start",
                     uc_emu_start(uc, ADDRESS | 1, 0, TEST_TIMEOUT, 1)) ||
        !check_error("Thumb uc_query(UC_QUERY_TIMEOUT)",
                     uc_query(uc, UC_QUERY_TIMEOUT, &timed_out)) ||
        !check_error("Thumb uc_reg_read(SP)",
                     uc_reg_read(uc, UC_ARM_REG_SP, &sp))) {
        uc_close(uc);
        return false;
    }

    if (timed_out != 0) {
        fprintf(stderr, "Thumb iteration %u timed out\n", iteration + 1);
        uc_close(uc);
        return false;
    }
    if (sp != 0x1228) {
        fprintf(stderr, "unexpected Thumb result at iteration %u: SP=0x%x\n",
                iteration + 1, sp);
        uc_close(uc);
        return false;
    }
    return check_error("Thumb uc_close", uc_close(uc));
}

int main(void)
{
    unsigned int i;

    for (i = 0; i < TEST_ITERATIONS; i++) {
        if (!run_arm(i) || !run_thumb(i)) {
            return 1;
        }
    }
    return 0;
}
