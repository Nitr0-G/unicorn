#include "unicorn_test.h"

const uint64_t code_start = 0x10000;
const uint64_t code_len = 0x4000;
const uint64_t csa_start = 0x20000;
const uint64_t csa_len = 0x4000;
const uint64_t data_start = 0x30000;
const uint64_t data_len = 0x4000;

static uint32_t load_le32(const uint8_t *data)
{
    return (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
           ((uint32_t)data[2] << 16) | ((uint32_t)data[3] << 24);
}

static void store_le32(uint8_t *data, uint32_t value)
{
    data[0] = value;
    data[1] = value >> 8;
    data[2] = value >> 16;
    data[3] = value >> 24;
}

static void uc_map_code(uc_engine *uc, const uint8_t *code, size_t size)
{
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, size));
}

static void uc_common_setup(uc_engine **uc, const uint8_t *code, size_t size)
{
    OK(uc_open(UC_ARCH_TRICORE, UC_MODE_LITTLE_ENDIAN, uc));
    uc_map_code(*uc, code, size);
}

static uint32_t tricore_rr(uint8_t major, uint8_t op2, uint8_t dest,
                           uint8_t src1, uint8_t src2)
{
    return major | ((uint32_t)src1 << 8) | ((uint32_t)src2 << 12) |
           ((uint32_t)op2 << 20) | ((uint32_t)dest << 28);
}

static uint32_t tricore_bo(uint8_t major, uint8_t op2, uint8_t reg,
                           uint8_t base, int16_t offset)
{
    uint32_t off10 = (uint16_t)offset & 0x3ff;

    return major | ((uint32_t)reg << 8) | ((uint32_t)base << 12) |
           ((off10 & 0x3f) << 16) | ((uint32_t)op2 << 22) |
           ((off10 >> 6) << 28);
}

static uint32_t tricore_bol(uint8_t major, uint8_t reg, uint8_t base,
                            int16_t offset)
{
    uint32_t off16 = (uint16_t)offset;

    return major | ((uint32_t)reg << 8) | ((uint32_t)base << 12) |
           ((off16 & 0x3f) << 16) | (((off16 >> 10) & 0x3f) << 22) |
           (((off16 >> 6) & 0xf) << 28);
}

static uint32_t tricore_brc(uint8_t major, bool op2, uint8_t src,
                            int8_t constant, int16_t displacement)
{
    return major | ((uint32_t)src << 8) | (((uint32_t)constant & 0xf) << 12) |
           (((uint32_t)displacement & 0x7fff) << 16) | ((uint32_t)op2 << 31);
}

static void append_insn32(uint8_t *code, size_t *offset, uint32_t insn)
{
    store_le32(code + *offset, insn);
    *offset += 4;
}

static void append_insn16(uint8_t *code, size_t *offset, uint16_t insn)
{
    code[*offset] = insn;
    code[*offset + 1] = insn >> 8;
    *offset += 2;
}

static void test_tricore_mov_dreg(void)
{
    const uint8_t code[] = {
        0x82, 0x11, 0xbb, 0x00, 0x00, 0x08,
    };
    uc_engine *uc;
    uint32_t d0 = 0;
    uint32_t d1 = 0;
    uint32_t pc = 0;

    uc_common_setup(&uc, code, sizeof(code));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));

    OK(uc_reg_read(uc, UC_TRICORE_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_TRICORE_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &pc));
    TEST_CHECK_(d0 == 0x8000, "d0 = 0x%08x", d0);
    TEST_CHECK_(d1 == 1, "d1 = 0x%08x", d1);
    TEST_CHECK(pc == code_start + sizeof(code));

    OK(uc_close(uc));
}

static void test_tricore_reg_roundtrip_one(uc_engine *uc, int reg,
                                           uint32_t value)
{
    uint32_t out = 0;

    OK(uc_reg_write(uc, reg, &value));
    OK(uc_reg_read(uc, reg, &out));
    TEST_CHECK(out == value);
}

