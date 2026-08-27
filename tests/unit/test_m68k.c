#include "unicorn_test.h"

#include <string.h>

const uint64_t code_start = 0x1000;
const uint64_t code_len = 0x4000;

static void uc_common_setup(uc_engine **uc, uc_arch arch, uc_mode mode,
                            const char *code, uint64_t size,
                            uc_cpu_m68k cpu_model)
{
    OK(uc_open(arch, mode, uc));
    OK(uc_ctl_set_cpu_model(*uc, cpu_model));
    OK(uc_mem_map(*uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(*uc, code_start, code, size));
}

static void setup_stack_pointers(uc_engine *uc, uint32_t ssp,
                                 uint32_t usp, uint32_t isp)
{
    uint32_t sr = 0;

    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_write(uc, UC_M68K_REG_A7, &usp));
    OK(uc_reg_write(uc, UC_M68K_REG_CR_MSP, &ssp));
    OK(uc_reg_write(uc, UC_M68K_REG_CR_ISP, &isp));
}

static void test_move_to_sr(void)
{

    uc_engine *uc;
    char code[] = "\x46\xfc\x27\x00"; // move    #$2700,sr
    int r_sr;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, code,
                    sizeof(code) - 1, UC_CPU_M68K_M68000);
    OK(uc_reg_read(uc, UC_M68K_REG_SR, &r_sr));

    r_sr = r_sr | 0x2000;

    OK(uc_reg_write(uc, UC_M68K_REG_SR, &r_sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_M68K_REG_SR, &r_sr));

    TEST_CHECK(r_sr == 0x2700);

    OK(uc_close(uc));
}

static void test_sr_contains_flags(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x76, 0xed, // moveq #-19, %d3
    };

    uint32_t d3, sr;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, code, sizeof(code),
                    UC_CPU_M68K_M68000);

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));

    OK(uc_reg_read(uc, UC_M68K_REG_D3, &d3));
    OK(uc_reg_read(uc, UC_M68K_REG_SR, &sr));

    TEST_CHECK(d3 == 0xFFFFFFED);
    TEST_CHECK((sr & 0x8) /* N flag */ == 0x8);

    OK(uc_close(uc));
}

static void test_fetoxm1(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0xf2, 0x10, 0x54, 0x00, /* fmove.d (a0), fp0 */
        0xf2, 0x00, 0x00, 0x08, /* fetoxm1 fp0, fp0 */
        0xf2, 0x10, 0x74, 0x00, /* fmove.d fp0, (a0) */
    };
    uint8_t input[] = {
        0x3f, 0xf0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };
    uint8_t expected[] = {
        0x3f, 0xfb, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd3,
    };
    uint8_t result[sizeof(expected)];
    uint32_t a0 = code_start + 0x100;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);
    OK(uc_mem_write(uc, a0, input, sizeof(input)));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, a0, result, sizeof(result)));

    TEST_CHECK(memcmp(result, expected, sizeof(expected)) == 0);

    OK(uc_close(uc));
}

static void test_coldfire_macsr_to_ccr(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0xa9, 0x00, /* move.l d0, MACSR */
        0xa9, 0xc0, /* MACSR -> CCR */
    };
    uint32_t d0 = 0x0f;
    uint32_t sr = 0x2700;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_CFV4E);
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_SR, &sr));

    TEST_CHECK(sr == 0x270e);

    OK(uc_close(uc));
}

static void test_ftrapcc_false_consumes_immediate(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0xf2, 0x7a, 0x00, 0x00, 0x12, 0x34, /* ftrapf.w #$1234 */
        0x70, 0x2a,                         /* moveq #42, d0 */
    };
    uint32_t d0;
    uint32_t pc;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_PC, &pc));

    TEST_CHECK(d0 == 42);
    TEST_CHECK(pc == (uint32_t)(code_start + sizeof(code)));

    OK(uc_close(uc));
}

static void test_ftrapcc_true_raises(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0xf2, 0x7c, 0x00, 0x0f, /* ftrapt */
        0x70, 0x2a,             /* moveq #42, d0 */
    };
    uint32_t d0 = 0;
    uint32_t pc;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);

    uc_assert_err(UC_ERR_EXCEPTION,
                  uc_emu_start(uc, code_start, code_start + sizeof(code),
                               0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_PC, &pc));

    TEST_CHECK(d0 == 0);
    TEST_CHECK(pc == (uint32_t)(code_start + 4));

    OK(uc_close(uc));
}

