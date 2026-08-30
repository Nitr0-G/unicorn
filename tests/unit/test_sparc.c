#include "unicorn_test.h"

const uint64_t code_start = 0x1000;
const uint64_t code_len = 0x4000;
const uint64_t sparc64_code_start = 0x10000;
const uint64_t data_start = 0x8000;
const uint64_t data_len = 0x2000;

static uint32_t load_be32(const uint8_t *data)
{
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
           ((uint32_t)data[2] << 8) | data[3];
}

static uint64_t load_be64(const uint8_t *data)
{
    return ((uint64_t)load_be32(data) << 32) | load_be32(data + 4);
}

static void store_be32(uint8_t *data, uint32_t value)
{
    data[0] = value >> 24;
    data[1] = value >> 16;
    data[2] = value >> 8;
    data[3] = value;
}

static void sparc64_setup(uc_engine **uc, const uint8_t *code, size_t size)
{
    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC64 | UC_MODE_BIG_ENDIAN, uc));
    OK(uc_ctl_set_cpu_model(*uc, UC_CPU_SPARC64_SUN_ULTRASPARC_IV));
    OK(uc_mem_map(*uc, sparc64_code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(*uc, sparc64_code_start, code, size));
}

static void test_sparc64_engine_lifecycle(void)
{
    size_t i;

    for (i = 0; i < 16; i++) {
        uc_engine *uc;

        OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC64 | UC_MODE_BIG_ENDIAN,
                   &uc));
        OK(uc_close(uc));
    }
}

static void test_sparc64_bpr_reserved_condition(void)
{
    static const uint8_t code[] = { 0x00, 0xf0, 0x20, 0xe3 };
    uc_engine *uc;

    sparc64_setup(&uc, code, sizeof(code));
    TEST_CHECK(uc_emu_start(uc, sparc64_code_start,
                            sparc64_code_start + sizeof(code), 0, 0) ==
               UC_ERR_INSN_INVALID);
    OK(uc_close(uc));
}

static void test_virtual_read(void)
{
    uc_engine *uc;
    uint8_t u8 = 8;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));

    uc_assert_err(UC_ERR_ARG,
                  uc_vmem_read(uc, code_start, UC_PROT_READ, &u8, sizeof(u8)));
    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_vmem_read(uc, code_start, UC_PROT_READ, &u8, sizeof(u8)));

    OK(uc_close(uc));
}

static void test_sparc32_public_registers(void)
{
    uc_engine *uc;
    uint32_t f0 = 0x11223344;
    uint32_t f1 = 0x55667788;
    uint32_t f31 = 0xaabbccdd;
    uint32_t fcc0 = 3;
    uint32_t icc = 0xb;
    uint32_t y = 0xcafebabe;
    uint32_t psr;
    uint32_t got;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));

    OK(uc_reg_write(uc, UC_SPARC_REG_F0, &f0));
    OK(uc_reg_write(uc, UC_SPARC_REG_F1, &f1));
    OK(uc_reg_write(uc, UC_SPARC_REG_F31, &f31));
    OK(uc_reg_write(uc, UC_SPARC_REG_FCC0, &fcc0));
    OK(uc_reg_write(uc, UC_SPARC_REG_ICC, &icc));
    OK(uc_reg_write(uc, UC_SPARC_REG_Y, &y));

    OK(uc_reg_read(uc, UC_SPARC_REG_F0, &got));
    TEST_CHECK(got == f0);
    OK(uc_reg_read(uc, UC_SPARC_REG_F1, &got));
    TEST_CHECK(got == f1);
    OK(uc_reg_read(uc, UC_SPARC_REG_F31, &got));
    TEST_CHECK(got == f31);
    OK(uc_reg_read(uc, UC_SPARC_REG_FCC0, &got));
    TEST_CHECK(got == fcc0);
    OK(uc_reg_read(uc, UC_SPARC_REG_ICC, &got));
    TEST_CHECK(got == icc);
    OK(uc_reg_read(uc, UC_SPARC_REG_Y, &got));
    TEST_CHECK(got == y);
    OK(uc_reg_read(uc, UC_SPARC_REG_PSR, &psr));
    TEST_CHECK(((psr >> 20) & 0xf) == icc);

    OK(uc_close(uc));
}