static void test_tricore_csfr_reg_roundtrip(void)
{
    uc_engine *uc;
    uint32_t code = 0;

    uc_common_setup(&uc, (const uint8_t *)&code, sizeof(code));

    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_DPR0_U, 0x11112222);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_DPR3_L, 0x33334444);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_CPR0_U, 0x55556666);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_CPR3_L, 0x77778888);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_DPM2, 0x10203040);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_CPM3, 0x50607080);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_MMU_CON, 0xaabbccdd);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_MMU_TFA, 0x01020304);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_BMACON, 0x11223344);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_CCPIER, 0x55667788);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_DBGSR, 0x00000001);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_TR1EVT, 0x13579bdf);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_CCTRL, 0x2468ace0);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_M3CNT, 0xdeadbeef);
    test_tricore_reg_roundtrip_one(uc, UC_TRICORE_REG_PSW, 0xf8000123);

    OK(uc_close(uc));
}

static void test_tricore_cpu_model(void)
{
    uc_engine *uc;

    OK(uc_open(UC_ARCH_TRICORE, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_TRICORE_TC1796));
    TEST_CHECK(uc_ctl_set_cpu_model(uc, UC_CPU_TRICORE_ENDING) == UC_ERR_ARG);
    OK(uc_close(uc));
}

static void test_tricore_fast_count_boundary(void)
{
    const uint8_t code[] = {
        0x82, 0x11,             /* mov d1, #1 */
        0xbb, 0x00, 0x00, 0x08, /* mov.u d0, #0x8000 */
    };
    uc_engine *uc;
    uint32_t d0 = 0;
    uint32_t d1 = 0;
    uint32_t pc = 0;

    uc_common_setup(&uc, code, sizeof(code));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 1));
    OK(uc_reg_read(uc, UC_TRICORE_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_TRICORE_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &pc));
    TEST_CHECK(d0 == 0);
    TEST_CHECK(d1 == 1);
    TEST_CHECK(pc == (uint32_t)(code_start + 2));

    d1 = 0;
    OK(uc_reg_write(uc, UC_TRICORE_REG_D1, &d1));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 2));
    OK(uc_reg_read(uc, UC_TRICORE_REG_D0, &d0));
    OK(uc_reg_read(uc, UC_TRICORE_REG_D1, &d1));
    OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &pc));
    TEST_CHECK(d0 == 0x8000);
    TEST_CHECK(d1 == 1);
    TEST_CHECK(pc == (uint32_t)(code_start + sizeof(code)));

    OK(uc_close(uc));
}