static void test_trapcc_false_consumes_immediate(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x51, 0xfa, 0x12, 0x34, /* trapf.w #$1234 */
        0x70, 0x2a,             /* moveq #42, d0 */
    };
    uint32_t d0;
    uint32_t pc;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_PC, &pc));

    TEST_CHECK(d0 == 42);
    TEST_CHECK(pc == (uint32_t)(code_start + sizeof(code)));

    OK(uc_close(uc));
}

static void test_trapcc_true_raises(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x50, 0xfc, /* trapt */
        0x70, 0x2a, /* moveq #42, d0 */
    };
    uint32_t d0 = 0;
    uint32_t pc;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);

    uc_assert_err(UC_ERR_EXCEPTION,
                  uc_emu_start(uc, code_start, code_start + sizeof(code),
                               0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_PC, &pc));

    TEST_CHECK(d0 == 0);
    TEST_CHECK(pc == (uint32_t)(code_start + 2));

    OK(uc_close(uc));
}

static void test_m68010_rtd(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x4e, 0x74, 0x00, 0x00, /* rtd #0 */
    };
    uint8_t retaddr[] = {
        0x00, 0x00, 0x10, 0x04,
    };
    uint32_t sp = code_start + 0x100;
    uint32_t pc;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68010);
    OK(uc_mem_write(uc, sp, retaddr, sizeof(retaddr)));
    OK(uc_reg_write(uc, UC_M68K_REG_A7, &sp));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_PC, &pc));

    TEST_CHECK(pc == (uint32_t)(code_start + sizeof(code)));

    OK(uc_close(uc));
}

static void test_m68010_supervisor_uses_ssp(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x4e, 0x71, /* nop */
    };
    uint32_t ssp = code_start + 0x300;
    uint32_t usp = code_start + 0x500;
    uint32_t isp = code_start + 0x700;
    uint32_t sr;
    uint32_t a7;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68010);
    setup_stack_pointers(uc, ssp, usp, isp);

    OK(uc_reg_read(uc, UC_M68K_REG_A7, &a7));
    TEST_CHECK(a7 == usp);

    sr = 0x2000;
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_read(uc, UC_M68K_REG_A7, &a7));
    TEST_CHECK(a7 == ssp);

    sr = 0x3000;
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_read(uc, UC_M68K_REG_A7, &a7));
    TEST_CHECK(a7 == ssp);

    OK(uc_close(uc));
}

static void test_m68020_supervisor_uses_isp_and_msp(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x4e, 0x71, /* nop */
    };
    uint32_t ssp = code_start + 0x300;
    uint32_t usp = code_start + 0x500;
    uint32_t isp = code_start + 0x700;
    uint32_t sr;
    uint32_t a7;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);
    setup_stack_pointers(uc, ssp, usp, isp);

    sr = 0x2000;
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_read(uc, UC_M68K_REG_A7, &a7));
    TEST_CHECK(a7 == isp);

    sr = 0x3000;
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_read(uc, UC_M68K_REG_A7, &a7));
    TEST_CHECK(a7 == ssp);

    sr = 0;
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_read(uc, UC_M68K_REG_A7, &a7));
    TEST_CHECK(a7 == usp);

    OK(uc_close(uc));
}

static void test_m68060_supervisor_uses_isp_and_msp(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x4e, 0x71, /* nop */
    };
    uint32_t ssp = code_start + 0x300;
    uint32_t usp = code_start + 0x500;
    uint32_t isp = code_start + 0x700;
    uint32_t sr;
    uint32_t a7;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68060);
    setup_stack_pointers(uc, ssp, usp, isp);

    sr = 0x2000;
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_read(uc, UC_M68K_REG_A7, &a7));
    TEST_CHECK(a7 == isp);

    sr = 0x3000;
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_read(uc, UC_M68K_REG_A7, &a7));
    TEST_CHECK(a7 == ssp);

    sr = 0;
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_read(uc, UC_M68K_REG_A7, &a7));
    TEST_CHECK(a7 == usp);

    OK(uc_close(uc));
}