static void test_sparc64_public_registers(void)
{
    uc_engine *uc;
    uint64_t f32 = 0x1122334455667788ull;
    uint64_t f62 = 0x8877665544332211ull;
    uint64_t y = 0x123456789abcdef0ull;
    uint64_t got64;
    uint32_t f0 = 0xa1b2c3d4;
    uint32_t f31 = 0x0badf00d;
    uint32_t fcc0 = 1;
    uint32_t fcc1 = 2;
    uint32_t fcc2 = 3;
    uint32_t fcc3 = 0;
    uint32_t icc = 5;
    uint32_t xcc = 0xa;
    uint32_t got32;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC64 | UC_MODE_BIG_ENDIAN, &uc));

    OK(uc_reg_write(uc, UC_SPARC_REG_F0, &f0));
    OK(uc_reg_write(uc, UC_SPARC_REG_F31, &f31));
    OK(uc_reg_write(uc, UC_SPARC_REG_F32, &f32));
    OK(uc_reg_write(uc, UC_SPARC_REG_F62, &f62));
    OK(uc_reg_write(uc, UC_SPARC_REG_FCC0, &fcc0));
    OK(uc_reg_write(uc, UC_SPARC_REG_FCC1, &fcc1));
    OK(uc_reg_write(uc, UC_SPARC_REG_FCC2, &fcc2));
    OK(uc_reg_write(uc, UC_SPARC_REG_FCC3, &fcc3));
    OK(uc_reg_write(uc, UC_SPARC_REG_ICC, &icc));
    OK(uc_reg_write(uc, UC_SPARC_REG_XCC, &xcc));
    OK(uc_reg_write(uc, UC_SPARC_REG_Y, &y));

    OK(uc_reg_read(uc, UC_SPARC_REG_F0, &got32));
    TEST_CHECK(got32 == f0);
    OK(uc_reg_read(uc, UC_SPARC_REG_F31, &got32));
    TEST_CHECK(got32 == f31);
    OK(uc_reg_read(uc, UC_SPARC_REG_F32, &got64));
    TEST_CHECK(got64 == f32);
    OK(uc_reg_read(uc, UC_SPARC_REG_F62, &got64));
    TEST_CHECK(got64 == f62);
    OK(uc_reg_read(uc, UC_SPARC_REG_FCC0, &got32));
    TEST_CHECK(got32 == fcc0);
    OK(uc_reg_read(uc, UC_SPARC_REG_FCC1, &got32));
    TEST_CHECK(got32 == fcc1);
    OK(uc_reg_read(uc, UC_SPARC_REG_FCC2, &got32));
    TEST_CHECK(got32 == fcc2);
    OK(uc_reg_read(uc, UC_SPARC_REG_FCC3, &got32));
    TEST_CHECK(got32 == fcc3);
    OK(uc_reg_read(uc, UC_SPARC_REG_ICC, &got32));
    TEST_CHECK(got32 == icc);
    OK(uc_reg_read(uc, UC_SPARC_REG_XCC, &got32));
    TEST_CHECK(got32 == xcc);
    OK(uc_reg_read(uc, UC_SPARC_REG_Y, &got64));
    TEST_CHECK(got64 == y);

    OK(uc_close(uc));
}

typedef struct SparcIntrCapture {
    uint32_t count;
    uint32_t intno;
} SparcIntrCapture;

static void test_sparc32_unaligned_cb(uc_engine *uc, uint32_t intno, void *data)
{
    SparcIntrCapture *capture = (SparcIntrCapture *)data;
    uint32_t mmu_fault_address = 0x1400;

    capture->count++;
    capture->intno = intno;
    OK(uc_reg_write(uc, UC_SPARC_REG_G1, &mmu_fault_address));
}