static void test_tricore_context_roundtrip(void)
{
    uc_engine *uc;
    uc_context *context;
    uint32_t d0 = 0x11223344;
    uint32_t pc = code_start + 0x20;
    uint32_t changed = 0;

    OK(uc_open(UC_ARCH_TRICORE, UC_MODE_LITTLE_ENDIAN, &uc));
    OK(uc_reg_write(uc, UC_TRICORE_REG_D0, &d0));
    OK(uc_reg_write(uc, UC_TRICORE_REG_PC, &pc));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_reg_write(uc, UC_TRICORE_REG_D0, &changed));
    OK(uc_reg_write(uc, UC_TRICORE_REG_PC, &changed));
    OK(uc_context_restore(uc, context));

    OK(uc_reg_read(uc, UC_TRICORE_REG_D0, &changed));
    TEST_CHECK(changed == d0);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &changed));
    TEST_CHECK(changed == pc);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_tricore_call_ret_csa(void)
{
    const uint8_t code[] = {
        0x6d, 0x00, 0x04, 0x00, /* call #code_start + 8 */
        0x00, 0x09,             /* nop */
        0x00, 0x09,             /* nop */
        0x0d, 0x00, 0x80, 0x01, /* ret */
    };
    const int regs[] = {
        UC_TRICORE_REG_A10, UC_TRICORE_REG_A11, UC_TRICORE_REG_D8,
        UC_TRICORE_REG_D9,  UC_TRICORE_REG_D10, UC_TRICORE_REG_D11,
        UC_TRICORE_REG_A12, UC_TRICORE_REG_A13, UC_TRICORE_REG_A14,
        UC_TRICORE_REG_A15, UC_TRICORE_REG_D12, UC_TRICORE_REG_D13,
        UC_TRICORE_REG_D14, UC_TRICORE_REG_D15,
    };
    const uint32_t values[] = {
        0xa10a10a1, 0xa11a11a1, 0xd08d08d0, 0xd09d09d0, 0xd10d10d1,
        0xd11d11d1, 0xa12a12a1, 0xa13a13a1, 0xa14a14a1, 0xa15a15a1,
        0xd12d12d1, 0xd13d13d1, 0xd14d14d1, 0xd15d15d1,
    };
    const uint32_t csa_first = csa_start >> 6;
    const uint32_t csa_second = (csa_start + 64) >> 6;
    const uint32_t csa_limit = (csa_start + 128) >> 6;
    const uint32_t initial_pcxi = 0;
    const uint32_t initial_psw = 0xb80;
    uint8_t frame[64];
    uc_engine *uc;
    uint32_t value;
    size_t i;

    memset(frame, 0xcc, sizeof(frame));
    store_le32(frame, csa_second);

    uc_common_setup(&uc, code, sizeof(code));
    OK(uc_mem_map(uc, csa_start, csa_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, csa_start, frame, sizeof(frame)));
    OK(uc_reg_write(uc, UC_TRICORE_REG_FCX, &csa_first));
    OK(uc_reg_write(uc, UC_TRICORE_REG_LCX, &csa_limit));
    OK(uc_reg_write(uc, UC_TRICORE_REG_PCXI, &initial_pcxi));
    OK(uc_reg_write(uc, UC_TRICORE_REG_PSW, &initial_psw));
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        OK(uc_reg_write(uc, regs[i], &values[i]));
    }

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 1));

    OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &value));
    TEST_CHECK(value == code_start + 8);
    OK(uc_reg_read(uc, UC_TRICORE_REG_A11, &value));
    TEST_CHECK(value == code_start + 4);
    OK(uc_reg_read(uc, UC_TRICORE_REG_FCX, &value));
    TEST_CHECK(value == csa_second);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PCXI, &value));
    TEST_CHECK(value == (0x00400000 | csa_first));

    OK(uc_mem_read(uc, csa_start, frame, sizeof(frame)));
    TEST_CHECK(load_le32(frame) == initial_pcxi);
    TEST_CHECK(load_le32(frame + 4) == initial_psw);
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        TEST_CHECK(load_le32(frame + 8 + i * 4) == values[i]);
        if (regs[i] != UC_TRICORE_REG_A11) {
            value = 0x5a5a0000 + i;
            OK(uc_reg_write(uc, regs[i], &value));
        }
    }

    OK(uc_emu_start(uc, code_start + 8, code_start + sizeof(code), 0, 1));

    OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &value));
    TEST_CHECK(value == code_start + 4);
    OK(uc_reg_read(uc, UC_TRICORE_REG_FCX, &value));
    TEST_CHECK(value == csa_first);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PCXI, &value));
    TEST_CHECK(value == initial_pcxi);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PSW, &value));
    TEST_CHECK(value == initial_psw);
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        OK(uc_reg_read(uc, regs[i], &value));
        TEST_CHECK(value == values[i]);
    }
    OK(uc_mem_read(uc, csa_start, frame, sizeof(frame)));
    TEST_CHECK(load_le32(frame) == csa_second);

    OK(uc_close(uc));
}