static void test_m68020_movec_msp_isp(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x4e, 0x7b, 0x08, 0x03, /* movec d0, msp */
        0x4e, 0x7b, 0x18, 0x04, /* movec d1, isp */
        0x4e, 0x7a, 0x28, 0x03, /* movec msp, d2 */
        0x4e, 0x7a, 0x38, 0x04, /* movec isp, d3 */
    };
    uint32_t sr = 0x2000;
    uint32_t d0 = code_start + 0x300;
    uint32_t d1 = code_start + 0x700;
    uint32_t d2 = 0;
    uint32_t d3 = 0;
    uint32_t msp = 0;
    uint32_t isp = 0;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_D1, &d1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_CR_MSP, &msp));
    OK(uc_reg_read(uc, UC_M68K_REG_CR_ISP, &isp));
    OK(uc_reg_read(uc, UC_M68K_REG_D2, &d2));
    OK(uc_reg_read(uc, UC_M68K_REG_D3, &d3));

    TEST_CHECK(msp == d0);
    TEST_CHECK(isp == d1);
    TEST_CHECK(d2 == d0);
    TEST_CHECK(d3 == d1);

    OK(uc_close(uc));
}

static void test_m68010_movec_msp_invalid(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x4e, 0x7b, 0x08, 0x03, /* movec d0, msp */
    };
    uint32_t sr = 0x2000;
    uint32_t d0 = code_start + 0x300;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68010);
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));

    uc_assert_err(UC_ERR_EXCEPTION,
                  uc_emu_start(uc, code_start, code_start + sizeof(code),
                               0, 0));

    OK(uc_close(uc));
}

static void test_m68060_movec_msp_invalid(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x4e, 0x7a, 0x08, 0x03, /* movec msp, d0 */
    };
    uint32_t sr = 0x2000;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68060);
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    uc_assert_err(UC_ERR_EXCEPTION,
                  uc_emu_start(uc, code_start, code_start + sizeof(code),
                               0, 0));

    OK(uc_close(uc));
}

static void test_rtr(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x4e, 0x77, /* rtr */
    };
    uint8_t frame[] = {
        0x00, 0x15,             /* ccr: X, Z, C */
        0x00, 0x00, 0x10, 0x02, /* pc */
    };
    uint32_t sp = code_start + 0x100;
    uint32_t sr = 0x2700;
    uint32_t pc;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68000);
    OK(uc_mem_write(uc, sp, frame, sizeof(frame)));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_write(uc, UC_M68K_REG_A7, &sp));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_PC, &pc));
    OK(uc_reg_read(uc, UC_M68K_REG_A7, &sp));
    OK(uc_reg_read(uc, UC_M68K_REG_SR, &sr));

    TEST_CHECK(pc == (uint32_t)(code_start + sizeof(code)));
    TEST_CHECK(sp == (uint32_t)(code_start + 0x100 + sizeof(frame)));
    TEST_CHECK(sr == 0x2715);

    OK(uc_close(uc));
}

static void test_m68010_move_from_sr_privileged(void)
{
    uc_engine *uc;
    uint8_t code[] = {
        0x40, 0xc0, /* move.w sr, d0 */
    };
    uint32_t sr = 0;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (char *)code,
                    sizeof(code), UC_CPU_M68K_M68010);
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    uc_assert_err(UC_ERR_EXCEPTION,
                  uc_emu_start(uc, code_start, code_start + sizeof(code),
                               0, 0));

    OK(uc_close(uc));
}

static void test_m68020_cas_word(void)
{
    const uint8_t code[] = {
        0x0c, 0xd0, 0x00, 0x40, /* cas.w d0, d1, (a0) */
        0x40, 0xc4,             /* move.w sr, d4 */
    };
    const uint8_t compare[] = { 0x12, 0x34 };
    const uint8_t update[] = { 0x56, 0x78 };
    const uint8_t mismatch[] = { 0x20, 0x02 };
    uint8_t result[sizeof(compare)];
    uc_engine *uc;
    uint32_t a0 = code_start + 0x1000;
    uint32_t d0 = 0xaaaa1234;
    uint32_t d1 = 0xbbbb5678;
    uint32_t d4;
    uint32_t pc = code_start;
    uint32_t sr = 0x2710;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN,
                    (const char *)code, sizeof(code),
                    UC_CPU_M68K_M68020);
    OK(uc_mem_write(uc, a0, compare, sizeof(compare)));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, a0, result, sizeof(result)));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D4, &d4));
    TEST_CHECK(memcmp(result, update, sizeof(result)) == 0);
    TEST_CHECK(d0 == 0xaaaa1234);
    TEST_CHECK((d4 & 0x1f) == 0x14);

    d0 = 0xaaaa1001;
    sr = 0x2710;
    OK(uc_mem_write(uc, a0, mismatch, sizeof(mismatch)));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_PC, &pc));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, a0, result, sizeof(result)));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_M68K_REG_D4, &d4));
    TEST_CHECK(memcmp(result, mismatch, sizeof(result)) == 0);
    TEST_CHECK(d0 == 0xaaaa2002);
    TEST_CHECK(d1 == 0xbbbb5678);
    TEST_CHECK((d4 & 0x1f) == 0x10);

    OK(uc_close(uc));
}