static void test_sparc32_unaligned_access_sets_fault_address(void)
{
    uc_engine *uc;
    uc_hook hook;
    SparcIntrCapture capture = {0};
    char code[] = ("\xc1\x18\x60\x00" /* ldd [g1],f0 */
                   "\xc4\x80\x40\x80" /* lda [g1] 4,g2 */);
    uint32_t address = 0x8001;
    uint32_t psr;
    uint32_t value = 0;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));
    OK(uc_mem_map(uc, 0x8000, 0x1000, UC_PROT_ALL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_INTR, test_sparc32_unaligned_cb, &capture,
                   1, 0));
    OK(uc_reg_read(uc, UC_SPARC_REG_PSR, &psr));
    psr |= (1 << 12) | (1 << 7);
    OK(uc_reg_write(uc, UC_SPARC_REG_PSR, &psr));
    OK(uc_reg_write(uc, UC_SPARC_REG_G1, &address));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_SPARC_REG_G2, &value));

    TEST_CHECK(capture.count == 1);
    TEST_MSG("interrupts=%u", capture.count);
    TEST_CHECK(capture.intno == 7);
    TEST_MSG("intno=%u", capture.intno);
    TEST_CHECK(value == address);
    TEST_MSG("expected=0x%x actual=0x%x", address, value);

    OK(uc_close(uc));
}

static void test_sparc32_delay_slot_count(void)
{
    const uint8_t code[] = {
        0x10, 0x80, 0x00, 0x03, /* ba code_start + 0xc */
        0x82, 0x10, 0x20, 0x01, /* mov 1, %g1 */
        0x82, 0x10, 0x20, 0x02, /* mov 2, %g1 */
        0x84, 0x10, 0x20, 0x03, /* mov 3, %g2 */
    };
    uc_engine *uc;
    uint32_t g1 = 0;
    uint32_t g2 = 0;
    uint32_t pc = 0;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 1));
    OK(uc_reg_read(uc, UC_SPARC_REG_G1, &g1));
    OK(uc_reg_read(uc, UC_SPARC_REG_G2, &g2));
    OK(uc_reg_read(uc, UC_SPARC_REG_PC, &pc));
    TEST_CHECK(g1 == 0);
    TEST_CHECK(g2 == 0);
    TEST_CHECK(pc == (uint32_t)(code_start + 4));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 2));
    OK(uc_reg_read(uc, UC_SPARC_REG_G1, &g1));
    OK(uc_reg_read(uc, UC_SPARC_REG_G2, &g2));
    OK(uc_reg_read(uc, UC_SPARC_REG_PC, &pc));
    TEST_CHECK(g1 == 1);
    TEST_CHECK(g2 == 0);
    TEST_CHECK(pc == (uint32_t)(code_start + 12));

    OK(uc_close(uc));
}

typedef struct SparcDelaySlotStop {
    uc_err error;
    uint32_t calls;
} SparcDelaySlotStop;

static void sparc_delay_slot_stop_cb(uc_engine *uc, uint64_t address,
                                     uint32_t size, void *user_data)
{
    SparcDelaySlotStop *stop = (SparcDelaySlotStop *)user_data;

    stop->calls++;
    stop->error = uc_emu_stop(uc);
}

static void test_sparc32_delay_slot_pending_stop(void)
{
    const uint8_t code[] = {
        0x10, 0x80, 0x00, 0x02, /* ba code_start + 8 */
        0x82, 0x10, 0x20, 0x01, /* mov 1, %g1 */
        0x84, 0x10, 0x20, 0x02, /* mov 2, %g2 */
    };
    SparcDelaySlotStop stop = {0};
    uint32_t g1 = 0;
    uint32_t g2 = 0;
    uint32_t pc;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE, sparc_delay_slot_stop_cb,
                   &stop, code_start + 4, code_start + 4));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(stop.error);
    OK(uc_reg_read(uc, UC_SPARC_REG_G1, &g1));
    OK(uc_reg_read(uc, UC_SPARC_REG_G2, &g2));
    OK(uc_reg_read(uc, UC_SPARC_REG_PC, &pc));
    TEST_CHECK(stop.calls == 1);
    TEST_CHECK(g1 == 0);
    TEST_CHECK(g2 == 0);
    TEST_CHECK_(pc == code_start + 4, "pc=0x%x", pc);

    OK(uc_close(uc));
}

