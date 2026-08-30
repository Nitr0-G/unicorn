#include "unicorn_test.h"

#include <string.h>

const uint64_t code_start = 0x1000;
const uint64_t code_len = 0x4000;

#define M68K_FPCR_RND_NEAREST 0x0000
#define M68K_FPCR_RND_ZERO 0x0010
#define M68K_FPCR_RND_MINUS 0x0020
#define M68K_FPCR_RND_PLUS 0x0030
#define M68K_FPSR_CC_NAN 0x01000000
#define M68K_FPSR_CC_INFINITY 0x02000000
#define M68K_FPSR_CC_ZERO 0x04000000
#define M68K_FPSR_CC_MASK 0x0f000000
#define M68K_MMU_TCR_ENABLED 0x8000
#define M68K_MMU_TTR_ALL 0x0000c000
#define M68K_MMU_DESC_VALID 0x00000001
#define M68K_MMU_DESC_TABLE 0x00000002
#define M68K_MMU_DESC_WRITE_PROTECT 0x00000004
#define M68K_MMUSR_WRITE_PROTECT 0x00000004
#define M68K_MMUSR_RESIDENT 0x00000001

static uint32_t m68k_load_be32(const uint8_t *value)
{
    return ((uint32_t)value[0] << 24) | ((uint32_t)value[1] << 16) |
           ((uint32_t)value[2] << 8) | value[3];
}

static void m68k_write_be32(uc_engine *uc, uint64_t address, uint32_t value)
{
    uint8_t bytes[] = {
        value >> 24,
        value >> 16,
        value >> 8,
        value,
    };

    OK(uc_mem_write(uc, address, bytes, sizeof(bytes)));
}

static void m68k_write_be64(uc_engine *uc, uint64_t address, uint64_t value)
{
    uint8_t bytes[] = {
        value >> 56, value >> 48, value >> 40, value >> 32,
        value >> 24, value >> 16, value >> 8,  value,
    };

    OK(uc_mem_write(uc, address, bytes, sizeof(bytes)));
}

static void uc_common_setup(uc_engine **uc, uc_arch arch, uc_mode mode,
                            const void *code, uint64_t size,
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

static void test_m68k_reset_and_lazy_ccr(void)
{
    const uint8_t code[] = {
        0x40, 0xc1, /* move.w sr, d1 */
        0x70, 0xff, /* moveq #-1, d0 */
        0x52, 0x00, /* addq.b #1, d0 */
    };
    uc_engine *uc;
    uint32_t d1 = UINT32_MAX;
    uint32_t sr = 0;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN,
                    (const char *)code, sizeof(code),
                    UC_CPU_M68K_M68000);

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_M68K_REG_SR, &sr));

    TEST_CHECK_(d1 == 0, "reset sr = 0x%08x", d1);
    TEST_CHECK_((sr & 0x1f) == 0x15, "lazy ccr = 0x%02x", sr & 0x1f);

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

static int32_t run_m68k_fint(uint64_t input, uint32_t fpcr)
{
    const uint8_t code[] = {
        0xf2, 0x00, 0x90, 0x00, /* fmove.l d0, fpcr */
        0xf2, 0x10, 0x54, 0x00, /* fmove.d (a0), fp0 */
        0xf2, 0x00, 0x00, 0x81, /* fint.x fp0, fp1 */
        0xf2, 0x11, 0x60, 0x80, /* fmove.l fp1, (a1) */
        0xf2, 0x02, 0xb0, 0x00, /* fmove.l fpcr, d2 */
    };
    const uint32_t input_address = code_start + 0x800;
    const uint32_t result_address = input_address + 8;
    uint8_t result[4];
    uc_engine *uc;
    uint32_t observed_fpcr = 0;
    uint32_t a0 = input_address;
    uint32_t a1 = result_address;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (const char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);
    m68k_write_be64(uc, input_address, input);
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &fpcr));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));
    OK(uc_reg_write(uc, UC_M68K_REG_A1, &a1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, result_address, result, sizeof(result)));
    OK(uc_reg_read(uc, UC_M68K_REG_D2, &observed_fpcr));
    TEST_CHECK_(observed_fpcr == fpcr, "fpcr = 0x%08x", observed_fpcr);

    OK(uc_close(uc));
    return (int32_t)m68k_load_be32(result);
}

static void test_m68k_fpcr_rounding_modes(void)
{
    static const struct {
        uint32_t fpcr;
        int32_t positive;
        int32_t negative;
    } cases[] = {
        {M68K_FPCR_RND_NEAREST, 2, -2},
        {M68K_FPCR_RND_ZERO, 1, -1},
        {M68K_FPCR_RND_MINUS, 1, -2},
        {M68K_FPCR_RND_PLUS, 2, -1},
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        TEST_CHECK_(run_m68k_fint(UINT64_C(0x3ff8000000000000),
                                  cases[i].fpcr) == cases[i].positive,
                    "positive rounding mode 0x%02x", cases[i].fpcr);
        TEST_CHECK_(run_m68k_fint(UINT64_C(0xbff8000000000000),
                                  cases[i].fpcr) == cases[i].negative,
                    "negative rounding mode 0x%02x", cases[i].fpcr);
    }
}