static void test_m68020_cas_long(void)
{
    const uint8_t code[] = {
        0x0e, 0xd0, 0x00, 0x40, /* cas.l d0, d1, (a0) */
        0x40, 0xc4,             /* move.w sr, d4 */
    };
    const uint8_t compare[] = { 0x12, 0x34, 0x56, 0x78 };
    const uint8_t update[] = { 0x9a, 0xbc, 0xde, 0xf0 };
    const uint8_t mismatch[] = { 0x30, 0x00, 0x30, 0x00 };
    uint8_t result[sizeof(compare)];
    uc_engine *uc;
    uint32_t a0 = code_start + 0x1000;
    uint32_t d0 = 0x12345678;
    uint32_t d1 = 0x9abcdef0;
    uint32_t d4;
    uint32_t pc = code_start;
    uint32_t sr = 0x2710;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN,
                    (const char *)code, sizeof(code),
                    UC_CPU_M68K_M68020);
    OK(uc_mem_write(uc, a0, compare, sizeof(compare)));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, a0, result, sizeof(result)));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D4, &d4));
    TEST_CHECK(memcmp(result, update, sizeof(result)) == 0);
    TEST_CHECK(d0 == 0x12345678);
    TEST_CHECK((d4 & 0x1f) == 0x14);

    d0 = 0x20002000;
    sr = 0x2710;
    OK(uc_mem_write(uc, a0, mismatch, sizeof(mismatch)));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_PC, &pc));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, a0, result, sizeof(result)));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_M68K_REG_D4, &d4));
    TEST_CHECK(memcmp(result, mismatch, sizeof(result)) == 0);
    TEST_CHECK(d0 == 0x30003000);
    TEST_CHECK(d1 == 0x9abcdef0);
    TEST_CHECK((d4 & 0x1f) == 0x10);

    OK(uc_close(uc));
}

static void test_m68020_cas2_word(void)
{
    const uint8_t code[] = {
        0x0c, 0xfc, 0x80, 0x80, 0x90, 0xc1,
        /* cas2.w d0:d1, d2:d3, (a0):(a1) */
        0x40, 0xc4, /* move.w sr, d4 */
    };
    const uint8_t compare1[] = { 0x12, 0x34 };
    const uint8_t compare2[] = { 0x6a, 0xbc };
    const uint8_t update1[] = { 0x56, 0x78 };
    const uint8_t update2[] = { 0x2e, 0xf0 };
    const uint8_t mismatch1[] = { 0x30, 0x03 };
    const uint8_t mismatch2[] = { 0x40, 0x04 };
    uint8_t result1[sizeof(compare1)];
    uint8_t result2[sizeof(compare2)];
    uc_engine *uc;
    uint32_t a0 = code_start + 0x1000;
    uint32_t a1 = code_start + 0x1100;
    uint32_t d0 = 0xaaaa1234;
    uint32_t d1 = 0xbbbb6abc;
    uint32_t d2 = 0xcccc5678;
    uint32_t d3 = 0xdddd2ef0;
    uint32_t d4;
    uint32_t pc = code_start;
    uint32_t sr = 0x2710;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN,
                    (const char *)code, sizeof(code),
                    UC_CPU_M68K_M68020);
    OK(uc_mem_write(uc, a0, compare1, sizeof(compare1)));
    OK(uc_mem_write(uc, a1, compare2, sizeof(compare2)));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));
    OK(uc_reg_write(uc, UC_M68K_REG_A1, &a1));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_write(uc, UC_M68K_REG_D2, &d2));
    OK(uc_reg_write(uc, UC_M68K_REG_D3, &d3));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, a0, result1, sizeof(result1)));
    OK(uc_mem_read(uc, a1, result2, sizeof(result2)));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_M68K_REG_D4, &d4));
    TEST_CHECK(memcmp(result1, update1, sizeof(result1)) == 0);
    TEST_CHECK(memcmp(result2, update2, sizeof(result2)) == 0);
    TEST_CHECK(d0 == 0xaaaa1234);
    TEST_CHECK(d1 == 0xbbbb6abc);
    TEST_CHECK((d4 & 0x1f) == 0x14);

    d0 = 0xaaaa2002;
    d1 = 0xbbbb1001;
    sr = 0x2710;
    OK(uc_mem_write(uc, a0, mismatch1, sizeof(mismatch1)));
    OK(uc_mem_write(uc, a1, mismatch2, sizeof(mismatch2)));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_write(uc, UC_M68K_REG_PC, &pc));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, a0, result1, sizeof(result1)));
    OK(uc_mem_read(uc, a1, result2, sizeof(result2)));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_M68K_REG_D4, &d4));
    TEST_CHECK(memcmp(result1, mismatch1, sizeof(result1)) == 0);
    TEST_CHECK(memcmp(result2, mismatch2, sizeof(result2)) == 0);
    TEST_CHECK(d0 == 0xaaaa3003);
    TEST_CHECK(d1 == 0xbbbb4004);
    TEST_CHECK((d4 & 0x1f) == 0x10);

    OK(uc_close(uc));
}