static void test_sparc32_branch_always_annul(void)
{
    const uint8_t code[] = {
        0x30, 0x80, 0x00, 0x03, /* ba,a code_start + 0xc */
        0x82, 0x10, 0x20, 0x01, /* mov 1, %g1 */
        0x82, 0x10, 0x20, 0x02, /* mov 2, %g1 */
        0x84, 0x10, 0x20, 0x03, /* mov 3, %g2 */
    };
    uc_engine *uc;
    uint32_t g1 = 0;
    uint32_t g2 = 0;
    uint32_t pc = 0;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 1));
    OK(uc_reg_read(uc, UC_SPARC_REG_G1, &g1));
    OK(uc_reg_read(uc, UC_SPARC_REG_G2, &g2));
    OK(uc_reg_read(uc, UC_SPARC_REG_PC, &pc));
    TEST_CHECK(g1 == 0);
    TEST_CHECK(g2 == 0);
    TEST_CHECK(pc == (uint32_t)(code_start + 12));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 2));
    OK(uc_reg_read(uc, UC_SPARC_REG_G1, &g1));
    OK(uc_reg_read(uc, UC_SPARC_REG_G2, &g2));
    OK(uc_reg_read(uc, UC_SPARC_REG_PC, &pc));
    TEST_CHECK(g1 == 0);
    TEST_CHECK(g2 == 3);
    TEST_CHECK(pc == (uint32_t)(code_start + sizeof(code)));

    OK(uc_close(uc));
}

static void test_sparc32_context_roundtrip(void)
{
    uc_engine *uc;
    uc_context *context;
    uint32_t g1 = 0x11223344;
    uint32_t pc = code_start + 0x20;
    uint32_t changed = 0;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_reg_write(uc, UC_SPARC_REG_G1, &g1));
    OK(uc_reg_write(uc, UC_SPARC_REG_PC, &pc));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_reg_write(uc, UC_SPARC_REG_G1, &changed));
    OK(uc_reg_write(uc, UC_SPARC_REG_PC, &changed));
    OK(uc_context_restore(uc, context));

    OK(uc_reg_read(uc, UC_SPARC_REG_G1, &changed));
    TEST_CHECK(changed == g1);
    OK(uc_reg_read(uc, UC_SPARC_REG_PC, &changed));
    TEST_CHECK(changed == pc);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_sparc64_vis1_vis2(void)
{
    const uint8_t code[] = {
        0x8d, 0x80, 0x20, 0x04, /* wr %g0,4,%fprs */
        0x89, 0xb0, 0x09, 0x62, /* fpmerge %f0,%f2,%f4 */
        0xa7, 0x80, 0x40, 0x00, /* wr %g1,%g0,%gsr */
        0x8d, 0xb0, 0x09, 0x82, /* bshuffle %f0,%f2,%f6 */
    };
    uc_engine *uc;
    uint64_t gsr = UINT64_C(0x08192a3b) << 32;
    uint32_t f0 = 0x00112233;
    uint32_t f1 = 0x44556677;
    uint32_t f2 = 0x8899aabb;
    uint32_t f3 = 0xccddeeff;
    uint32_t value;
    uint64_t pc;
    uc_err err;

    sparc64_setup(&uc, code, sizeof(code));
    OK(uc_reg_write(uc, UC_SPARC_REG_G1, &gsr));
    OK(uc_reg_write(uc, UC_SPARC_REG_F0, &f0));
    OK(uc_reg_write(uc, UC_SPARC_REG_F1, &f1));
    OK(uc_reg_write(uc, UC_SPARC_REG_F2, &f2));
    OK(uc_reg_write(uc, UC_SPARC_REG_F3, &f3));

    err = uc_emu_start(uc, sparc64_code_start,
                       sparc64_code_start + sizeof(code), 0, 4);
    OK(uc_reg_read(uc, UC_SPARC_REG_PC, &pc));
    TEST_CHECK_(err == UC_ERR_OK, "err=%u pc=0x%016" PRIx64,
                (unsigned int)err, pc);

    OK(uc_reg_read(uc, UC_SPARC_REG_F4, &value));
    TEST_CHECK_(value == 0x44cc55dd, "f4 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_SPARC_REG_F5, &value));
    TEST_CHECK_(value == 0x66ee77ff, "f5 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_SPARC_REG_F6, &value));
    TEST_CHECK_(value == 0xbb33aa22, "f6 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_SPARC_REG_F7, &value));
    TEST_CHECK_(value == 0x99118800, "f7 = 0x%08x", value);

    OK(uc_close(uc));
}

static void test_sparc32_fpu_fsr(void)
{
    const uint8_t code[] = {
        0x8d, 0xa0, 0x09, 0xa2, /* fdivs %f0,%f2,%f6 */
        0xc1, 0x28, 0x40, 0x00, /* st %fsr,[%g1] */
    };
    uc_engine *uc;
    uint32_t address = data_start;
    uint32_t numerator = 0x3f800000;
    uint32_t denominator = 0;
    uint32_t result;
    uint32_t psr;
    uint8_t fsr_data[4];
    uint32_t fsr;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_mem_map(uc, data_start, data_len, UC_PROT_ALL));
    OK(uc_reg_read(uc, UC_SPARC_REG_PSR, &psr));
    psr |= (1 << 12) | (1 << 7);
    OK(uc_reg_write(uc, UC_SPARC_REG_PSR, &psr));
    OK(uc_reg_write(uc, UC_SPARC_REG_G1, &address));
    OK(uc_reg_write(uc, UC_SPARC_REG_F0, &numerator));
    OK(uc_reg_write(uc, UC_SPARC_REG_F2, &denominator));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 2));

    OK(uc_reg_read(uc, UC_SPARC_REG_F6, &result));
    TEST_CHECK_(result == 0x7f800000, "f6 = 0x%08x", result);
    OK(uc_mem_read(uc, data_start, fsr_data, sizeof(fsr_data)));
    fsr = load_be32(fsr_data);
    TEST_CHECK_((fsr & 0x7f) == 0x42, "fsr = 0x%08x", fsr);

    OK(uc_close(uc));
}