static void test_tricore_svlcx_rslcx_csa(void)
{
    const uint8_t code[] = {
        0x0d, 0x00, 0x00, 0x02, /* svlcx */
        0x0d, 0x00, 0x40, 0x02, /* rslcx */
    };
    const int regs[] = {
        UC_TRICORE_REG_A11, UC_TRICORE_REG_A2, UC_TRICORE_REG_A3,
        UC_TRICORE_REG_D0,  UC_TRICORE_REG_D1, UC_TRICORE_REG_D2,
        UC_TRICORE_REG_D3,  UC_TRICORE_REG_A4, UC_TRICORE_REG_A5,
        UC_TRICORE_REG_A6,  UC_TRICORE_REG_A7, UC_TRICORE_REG_D4,
        UC_TRICORE_REG_D5,  UC_TRICORE_REG_D6, UC_TRICORE_REG_D7,
    };
    const uint32_t values[] = {
        0xa11a11a1, 0xa02a02a0, 0xa03a03a0, 0xd00d00d0, 0xd01d01d0,
        0xd02d02d0, 0xd03d03d0, 0xa04a04a0, 0xa05a05a0, 0xa06a06a0,
        0xa07a07a0, 0xd04d04d0, 0xd05d05d0, 0xd06d06d0, 0xd07d07d0,
    };
    const uint32_t csa_first = csa_start >> 6;
    const uint32_t csa_second = (csa_start + 64) >> 6;
    const uint32_t csa_limit = (csa_start + 128) >> 6;
    const uint32_t initial_pcxi = 0;
    uint8_t frame[64];
    uc_engine *uc;
    uint32_t value;
    size_t i;

    memset(frame, 0xcc, sizeof(frame));
    store_le32(frame, csa_second);

    uc_common_setup(&uc, code, sizeof(code));
    OK(uc_mem_map(uc, csa_start, csa_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, csa_start, frame, sizeof(frame)));
    OK(uc_reg_write(uc, UC_TRICORE_REG_FCX, &csa_first));
    OK(uc_reg_write(uc, UC_TRICORE_REG_LCX, &csa_limit));
    OK(uc_reg_write(uc, UC_TRICORE_REG_PCXI, &initial_pcxi));
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        OK(uc_reg_write(uc, regs[i], &values[i]));
    }

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 1));

    OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &value));
    TEST_CHECK(value == code_start + 4);
    OK(uc_reg_read(uc, UC_TRICORE_REG_FCX, &value));
    TEST_CHECK(value == csa_second);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PCXI, &value));
    TEST_CHECK(value == csa_first);
    OK(uc_mem_read(uc, csa_start, frame, sizeof(frame)));
    TEST_CHECK(load_le32(frame) == initial_pcxi);
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        TEST_CHECK(load_le32(frame + 4 + i * 4) == values[i]);
        value = 0x5a5a0000 + i;
        OK(uc_reg_write(uc, regs[i], &value));
    }

    OK(uc_emu_start(uc, code_start + 4, code_start + sizeof(code), 0, 1));

    OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &value));
    TEST_CHECK(value == code_start + sizeof(code));
    OK(uc_reg_read(uc, UC_TRICORE_REG_FCX, &value));
    TEST_CHECK(value == csa_first);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PCXI, &value));
    TEST_CHECK(value == initial_pcxi);
    for (i = 0; i < sizeof(regs) / sizeof(regs[0]); i++) {
        OK(uc_reg_read(uc, regs[i], &value));
        TEST_CHECK(value == values[i]);
    }
    OK(uc_mem_read(uc, csa_start, frame, sizeof(frame)));
    TEST_CHECK(load_le32(frame) == csa_second);

    OK(uc_close(uc));
}

static void test_tricore_csa_exhaustion(void)
{
    const uint8_t code[] = {
        0x0d,
        0x00,
        0x00,
        0x02, /* svlcx */
    };
    uc_engine *uc;
    uc_err err;
    uint32_t btv = 0x30000;
    uint32_t fcx = 0;
    uint32_t value;

    uc_common_setup(&uc, code, sizeof(code));
    OK(uc_reg_write(uc, UC_TRICORE_REG_BTV, &btv));
    OK(uc_reg_write(uc, UC_TRICORE_REG_FCX, &fcx));
    err = uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0);
    TEST_CHECK_(err == UC_ERR_FETCH_UNMAPPED, "err = %u", (unsigned)err);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &value));
    TEST_CHECK(value == btv + 0x60);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D15, &value));
    TEST_CHECK(value == 4);
    OK(uc_reg_read(uc, UC_TRICORE_REG_A11, &value));
    TEST_CHECK(value == code_start);
    OK(uc_reg_read(uc, UC_TRICORE_REG_FCX, &value));
    TEST_CHECK(value == 0);

    OK(uc_close(uc));
}