static void test_m68020_cas2_long(void)
{
    const uint8_t code[] = {
        0x0e, 0xfc, 0x80, 0x80, 0x90, 0xc1,
        /* cas2.l d0:d1, d2:d3, (a0):(a1) */
        0x40, 0xc4, /* move.w sr, d4 */
    };
    const uint8_t compare1[] = { 0x12, 0x34, 0x56, 0x78 };
    const uint8_t compare2[] = { 0x1a, 0xbc, 0xde, 0xf0 };
    const uint8_t update1[] = { 0x20, 0x00, 0x20, 0x00 };
    const uint8_t update2[] = { 0x21, 0x00, 0x21, 0x00 };
    const uint8_t mismatch1[] = { 0x30, 0x00, 0x30, 0x00 };
    const uint8_t mismatch2[] = { 0x40, 0x00, 0x40, 0x00 };
    uint8_t result1[sizeof(compare1)];
    uint8_t result2[sizeof(compare2)];
    uc_engine *uc;
    uint32_t a0 = code_start + 0x1000;
    uint32_t a1 = code_start + 0x1100;
    uint32_t d0 = 0x12345678;
    uint32_t d1 = 0x1abcdef0;
    uint32_t d2 = 0x20002000;
    uint32_t d3 = 0x21002100;
    uint32_t d4;
    uint32_t pc = code_start;
    uint32_t sr = 0x2710;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN,
                    (const char *)code, sizeof(code),
                    UC_CPU_M68K_M68020);
    OK(uc_mem_write(uc, a0, compare1, sizeof(compare1)));
    OK(uc_mem_write(uc, a1, compare2, sizeof(compare2)));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));
    OK(uc_reg_write(uc, UC_M68K_REG_A1, &a1));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_write(uc, UC_M68K_REG_D2, &d2));
    OK(uc_reg_write(uc, UC_M68K_REG_D3, &d3));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, a0, result1, sizeof(result1)));
    OK(uc_mem_read(uc, a1, result2, sizeof(result2)));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_M68K_REG_D4, &d4));
    TEST_CHECK(memcmp(result1, update1, sizeof(result1)) == 0);
    TEST_CHECK(memcmp(result2, update2, sizeof(result2)) == 0);
    TEST_CHECK(d0 == 0x12345678);
    TEST_CHECK(d1 == 0x1abcdef0);
    TEST_CHECK((d4 & 0x1f) == 0x14);

    d0 = 0x20002000;
    d1 = 0x10001000;
    sr = 0x2710;
    OK(uc_mem_write(uc, a0, mismatch1, sizeof(mismatch1)));
    OK(uc_mem_write(uc, a1, mismatch2, sizeof(mismatch2)));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_write(uc, UC_M68K_REG_PC, &pc));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, a0, result1, sizeof(result1)));
    OK(uc_mem_read(uc, a1, result2, sizeof(result2)));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_M68K_REG_D4, &d4));
    TEST_CHECK(memcmp(result1, mismatch1, sizeof(result1)) == 0);
    TEST_CHECK(memcmp(result2, mismatch2, sizeof(result2)) == 0);
    TEST_CHECK(d0 == 0x30003000);
    TEST_CHECK(d1 == 0x40004000);
    TEST_CHECK((d4 & 0x1f) == 0x10);

    OK(uc_close(uc));
}