static void test_sparc64_fpu_fsr(void)
{
    const uint8_t code[] = {
        0x8d, 0x80, 0x20, 0x04, /* wr %g0,4,%fprs */
        0x8d, 0xa0, 0x09, 0xa2, /* fdivs %f0,%f2,%f6 */
        0xc3, 0x28, 0x40, 0x00, /* stx %fsr,[%g1] */
    };
    uc_engine *uc;
    uint64_t address = data_start;
    uint32_t numerator = 0x3f800000;
    uint32_t denominator = 0;
    uint32_t result;
    uint8_t fsr_data[8];
    uint64_t fsr;

    sparc64_setup(&uc, code, sizeof(code));
    OK(uc_mem_map(uc, data_start, data_len, UC_PROT_ALL));
    OK(uc_reg_write(uc, UC_SPARC_REG_G1, &address));
    OK(uc_reg_write(uc, UC_SPARC_REG_F0, &numerator));
    OK(uc_reg_write(uc, UC_SPARC_REG_F2, &denominator));

    OK(uc_emu_start(uc, sparc64_code_start,
                    sparc64_code_start + sizeof(code), 0, 3));

    OK(uc_reg_read(uc, UC_SPARC_REG_F6, &result));
    TEST_CHECK_(result == 0x7f800000, "f6 = 0x%08x", result);
    OK(uc_mem_read(uc, data_start, fsr_data, sizeof(fsr_data)));
    fsr = load_be64(fsr_data);
    TEST_CHECK_((fsr & 0x7f) == 0x42, "fsr = 0x%016" PRIx64, fsr);

    OK(uc_close(uc));
}

static void test_sparc32_register_window_save_restore(void)
{
    const uint8_t code[] = {
        0x81, 0x90, 0x00, 0x00, /* wr %g0,%g0,%wim */
        0x9d, 0xe3, 0xbf, 0xc0, /* save %sp,-64,%sp */
        0x82, 0x10, 0x00, 0x19, /* mov %i1,%g1 */
        0x81, 0xe8, 0x00, 0x00, /* restore */
    };
    uc_engine *uc;
    uint32_t sp = 0x9000;
    uint32_t o1 = 0x11223344;
    uint32_t value;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_SPARC_REG_SP, &sp));
    OK(uc_reg_write(uc, UC_SPARC_REG_O1, &o1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));

    OK(uc_reg_read(uc, UC_SPARC_REG_G1, &value));
    TEST_CHECK_(value == o1, "g1 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_SPARC_REG_SP, &value));
    TEST_CHECK_(value == sp, "sp = 0x%08x", value);
    OK(uc_reg_read(uc, UC_SPARC_REG_O1, &value));
    TEST_CHECK_(value == o1, "o1 = 0x%08x", value);

    OK(uc_close(uc));
}