static void test_m68k_fpsr_exception_condition_codes(void)
{
    const uint8_t code[] = {
        0xf2, 0x10, 0x54, 0x00, /* fmove.d (a0), fp0 */
        0xf2, 0x00, 0x00, 0x84, /* fsqrt.x fp0, fp1 */
        0xf2, 0x00, 0xa8, 0x00, /* fmove.l fpsr, d0 */
        0xf2, 0x11, 0x54, 0x00, /* fmove.d (a1), fp0 */
        0xf2, 0x12, 0x54, 0x80, /* fmove.d (a2), fp1 */
        0xf2, 0x00, 0x00, 0xa0, /* fdiv.x fp0, fp1 */
        0xf2, 0x01, 0xa8, 0x00, /* fmove.l fpsr, d1 */
    };
    const uint32_t data_address = code_start + 0x800;
    uc_engine *uc;
    uint32_t a0 = data_address;
    uint32_t a1 = data_address + 8;
    uint32_t a2 = data_address + 16;
    uint32_t invalid_fpsr = 0;
    uint32_t divzero_fpsr = 0;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (const char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);
    m68k_write_be64(uc, a0, UINT64_C(0xbff0000000000000));
    m68k_write_be64(uc, a1, UINT64_C(0));
    m68k_write_be64(uc, a2, UINT64_C(0x3ff0000000000000));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));
    OK(uc_reg_write(uc, UC_M68K_REG_A1, &a1));
    OK(uc_reg_write(uc, UC_M68K_REG_A2, &a2));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &invalid_fpsr));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &divzero_fpsr));

    TEST_CHECK_((invalid_fpsr & (M68K_FPSR_CC_NAN | M68K_FPSR_CC_INFINITY |
                                 M68K_FPSR_CC_ZERO)) == M68K_FPSR_CC_NAN,
                "invalid fpsr = 0x%08x", invalid_fpsr);
    TEST_CHECK_((divzero_fpsr & M68K_FPSR_CC_MASK) == M68K_FPSR_CC_INFINITY,
                "divide-by-zero fpsr = 0x%08x", divzero_fpsr);

    OK(uc_close(uc));
}

static void setup_m68040_mmu(uc_engine **uc, const uint8_t *code,
                             size_t code_size, uint32_t page_descriptor)
{
    const uint32_t root_address = 0x2000;
    const uint32_t pointer_address = 0x2800;
    const uint32_t page_table_address = 0x3000;
    const uint32_t physical_page = 0x4000;
    const uint32_t page_entry = page_table_address + 0x20;
    uint32_t sr = 0x2000;
    uint32_t dfc = 1;
    uint32_t itt0 = M68K_MMU_TTR_ALL;
    uint16_t tcr = M68K_MMU_TCR_ENABLED;

    uc_common_setup(uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (const char *)code,
                    code_size, UC_CPU_M68K_M68040);
    m68k_write_be32(*uc, root_address, pointer_address | M68K_MMU_DESC_TABLE);
    m68k_write_be32(*uc, pointer_address,
                    page_table_address | M68K_MMU_DESC_TABLE);
    m68k_write_be32(*uc, page_entry, physical_page | page_descriptor);
    OK(uc_reg_write(*uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_write(*uc, UC_M68K_REG_CR_DFC, &dfc));
    OK(uc_reg_write(*uc, UC_M68K_REG_CR_SRP, &root_address));
    OK(uc_reg_write(*uc, UC_M68K_REG_CR_URP, &root_address));
    OK(uc_reg_write(*uc, UC_M68K_REG_CR_ITT0, &itt0));
    OK(uc_reg_write(*uc, UC_M68K_REG_CR_TC, &tcr));
}

static void test_m68040_ptest_updates_mmusr(void)
{
    const uint8_t code[] = {
        0xf5, 0x68, /* ptestr (a0) */
    };
    const uint32_t logical_address = 0x8000;
    uc_engine *uc;
    uint32_t a0 = logical_address;
    uint32_t mmusr = 0;

    setup_m68040_mmu(&uc, code, sizeof(code), M68K_MMU_DESC_VALID);
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_CR_MMUSR, &mmusr));
    TEST_CHECK_(mmusr == (UINT32_C(0x4000) | M68K_MMUSR_RESIDENT),
                "mmusr = 0x%08x", mmusr);

    OK(uc_close(uc));
}