static void test_m68k_fast_count_boundary(void)
{
    const uint8_t code[] = {
        0x70, 0x11, /* moveq #0x11, d0 */
        0x72, 0x22, /* moveq #0x22, d1 */
        0x74, 0x33, /* moveq #0x33, d2 */
    };
    uc_engine *uc;
    uint32_t d0 = 0;
    uint32_t d1 = 0;
    uint32_t d2 = 0;
    uint32_t pc = 0;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN,
                    (const char *)code, sizeof(code),
                    UC_CPU_M68K_M68000);

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 1));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_M68K_REG_D2, &d2));
    OK(uc_reg_read(uc, UC_M68K_REG_PC, &pc));
    TEST_CHECK_(d0 == 0x11, "d0 = 0x%08x", d0);
    TEST_CHECK_(d1 == 0, "d1 = 0x%08x", d1);
    TEST_CHECK_(d2 == 0, "d2 = 0x%08x", d2);
    TEST_CHECK_(pc == (uint32_t)(code_start + 2), "pc = 0x%08x", pc);

    d0 = 0;
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 2));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_M68K_REG_D2, &d2));
    OK(uc_reg_read(uc, UC_M68K_REG_PC, &pc));
    TEST_CHECK_(d0 == 0x11, "d0 = 0x%08x", d0);
    TEST_CHECK_(d1 == 0x22, "d1 = 0x%08x", d1);
    TEST_CHECK_(d2 == 0, "d2 = 0x%08x", d2);
    TEST_CHECK_(pc == (uint32_t)(code_start + 4), "pc = 0x%08x", pc);

    OK(uc_close(uc));
}

static void test_m68k_context_roundtrip(void)
{
    uc_engine *uc;
    uc_context *context;
    uint32_t d0 = 0x11223344;
    uint32_t pc = code_start + 0x20;
    uint32_t changed = 0;

    OK(uc_open(UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_M68K_REG_PC, &pc));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_reg_write(uc, UC_M68K_REG_D0, &changed));
    OK(uc_reg_write(uc, UC_M68K_REG_PC, &changed));
    OK(uc_context_restore(uc, context));

    OK(uc_reg_read(uc, UC_M68K_REG_D0, &changed));
    TEST_CHECK(changed == d0);
    OK(uc_reg_read(uc, UC_M68K_REG_PC, &changed));
    TEST_CHECK(changed == pc);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

TEST_LIST = {{"test_move_to_sr", test_move_to_sr},
             {"test_sr_contains_flags", test_sr_contains_flags},
             {"test_fetoxm1", test_fetoxm1},
             {"test_coldfire_macsr_to_ccr", test_coldfire_macsr_to_ccr},
             {"test_ftrapcc_false_consumes_immediate",
              test_ftrapcc_false_consumes_immediate},
             {"test_ftrapcc_true_raises", test_ftrapcc_true_raises},
             {"test_trapcc_false_consumes_immediate",
              test_trapcc_false_consumes_immediate},
             {"test_trapcc_true_raises", test_trapcc_true_raises},
             {"test_m68010_rtd", test_m68010_rtd},
             {"test_m68010_supervisor_uses_ssp",
              test_m68010_supervisor_uses_ssp},
             {"test_m68020_supervisor_uses_isp_and_msp",
              test_m68020_supervisor_uses_isp_and_msp},
             {"test_m68060_supervisor_uses_isp_and_msp",
              test_m68060_supervisor_uses_isp_and_msp},
             {"test_m68020_movec_msp_isp", test_m68020_movec_msp_isp},
             {"test_m68010_movec_msp_invalid",
              test_m68010_movec_msp_invalid},
             {"test_m68060_movec_msp_invalid",
              test_m68060_movec_msp_invalid},
             {"test_rtr", test_rtr},
             {"test_m68010_move_from_sr_privileged",
              test_m68010_move_from_sr_privileged},
             {"test_m68020_cas_word", test_m68020_cas_word},
             {"test_m68020_cas_long", test_m68020_cas_long},
             {"test_m68020_cas2_word", test_m68020_cas2_word},
             {"test_m68020_cas2_long", test_m68020_cas2_long},
             {"test_m68k_fast_count_boundary",
              test_m68k_fast_count_boundary},
             {"test_m68k_context_roundtrip",
              test_m68k_context_roundtrip},
             {NULL, NULL}};