static void test_tricore_integer_memory_branch(void)
{
    uint8_t code[24] = {0};
    size_t offset = 0;
    uc_engine *uc;
    uint32_t value;

    append_insn32(code, &offset,
                  tricore_rr(0x0b, 0x00, 3, 1, 2));           /* add d3,d1,d2 */
    append_insn32(code, &offset, tricore_bol(0x59, 3, 2, 0)); /* st.w [a2],d3 */
    append_insn32(code, &offset, tricore_bol(0x19, 4, 2, 0)); /* ld.w d4,[a2] */
    append_insn32(code, &offset,
                  tricore_brc(0xdf, false, 4, 7, 4)); /* jeq d4,7,+8 */
    append_insn16(code, &offset, 0x1582);             /* mov d5,1 */
    append_insn16(code, &offset, 0x0900);             /* nop */
    append_insn16(code, &offset, 0x2682);             /* mov d6,2 */

    uc_common_setup(&uc, code, offset);
    OK(uc_mem_map(uc, data_start, data_len, UC_PROT_ALL));
    value = 3;
    OK(uc_reg_write(uc, UC_TRICORE_REG_D1, &value));
    value = 4;
    OK(uc_reg_write(uc, UC_TRICORE_REG_D2, &value));
    value = data_start;
    OK(uc_reg_write(uc, UC_TRICORE_REG_A2, &value));

    OK(uc_emu_start(uc, code_start, code_start + offset, 0, 0));

    OK(uc_reg_read(uc, UC_TRICORE_REG_D3, &value));
    TEST_CHECK_(value == 7, "d3 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D4, &value));
    TEST_CHECK_(value == 7, "d4 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D5, &value));
    TEST_CHECK_(value == 0, "d5 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D6, &value));
    TEST_CHECK_(value == 2, "d6 = 0x%08x", value);
    OK(uc_mem_read(uc, data_start, &value, sizeof(value)));
    TEST_CHECK_(value == 7, "memory = 0x%08x", value);

    OK(uc_close(uc));
}

static void test_tricore_saturation_and_flags(void)
{
    uint8_t code[8];
    size_t offset = 0;
    uc_engine *uc;
    uint32_t value;

    append_insn32(code, &offset,
                  tricore_rr(0x0b, 0x02, 3, 1, 2)); /* adds d3,d1,d2 */
    append_insn32(code, &offset,
                  tricore_rr(0x0b, 0x5e, 4, 5, 0)); /* sat.b d4,d5 */

    uc_common_setup(&uc, code, offset);
    value = 0x7fffffff;
    OK(uc_reg_write(uc, UC_TRICORE_REG_D1, &value));
    value = 1;
    OK(uc_reg_write(uc, UC_TRICORE_REG_D2, &value));
    value = 0x180;
    OK(uc_reg_write(uc, UC_TRICORE_REG_D5, &value));

    OK(uc_emu_start(uc, code_start, code_start + offset, 0, 0));

    OK(uc_reg_read(uc, UC_TRICORE_REG_D3, &value));
    TEST_CHECK_(value == 0x7fffffff, "d3 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D4, &value));
    TEST_CHECK_(value == 0x7f, "d4 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PSW_USB_V, &value));
    TEST_CHECK_(value == 0x80000000, "V = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PSW_USB_SV, &value));
    TEST_CHECK_(value == 0x80000000, "SV = 0x%08x", value);

    OK(uc_close(uc));
}

static void test_tricore_circular_addressing(void)
{
    uint8_t code[8];
    size_t offset = 0;
    const uint32_t words[] = {0x11223344, 0x55667788};
    uc_engine *uc;
    uint32_t value;

    append_insn32(code, &offset, tricore_bo(0x29, 0x14, 1, 2, 4));
    append_insn32(code, &offset, tricore_bo(0x29, 0x14, 2, 2, 4));

    uc_common_setup(&uc, code, offset);
    OK(uc_mem_map(uc, data_start, data_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_start, words, sizeof(words)));
    value = data_start;
    OK(uc_reg_write(uc, UC_TRICORE_REG_A2, &value));
    value = (8u << 16) | 4;
    OK(uc_reg_write(uc, UC_TRICORE_REG_A3, &value));

    OK(uc_emu_start(uc, code_start, code_start + offset, 0, 0));

    OK(uc_reg_read(uc, UC_TRICORE_REG_D1, &value));
    TEST_CHECK_(value == words[1], "d1 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D2, &value));
    TEST_CHECK_(value == words[0], "d2 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_A3, &value));
    TEST_CHECK_(value == ((8u << 16) | 4), "a3 = 0x%08x", value);

    OK(uc_close(uc));
}

static void test_tricore_bit_reverse_addressing(void)
{
    const uint8_t bytes[] = {
        0x10, 0x11, 0x12, 0x13, 0x14, 0x15, 0x16, 0x17,
    };
    uint8_t code[16];
    size_t offset = 0;
    uc_engine *uc;
    uint32_t value;

    append_insn32(code, &offset, tricore_bo(0x29, 0x01, 0, 2, 0));
    append_insn32(code, &offset, tricore_bo(0x29, 0x01, 1, 2, 0));
    append_insn32(code, &offset, tricore_bo(0x29, 0x01, 2, 2, 0));
    append_insn32(code, &offset, tricore_bo(0x29, 0x01, 3, 2, 0));

    uc_common_setup(&uc, code, offset);
    OK(uc_mem_map(uc, data_start, data_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_start, bytes, sizeof(bytes)));
    value = data_start;
    OK(uc_reg_write(uc, UC_TRICORE_REG_A2, &value));
    value = 4u << 16;
    OK(uc_reg_write(uc, UC_TRICORE_REG_A3, &value));

    OK(uc_emu_start(uc, code_start, code_start + offset, 0, 0));

    OK(uc_reg_read(uc, UC_TRICORE_REG_D0, &value));
    TEST_CHECK_(value == bytes[0], "d0 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D1, &value));
    TEST_CHECK_(value == bytes[4], "d1 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D2, &value));
    TEST_CHECK_(value == bytes[2], "d2 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D3, &value));
    TEST_CHECK_(value == bytes[6], "d3 = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_A3, &value));
    TEST_CHECK_(value == ((4u << 16) | 1), "a3 = 0x%08x", value);

    OK(uc_close(uc));
}

static void test_tricore_fpu_rounding_and_flags(void)
{
    uint8_t code[8];
    size_t offset = 0;
    uc_engine *uc;
    uint32_t value = 0x3fc00000; /* 1.5f */
    uint32_t psw;

    append_insn32(code, &offset,
                  tricore_rr(0x4b, 0x10, 2, 1, 0)); /* ftoi d2,d1 */
    append_insn32(code, &offset,
                  tricore_rr(0x4b, 0x13, 3, 1, 0)); /* ftoiz d3,d1 */

    uc_common_setup(&uc, code, offset);
    OK(uc_reg_write(uc, UC_TRICORE_REG_D1, &value));
    OK(uc_emu_start(uc, code_start, code_start + offset, 0, 0));

    OK(uc_reg_read(uc, UC_TRICORE_REG_D2, &value));
    TEST_CHECK_(value == 2, "ftoi result=%u", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_D3, &value));
    TEST_CHECK_(value == 1, "ftoiz result=%u", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PSW, &psw));
    TEST_CHECK_((psw & 0x84000000) == 0x84000000, "psw=0x%08x", psw);

    OK(uc_close(uc));
}

static void test_tricore_fpu_divide_by_zero_status(void)
{
    uint8_t code[4];
    size_t offset = 0;
    uc_engine *uc;
    uint32_t value;

    append_insn32(code, &offset,
                  tricore_rr(0x4b, 0x05, 3, 1, 2)); /* div.f d3,d1,d2 */

    uc_common_setup(&uc, code, offset);
    value = 0x3f800000; /* 1.0f */
    OK(uc_reg_write(uc, UC_TRICORE_REG_D1, &value));
    value = 0;
    OK(uc_reg_write(uc, UC_TRICORE_REG_D2, &value));

    OK(uc_emu_start(uc, code_start, code_start + offset, 0, 0));

    OK(uc_reg_read(uc, UC_TRICORE_REG_D3, &value));
    TEST_CHECK_(value == 0x7f800000, "result = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PSW_USB_C, &value));
    TEST_CHECK_(value == 1, "FS = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PSW_USB_V, &value));
    TEST_CHECK_(value == 0, "FI = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PSW_USB_SV, &value));
    TEST_CHECK_(value == 0, "FV = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PSW_USB_AV, &value));
    TEST_CHECK_(value == 0x80000000, "FZ = 0x%08x", value);
    OK(uc_reg_read(uc, UC_TRICORE_REG_PSW_USB_SAV, &value));
    TEST_CHECK_(value == 0, "FU = 0x%08x", value);

    OK(uc_close(uc));
}

static void test_tricore_swap_word(void)
{
    uint8_t code[4];
    size_t offset = 0;
    uc_engine *uc;
    uint32_t value;

    append_insn32(code, &offset,
                  tricore_bo(0x49, 0x20, 0, 2, 0)); /* swap.w [a2],d0 */
    uc_common_setup(&uc, code, offset);
    OK(uc_mem_map(uc, data_start, data_len, UC_PROT_ALL));
    value = 0x11223344;
    OK(uc_mem_write(uc, data_start, &value, sizeof(value)));
    value = data_start;
    OK(uc_reg_write(uc, UC_TRICORE_REG_A2, &value));
    value = 0xaabbccdd;
    OK(uc_reg_write(uc, UC_TRICORE_REG_D0, &value));

    OK(uc_emu_start(uc, code_start, code_start + offset, 0, 0));

    OK(uc_reg_read(uc, UC_TRICORE_REG_D0, &value));
    TEST_CHECK_(value == 0x11223344, "d0 = 0x%08x", value);
    OK(uc_mem_read(uc, data_start, &value, sizeof(value)));
    TEST_CHECK_(value == 0xaabbccdd, "memory = 0x%08x", value);

    OK(uc_close(uc));
}

static void test_tricore_cmpswap_word(void)
{
    static const struct {
        uint32_t compare;
        uint32_t expected_memory;
    } cases[] = {
        {0x11223344, 0xaabbccdd},
        {0x55667788, 0x11223344},
    };
    uint8_t code[4];
    size_t offset = 0;
    size_t i;

    append_insn32(code, &offset,
                  tricore_bo(0x49, 0x23, 0, 2, 0)); /* cmpswap.w [a2],e0 */

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint8_t memory[4];
        uc_engine *uc;
        uint32_t value;

        uc_common_setup(&uc, code, offset);
        OK(uc_mem_map(uc, data_start, data_len, UC_PROT_ALL));
        store_le32(memory, 0x11223344);
        OK(uc_mem_write(uc, data_start, memory, sizeof(memory)));
        value = data_start;
        OK(uc_reg_write(uc, UC_TRICORE_REG_A2, &value));
        value = 0xaabbccdd;
        OK(uc_reg_write(uc, UC_TRICORE_REG_D0, &value));
        OK(uc_reg_write(uc, UC_TRICORE_REG_D1, &cases[i].compare));

        OK(uc_emu_start(uc, code_start, code_start + offset, 0, 0));

        OK(uc_reg_read(uc, UC_TRICORE_REG_D0, &value));
        TEST_CHECK_(value == 0x11223344, "case=%u d0=0x%08x",
                    (unsigned int)i, value);
        OK(uc_reg_read(uc, UC_TRICORE_REG_D1, &value));
        TEST_CHECK_(value == cases[i].compare, "case=%u d1=0x%08x",
                    (unsigned int)i, value);
        OK(uc_mem_read(uc, data_start, memory, sizeof(memory)));
        value = load_le32(memory);
        TEST_CHECK_(value == cases[i].expected_memory,
                    "case=%u memory=0x%08x", (unsigned int)i, value);

        OK(uc_close(uc));
    }
}

static void setup_tricore_exception_context(uc_engine *uc)
{
    uint8_t frame[64] = {0};
    uint32_t fcx = csa_start >> 6;
    uint32_t lcx = (csa_start + 64) >> 6;
    uint32_t btv = data_start;

    OK(uc_mem_map(uc, csa_start, csa_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, csa_start, frame, sizeof(frame)));
    OK(uc_reg_write(uc, UC_TRICORE_REG_FCX, &fcx));
    OK(uc_reg_write(uc, UC_TRICORE_REG_LCX, &lcx));
    OK(uc_reg_write(uc, UC_TRICORE_REG_BTV, &btv));
}

static void test_tricore_crc32_model_gating(void)
{
    static const struct {
        int model;
        bool supported;
    } cases[] = {
        {UC_CPU_TRICORE_TC1796, false},
        {UC_CPU_TRICORE_TC1797, false},
        {UC_CPU_TRICORE_TC27X, true},
    };
    uint8_t code[4];
    size_t offset = 0;
    size_t i;

    append_insn32(code, &offset,
                  tricore_rr(0x4b, 0x03, 3, 1, 2)); /* crc32 d3,d1,d2 */

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uc_engine *uc;
        uc_err err;
        uint32_t value = 0x12345678;
        uint32_t pc;

        OK(uc_open(UC_ARCH_TRICORE, UC_MODE_LITTLE_ENDIAN, &uc));
        OK(uc_ctl_set_cpu_model(uc, cases[i].model));
        uc_map_code(uc, code, offset);
        OK(uc_reg_write(uc, UC_TRICORE_REG_D1, &value));
        value = 0x89abcdef;
        OK(uc_reg_write(uc, UC_TRICORE_REG_D2, &value));

        if (!cases[i].supported) {
            setup_tricore_exception_context(uc);
        }
        err = uc_emu_start(uc, code_start, code_start + offset, 0, 0);

        if (cases[i].supported) {
            TEST_CHECK_(err == UC_ERR_OK, "model=%d err=%u", cases[i].model,
                        (unsigned int)err);
            OK(uc_reg_read(uc, UC_TRICORE_REG_D3, &value));
            TEST_CHECK_(value == 0x84f50443, "model=%d crc=0x%08x",
                        cases[i].model, value);
        } else {
            TEST_CHECK_(err == UC_ERR_FETCH_UNMAPPED, "model=%d err=%u",
                        cases[i].model, (unsigned int)err);
            OK(uc_reg_read(uc, UC_TRICORE_REG_PC, &pc));
            TEST_CHECK_(pc == data_start + 0x40, "model=%d pc=0x%08x",
                        cases[i].model, pc);
            OK(uc_reg_read(uc, UC_TRICORE_REG_D15, &value));
            TEST_CHECK_(value == 1, "model=%d tin=%u", cases[i].model, value);
        }

        OK(uc_close(uc));
    }
}

TEST_LIST = {
    {"test_tricore_mov_dreg", test_tricore_mov_dreg},
    {"test_tricore_csfr_reg_roundtrip", test_tricore_csfr_reg_roundtrip},
    {"test_tricore_cpu_model", test_tricore_cpu_model},
    {"test_tricore_fast_count_boundary", test_tricore_fast_count_boundary},
    {"test_tricore_context_roundtrip", test_tricore_context_roundtrip},
    {"test_tricore_call_ret_csa", test_tricore_call_ret_csa},
    {"test_tricore_svlcx_rslcx_csa", test_tricore_svlcx_rslcx_csa},
    {"test_tricore_csa_exhaustion", test_tricore_csa_exhaustion},
    {"test_tricore_integer_memory_branch", test_tricore_integer_memory_branch},
    {"test_tricore_saturation_and_flags", test_tricore_saturation_and_flags},
    {"test_tricore_circular_addressing", test_tricore_circular_addressing},
    {"test_tricore_bit_reverse_addressing",
     test_tricore_bit_reverse_addressing},
    {"test_tricore_fpu_rounding_and_flags",
     test_tricore_fpu_rounding_and_flags},
    {"test_tricore_fpu_divide_by_zero_status",
     test_tricore_fpu_divide_by_zero_status},
    {"test_tricore_swap_word", test_tricore_swap_word},
    {"test_tricore_cmpswap_word", test_tricore_cmpswap_word},
    {"test_tricore_crc32_model_gating", test_tricore_crc32_model_gating},
    {NULL, NULL},
};
