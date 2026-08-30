#include "unicorn_test.h"
#include "uc_priv.h"
#include "target/arm/cpu.h"

#include <string.h>

static const uint64_t m_profile_code_start = 0x1000;
static const uint64_t m_profile_code_len = 0x4000;

static void setup_m_profile(uc_engine **uc, uc_cpu_arm cpu,
                            const uint8_t *code, size_t code_size)
{
    OK(uc_open(UC_ARCH_ARM, UC_MODE_THUMB | UC_MODE_MCLASS, uc));
    OK(uc_ctl_set_cpu_model(*uc, cpu));
    OK(uc_mem_map(*uc, m_profile_code_start, m_profile_code_len,
                  UC_PROT_ALL));
    OK(uc_mem_write(*uc, m_profile_code_start, code, code_size));
}

static uint32_t run_tt_query(uc_engine *uc, uint32_t address, uint8_t op)
{
    uint8_t code[] = {
        0x41, 0xe8, 0x00, 0xf0, /* tt-family r0,r1 */
    };
    uint32_t result = 0;

    code[2] = op << 6;
    OK(uc_mem_write(uc, m_profile_code_start, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_ARM_REG_R0, &result));
    OK(uc_reg_write(uc, UC_ARM_REG_R1, &address));
    OK(uc_emu_start(uc, m_profile_code_start | 1,
                    m_profile_code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_ARM_REG_R0, &result));

    return result;
}

static void set_sau_region(CPUARMState *env, uint32_t region,
                           uint32_t base, uint32_t limit,
                           bool non_secure_callable)
{
    env->sau.rbar[region] = base & ~0x1fU;
    env->sau.rlar[region] = (limit & ~0x1fU) | 1U;
    if (non_secure_callable) {
        env->sau.rlar[region] |= 2U;
    }
}

static void test_arm_m33_sau_tt(void)
{
    const uint32_t tt_secure = 1U << 22;
    const uint32_t tt_sregion_valid = 1U << 17;
    const uint8_t tt = 0;
    const uint8_t tta = 2;
    const uint8_t nop[] = { 0x00, 0xbf };
    uc_engine *uc;
    ARMCPU *cpu;
    CPUARMState *env;
    uint32_t result;

    setup_m_profile(&uc, UC_CPU_ARM_CORTEX_M33, nop, sizeof(nop));
    cpu = ARM_CPU(uc->cpu);
    env = &cpu->env;
    if (!TEST_CHECK(cpu->sau_sregion >= 2)) {
        OK(uc_close(uc));
        return;
    }

    env->sau.ctrl = 0;
    memset(env->sau.rbar, 0,
           sizeof(*env->sau.rbar) * cpu->sau_sregion);
    memset(env->sau.rlar, 0,
           sizeof(*env->sau.rlar) * cpu->sau_sregion);
    result = run_tt_query(uc, 0x2000, tt);
    TEST_CHECK((result & tt_secure) != 0);
    TEST_CHECK((result & tt_sregion_valid) == 0);

    env->sau.ctrl = 2;
    result = run_tt_query(uc, 0x2000, tt);
    TEST_CHECK((result & tt_secure) == 0);
    TEST_CHECK((result & tt_sregion_valid) == 0);

    env->sau.ctrl = 1;
    set_sau_region(env, 0, 0x2000, 0x2fff, false);
    result = run_tt_query(uc, 0x2000, tt);
    TEST_CHECK((result & tt_secure) == 0);
    TEST_CHECK((result & tt_sregion_valid) != 0);
    TEST_CHECK(((result >> 8) & 0xff) == 0);

    set_sau_region(env, 1, 0x3000, 0x3fff, true);
    result = run_tt_query(uc, 0x3000, tt);
    TEST_CHECK((result & tt_secure) != 0);
    TEST_CHECK((result & tt_sregion_valid) != 0);
    TEST_CHECK(((result >> 8) & 0xff) == 1);

    set_sau_region(env, 1, 0x2000, 0x2fff, true);
    result = run_tt_query(uc, 0x2000, tt);
    TEST_CHECK((result & tt_secure) != 0);
    TEST_CHECK((result & tt_sregion_valid) == 0);

    result = run_tt_query(uc, 0xe000e010, tt);
    TEST_CHECK((result & tt_secure) != 0);
    result = run_tt_query(uc, 0xe000e010, tta);
    TEST_CHECK((result & tt_secure) == 0);

    OK(uc_close(uc));
}

static void test_arm_m55_privileged_pxn(void)
{
    const uint8_t code[] = {
        0x2a, 0x20, /* movs r0,#42 */
    };
    uc_engine *uc;
    ARMCPU *cpu;
    CPUARMState *env;
    uint32_t control = 0;
    uint32_t r0 = 0;
    uc_err err;

    setup_m_profile(&uc, UC_CPU_ARM_CORTEX_M55, code, sizeof(code));
    OK(uc_reg_write(uc, UC_ARM_REG_CONTROL, &control));
    cpu = ARM_CPU(uc->cpu);
    env = &cpu->env;
    if (!TEST_CHECK(cpu->pmsav7_dregion >= 1)) {
        OK(uc_close(uc));
        return;
    }

    memset(env->pmsav8.rbar[M_REG_S], 0,
           sizeof(*env->pmsav8.rbar[M_REG_S]) * cpu->pmsav7_dregion);
    memset(env->pmsav8.rlar[M_REG_S], 0,
           sizeof(*env->pmsav8.rlar[M_REG_S]) * cpu->pmsav7_dregion);
    env->v7m.mpu_ctrl[M_REG_S] = R_V7M_MPU_CTRL_ENABLE_MASK;
    env->pmsav8.rbar[M_REG_S][0] =
        (uint32_t)m_profile_code_start | (1U << 1);
    env->pmsav8.rlar[M_REG_S][0] =
        ((uint32_t)(m_profile_code_start + m_profile_code_len - 1) &
         ~0x1fU) |
        (1U << 4) | 1U;

    err = uc_emu_start(uc, m_profile_code_start | 1,
                       m_profile_code_start + sizeof(code), 0, 0);
    TEST_CHECK_(err == UC_ERR_EXCEPTION, "err=%u", (unsigned)err);
    OK(uc_reg_read(uc, UC_ARM_REG_R0, &r0));
    TEST_CHECK_(r0 == 0, "r0=0x%08x", r0);

    OK(uc_close(uc));
}

TEST_LIST = {
    {"test_arm_m33_sau_tt", test_arm_m33_sau_tt},
    {"test_arm_m55_privileged_pxn", test_arm_m55_privileged_pxn},
    {NULL, NULL}};