static void test_sparc64_register_window_save_restore(void)
{
    const uint8_t code[] = {
        0x95, 0x90, 0x20, 0x06, /* wrpr %g0,6,%cansave */
        0x97, 0x90, 0x20, 0x00, /* wrpr %g0,0,%canrestore */
        0x99, 0x90, 0x20, 0x07, /* wrpr %g0,7,%cleanwin */
        0x9d, 0xe3, 0xbf, 0x50, /* save %sp,-176,%sp */
        0x82, 0x10, 0x00, 0x19, /* mov %i1,%g1 */
        0x81, 0xe8, 0x00, 0x00, /* restore */
    };
    uc_engine *uc;
    uint64_t sp = 0x9000;
    uint64_t o1 = UINT64_C(0x1122334455667788);
    uint64_t value;

    sparc64_setup(&uc, code, sizeof(code));
    OK(uc_reg_write(uc, UC_SPARC_REG_SP, &sp));
    OK(uc_reg_write(uc, UC_SPARC_REG_O1, &o1));

    OK(uc_emu_start(uc, sparc64_code_start,
                    sparc64_code_start + sizeof(code), 0, 6));

    OK(uc_reg_read(uc, UC_SPARC_REG_G1, &value));
    TEST_CHECK_(value == o1, "g1 = 0x%016" PRIx64, value);
    OK(uc_reg_read(uc, UC_SPARC_REG_SP, &value));
    TEST_CHECK_(value == sp, "sp = 0x%016" PRIx64, value);
    OK(uc_reg_read(uc, UC_SPARC_REG_O1, &value));
    TEST_CHECK_(value == o1, "o1 = 0x%016" PRIx64, value);

    OK(uc_close(uc));
}

static void test_sparc_window_trap_cb(uc_engine *uc, uint32_t intno,
                                      void *data)
{
    SparcIntrCapture *capture = (SparcIntrCapture *)data;

    capture->count++;
    capture->intno = intno;
    OK(uc_emu_stop(uc));
}

static void test_sparc32_register_window_traps(void)
{
    static const struct {
        uint32_t instruction;
        uint32_t intno;
    } cases[] = {
        {0x9de3bfc0, 0x05}, /* save %sp,-64,%sp: window overflow */
        {0x81e80000, 0x06}, /* restore: window underflow */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t code[8];
        SparcIntrCapture capture = {0};
        uc_engine *uc;
        uc_hook hook;
        uint32_t psr;
        uint32_t wim = UINT32_MAX;

        store_be32(code, 0x81900001); /* wr %g0,%g1,%wim */
        store_be32(code + 4, cases[i].instruction);
        OK(uc_open(UC_ARCH_SPARC,
                   UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
        OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
        OK(uc_mem_write(uc, code_start, code, sizeof(code)));
        OK(uc_hook_add(uc, &hook, UC_HOOK_INTR, test_sparc_window_trap_cb,
                       &capture, 1, 0));
        OK(uc_reg_read(uc, UC_SPARC_REG_PSR, &psr));
        psr = (psr & ~0x1f) | (1 << 7);
        OK(uc_reg_write(uc, UC_SPARC_REG_PSR, &psr));
        OK(uc_reg_write(uc, UC_SPARC_REG_G1, &wim));

        OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));

        TEST_CHECK_(capture.count == 1, "case=%u interrupts=%u",
                    (unsigned int)i, capture.count);
        TEST_CHECK_(capture.intno == cases[i].intno, "case=%u intno=0x%x",
                    (unsigned int)i, capture.intno);
        OK(uc_reg_read(uc, UC_SPARC_REG_PSR, &psr));
        TEST_CHECK_((psr & 0x1f) == 0, "case=%u cwp=%u", (unsigned int)i,
                    psr & 0x1f);

        OK(uc_close(uc));
    }
}