static void test_m68040_write_protection(void)
{
    const uint8_t ptest_code[] = {
        0xf5, 0x48, /* ptestw (a0) */
    };
    const uint8_t store_code[] = {
        0x10, 0x80, /* move.b d0, (a0) */
    };
    const uint32_t logical_address = 0x8000;
    const uint32_t page_descriptor =
        M68K_MMU_DESC_VALID | M68K_MMU_DESC_WRITE_PROTECT;
    uc_engine *uc;
    uint32_t a0 = logical_address;
    uint32_t d0 = 0xa5;
    uint32_t mmusr = 0;
    uint32_t sr = 0;
    uint8_t original = 0x5a;
    uint8_t result = 0;

    setup_m68040_mmu(&uc, ptest_code, sizeof(ptest_code), page_descriptor);
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(ptest_code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_CR_MMUSR, &mmusr));
    TEST_CHECK_(mmusr == (UINT32_C(0x4000) | M68K_MMUSR_RESIDENT |
                          M68K_MMUSR_WRITE_PROTECT),
                "mmusr = 0x%08x", mmusr);
    OK(uc_close(uc));

    setup_m68040_mmu(&uc, store_code, sizeof(store_code), page_descriptor);
    OK(uc_mem_write(uc, 0x4000, &original, sizeof(original)));
    OK(uc_reg_write(uc, UC_M68K_REG_SR, &sr));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));
    OK(uc_reg_write(uc, UC_M68K_REG_D0, &d0));

    uc_assert_err(
        UC_ERR_EXCEPTION,
        uc_emu_start(uc, code_start, code_start + sizeof(store_code), 0, 0));
    OK(uc_mem_read(uc, 0x4000, &result, sizeof(result)));
    TEST_CHECK_(result == original, "protected byte = 0x%02x", result);

    OK(uc_close(uc));
}

static void test_m68020_bitfield_crosses_byte_boundary(void)
{
    const uint8_t code[] = {
        0xe9, 0xd0, 0x01, 0x10, /* bfextu (a0){4:16}, d0 */
        0xeb, 0xd0, 0x11, 0x10, /* bfexts (a0){4:16}, d1 */
    };
    const uint8_t input[] = {0x1a, 0xbc, 0xd6};
    const uint32_t data_address = code_start + 0x800;
    uc_engine *uc;
    uint32_t a0 = data_address;
    uint32_t d0 = 0;
    uint32_t d1 = 0;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (const char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);
    OK(uc_mem_write(uc, data_address, input, sizeof(input)));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    TEST_CHECK_(d0 == 0xabcd, "bfextu = 0x%08x", d0);
    TEST_CHECK_(d1 == 0xffffabcd, "bfexts = 0x%08x", d1);

    OK(uc_close(uc));
}

static void test_m68020_bitfield_crosses_page_boundary(void)
{
    const uint8_t code[] = {
        0xef, 0xd0, 0x21, 0x10, /* bfins d2, (a0){4:16} */
        0xe9, 0xd0, 0x01, 0x10, /* bfextu (a0){4:16}, d0 */
        0xea, 0xd0, 0x01, 0x10, /* bfchg (a0){4:16} */
        0xe9, 0xd0, 0x11, 0x10, /* bfextu (a0){4:16}, d1 */
    };
    const uint8_t input[] = {0x12, 0x34, 0x56};
    const uint8_t expected[] = {0x15, 0x43, 0x26};
    const uint32_t data_address = 0x1fff;
    uint8_t result[sizeof(expected)];
    uc_engine *uc;
    uint32_t a0 = data_address;
    uint32_t d0 = 0;
    uint32_t d1 = 0;
    uint32_t d2 = 0xabcd;

    uc_common_setup(&uc, UC_ARCH_M68K, UC_MODE_BIG_ENDIAN, (const char *)code,
                    sizeof(code), UC_CPU_M68K_M68020);
    OK(uc_mem_write(uc, data_address, input, sizeof(input)));
    OK(uc_reg_write(uc, UC_M68K_REG_A0, &a0));
    OK(uc_reg_write(uc, UC_M68K_REG_D2, &d2));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_M68K_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_M68K_REG_D1, &d1));
    OK(uc_mem_read(uc, data_address, result, sizeof(result)));
    TEST_CHECK_(d0 == 0xabcd, "inserted field = 0x%08x", d0);
    TEST_CHECK_(d1 == 0x5432, "changed field = 0x%08x", d1);
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
             {"test_m68k_reset_and_lazy_ccr",
              test_m68k_reset_and_lazy_ccr},
             {"test_fetoxm1", test_fetoxm1},
             {"test_m68k_fpcr_rounding_modes",
              test_m68k_fpcr_rounding_modes},
             {"test_m68k_fpsr_exception_condition_codes",
              test_m68k_fpsr_exception_condition_codes},
             {"test_m68040_ptest_updates_mmusr",
              test_m68040_ptest_updates_mmusr},
             {"test_m68040_write_protection",
              test_m68040_write_protection},
             {"test_m68020_bitfield_crosses_byte_boundary",
              test_m68020_bitfield_crosses_byte_boundary},
             {"test_m68020_bitfield_crosses_page_boundary",
              test_m68020_bitfield_crosses_page_boundary},
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