static void test_sparc64_register_window_traps(void)
{
    static const struct {
        uint32_t window_state_instruction;
        uint32_t instruction;
        uint32_t intno;
    } cases[] = {
        {0x95902000, 0x9de3bf50, 0x80}, /* cansave=0; save: spill */
        {0x97902000, 0x81e80000, 0xc0}, /* canrestore=0; restore: fill */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t code[16];
        SparcIntrCapture capture = {0};
        uc_engine *uc;
        uc_hook hook;

        store_be32(code, cases[i].window_state_instruction);
        store_be32(code + 4, 0x9b902000); /* wrpr %g0,0,%otherwin */
        store_be32(code + 8, 0x9d902000); /* wrpr %g0,0,%wstate */
        store_be32(code + 12, cases[i].instruction);
        sparc64_setup(&uc, code, sizeof(code));
        OK(uc_hook_add(uc, &hook, UC_HOOK_INTR, test_sparc_window_trap_cb,
                       &capture, 1, 0));

        OK(uc_emu_start(uc, sparc64_code_start,
                        sparc64_code_start + sizeof(code), 0, 0));

        TEST_CHECK_(capture.count == 1, "case=%u interrupts=%u",
                    (unsigned int)i, capture.count);
        TEST_CHECK_(capture.intno == cases[i].intno, "case=%u intno=0x%x",
                    (unsigned int)i, capture.intno);

        OK(uc_close(uc));
    }
}

static void test_sparc32_asi_swap(void)
{
    const uint8_t code[] = {
        0xc4, 0x80, 0x40, 0x80, /* lda [%g1] 4,%g2 */
        0xc6, 0x78, 0x40, 0x00, /* swap [%g1],%g3 */
    };
    uint8_t memory[4];
    uc_engine *uc;
    uint32_t address = data_start;
    uint32_t replacement = 0xaabbccdd;
    uint32_t psr;
    uint32_t value;

    OK(uc_open(UC_ARCH_SPARC, UC_MODE_SPARC32 | UC_MODE_BIG_ENDIAN, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_mem_map(uc, data_start, data_len, UC_PROT_ALL));
    store_be32(memory, 0x11223344);
    OK(uc_mem_write(uc, data_start, memory, sizeof(memory)));
    OK(uc_reg_read(uc, UC_SPARC_REG_PSR, &psr));
    psr |= 1 << 7;
    OK(uc_reg_write(uc, UC_SPARC_REG_PSR, &psr));
    OK(uc_reg_write(uc, UC_SPARC_REG_G1, &address));
    OK(uc_reg_write(uc, UC_SPARC_REG_G3, &replacement));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 2));

    OK(uc_reg_read(uc, UC_SPARC_REG_G2, &value));
    TEST_CHECK_(value == 0xf3000000, "g2 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_SPARC_REG_G3, &value));
    TEST_CHECK_(value == 0x11223344, "g3 = 0x%08x", value);
    OK(uc_mem_read(uc, data_start, memory, sizeof(memory)));
    value = load_be32(memory);
    TEST_CHECK_(value == replacement, "memory = 0x%08x", value);

    OK(uc_close(uc));
}

static void test_sparc64_asi_swap(void)
{
    const uint8_t code[] = {
        0xc4, 0x80, 0x50, 0x00, /* lduwa [%g1] 0x80,%g2 */
        0xc6, 0xf8, 0x50, 0x00, /* swapa [%g1] 0x80,%g3 */
    };
    uint8_t memory[4];
    uc_engine *uc;
    uint64_t address = data_start;
    uint64_t replacement = 0xaabbccdd;
    uint64_t value;

    sparc64_setup(&uc, code, sizeof(code));
    OK(uc_mem_map(uc, data_start, data_len, UC_PROT_ALL));
    store_be32(memory, 0x11223344);
    OK(uc_mem_write(uc, data_start, memory, sizeof(memory)));
    OK(uc_reg_write(uc, UC_SPARC_REG_G1, &address));
    OK(uc_reg_write(uc, UC_SPARC_REG_G3, &replacement));

    OK(uc_emu_start(uc, sparc64_code_start,
                    sparc64_code_start + sizeof(code), 0, 2));

    OK(uc_reg_read(uc, UC_SPARC_REG_G2, &value));
    TEST_CHECK_(value == 0x11223344, "g2 = 0x%016" PRIx64, value);
    OK(uc_reg_read(uc, UC_SPARC_REG_G3, &value));
    TEST_CHECK_(value == 0x11223344, "g3 = 0x%016" PRIx64, value);
    OK(uc_mem_read(uc, data_start, memory, sizeof(memory)));
    value = load_be32(memory);
    TEST_CHECK_(value == replacement, "memory = 0x%016" PRIx64, value);

    OK(uc_close(uc));
}

static void test_sparc64_branch_annul(void)
{
    static const struct {
        uint32_t xcc;
        uint64_t g2;
        uint64_t g3;
    } cases[] = {
        {4, 1, 0},
        {0, 0, 2},
    };
    const uint8_t code[] = {
        0x22, 0x68, 0x00, 0x03, /* be,a %xcc,sparc64_code_start+12 */
        0x84, 0x10, 0x20, 0x01, /* mov 1,%g2 */
        0x86, 0x10, 0x20, 0x02, /* mov 2,%g3 */
        0x88, 0x10, 0x20, 0x03, /* mov 3,%g4 */
    };
    size_t i;

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uc_engine *uc;
        uint64_t value;

        sparc64_setup(&uc, code, sizeof(code));
        OK(uc_reg_write(uc, UC_SPARC_REG_XCC, &cases[i].xcc));
        OK(uc_emu_start(uc, sparc64_code_start,
                        sparc64_code_start + sizeof(code), 0, 2));

        OK(uc_reg_read(uc, UC_SPARC_REG_G2, &value));
        TEST_CHECK_(value == cases[i].g2, "case=%u g2=%" PRIu64,
                    (unsigned int)i, value);
        OK(uc_reg_read(uc, UC_SPARC_REG_G3, &value));
        TEST_CHECK_(value == cases[i].g3, "case=%u g3=%" PRIu64,
                    (unsigned int)i, value);
        OK(uc_reg_read(uc, UC_SPARC_REG_PC, &value));
        TEST_CHECK_(value == sparc64_code_start + 12,
                    "case=%u pc=0x%016" PRIx64, (unsigned int)i, value);

        OK(uc_close(uc));
    }
}

TEST_LIST = {
    {"test_sparc64_engine_lifecycle", test_sparc64_engine_lifecycle},
    {"test_sparc64_bpr_reserved_condition",
     test_sparc64_bpr_reserved_condition},
    {"test_virtual_read", test_virtual_read},
    {"test_sparc32_public_registers", test_sparc32_public_registers},
    {"test_sparc64_public_registers", test_sparc64_public_registers},
    {"test_sparc32_unaligned_access_sets_fault_address",
     test_sparc32_unaligned_access_sets_fault_address},
    {"test_sparc32_delay_slot_count", test_sparc32_delay_slot_count},
    {"test_sparc32_delay_slot_pending_stop",
     test_sparc32_delay_slot_pending_stop},
    {"test_sparc32_branch_always_annul", test_sparc32_branch_always_annul},
    {"test_sparc32_context_roundtrip", test_sparc32_context_roundtrip},
    {"test_sparc64_vis1_vis2", test_sparc64_vis1_vis2},
    {"test_sparc32_fpu_fsr", test_sparc32_fpu_fsr},
    {"test_sparc64_fpu_fsr", test_sparc64_fpu_fsr},
    {"test_sparc32_register_window_save_restore",
     test_sparc32_register_window_save_restore},
    {"test_sparc64_register_window_save_restore",
     test_sparc64_register_window_save_restore},
    {"test_sparc32_register_window_traps",
     test_sparc32_register_window_traps},
    {"test_sparc64_register_window_traps",
     test_sparc64_register_window_traps},
    {"test_sparc32_asi_swap", test_sparc32_asi_swap},
    {"test_sparc64_asi_swap", test_sparc64_asi_swap},
    {"test_sparc64_branch_annul", test_sparc64_branch_annul},
    {NULL, NULL}};
