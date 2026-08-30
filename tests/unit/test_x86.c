#include "unicorn_test.h"

const uint64_t code_start = 0x1000;
const uint64_t code_len = 0x4000;

#define MEM_BASE 0x40000000
#define MEM_SIZE 1024 * 1024
#define MEM_STACK MEM_BASE + (MEM_SIZE / 2)
#define MEM_TEXT MEM_STACK + 4096
#define TEST_MSR_IA32_XFD 0x000001c4
#define TEST_MSR_IA32_XFD_ERR 0x000001c5
#define TEST_MSR_IA32_PKRS 0x000006e1
#define TEST_MSR_ARCH_LBR_CTL 0x000014ce
#define TEST_MSR_ARCH_LBR_DEPTH 0x000014cf
#define TEST_MSR_ARCH_LBR_FROM_0 0x00001500
#define TEST_MSR_ARCH_LBR_TO_0 0x00001600
#define TEST_MSR_ARCH_LBR_INFO_0 0x00001200
#define TEST_MSR_IA32_XSS 0x00000da0
#define TEST_X86_CPUID_7_0_EBX_AVX2 (1U << 5)
#define TEST_X86_CPUID_7_0_EBX_AVX512F (1U << 16)
#define TEST_X86_CPUID_7_0_EBX_AVX512DQ (1U << 17)
#define TEST_X86_CPUID_7_0_EBX_AVX512CD (1U << 28)
#define TEST_X86_CPUID_7_0_EBX_AVX512BW (1U << 30)
#define TEST_X86_CPUID_7_0_EBX_AVX512VL (1U << 31)
#define TEST_X86_CPUID_1_ECX_XSAVE (1U << 26)
#define TEST_X86_CPUID_7_0_ECX_PKU (1U << 3)
#define TEST_X86_CPUID_7_0_ECX_VAES (1U << 9)
#define TEST_X86_CPUID_7_0_ECX_VPCLMULQDQ (1U << 10)
#define TEST_X86_CPUID_D_1_EAX_XSAVEOPT (1U << 0)
#define TEST_X86_XSTATE_FP (1ULL << 0)
#define TEST_X86_XSTATE_SSE (1ULL << 1)
#define TEST_X86_XSTATE_YMM (1ULL << 2)
#define TEST_X86_XSTATE_OPMASK (1ULL << 5)
#define TEST_X86_XSTATE_ZMM_HI256 (1ULL << 6)
#define TEST_X86_XSTATE_HI16_ZMM (1ULL << 7)
#define TEST_X86_XSTATE_PKRU (1ULL << 9)
#define TEST_X86_XSAVE_AREA 0x200000ULL
#define TEST_X86_XSAVE_AREA_SIZE 0x2000
#define TEST_X86_XSAVE_HEADER_OFFSET 512

static void uc_common_setup(uc_engine **uc, uc_arch arch, uc_mode mode,
                            const char *code, uint64_t size)
{
    OK(uc_open(arch, mode, uc));
    OK(uc_mem_map(*uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(*uc, code_start, code, size));
}

typedef struct RegInfo_t {
    const char *file;
    int line;
    const char *name;
    uc_x86_reg reg;
    uint64_t value;
} RegInfo;

typedef struct QuickTest_t {
    uc_mode mode;
    uint8_t *code_data;
    size_t code_size;
    size_t in_count;
    RegInfo in_regs[32];
    size_t out_count;
    RegInfo out_regs[32];
} QuickTest;

static void QuickTest_run(QuickTest *test)
{
    uc_engine *uc;

    // initialize emulator in X86-64bit mode
    OK(uc_open(UC_ARCH_X86, test->mode, &uc));

    // map 1MB of memory for this emulation
    OK(uc_mem_map(uc, MEM_BASE, MEM_SIZE, UC_PROT_ALL));
    OK(uc_mem_write(uc, MEM_TEXT, test->code_data, test->code_size));
    if (test->mode == UC_MODE_64) {
        uint64_t stack_top = MEM_STACK;
        OK(uc_reg_write(uc, UC_X86_REG_RSP, &stack_top));
    } else {
        uint32_t stack_top = MEM_STACK;
        OK(uc_reg_write(uc, UC_X86_REG_ESP, &stack_top));
    }
    for (size_t i = 0; i < test->in_count; i++) {
        if (test->mode == UC_MODE_64) {
            OK(uc_reg_write(uc, test->in_regs[i].reg, &test->in_regs[i].value));
        } else {
            uint32_t reg = test->in_regs[i].value & 0xFFFFFFFF;
            OK(uc_reg_write(uc, test->in_regs[i].reg, &reg));
        }
    }
    OK(uc_emu_start(uc, MEM_TEXT, MEM_TEXT + test->code_size, 0, 0));
    for (size_t i = 0; i < test->out_count; i++) {
        RegInfo *out = &test->out_regs[i];
        if (test->mode == UC_MODE_64) {
            uint64_t value = 0;
            OK(uc_reg_read(uc, out->reg, &value));
            acutest_check_(value == out->value, out->file, out->line,
                           "OUT_REG(%s, 0x%" PRIx64 ") = 0x%" PRIx64 "",
                           out->name, out->value, value);
        } else {
            uint32_t value = 0;
            OK(uc_reg_read(uc, out->reg, &value));
            acutest_check_(value == (uint32_t)out->value, out->file, out->line,
                           "OUT_REG(%s, 0x%X) = 0x%X", out->name,
                           (uint32_t)out->value, value);
        }
    }
    OK(uc_mem_unmap(uc, MEM_BASE, MEM_SIZE));
    OK(uc_close(uc));
}

#define TEST_CODE(MODE, CODE)                                                  \
    QuickTest t;                                                               \
    memset(&t, 0, sizeof(t));                                                  \
    t.mode = MODE;                                                             \
    t.code_data = CODE;                                                        \
    t.code_size = sizeof(CODE)

#define TEST_IN_REG(NAME, VALUE)                                               \
    t.in_regs[t.in_count].file = __FILE__;                                     \
    t.in_regs[t.in_count].line = __LINE__;                                     \
    t.in_regs[t.in_count].name = #NAME;                                        \
    t.in_regs[t.in_count].reg = UC_X86_REG_##NAME;                             \
    t.in_regs[t.in_count].value = VALUE;                                       \
    t.in_count++

#define TEST_OUT_REG(NAME, VALUE)                                              \
    t.out_regs[t.out_count].file = __FILE__;                                   \
    t.out_regs[t.out_count].line = __LINE__;                                   \
    t.out_regs[t.out_count].name = #NAME;                                      \
    t.out_regs[t.out_count].reg = UC_X86_REG_##NAME;                           \
    t.out_regs[t.out_count].value = VALUE;                                     \
    t.out_count++

#define TEST_RUN() QuickTest_run(&t)

typedef struct _INSN_IN_RESULT {
    uint32_t port;
    int size;
} INSN_IN_RESULT;

static uint32_t test_x86_in_callback(uc_engine *uc, uint32_t port, int size,
                                     void *user_data)
{
    INSN_IN_RESULT *result = (INSN_IN_RESULT *)user_data;
    uint32_t eip;

    result->port = port;
    result->size = size;

    OK(uc_reg_read(uc, UC_X86_REG_EIP, (void*)&eip));
    TEST_CHECK(eip == code_start);

    return 0;
}

static void test_x86_in(void)
{
    uc_engine *uc;
    uc_hook hook;
    char code[] = "\xe5\x10"; // IN eax, 0x10
    INSN_IN_RESULT result;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_hook_add(uc, &hook, UC_HOOK_INSN, test_x86_in_callback, &result, 1, 0,
                   UC_X86_INS_IN));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    TEST_CHECK(result.port == 0x10);
    TEST_CHECK(result.size == 4);

    OK(uc_hook_del(uc, hook));
    OK(uc_close(uc));
}

typedef struct _INSN_OUT_RESULT {
    uint32_t port;
    int size;
    uint32_t value;
} INSN_OUT_RESULT;

static void test_x86_out_callback(uc_engine *uc, uint32_t port, int size,
                                  uint32_t value, void *user_data)
{
    INSN_OUT_RESULT *result = (INSN_OUT_RESULT *)user_data;

    result->port = port;
    result->size = size;
    result->value = value;
}

static void test_x86_out(void)
{
    uc_engine *uc;
    uc_hook hook;
    char code[] = "\xb0\x32\xe6\x46"; // MOV al, 0x32; OUT  0x46, al;
    INSN_OUT_RESULT result;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_hook_add(uc, &hook, UC_HOOK_INSN, test_x86_out_callback, &result, 1,
                   0, UC_X86_INS_OUT));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    TEST_CHECK(result.port == 0x46);
    TEST_CHECK(result.size == 1);
    TEST_CHECK(result.value == 0x32);

    OK(uc_hook_del(uc, hook));
    OK(uc_close(uc));
}

typedef struct _MEM_HOOK_RESULT {
    uc_mem_type type;
    uint64_t address;
    int size;
    uint64_t value;
} MEM_HOOK_RESULT;

typedef struct _MEM_HOOK_RESULTS {
    uint64_t count;
    MEM_HOOK_RESULT results[16];
} MEM_HOOK_RESULTS;

static void test_x86_record_mem_hook(uc_engine *uc, uc_mem_type type,
                                     uint64_t address, int size,
                                     int64_t value, void *user_data)
{
    MEM_HOOK_RESULTS *r = (MEM_HOOK_RESULTS *)user_data;
    uint64_t count = r->count;

    if (count >= 16) {
        TEST_ASSERT(false);
    }

    r->results[count].type = type;
    r->results[count].address = address;
    r->results[count].size = size;
    r->results[count].value = value;
    r->count++;

    if (type == UC_MEM_READ_UNMAPPED) {
        uc_mem_map(uc, address, 0x1000, UC_PROT_ALL);
    }
}

static void test_x86_mem_hook_all_callback(uc_engine *uc, uc_mem_type type,
                                           uint64_t address, int size,
                                           int64_t value, void *user_data)
{
    test_x86_record_mem_hook(uc, type, address, size, value, user_data);
}

static bool test_x86_invalid_mem_hook_all_callback(
    uc_engine *uc, uc_mem_type type, uint64_t address, int size,
    int64_t value, void *user_data)
{
    test_x86_record_mem_hook(uc, type, address, size, value, user_data);
    return true;
}

static void test_x86_mem_hook_all(void)
{
    uc_engine *uc;
    uc_hook invalid_hook;
    uc_hook valid_hook;
    // mov eax, 0xdeadbeef;
    // mov [0x8000], eax;
    // mov eax, [0x10000];
    char code[] =
        "\xb8\xef\xbe\xad\xde\xa3\x00\x80\x00\x00\xa1\x00\x00\x01\x00";
    MEM_HOOK_RESULTS r = {0};
    MEM_HOOK_RESULT expects[] = {
        {UC_MEM_FETCH, 0x1000, 5, 0},
        {UC_MEM_FETCH, 0x1005, 5, 0},
        {UC_MEM_WRITE, 0x8000, 4, 0xdeadbeef},
        {UC_MEM_FETCH, 0x100a, 5, 0},
        {UC_MEM_READ_UNMAPPED, 0x10000, 4, 0},
        {UC_MEM_READ, 0x10000, 4, 0},
    };

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, 0x8000, 0x1000, UC_PROT_ALL));
    OK(uc_hook_add(uc, &valid_hook, UC_HOOK_MEM_VALID,
                   test_x86_mem_hook_all_callback, &r, 1, 0));
    OK(uc_hook_add(uc, &invalid_hook, UC_HOOK_MEM_INVALID,
                   test_x86_invalid_mem_hook_all_callback, &r, 1, 0));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    TEST_ASSERT(r.count == sizeof(expects) / sizeof(expects[0]));
    for (int i = 0; i < r.count; i++) {
        TEST_CHECK(expects[i].type == r.results[i].type);
        TEST_CHECK(expects[i].address == r.results[i].address);
        TEST_CHECK(expects[i].size == r.results[i].size);
        TEST_CHECK(expects[i].value == r.results[i].value);
    }

    OK(uc_hook_del(uc, invalid_hook));
    OK(uc_hook_del(uc, valid_hook));
    OK(uc_close(uc));
}

typedef struct X86FetchTrace {
    uint64_t address;
    uint32_t pc;
    uint32_t size;
    uint32_t count;
} X86FetchTrace;

static void test_x86_fetch_callback(uc_engine *uc, uc_mem_type type,
                                    uint64_t address, int size,
                                    int64_t value, void *user_data)
{
    X86FetchTrace *trace = (X86FetchTrace *)user_data;

    TEST_CHECK(type == UC_MEM_FETCH);
    TEST_CHECK(value == 0);
    trace->count++;
    trace->address = address;
    trace->size = size;
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &trace->pc));
}

static void test_x86_invalid_decode_fetch_size(void)
{
    const uint8_t invalid_code[] = {0x0f, 0xff};
    const uint8_t valid_code[] = {0x40}; /* inc eax */
    const uint64_t valid_pc = code_start + 0x10;
    X86FetchTrace trace = {0};
    uint32_t eax = 0;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)invalid_code, sizeof(invalid_code));
    OK(uc_mem_write(uc, valid_pc, valid_code, sizeof(valid_code)));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_FETCH, test_x86_fetch_callback,
                   &trace, 1, 0));

    uc_assert_err(UC_ERR_INSN_INVALID,
                  uc_emu_start(uc, code_start,
                               code_start + sizeof(invalid_code), 0, 1));
    TEST_CHECK(trace.count == 1);
    TEST_CHECK(trace.address == code_start);
    TEST_CHECK(trace.pc == code_start);
    TEST_CHECK(trace.size == sizeof(invalid_code));

    OK(uc_emu_start(uc, valid_pc, valid_pc + sizeof(valid_code), 0, 1));
    TEST_CHECK(trace.count == 2);
    TEST_CHECK(trace.address == valid_pc);
    TEST_CHECK(trace.pc == valid_pc);
    TEST_CHECK(trace.size == sizeof(valid_code));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 1);

    OK(uc_hook_del(uc, hook));
    OK(uc_close(uc));
}

static void test_x86_inc_dec_pxor(void)
{
    uc_engine *uc;
    char code[] =
        "\x41\x4a\x66\x0f\xef\xc1"; // INC ecx; DEC edx; PXOR xmm0, xmm1
    int r_ecx = 0x1234;
    int r_edx = 0x7890;
    uint64_t r_xmm0[2] = {0x08090a0b0c0d0e0f, 0x0001020304050607};
    uint64_t r_xmm1[2] = {0x8090a0b0c0d0e0f0, 0x0010203040506070};

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));
    OK(uc_reg_write(uc, UC_X86_REG_XMM0, &r_xmm0));
    OK(uc_reg_write(uc, UC_X86_REG_XMM1, &r_xmm1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));
    OK(uc_reg_read(uc, UC_X86_REG_XMM0, &r_xmm0));

    TEST_CHECK(r_ecx == 0x1235);
    TEST_CHECK(r_edx == 0x788f);
    TEST_CHECK(r_xmm0[0] == 0x8899aabbccddeeff);
    TEST_CHECK(r_xmm0[1] == 0x0011223344556677);

    OK(uc_close(uc));
}

static void test_x86_avx_vpxor_ymm(void)
{
    uc_engine *uc;
    char code[] = "\xc5\xfd\xef\xc1";
    uint64_t ymm0[4] = {0x08090a0b0c0d0e0fULL, 0x0001020304050607ULL,
                        0x8899aabbccddeeffULL, 0x0011223344556677ULL};
    uint64_t ymm1[4] = {0x8090a0b0c0d0e0f0ULL, 0x0010203040506070ULL,
                        0x1020304050607080ULL, 0xfedcba9876543210ULL};

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_write(uc, UC_X86_REG_YMM1, &ymm1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));

    TEST_CHECK(ymm0[0] == 0x8899aabbccddeeffULL);
    TEST_CHECK(ymm0[1] == 0x0011223344556677ULL);
    TEST_CHECK(ymm0[2] == 0x98b99afb9cbd9e7fULL);
    TEST_CHECK(ymm0[3] == 0xfecd98ab32015467ULL);

    OK(uc_close(uc));
}

static void test_x86_avx_vex128_zero_upper_one(int cpu_model)
{
    uc_engine *uc;
    char code[] = "\xc5\xf1\xef\xc2";
    uint64_t ymm0[4] = {0xffffffffffffffffULL, 0xeeeeeeeeeeeeeeeeULL,
                        0xddddddddddddddddULL, 0xccccccccccccccccULL};
    uint64_t ymm1[4] = {0x0011223344556677ULL, 0x8899aabbccddeeffULL,
                        0x1020304050607080ULL, 0xfedcba9876543210ULL};
    uint64_t ymm2[4] = {0xff00ff00aa55aa55ULL, 0x123456789abcdef0ULL,
                        0x0f1e2d3c4b5a6978ULL, 0x8877665544332211ULL};
    uint64_t expected[4] = {0xff11dd33ee00cc22ULL, 0x9aadfcc35661300fULL,
                            0, 0};

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, cpu_model));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_write(uc, UC_X86_REG_YMM1, &ymm1));
    OK(uc_reg_write(uc, UC_X86_REG_YMM2, &ymm2));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));

    TEST_CHECK(memcmp(ymm0, expected, sizeof(ymm0)) == 0);

    OK(uc_close(uc));
}

static void test_x86_avx_vex128_zero_upper(void)
{
    test_x86_avx_vex128_zero_upper_one(UC_CPU_X86_HASWELL);
    test_x86_avx_vex128_zero_upper_one(UC_CPU_X86_ICELAKE_CLIENT);
}

static void test_x86_avx_scalar_zero_upper_ss_one(int cpu_model)
{
    uc_engine *uc;
    char code[] = "\xc5\xf2\x58\xc2";
    uint32_t ymm0[8] = {
        0xffffffff, 0xeeeeeeee, 0xdddddddd, 0xcccccccc,
        0xbbbbbbbb, 0xaaaaaaaa, 0x99999999, 0x88888888,
    };
    uint32_t ymm1[8] = {
        0x3fc00000, 0x11223344, 0x55667788, 0x99aabbcc,
        0x12345678, 0x23456789, 0x3456789a, 0x456789ab,
    };
    uint32_t ymm2[8] = {
        0x40100000, 0x80818283, 0x84858687, 0x88898a8b,
        0x8c8d8e8f, 0x90919293, 0x94959697, 0x98999a9b,
    };
    uint32_t expected[8] = {
        0x40700000, 0x11223344, 0x55667788, 0x99aabbcc,
        0, 0, 0, 0,
    };

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, cpu_model));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_write(uc, UC_X86_REG_YMM1, &ymm1));
    OK(uc_reg_write(uc, UC_X86_REG_YMM2, &ymm2));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));

    TEST_CHECK(memcmp(ymm0, expected, sizeof(ymm0)) == 0);

    OK(uc_close(uc));
}

static void test_x86_avx_scalar_zero_upper_sd_one(int cpu_model)
{
    uc_engine *uc;
    char code[] = "\xc5\xf3\x58\xc2";
    uint64_t ymm0[4] = {0xffffffffffffffffULL, 0xeeeeeeeeeeeeeeeeULL,
                        0xddddddddddddddddULL, 0xccccccccccccccccULL};
    uint64_t ymm1[4] = {0x3ff8000000000000ULL, 0x1122334455667788ULL,
                        0x123456789abcdef0ULL, 0x0f1e2d3c4b5a6978ULL};
    uint64_t ymm2[4] = {0x4002000000000000ULL, 0x8899aabbccddeeffULL,
                        0x1020304050607080ULL, 0xfedcba9876543210ULL};
    uint64_t expected[4] = {0x400e000000000000ULL, 0x1122334455667788ULL,
                            0, 0};

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, cpu_model));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_write(uc, UC_X86_REG_YMM1, &ymm1));
    OK(uc_reg_write(uc, UC_X86_REG_YMM2, &ymm2));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));

    TEST_CHECK(memcmp(ymm0, expected, sizeof(ymm0)) == 0);

    OK(uc_close(uc));
}

static void test_x86_avx_scalar_zero_upper(void)
{
    test_x86_avx_scalar_zero_upper_ss_one(UC_CPU_X86_HASWELL);
    test_x86_avx_scalar_zero_upper_sd_one(UC_CPU_X86_HASWELL);
    test_x86_avx_scalar_zero_upper_ss_one(UC_CPU_X86_ICELAKE_CLIENT);
    test_x86_avx_scalar_zero_upper_sd_one(UC_CPU_X86_ICELAKE_CLIENT);
}

static void test_x86_avx_fma_ps(void)
{
    uc_engine *uc;
    char code[] = "\xc4\xe2\x79\x98\xc1";
    uint32_t xmm0[4] = {
        0x40000000, 0x40400000, 0x40800000, 0x40a00000,
    };
    uint32_t xmm1[4] = {
        0x41200000, 0x41a00000, 0x41f00000, 0x42200000,
    };

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_XMM0, &xmm0));
    OK(uc_reg_write(uc, UC_X86_REG_XMM1, &xmm1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_XMM0, &xmm0));

    TEST_CHECK(xmm0[0] == 0x41b00000);
    TEST_CHECK(xmm0[1] == 0x427c0000);
    TEST_CHECK(xmm0[2] == 0x42f80000);
    TEST_CHECK(xmm0[3] == 0x434d0000);

    OK(uc_close(uc));
}

static void test_x86_fma_scalar_variants(void)
{
    uc_engine *uc;
    char code[] =
        "\xc4\xe2\x71\x99\xc2"
        "\xc4\xe2\xd9\xbf\xdd"
        "\xc4\xc2\x45\xa6\xf0";
    uint32_t xmm0[4] = {
        0x40000000, 0x41300000, 0x41400000, 0x41500000,
    };
    uint32_t xmm1[4] = {
        0x41200000, 0, 0, 0,
    };
    uint32_t xmm2[4] = {
        0x40400000, 0, 0, 0,
    };
    uint64_t xmm3[2] = {
        0x4014000000000000ULL, 0,
    };
    uint64_t xmm4[2] = {
        0x4000000000000000ULL, 0,
    };
    uint64_t xmm5[2] = {
        0x4008000000000000ULL, 0,
    };
    uint32_t ymm6[8] = {
        0x3f800000, 0x40000000, 0x40400000, 0x40800000,
        0x40a00000, 0x40c00000, 0x40e00000, 0x41000000,
    };
    uint32_t ymm7[8] = {
        0x40000000, 0x40000000, 0x40000000, 0x40000000,
        0x40000000, 0x40000000, 0x40000000, 0x40000000,
    };
    uint32_t ymm8[8] = {
        0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
        0x3f800000, 0x3f800000, 0x3f800000, 0x3f800000,
    };
    uint32_t expected6[8] = {
        0x3f800000, 0x40a00000, 0x40a00000, 0x41100000,
        0x41100000, 0x41500000, 0x41500000, 0x41880000,
    };

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_XMM0, &xmm0));
    OK(uc_reg_write(uc, UC_X86_REG_XMM1, &xmm1));
    OK(uc_reg_write(uc, UC_X86_REG_XMM2, &xmm2));
    OK(uc_reg_write(uc, UC_X86_REG_XMM3, &xmm3));
    OK(uc_reg_write(uc, UC_X86_REG_XMM4, &xmm4));
    OK(uc_reg_write(uc, UC_X86_REG_XMM5, &xmm5));
    OK(uc_reg_write(uc, UC_X86_REG_YMM6, &ymm6));
    OK(uc_reg_write(uc, UC_X86_REG_YMM7, &ymm7));
    OK(uc_reg_write(uc, UC_X86_REG_YMM8, &ymm8));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_XMM0, &xmm0));
    OK(uc_reg_read(uc, UC_X86_REG_XMM3, &xmm3));
    OK(uc_reg_read(uc, UC_X86_REG_YMM6, &ymm6));

    TEST_CHECK(xmm0[0] == 0x41800000);
    TEST_CHECK(xmm3[0] == 0xc026000000000000ULL);
    TEST_CHECK(memcmp(ymm6, expected6, sizeof(ymm6)) == 0);

    OK(uc_close(uc));
}

static void test_x86_avx2_broadcast_permute(void)
{
    uc_engine *uc;
    char code[] =
        "\xc4\xe2\x7d\x58\x00"
        "\xc4\xe2\x4d\x36\xef"
        "\xc4\xe2\x7d\x5a\x50\x20"
        "\xc4\xe3\x75\x46\xda\x21";
    uint64_t rax = code_start + 0x100;
    uint32_t value = 0x11223344;
    uint32_t block[4] = {
        0xa0a1a2a3, 0xb0b1b2b3, 0xc0c1c2c3, 0xd0d1d2d3,
    };
    uint32_t ymm0[8];
    uint32_t ymm1[8] = { 1, 2, 3, 4, 5, 6, 7, 8 };
    uint32_t ymm2[8];
    uint32_t ymm3[8];
    uint32_t ymm5[8];
    uint32_t ymm6[8] = { 7, 0, 6, 1, 5, 2, 4, 3 };
    uint32_t ymm7[8] = { 10, 20, 30, 40, 50, 60, 70, 80 };
    uint32_t expected3[8] = {
        5, 6, 7, 8,
        0xa0a1a2a3, 0xb0b1b2b3, 0xc0c1c2c3, 0xd0d1d2d3,
    };

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));
    OK(uc_mem_write(uc, rax, &value, sizeof(value)));
    OK(uc_mem_write(uc, rax + 0x20, block, sizeof(block)));

    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_YMM1, &ymm1));
    OK(uc_reg_write(uc, UC_X86_REG_YMM6, &ymm6));
    OK(uc_reg_write(uc, UC_X86_REG_YMM7, &ymm7));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_read(uc, UC_X86_REG_YMM2, &ymm2));
    OK(uc_reg_read(uc, UC_X86_REG_YMM3, &ymm3));
    OK(uc_reg_read(uc, UC_X86_REG_YMM5, &ymm5));

    for (size_t i = 0; i < 8; i++) {
        TEST_CHECK(ymm0[i] == value);
    }
    TEST_CHECK(ymm5[0] == 80);
    TEST_CHECK(ymm5[1] == 10);
    TEST_CHECK(ymm5[2] == 70);
    TEST_CHECK(ymm5[3] == 20);
    TEST_CHECK(ymm5[4] == 60);
    TEST_CHECK(ymm5[5] == 30);
    TEST_CHECK(ymm5[6] == 50);
    TEST_CHECK(ymm5[7] == 40);
    TEST_CHECK(memcmp(ymm2, block, sizeof(block)) == 0);
    TEST_CHECK(memcmp(&ymm2[4], block, sizeof(block)) == 0);
    TEST_CHECK(memcmp(ymm3, expected3, sizeof(ymm3)) == 0);

    OK(uc_close(uc));
}

static void test_x86_avx2_variable_shifts(void)
{
    uc_engine *uc;
    char code[] =
        "\xc4\xe2\x75\x47\xc2"
        "\xc4\xe2\x5d\x45\xdd"
        "\xc4\xc2\x45\x46\xf0"
        "\xc4\x42\xad\x47\xcb"
        "\xc4\x42\x95\x45\xe6";
    uint32_t ymm1[8] = {
        1, 2, 0x80000000, 0xffffffff,
        0x12345678, 0x7fffffff, 0x89abcdef, 0x00010000,
    };
    uint32_t ymm2[8] = { 0, 1, 4, 31, 32, 33, 8, 16 };
    uint32_t ymm4[8] = {
        0xffffffff, 0x80000000, 0x7fffffff, 0x12345678,
        1, 0x80000001, 0xf0000000, 0x00ff00ff,
    };
    uint32_t ymm5[8] = { 0, 1, 4, 31, 32, 33, 8, 16 };
    uint32_t ymm7[8] = {
        0xffffffff, 0x80000000, 0x7fffffff, 0x80000000,
        1, 0x80000000, 0xf0000000, 0x00ff00ff,
    };
    uint32_t ymm8[8] = { 0, 1, 4, 31, 32, 33, 8, 16 };
    uint64_t ymm10[4] = {
        1, 0x8000000000000000ULL,
        0x0123456789abcdefULL, 0xffffffffffffffffULL,
    };
    uint64_t ymm11[4] = { 0, 1, 64, 8 };
    uint64_t ymm13[4] = {
        0xffffffffffffffffULL, 0x8000000000000000ULL,
        0x0123456789abcdefULL, 0x00ff00ff00ff00ffULL,
    };
    uint64_t ymm14[4] = { 0, 1, 64, 8 };
    uint32_t expected0[8] = {
        1, 4, 0, 0x80000000, 0, 0, 0xabcdef00, 0,
    };
    uint32_t expected3[8] = {
        0xffffffff, 0x40000000, 0x07ffffff, 0, 0, 0,
        0x00f00000, 0x000000ff,
    };
    uint32_t expected6[8] = {
        0xffffffff, 0xc0000000, 0x07ffffff, 0xffffffff,
        0, 0xffffffff, 0xfff00000, 0x000000ff,
    };
    uint64_t expected9[4] = {
        1, 0, 0, 0xffffffffffffff00ULL,
    };
    uint64_t expected12[4] = {
        0xffffffffffffffffULL, 0x4000000000000000ULL,
        0, 0x0000ff00ff00ff00ULL,
    };
    uint32_t ymm0[8];
    uint32_t ymm3[8];
    uint32_t ymm6[8];
    uint64_t ymm9[4];
    uint64_t ymm12[4];

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_YMM1, &ymm1));
    OK(uc_reg_write(uc, UC_X86_REG_YMM2, &ymm2));
    OK(uc_reg_write(uc, UC_X86_REG_YMM4, &ymm4));
    OK(uc_reg_write(uc, UC_X86_REG_YMM5, &ymm5));
    OK(uc_reg_write(uc, UC_X86_REG_YMM7, &ymm7));
    OK(uc_reg_write(uc, UC_X86_REG_YMM8, &ymm8));
    OK(uc_reg_write(uc, UC_X86_REG_YMM10, &ymm10));
    OK(uc_reg_write(uc, UC_X86_REG_YMM11, &ymm11));
    OK(uc_reg_write(uc, UC_X86_REG_YMM13, &ymm13));
    OK(uc_reg_write(uc, UC_X86_REG_YMM14, &ymm14));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_read(uc, UC_X86_REG_YMM3, &ymm3));
    OK(uc_reg_read(uc, UC_X86_REG_YMM6, &ymm6));
    OK(uc_reg_read(uc, UC_X86_REG_YMM9, &ymm9));
    OK(uc_reg_read(uc, UC_X86_REG_YMM12, &ymm12));

    TEST_CHECK(memcmp(ymm0, expected0, sizeof(ymm0)) == 0);
    TEST_CHECK(memcmp(ymm3, expected3, sizeof(ymm3)) == 0);
    TEST_CHECK(memcmp(ymm6, expected6, sizeof(ymm6)) == 0);
    TEST_CHECK(memcmp(ymm9, expected9, sizeof(ymm9)) == 0);
    TEST_CHECK(memcmp(ymm12, expected12, sizeof(ymm12)) == 0);

    OK(uc_close(uc));
}

static void test_x86_avx2_mask_gather(void)
{
    uc_engine *uc;
    char code[] =
        "\xc4\xe2\x45\x8c\x30"
        "\xc4\xe2\x6d\x90\x04\x88";
    uint64_t rax = code_start + 0x100;
    uint32_t data[8] = {
        0x10001000, 0x20002000, 0x30003000, 0x40004000,
        0x50005000, 0x60006000, 0x70007000, 0x80008000,
    };
    uint32_t ymm0[8] = {
        0x11111111, 0x22222222, 0x33333333, 0x44444444,
        0x55555555, 0x66666666, 0x77777777, 0x88888888,
    };
    uint32_t ymm1[8] = { 7, 0, 5, 2, 1, 4, 3, 6 };
    uint32_t ymm2[8] = {
        0x80000000, 0, 0xffffffff, 0,
        0x80000000, 0x7fffffff, 0, 0xffffffff,
    };
    uint32_t ymm7[8] = {
        0x80000000, 0, 0xffffffff, 0x7fffffff,
        0x80000000, 0, 0xffffffff, 0,
    };
    uint32_t expected0[8] = {
        0x80008000, 0x22222222, 0x60006000, 0x44444444,
        0x20002000, 0x66666666, 0x77777777, 0x70007000,
    };
    uint32_t expected2[8] = { 0 };
    uint32_t expected6[8] = {
        0x10001000, 0, 0x30003000, 0,
        0x50005000, 0, 0x70007000, 0,
    };
    uint32_t ymm6[8] = { 0 };

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));
    OK(uc_mem_write(uc, rax, data, sizeof(data)));

    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_write(uc, UC_X86_REG_YMM1, &ymm1));
    OK(uc_reg_write(uc, UC_X86_REG_YMM2, &ymm2));
    OK(uc_reg_write(uc, UC_X86_REG_YMM7, &ymm7));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_read(uc, UC_X86_REG_YMM2, &ymm2));
    OK(uc_reg_read(uc, UC_X86_REG_YMM6, &ymm6));

    TEST_CHECK(memcmp(ymm0, expected0, sizeof(ymm0)) == 0);
    TEST_CHECK(memcmp(ymm2, expected2, sizeof(ymm2)) == 0);
    TEST_CHECK(memcmp(ymm6, expected6, sizeof(ymm6)) == 0);

    OK(uc_close(uc));
}

static void test_x86_avx_vzeroall(void)
{
    uc_engine *uc;
    char code[] = "\xc5\xfc\x77";
    uint64_t ymm0[4] = {
        0x1111111111111111ULL, 0x2222222222222222ULL,
        0x3333333333333333ULL, 0x4444444444444444ULL,
    };
    uint64_t ymm15[4] = {
        0x5555555555555555ULL, 0x6666666666666666ULL,
        0x7777777777777777ULL, 0x8888888888888888ULL,
    };

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_write(uc, UC_X86_REG_YMM15, &ymm15));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_read(uc, UC_X86_REG_YMM15, &ymm15));

    for (size_t i = 0; i < 4; i++) {
        TEST_CHECK(ymm0[i] == 0);
        TEST_CHECK(ymm15[i] == 0);
    }

    OK(uc_close(uc));
}

static void test_x86_aes_pclmul(void)
{
    uc_engine *uc;
    char code[] =
        "\x66\x0f\x38\xdc\xc1"
        "\x66\x0f\x38\xde\xd3"
        "\x66\x0f\x38\xdb\xe5"
        "\x66\x0f\x3a\xdf\xf7\x1b"
        "\x66\x45\x0f\x3a\x44\xc1\x11";
    uint8_t xmm0[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    uint8_t xmm1[16] = {
        0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08,
        0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    };
    uint8_t xmm2[16] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
    };
    uint8_t xmm3[16] = {
        0x13, 0x11, 0x1d, 0x7f, 0xe3, 0x94, 0x4a, 0x17,
        0xf3, 0x07, 0xa7, 0x8b, 0x4d, 0x2b, 0x30, 0xc5,
    };
    uint8_t xmm5[16] = {
        0xac, 0x19, 0x28, 0x57, 0x77, 0xfa, 0xd1, 0x5c,
        0x66, 0xdc, 0x29, 0x00, 0xf3, 0x21, 0x41, 0x6a,
    };
    uint8_t xmm7[16] = {
        0x2b, 0x7e, 0x15, 0x16, 0x28, 0xae, 0xd2, 0xa6,
        0xab, 0xf7, 0x15, 0x88, 0x09, 0xcf, 0x4f, 0x3c,
    };
    uint8_t xmm8[16] = {
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };
    uint8_t xmm9[16] = {
        0x55, 0xaa, 0x00, 0xff, 0x11, 0xee, 0x22, 0xdd,
        0x33, 0xcc, 0x44, 0xbb, 0x55, 0xaa, 0x66, 0x99,
    };
    const uint8_t expected_xmm0[16] = {
        0x6c, 0x77, 0xeb, 0xd5, 0xff, 0x6d, 0xf2, 0x7e,
        0xaa, 0x00, 0x39, 0xf0, 0xd1, 0xe9, 0x8b, 0xa3,
    };
    const uint8_t expected_xmm2[16] = {
        0xd4, 0x4f, 0x0a, 0xfb, 0xa3, 0x23, 0x94, 0xd3,
        0x52, 0x84, 0x00, 0xc6, 0x83, 0x41, 0x84, 0x98,
    };
    const uint8_t expected_xmm4[16] = {
        0x3b, 0x98, 0x30, 0x59, 0xd8, 0x02, 0x7e, 0xa4,
        0x49, 0x17, 0x3b, 0xf6, 0xc2, 0xa6, 0x99, 0x04,
    };
    const uint8_t expected_xmm6[16] = {
        0x34, 0xe4, 0xb5, 0x24, 0xff, 0xb5, 0x24, 0x34,
        0x01, 0x8a, 0x84, 0xeb, 0x91, 0x84, 0xeb, 0x01,
    };
    const uint8_t expected_xmm8[16] = {
        0x33, 0xf9, 0xa9, 0x26, 0x51, 0x89, 0xaf, 0x7e,
        0x13, 0xd9, 0xa9, 0x26, 0x71, 0xa9, 0xaf, 0x7e,
    };

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_XMM0, &xmm0));
    OK(uc_reg_write(uc, UC_X86_REG_XMM1, &xmm1));
    OK(uc_reg_write(uc, UC_X86_REG_XMM2, &xmm2));
    OK(uc_reg_write(uc, UC_X86_REG_XMM3, &xmm3));
    OK(uc_reg_write(uc, UC_X86_REG_XMM5, &xmm5));
    OK(uc_reg_write(uc, UC_X86_REG_XMM7, &xmm7));
    OK(uc_reg_write(uc, UC_X86_REG_XMM8, &xmm8));
    OK(uc_reg_write(uc, UC_X86_REG_XMM9, &xmm9));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_XMM0, &xmm0));
    OK(uc_reg_read(uc, UC_X86_REG_XMM2, &xmm2));
    OK(uc_reg_read(uc, UC_X86_REG_XMM4, &xmm5));
    OK(uc_reg_read(uc, UC_X86_REG_XMM6, &xmm7));
    OK(uc_reg_read(uc, UC_X86_REG_XMM8, &xmm8));

    TEST_CHECK(memcmp(xmm0, expected_xmm0, sizeof(xmm0)) == 0);
    TEST_CHECK(memcmp(xmm2, expected_xmm2, sizeof(xmm2)) == 0);
    TEST_CHECK(memcmp(xmm5, expected_xmm4, sizeof(xmm5)) == 0);
    TEST_CHECK(memcmp(xmm7, expected_xmm6, sizeof(xmm7)) == 0);
    TEST_CHECK(memcmp(xmm8, expected_xmm8, sizeof(xmm8)) == 0);

    OK(uc_close(uc));
}

static uint32_t test_x86_cpuid_7_0_ecx(uc_cpu_x86 cpu_model)
{
    uc_engine *uc;
    char code[] = "\x0f\xa2";
    uint32_t eax = 7;
    uint32_t ecx = 0;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, cpu_model));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));

    OK(uc_close(uc));
    return ecx;
}

typedef struct X86CpuidResult {
    uint32_t eax;
    uint32_t ebx;
    uint32_t ecx;
    uint32_t edx;
} X86CpuidResult;

static X86CpuidResult test_x86_cpuid(uc_cpu_x86 cpu_model, uint32_t leaf,
                                     uint32_t subleaf)
{
    const char code[] = "\x0f\xa2";
    X86CpuidResult result = {leaf, 0, subleaf, 0};
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, cpu_model));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &result.eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &result.ecx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &result.eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &result.ebx));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &result.ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &result.edx));

    OK(uc_close(uc));
    return result;
}

static uint32_t test_x86_cpuid_7_0_ebx(uc_cpu_x86 cpu_model)
{
    uc_engine *uc;
    char code[] = "\x0f\xa2";
    uint32_t eax = 7;
    uint32_t ebx;
    uint32_t ecx = 0;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, cpu_model));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));

    OK(uc_close(uc));
    return ebx;
}

static void test_x86_avx512_tcg_mask(void)
{
    const uint32_t avx512_mask = TEST_X86_CPUID_7_0_EBX_AVX512F |
                                 TEST_X86_CPUID_7_0_EBX_AVX512DQ |
                                 TEST_X86_CPUID_7_0_EBX_AVX512CD |
                                 TEST_X86_CPUID_7_0_EBX_AVX512BW |
                                 TEST_X86_CPUID_7_0_EBX_AVX512VL;
    uint32_t ebx;

    ebx = test_x86_cpuid_7_0_ebx(UC_CPU_X86_SKYLAKE_SERVER);
    TEST_CHECK((ebx & TEST_X86_CPUID_7_0_EBX_AVX2) != 0);
    TEST_CHECK((ebx & avx512_mask) == 0);

    ebx = test_x86_cpuid_7_0_ebx(UC_CPU_X86_ICELAKE_SERVER);
    TEST_CHECK((ebx & TEST_X86_CPUID_7_0_EBX_AVX2) != 0);
    TEST_CHECK((ebx & avx512_mask) == 0);
}

static void test_x86_vaes_vex_gating(void)
{
    uc_engine *uc;
    char vaesenc_xmm[] = "\xc4\xe2\x79\xdc\xc1";
    char vaesenc_ymm[] = "\xc4\xe2\x7d\xdc\xc1";
    char vaes_ymm[] =
        "\xc4\xe2\x7d\xdc\xc1"
        "\xc4\xe2\x6d\xde\xd3";
    uint8_t xmm0[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    uint8_t xmm1[16] = {
        0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08,
        0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
    };
    uint8_t ymm0[32] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };
    uint8_t ymm1[32] = {
        0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08,
        0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
        0x55, 0xaa, 0x00, 0xff, 0x11, 0xee, 0x22, 0xdd,
        0x33, 0xcc, 0x44, 0xbb, 0x55, 0xaa, 0x66, 0x99,
    };
    uint8_t ymm2[32] = {
        0x69, 0xc4, 0xe0, 0xd8, 0x6a, 0x7b, 0x04, 0x30,
        0xd8, 0xcd, 0xb7, 0x80, 0x70, 0xb4, 0xc5, 0x5a,
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };
    uint8_t ymm3[32] = {
        0x13, 0x11, 0x1d, 0x7f, 0xe3, 0x94, 0x4a, 0x17,
        0xf3, 0x07, 0xa7, 0x8b, 0x4d, 0x2b, 0x30, 0xc5,
        0x55, 0xaa, 0x00, 0xff, 0x11, 0xee, 0x22, 0xdd,
        0x33, 0xcc, 0x44, 0xbb, 0x55, 0xaa, 0x66, 0x99,
    };
    const uint8_t expected_xmm0[16] = {
        0x6c, 0x77, 0xeb, 0xd5, 0xff, 0x6d, 0xf2, 0x7e,
        0xaa, 0x00, 0x39, 0xf0, 0xd1, 0xe9, 0x8b, 0xa3,
    };
    const uint8_t expected_ymm0[32] = {
        0x6c, 0x77, 0xeb, 0xd5, 0xff, 0x6d, 0xf2, 0x7e,
        0xaa, 0x00, 0x39, 0xf0, 0xd1, 0xe9, 0x8b, 0xa3,
        0x6c, 0xfe, 0x98, 0x85, 0x72, 0x00, 0x6b, 0xfc,
        0xf6, 0xaf, 0xcc, 0x10, 0x66, 0x5f, 0x61, 0xdf,
    };
    const uint8_t expected_ymm2[32] = {
        0xd4, 0x4f, 0x0a, 0xfb, 0xa3, 0x23, 0x94, 0xd3,
        0x52, 0x84, 0x00, 0xc6, 0x83, 0x41, 0x84, 0x98,
        0x3b, 0xc6, 0x56, 0xbd, 0x3d, 0x4c, 0xe5, 0x5d,
        0x85, 0x0f, 0xac, 0x73, 0x29, 0xbf, 0xc3, 0x09,
    };

    TEST_CHECK((test_x86_cpuid_7_0_ecx(UC_CPU_X86_HASWELL) &
                TEST_X86_CPUID_7_0_ECX_VAES) == 0);
    TEST_CHECK((test_x86_cpuid_7_0_ecx(UC_CPU_X86_ICELAKE_CLIENT) &
                TEST_X86_CPUID_7_0_ECX_VAES) != 0);

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, vaesenc_xmm, sizeof(vaesenc_xmm) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_XMM0, &xmm0));
    OK(uc_reg_write(uc, UC_X86_REG_XMM1, &xmm1));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(vaesenc_xmm) - 1,
                    0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_XMM0, &xmm0));
    TEST_CHECK(memcmp(xmm0, expected_xmm0, sizeof(xmm0)) == 0);
    OK(uc_close(uc));

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, vaesenc_ymm, sizeof(vaesenc_ymm) - 1));
    uc_assert_err(UC_ERR_INSN_INVALID,
                  uc_emu_start(uc, code_start,
                               code_start + sizeof(vaesenc_ymm) - 1, 0, 0));
    OK(uc_close(uc));

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_ICELAKE_CLIENT));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, vaes_ymm, sizeof(vaes_ymm) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_write(uc, UC_X86_REG_YMM1, &ymm1));
    OK(uc_reg_write(uc, UC_X86_REG_YMM2, &ymm2));
    OK(uc_reg_write(uc, UC_X86_REG_YMM3, &ymm3));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(vaes_ymm) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_read(uc, UC_X86_REG_YMM2, &ymm2));
    TEST_CHECK(memcmp(ymm0, expected_ymm0, sizeof(ymm0)) == 0);
    TEST_CHECK(memcmp(ymm2, expected_ymm2, sizeof(ymm2)) == 0);
    OK(uc_close(uc));
}

static void test_x86_vpclmulqdq_tcg_mask(void)
{
    uc_engine *uc;
    char pclmul_xmm[] = "\xc4\xe3\x79\x44\xc1\x11";
    char pclmul_ymm[] = "\xc4\xe3\x7d\x44\xc1\x11";
    uint8_t ymm0[32] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
        0x10, 0x32, 0x54, 0x76, 0x98, 0xba, 0xdc, 0xfe,
        0x01, 0x23, 0x45, 0x67, 0x89, 0xab, 0xcd, 0xef,
    };
    uint8_t ymm1[32] = {
        0x0f, 0x0e, 0x0d, 0x0c, 0x0b, 0x0a, 0x09, 0x08,
        0x07, 0x06, 0x05, 0x04, 0x03, 0x02, 0x01, 0x00,
        0x55, 0xaa, 0x00, 0xff, 0x11, 0xee, 0x22, 0xdd,
        0x33, 0xcc, 0x44, 0xbb, 0x55, 0xaa, 0x66, 0x99,
    };
    const uint8_t expected_ymm0[32] = {
        0xb8, 0xfc, 0xa8, 0x02, 0x00, 0xff, 0x10, 0x01,
        0xa8, 0xfd, 0xb8, 0x03, 0x10, 0xfe, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00,
    };

    TEST_CHECK((test_x86_cpuid_7_0_ecx(UC_CPU_X86_HASWELL) &
                TEST_X86_CPUID_7_0_ECX_VPCLMULQDQ) == 0);
    TEST_CHECK((test_x86_cpuid_7_0_ecx(UC_CPU_X86_ICELAKE_CLIENT) &
                TEST_X86_CPUID_7_0_ECX_VPCLMULQDQ) == 0);

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, pclmul_xmm, sizeof(pclmul_xmm) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_YMM0, &ymm0));
    OK(uc_reg_write(uc, UC_X86_REG_YMM1, &ymm1));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(pclmul_xmm) - 1,
                    0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_YMM0, &ymm0));
    TEST_CHECK(memcmp(ymm0, expected_ymm0, sizeof(ymm0)) == 0);
    OK(uc_close(uc));

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_ICELAKE_CLIENT));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, pclmul_ymm, sizeof(pclmul_ymm) - 1));
    uc_assert_err(UC_ERR_INSN_INVALID,
                  uc_emu_start(uc, code_start,
                               code_start + sizeof(pclmul_ymm) - 1, 0, 0));

    OK(uc_close(uc));
}

static void test_x86_relative_jump(void)
{
    uc_engine *uc;
    char code[] = "\xeb\x02\x90\x90\x90\x90\x90\x90"; // jmp 4; nop; nop; nop;
                                                      // nop; nop; nop
    int r_eip;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    OK(uc_emu_start(uc, code_start, code_start + 4, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EIP, &r_eip));

    TEST_CHECK(r_eip == code_start + 4);

    OK(uc_close(uc));
}

static void test_x86_loop(void)
{
    uc_engine *uc;
    char code[] = "\x41\x4a\xeb\xfe"; // inc ecx; dec edx; jmp $;
    int r_ecx = 0x1234;
    int r_edx = 0x7890;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 1 * 1000000,
                    0));

    OK(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));

    TEST_CHECK(r_ecx == 0x1235);
    TEST_CHECK(r_edx == 0x788f);

    OK(uc_close(uc));
}

static void test_x86_invalid_mem_read(void)
{
    uc_engine *uc;
    char code[] = "\x8b\x0d\xaa\xaa\xaa\xaa"; // mov  ecx, [0xAAAAAAAA]

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    uc_assert_err(
        UC_ERR_READ_UNMAPPED,
        uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

static void test_x86_invalid_mem_write(void)
{
    uc_engine *uc;
    char code[] = "\x89\x0d\xaa\xaa\xaa\xaa"; // mov  ecx, [0xAAAAAAAA]

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    uc_assert_err(
        UC_ERR_WRITE_UNMAPPED,
        uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

static void test_x86_invalid_jump(void)
{
    uc_engine *uc;
    char code[] = "\xe9\xe9\xee\xee\xee"; // jmp 0xEEEEEEEE

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    uc_assert_err(
        UC_ERR_FETCH_UNMAPPED,
        uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

static void test_x86_64_syscall_callback(uc_engine *uc, void *user_data)
{
    uint64_t rax;

    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));

    TEST_CHECK(rax == 0x100);
}

static void test_x86_64_syscall(void)
{
    uc_engine *uc;
    uc_hook hook;
    char code[] = "\x0f\x05"; // syscall
    uint64_t r_rax = 0x100;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_hook_add(uc, &hook, UC_HOOK_INSN, test_x86_64_syscall_callback, NULL,
                   1, 0, UC_X86_INS_SYSCALL));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_hook_del(uc, hook));
    OK(uc_close(uc));
}

static void test_x86_16_add(void)
{
    uc_engine *uc;
    char code[] = "\x00\x00"; // add   byte ptr [bx + si], al
    uint16_t r_ax = 7;
    uint16_t r_bx = 5;
    uint16_t r_si = 6;
    uint8_t result;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_16, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, 0, 0x1000, UC_PROT_ALL));
    OK(uc_reg_write(uc, UC_X86_REG_AX, &r_ax));
    OK(uc_reg_write(uc, UC_X86_REG_BX, &r_bx));
    OK(uc_reg_write(uc, UC_X86_REG_SI, &r_si));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_mem_read(uc, r_bx + r_si, &result, 1));
    TEST_CHECK(result == 7);
    OK(uc_close(uc));
}

static void test_x86_reg_save(void)
{
    uc_engine *uc;
    uc_context *ctx;
    char code[] = "\x40"; // inc eax
    int r_eax = 1;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &r_eax));

    OK(uc_context_alloc(uc, &ctx));
    OK(uc_context_save(uc, ctx));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &r_eax));
    TEST_CHECK(r_eax == 2);

    OK(uc_context_restore(uc, ctx));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &r_eax));
    TEST_CHECK(r_eax == 1);

    OK(uc_context_free(ctx));
    OK(uc_close(uc));
}

static void
test_x86_invalid_mem_read_stop_in_cb_callback(uc_engine *uc, uc_mem_type type,
                                              uint64_t address, int size,
                                              int64_t value, void *user_data)
{
}

static void test_x86_invalid_mem_read_stop_in_cb(void)
{
    uc_engine *uc;
    uc_hook hook;
    char code[] = "\x40\x8b\x1d\x00\x00\x10\x00\x42"; // inc eax; mov ebx,
                                                      // [0x100000]; inc edx
    int r_eax = 0x1234;
    int r_edx = 0x5678;
    int r_eip = 0;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_invalid_mem_read_stop_in_cb_callback, NULL, 1, 0));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));

    uc_assert_err(
        UC_ERR_READ_UNMAPPED,
        uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    // The state of Unicorn should be correct at this time.
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &r_eip));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));

    TEST_CHECK(r_eip == code_start + 1);
    TEST_CHECK(r_eax == 0x1235);
    TEST_CHECK(r_edx == 0x5678);

    OK(uc_close(uc));
}

static void test_x86_x87_fnstenv_callback(uc_engine *uc, uint64_t address,
                                          uint32_t size, void *user_data)
{
    uint32_t r_eip;
    uint32_t r_eax;
    uint32_t fnstenv[7];

    if (address == code_start + 4) { // The first fnstenv executed
        // Save the address of the fld.
        OK(uc_reg_read(uc, UC_X86_REG_EIP, &r_eip));
        *((uint32_t *)user_data) = r_eip;

        OK(uc_reg_read(uc, UC_X86_REG_EAX, &r_eax));
        OK(uc_mem_read(uc, r_eax, fnstenv, sizeof(fnstenv)));
        // Don't update FCS:FIP for fnop.
        TEST_CHECK(fnstenv[3] == 0);
    }
}

static void test_x86_x87_fnstenv(void)
{
    uc_engine *uc;
    uc_hook hook;
    char code[] =
        "\xd9\xd0\xd9\x30\xd9\x00\xd9\x30"; // fnop;fnstenv [eax];fld dword ptr
                                            // [eax];fnstenv [eax]
    uint32_t base = code_start + 3 * code_len;
    uint32_t last_eip;
    uint32_t fnstenv[7];

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, base, code_len, UC_PROT_ALL));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &base));

    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE, test_x86_x87_fnstenv_callback,
                   &last_eip, 1, 0));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_mem_read(uc, base, fnstenv, sizeof(fnstenv)));
    // But update FCS:FIP for fld.
    TEST_CHECK(LEINT32(fnstenv[3]) == last_eip);

    OK(uc_close(uc));
}

static uint64_t test_x86_mmio_read_callback(uc_engine *uc, uint64_t offset,
                                            unsigned size, void *user_data)
{
    TEST_CHECK(offset == 4);
    TEST_CHECK(size == 4);

    return 0x19260817;
}

static void test_x86_mmio_write_callback(uc_engine *uc, uint64_t offset,
                                         unsigned size, uint64_t value,
                                         void *user_data)
{
    TEST_CHECK(offset == 4);
    TEST_CHECK(size == 4);
    TEST_CHECK(value == 0xdeadbeef);

    return;
}

static void test_x86_mmio(void)
{
    uc_engine *uc;
    int r_ecx = 0xdeadbeef;
    char code[] =
        "\x89\x0d\x04\x00\x02\x00\x8b\x0d\x04\x00\x02\x00"; // mov [0x20004],
                                                            // ecx; mov ecx,
                                                            // [0x20004]

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_mmio_map(uc, 0x20000, 0x1000, test_x86_mmio_read_callback, NULL,
                   test_x86_mmio_write_callback, NULL));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));

    TEST_CHECK(r_ecx == 0x19260817);

    OK(uc_close(uc));
}

typedef struct X86ReadAfterExitData {
    uc_err stop_error;
    uint32_t count;
    uint32_t value;
    bool stop;
} X86ReadAfterExitData;

static void test_x86_read_after_exit_callback(uc_engine *uc,
                                               uc_mem_type type,
                                               uint64_t address, int size,
                                               int64_t value, void *user_data)
{
    X86ReadAfterExitData *data = (X86ReadAfterExitData *)user_data;

    TEST_CHECK(type == UC_MEM_READ_AFTER);
    TEST_CHECK(address == 0x200000);
    TEST_CHECK(size == 4);
    data->count++;
    data->value = (uint32_t)value;
    if (data->stop) {
        data->stop_error = uc_emu_stop(uc);
    }
}

static void test_x86_cputlb_read_after_exit(void)
{
    const uint8_t code[] = {
        0xa1, 0x00, 0x00, 0x20, 0x00, /* mov eax,[0x200000] */
        0x43,                         /* inc ebx */
    };
    const uint32_t memory_value = 0x44332211;
    const uint32_t initial_eax = 0xdeadbeef;
    X86ReadAfterExitData data = {.stop = true};
    uint32_t eax = initial_eax;
    uint32_t ebx = 0;
    uint32_t eip = 0;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_mem_map(uc, 0x200000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x200000, &memory_value, sizeof(memory_value)));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ_AFTER,
                   test_x86_read_after_exit_callback, &data, 0x200000,
                   0x200003));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(data.stop_error);
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    TEST_CHECK(data.count == 1);
    TEST_CHECK(data.value == memory_value);
    TEST_CHECK(eip == code_start);
    TEST_CHECK(eax == initial_eax);
    TEST_CHECK(ebx == 0);

    data.stop = false;
    OK(uc_emu_start(uc, eip, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    TEST_CHECK(data.count == 2);
    TEST_CHECK(eax == memory_value);
    TEST_CHECK(ebx == 1);

    OK(uc_close(uc));
}

typedef struct X86MmioExitData {
    uc_err stop_error;
    uint32_t read_count;
    uint32_t write_count;
    uint64_t write_value;
    bool stop_read;
    bool stop_write;
} X86MmioExitData;

static uint64_t test_x86_mmio_exit_read(uc_engine *uc, uint64_t offset,
                                        unsigned size, void *user_data)
{
    X86MmioExitData *data = (X86MmioExitData *)user_data;

    TEST_CHECK(offset == 0);
    TEST_CHECK(size == 4);
    data->read_count++;
    if (data->stop_read) {
        data->stop_error = uc_emu_stop(uc);
    }
    return 0x66554433;
}

static void test_x86_mmio_exit_write(uc_engine *uc, uint64_t offset,
                                     unsigned size, uint64_t value,
                                     void *user_data)
{
    X86MmioExitData *data = (X86MmioExitData *)user_data;

    TEST_CHECK(offset == 4);
    TEST_CHECK(size == 4);
    data->write_count++;
    data->write_value = value;
    if (data->stop_write) {
        data->stop_error = uc_emu_stop(uc);
    }
}

static void test_x86_cputlb_split_mmio_exit(void)
{
    const uint64_t test_code = 0x100000;
    const uint8_t read_code[] = {
        0xa1, 0xfe, 0x2f, 0x00, 0x00, /* mov eax,[0x2ffe] */
        0x43,                         /* inc ebx */
    };
    const uint8_t write_code[] = {
        0xa3, 0x04, 0x30, 0x00, 0x00, /* mov [0x3004],eax */
        0x41,                         /* inc ecx */
    };
    const uint8_t ram_tail[] = {0x11, 0x22};
    const uint32_t initial_eax = 0xdeadbeef;
    X86MmioExitData data = {.stop_read = true};
    uint32_t eax = initial_eax;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t eip = 0;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, test_code, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, test_code, read_code, sizeof(read_code)));
    OK(uc_mem_write(uc, test_code + 0x100, write_code, sizeof(write_code)));
    OK(uc_mem_map(uc, 0x2000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x2ffe, ram_tail, sizeof(ram_tail)));
    OK(uc_mmio_map(uc, 0x3000, 0x1000, test_x86_mmio_exit_read, &data,
                   test_x86_mmio_exit_write, &data));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));

    OK(uc_emu_start(uc, test_code, test_code + sizeof(read_code), 0, 0));
    OK(data.stop_error);
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    TEST_CHECK(data.read_count == 1);
    TEST_CHECK(eip == test_code);
    TEST_CHECK(eax == initial_eax);
    TEST_CHECK(ebx == 0);

    data.stop_read = false;
    OK(uc_emu_start(uc, eip, test_code + sizeof(read_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    TEST_CHECK(data.read_count == 2);
    TEST_CHECK(eax == 0x44332211);
    TEST_CHECK(ebx == 1);

    eax = 0x87654321;
    data.stop_write = true;
    data.stop_error = UC_ERR_OK;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_emu_start(uc, test_code + 0x100,
                    test_code + 0x100 + sizeof(write_code), 0, 0));
    OK(data.stop_error);
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK(data.write_count == 1);
    TEST_CHECK(data.write_value == eax);
    TEST_CHECK(eip == test_code + 0x100);
    TEST_CHECK(ecx == 0);

    data.stop_write = false;
    OK(uc_emu_start(uc, eip, test_code + 0x100 + sizeof(write_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK(data.write_count == 2);
    TEST_CHECK(data.write_value == eax);
    TEST_CHECK(ecx == 1);

    OK(uc_close(uc));
}

static bool test_x86_missing_code_callback(uc_engine *uc, uc_mem_type type,
                                           uint64_t address, int size,
                                           int64_t value, void *user_data)
{
    char code[] = "\x41\x4a"; // inc ecx; dec edx;
    uint64_t algined_address = address & 0xFFFFFFFFFFFFF000ULL;
    int aligned_size = ((int)(size / 0x1000) + 1) * 0x1000;

    OK(uc_mem_map(uc, algined_address, aligned_size, UC_PROT_ALL));

    OK(uc_mem_write(uc, algined_address, code, sizeof(code) - 1));

    return true;
}

static void test_x86_missing_code(void)
{
    uc_engine *uc;
    uc_hook hook;
    int r_ecx = 0x1234;
    int r_edx = 0x7890;

    // Don't write any code by design.
    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_UNMAPPED,
                   test_x86_missing_code_callback, NULL, 1, 0));

    OK(uc_emu_start(uc, code_start, code_start + 2, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));

    TEST_CHECK(r_ecx == 0x1235);
    TEST_CHECK(r_edx == 0x788f);

    OK(uc_close(uc));
}

static void test_x86_smc_xor(void)
{
    uc_engine *uc;
    /*
     * 0x1000 xor dword ptr [edi+0x3], eax ; edi=0x1000, eax=0xbc4177e6
     * 0x1003 dw 0x3ea98b13
     */
    char code[] = "\x31\x47\x03\x13\x8b\xa9\x3e";
    int r_edi = code_start;
    int r_eax = 0xbc4177e6;
    uint32_t result;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    uc_reg_write(uc, UC_X86_REG_EDI, &r_edi);
    uc_reg_write(uc, UC_X86_REG_EAX, &r_eax);

    OK(uc_emu_start(uc, code_start, code_start + 3, 0, 0));

    OK(uc_mem_read(uc, code_start + 3, (void *)&result, 4));

    TEST_CHECK(LEINT32(result) == (0x3ea98b13 ^ 0xbc4177e6));

    OK(uc_close(uc));
}

static void test_x86_smc_add(void)
{
    uc_engine *uc;
    uint64_t stack_base = 0x20000;
    uint64_t r_rsp;
    /*
     * mov qword ptr [rip+0x10], rax
     * mov word ptr [rip], 0x0548
     * [orig] mov eax, dword ptr [rax + 0x12345678]; [after SMC] 480578563412
     * add rax, 0x12345678 hlt
     */
    char code[] = "\x48\x89\x05\x10\x00\x00\x00\x66\xc7\x05\x00\x00\x00\x00\x48"
                  "\x05\x8b\x80\x78\x56\x34\x12\xf4";
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);

    OK(uc_mem_map(uc, stack_base, 0x2000, UC_PROT_ALL));
    r_rsp = stack_base + 0x1800;
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &r_rsp));
    OK(uc_emu_start(uc, code_start, -1, 0, 0));

    OK(uc_close(uc));
}

static void test_x86_smc_mem_hook_callback(uc_engine *uc, uc_mem_type t,
                                           uint64_t addr, int size,
                                           int64_t value, void *user_data)
{
    uint64_t write_addresses[] = {0x1030, 0x1010, 0x1010, 0x1018,
                                  0x1018, 0x1029, 0x1029};
    unsigned int *i = user_data;

    TEST_CHECK(*i < (sizeof(write_addresses) / sizeof(write_addresses[0])));
    TEST_CHECK(write_addresses[*i] == addr);
    (*i)++;
}

static void test_x86_smc_mem_hook(void)
{
    uc_engine *uc;
    uc_hook hook;
    uint64_t stack_base = 0x20000;
    uint64_t r_rsp;
    unsigned int i = 0;
    /*
     * mov qword ptr [rip+0x29], rax
     * mov word ptr [rip], 0x0548
     * [orig] mov eax, dword ptr [rax + 0x12345678]; [after SMC] 480578563412
     * add rax, 0x12345678 nop nop nop mov qword ptr [rip-0x08], rax mov word
     * ptr [rip], 0x0548 [orig] mov eax, dword ptr [rax + 0x12345678]; [after
     * SMC] 480578563412 add rax, 0x12345678 hlt
     */
    char code[] =
        "\x48\x89\x05\x29\x00\x00\x00\x66\xC7\x05\x00\x00\x00\x00\x48\x05\x8B"
        "\x80\x78\x56\x34\x12\x90\x90\x90\x48\x89\x05\xF8\xFF\xFF\xFF\x66\xC7"
        "\x05\x00\x00\x00\x00\x48\x05\x8B\x80\x78\x56\x34\x12\xF4";
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);

    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_WRITE, test_x86_smc_mem_hook_callback,
                   &i, 1, 0));
    OK(uc_mem_map(uc, stack_base, 0x2000, UC_PROT_ALL));
    r_rsp = stack_base + 0x1800;
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &r_rsp));
    OK(uc_emu_start(uc, code_start, -1, 0, 0));

    OK(uc_close(uc));
}

static uint64_t test_x86_mmio_uc_mem_rw_read_callback(uc_engine *uc,
                                                      uint64_t offset,
                                                      unsigned size,
                                                      void *user_data)
{
    TEST_CHECK(offset == 8);
    TEST_CHECK(size == 4);

    return 0x19260817;
}

static void test_x86_mmio_uc_mem_rw_write_callback(uc_engine *uc,
                                                   uint64_t offset,
                                                   unsigned size,
                                                   uint64_t value,
                                                   void *user_data)
{
    TEST_CHECK(offset == 4);
    TEST_CHECK(size == 4);
    TEST_CHECK(value == 0xdeadbeef);

    return;
}

static void test_x86_mmio_uc_mem_rw(void)
{
    uc_engine *uc;
    int data = LEINT32(0xdeadbeef);

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));

    OK(uc_mmio_map(uc, 0x20000, 0x1000, test_x86_mmio_uc_mem_rw_read_callback,
                   NULL, test_x86_mmio_uc_mem_rw_write_callback, NULL));

    OK(uc_mem_write(uc, 0x20004, (void *)&data, 4));
    OK(uc_mem_read(uc, 0x20008, (void *)&data, 4));

    TEST_CHECK(LEINT32(data) == 0x19260817);

    OK(uc_close(uc));
}

static void test_x86_sysenter_hook(uc_engine *uc, void *user)
{
    *(int *)user = 1;
}

static void test_x86_sysenter(void)
{
    uc_engine *uc;
    char code[] = "\x0F\x34"; // sysenter
    uc_hook h;
    int called = 0;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    OK(uc_hook_add(uc, &h, UC_HOOK_INSN, test_x86_sysenter_hook, &called, 1, 0,
                   UC_X86_INS_SYSENTER));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    TEST_CHECK(called == 1);

    OK(uc_close(uc));
}

static int test_x86_hook_cpuid_callback(uc_engine *uc, void *data)
{
    uint32_t reg = 7;
    uint32_t eip;

    OK(uc_reg_read(uc, UC_X86_REG_EIP, (void*)&eip));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &reg));

    TEST_CHECK(eip == code_start + 1);
    // Overwrite the cpuid instruction.
    return 1;
}

static void test_x86_hook_cpuid(void)
{
    uc_engine *uc;
    char code[] = "\x40\x0F\xA2"; // INC EAX; CPUID
    uc_hook h;
    int reg;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    OK(uc_hook_add(uc, &h, UC_HOOK_INSN, test_x86_hook_cpuid_callback, NULL, 1,
                   0, UC_X86_INS_CPUID));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &reg));

    TEST_CHECK(reg == 7);

    OK(uc_close(uc));
}

static void test_x86_486_cpuid(void)
{
    uc_engine *uc;
    uint32_t eax;
    uint32_t ebx;

    char code[] = {0x31, 0xC0, 0x0F, 0xA2}; // XOR EAX EAX; CPUID

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_486));
    OK(uc_mem_map(uc, 0, 4 * 1024, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0, code, sizeof(code) / sizeof(code[0])));
    OK(uc_emu_start(uc, 0, sizeof(code) / sizeof(code[0]), 0, 0));

    /* Read eax after emulation */
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));

    TEST_CHECK(eax != 0);
    TEST_CHECK(ebx == 0x756e6547); // magic string "Genu" for intel cpu

    OK(uc_close(uc));
}

static void test_x86_qemu72_xsave_cpuid(void)
{
    uc_engine *uc;
    char code[] = "\x0f\xa2";
    uint32_t eax = 0xd;
    uint32_t ebx;
    uint32_t ecx = 1;
    uint32_t edx;
    uint32_t eip = code_start;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &edx));

    TEST_CHECK(eax != 0);
    TEST_CHECK(ebx >= 512);
    TEST_CHECK((ecx & ~(1U << 15)) == 0);
    TEST_CHECK(edx == 0);

    eax = 0xd;
    ecx = 0;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EIP, &eip));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &edx));

    TEST_CHECK((eax & 0x7) == 0x7);
    TEST_CHECK(ebx >= 512);
    TEST_CHECK(ecx >= ebx);
    TEST_CHECK(edx == 0);

    OK(uc_close(uc));
}

static void test_x86_opmask_registers(void)
{
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    for (int i = 0; i < 8; i++) {
        uint64_t in = 0x1122334455667700ULL + i;
        uint64_t out = 0;
        int reg = UC_X86_REG_K0 + i;

        OK(uc_reg_write(uc, reg, &in));
        OK(uc_reg_read(uc, reg, &out));
        TEST_CHECK(out == in);
    }

    OK(uc_close(uc));
}

static void test_x86_qemu72_msr_state(void)
{
    uc_engine *uc;
    uc_x86_msr msr;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    msr.rid = TEST_MSR_IA32_XFD;
    msr.value = 0x12345678abcdef00ULL;
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
    msr.value = 0;
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    TEST_CHECK(msr.value == 0x12345678abcdef00ULL);

    msr.rid = TEST_MSR_IA32_XFD_ERR;
    msr.value = 0xfedcba9876543210ULL;
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
    msr.value = 0;
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    TEST_CHECK(msr.value == 0xfedcba9876543210ULL);

    msr.rid = TEST_MSR_IA32_PKRS;
    msr.value = 0xa5a55a5aULL;
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
    msr.value = 0;
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    TEST_CHECK(msr.value == 0xa5a55a5aULL);

    msr.rid = TEST_MSR_ARCH_LBR_CTL;
    msr.value = 0x19;
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
    msr.value = 0;
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    TEST_CHECK(msr.value == 0x19);

    msr.rid = TEST_MSR_ARCH_LBR_DEPTH;
    msr.value = 32;
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
    msr.value = 0;
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    TEST_CHECK(msr.value == 32);

    msr.rid = TEST_MSR_ARCH_LBR_FROM_0 + 3;
    msr.value = 0x1111222233334444ULL;
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
    msr.value = 0;
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    TEST_CHECK(msr.value == 0x1111222233334444ULL);

    msr.rid = TEST_MSR_ARCH_LBR_TO_0 + 3;
    msr.value = 0x5555666677778888ULL;
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
    msr.value = 0;
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    TEST_CHECK(msr.value == 0x5555666677778888ULL);

    msr.rid = TEST_MSR_ARCH_LBR_INFO_0 + 3;
    msr.value = 0x9999aaaabbbbccccULL;
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
    msr.value = 0;
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    TEST_CHECK(msr.value == 0x9999aaaabbbbccccULL);

    msr.rid = TEST_MSR_IA32_XSS;
    msr.value = UINT64_MAX;
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
    msr.value = UINT64_MAX;
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    TEST_CHECK((msr.value & ~(1ULL << 15)) == 0);

    OK(uc_close(uc));
}

// This is a regression bug.
static void test_x86_clear_tb_cache(void)
{
    uc_engine *uc;
    char code[] = "\x83\xc1\x01\x4a"; // ADD ecx, 1; DEC edx;
    int r_ecx = 0x1234;
    int r_edx = 0x7890;
    uint64_t code_start = 0x1240; // Choose this address by design
    uint64_t code_len = 0x1000;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, code_start & (1 << 12), code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));

    // This emulation should take no effect at all.
    OK(uc_emu_start(uc, code_start, code_start, 0, 0));

    // Emulate ADD ecx, 1.
    OK(uc_emu_start(uc, code_start, code_start + 3, 0, 0));

    // If tb cache is not cleared, edx would be still 0x7890
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));

    TEST_CHECK(r_ecx == 0x1236);
    TEST_CHECK(r_edx == 0x788f);

    OK(uc_close(uc));
}

static void test_x86_clear_count_cache(void)
{
    uc_engine *uc;
    // uc_emu_start will clear last TB when exiting so generating a tb at last
    // by design
    char code[] =
        "\x83\xc1\x01\x4a\xeb\x00\x83\xc3\x01"; // ADD ecx, 1; DEC edx;
                                                // jmp t;
                                                // t:
                                                // ADD ebx, 1
    int r_ecx = 0x1234;
    int r_edx = 0x7890;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 2));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));

    TEST_CHECK(r_ecx == 0x1236);
    TEST_CHECK(r_edx == 0x788e);

    OK(uc_close(uc));
}

static void test_x86_large_instruction_count(void)
{
    const uint8_t code[] = {
        0x40,             /* inc eax */
        0xeb, 0xfd,       /* jmp loop */
    };
    const size_t counts[] = { 1, 2, 3, 65535, 65536, 70000 };
    uc_engine *uc;
    size_t i;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    for (i = 0; i < sizeof(counts) / sizeof(counts[0]); i++) {
        uint32_t eax = 0;
        uint32_t eip;

        OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
        OK(uc_emu_start(uc, code_start, 0, 0, counts[i]));
        OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
        OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
        TEST_CHECK_(eax == (counts[i] + 1) / 2,
                    "count=%zu eax=%u", counts[i], eax);
        TEST_CHECK_(eip == code_start + (counts[i] & 1),
                    "count=%zu eip=0x%x", counts[i], eip);
    }

    OK(uc_close(uc));
}

typedef struct X86CountPcChangeData {
    uint32_t count;
    uint32_t destination;
} X86CountPcChangeData;

static void test_x86_count_pc_change_callback(uc_engine *uc,
                                              uint64_t address,
                                              uint32_t size,
                                              void *user_data)
{
    X86CountPcChangeData *data =
        (X86CountPcChangeData *)user_data;

    data->count++;
    OK(uc_reg_write(uc, UC_X86_REG_EIP, &data->destination));
}

static void test_x86_count_nonmatching_callback(uc_engine *uc,
                                                uint64_t address,
                                                uint32_t size,
                                                void *user_data)
{
    TEST_CHECK(false);
}

static void test_x86_instruction_count_pc_change_refund(void)
{
    const uint64_t destination = code_start + 0x1000;
    const uint8_t code[] = { 0x40, 0x40, 0x40, 0x40 }; /* inc eax */
    X86CountPcChangeData data = {
        .destination = (uint32_t)destination,
    };
    uint32_t eax = 0;
    uint32_t eip;
    uc_engine *uc;
    uc_hook hook;
    uc_hook nonmatching_hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_mem_write(uc, destination, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_hook_add(uc, &nonmatching_hook, UC_HOOK_CODE,
                   test_x86_count_nonmatching_callback, NULL,
                   code_start + 0x100, code_start + 0x100));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE,
                   test_x86_count_pc_change_callback, &data,
                   code_start + 1, code_start + 1));

    OK(uc_emu_start(uc, code_start, 0, 0, 3));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    TEST_CHECK_(eax == 3, "eax=%u", eax);
    TEST_CHECK_(eip == destination + 2, "eip=0x%x", eip);
    TEST_CHECK(data.count == 1);

    OK(uc_close(uc));
}

// This is a regression bug.
static void test_x86_clear_empty_tb(void)
{
    uc_engine *uc;
    // lb:
    //    add ecx, 1;
    //    cmp ecx, 0;
    //    jz lb;
    //    dec edx;
    char code[] = "\x83\xc1\x01\x83\xf9\x00\x74\xf8\x4a";
    int r_edx = 0x7890;
    uint64_t code_start = 0x1240; // Choose this address by design
    uint64_t code_len = 0x1000;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, code_start & (1 << 12), code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));

    // Make sure we generate an empty tb at the exit address by stopping at dec
    // edx.
    OK(uc_emu_start(uc, code_start, code_start + 8, 0, 0));

    // If tb cache is not cleared, edx would be still 0x7890
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));

    TEST_CHECK(r_edx == 0x788f);

    OK(uc_close(uc));
}

typedef struct TbLoopStop_t {
    unsigned int block_count;
    unsigned int stop_at;
} TbLoopStop;

static void test_x86_tb_loop_stop_cb(uc_engine *uc, uint64_t address,
                                     uint32_t size, void *user_data)
{
    TbLoopStop *stop = user_data;

    stop->block_count++;
    if (stop->block_count == stop->stop_at) {
        OK(uc_emu_stop(uc));
    }
}

static void test_x86_self_linked_tb_guest_smc(void)
{
    uc_engine *uc;
    uc_hook hook;
    char loop[] = "\xeb\xfe";
    char smc[] = "\xc6\x05\x00\x10\x00\x00\x40"
                 "\xc6\x05\x01\x10\x00\x00\x90";
    TbLoopStop stop = {0, 2};
    uint32_t eax = 0x1234;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, loop, sizeof(loop) - 1);
    OK(uc_mem_write(uc, code_start + 0x1000, smc, sizeof(smc) - 1));
    OK(uc_hook_add(uc, &hook, UC_HOOK_BLOCK, test_x86_tb_loop_stop_cb,
                   &stop, code_start, code_start));

    OK(uc_emu_start(uc, code_start, -1, 0, 0));
    TEST_CHECK(stop.block_count == 2);

    OK(uc_emu_start(uc, code_start + 0x1000,
                    code_start + 0x1000 + sizeof(smc) - 1, 0, 0));
    stop.stop_at = 4;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_emu_start(uc, code_start, code_start + 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0x1235);
    TEST_CHECK(stop.block_count == 3);

    OK(uc_close(uc));
}

static void test_x86_two_page_tb_invalidation(void)
{
    const uint64_t tb_start = 0x1ffe;
    uc_engine *uc;
    char code[] = "\xb8\x11\x11\x11\x11";
    char second_page_byte = '\x22';
    char first_page_byte = '\x33';
    uint32_t eax;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, 0x1000, 0x2000, UC_PROT_ALL));
    OK(uc_mem_write(uc, tb_start, code, sizeof(code) - 1));

    OK(uc_emu_start(uc, tb_start, tb_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0x11111111);

    OK(uc_mem_write(uc, tb_start + 2, &second_page_byte, 1));
    OK(uc_emu_start(uc, tb_start, tb_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0x11112211);

    OK(uc_mem_write(uc, tb_start + 1, &first_page_byte, 1));
    OK(uc_emu_start(uc, tb_start, tb_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0x11112233);

    OK(uc_close(uc));
}

static void test_x86_tb_cache_engine_isolation(void)
{
    uc_engine *uc1;
    uc_engine *uc2;
    char code1[] = "\xb8\x11\x11\x11\x11";
    char code2[] = "\xb8\x22\x22\x22\x22";
    char replacement[] = "\xb8\x33\x33\x33\x33";
    uint32_t eax;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc1));
    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc2));
    OK(uc_mem_map(uc1, code_start, 0x1000, UC_PROT_ALL));
    OK(uc_mem_map(uc2, code_start, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc1, code_start, code1, sizeof(code1) - 1));
    OK(uc_mem_write(uc2, code_start, code2, sizeof(code2) - 1));

    OK(uc_emu_start(uc1, code_start, code_start + sizeof(code1) - 1, 0, 0));
    OK(uc_emu_start(uc2, code_start, code_start + sizeof(code2) - 1, 0, 0));

    OK(uc_ctl_flush_tb(uc1));
    OK(uc_mem_write(uc1, code_start, replacement, sizeof(replacement) - 1));
    OK(uc_emu_start(uc1, code_start,
                    code_start + sizeof(replacement) - 1, 0, 0));
    OK(uc_reg_read(uc1, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0x33333333);

    OK(uc_emu_start(uc2, code_start, code_start + sizeof(code2) - 1, 0, 0));
    OK(uc_reg_read(uc2, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0x22222222);

    OK(uc_close(uc1));
    OK(uc_close(uc2));
}

typedef struct _HOOK_TCG_OP_RESULT {
    uint64_t address;
    uint64_t arg1;
    uint64_t arg2;
} HOOK_TCG_OP_RESULT;

typedef struct _HOOK_TCG_OP_RESULTS {
    HOOK_TCG_OP_RESULT results[128];
    uint64_t len;
} HOOK_TCG_OP_RESULTS;

static void test_x86_hook_tcg_op_cb(uc_engine *uc, uint64_t address,
                                    uint64_t arg1, uint64_t arg2, uint32_t size,
                                    void *data)
{
    HOOK_TCG_OP_RESULTS *results = (HOOK_TCG_OP_RESULTS *)data;
    HOOK_TCG_OP_RESULT *result = &results->results[results->len++];

    result->address = address;
    result->arg1 = arg1;
    result->arg2 = arg2;
}

static void test_x86_hook_tcg_op(void)
{
    uc_engine *uc;
    uc_hook h;
    int flag;
    HOOK_TCG_OP_RESULTS results;
    // sub esi, [0x1000];
    // sub eax, ebx;
    // sub eax, 1;
    // cmp eax, 0;
    // cmp ebx, edx;
    // cmp esi, [0x1000];
    char code[] = "\x2b\x35\x00\x10\x00\x00\x29\xd8\x83\xe8\x01\x83\xf8\x00\x39"
                  "\xd3\x3b\x35\x00\x10\x00\x00";
    int r_eax = 0x1234;
    int r_ebx = 2;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &r_ebx));

    memset(&results, 0, sizeof(HOOK_TCG_OP_RESULTS));
    flag = 0;
    OK(uc_hook_add(uc, &h, UC_HOOK_TCG_OPCODE, test_x86_hook_tcg_op_cb,
                   &results, 0, -1, UC_TCG_OP_SUB, flag));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_hook_del(uc, h));

    TEST_CHECK(results.len == 6);

    memset(&results, 0, sizeof(HOOK_TCG_OP_RESULTS));
    flag = UC_TCG_OP_FLAG_DIRECT;
    OK(uc_hook_add(uc, &h, UC_HOOK_TCG_OPCODE, test_x86_hook_tcg_op_cb,
                   &results, 0, -1, UC_TCG_OP_SUB, flag));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_hook_del(uc, h));

    TEST_CHECK(results.len == 3);

    memset(&results, 0, sizeof(HOOK_TCG_OP_RESULTS));
    flag = UC_TCG_OP_FLAG_CMP;
    OK(uc_hook_add(uc, &h, UC_HOOK_TCG_OPCODE, test_x86_hook_tcg_op_cb,
                   &results, 0, -1, UC_TCG_OP_SUB, flag));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_hook_del(uc, h));

    TEST_CHECK(results.len == 3);

    OK(uc_close(uc));
}

static void test_x86_cmpxchg_mem_hook(uc_engine *uc, uc_mem_type type,
                                     uint64_t address, int size, int64_t val,
                                     void *data)
{
    if (type == UC_MEM_READ) {
        *((int *)data) |= 1;
    } else {
        *((int *)data) |= 2;
    }

}

static void test_x86_cmpxchg(void)
{
    uc_engine *uc;
    char code[] = "\x0F\xC7\x0D\xE0\xBE\xAD\xDE"; // cmpxchg8b [0xdeadbee0]
    int r_zero = 0;
    int r_aaaa = 0x41414141;
    uint64_t mem;
    uc_hook h;
    int result = 0;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, 0xdeadb000, 0x1000, UC_PROT_ALL));
    OK(uc_hook_add(uc, &h, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
                   test_x86_cmpxchg_mem_hook, &result, 1, 0));

    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_zero));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &r_zero));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_aaaa));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &r_aaaa));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_mem_read(uc, 0xdeadbee0, &mem, 8));

    TEST_CHECK(mem == 0x4141414141414141);

    // Both read and write happened.
    TEST_CHECK(result == 3);

    OK(uc_close(uc));
}

static void test_x86_cmpxchg32_acc_case(uint64_t initial_rax,
                                        uint64_t initial_mem,
                                        uint64_t expected_rax,
                                        uint64_t expected_mem,
                                        bool expected_zf)
{
    uc_engine *uc;
    char code[] = "\x41\x0f\xb1\x18"; /* cmpxchg dword ptr [r8], ebx */
    uint64_t data_address = 0x2000000;
    uint64_t rax = initial_rax;
    uint64_t rbx = 0;
    uint64_t r8 = data_address;
    uint64_t rflags;
    uint64_t mem;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &initial_mem, sizeof(initial_mem)));
    OK(uc_reg_write(uc, UC_X86_REG_R8, &r8));
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_read(uc, UC_X86_REG_RFLAGS, &rflags));
    OK(uc_mem_read(uc, data_address, &mem, sizeof(mem)));

    TEST_CHECK(rax == expected_rax);
    TEST_CHECK(mem == expected_mem);
    TEST_CHECK((bool)(rflags & 0x40) == expected_zf);

    OK(uc_close(uc));
}

static void test_x86_cmpxchg32_accumulator(void)
{
    test_x86_cmpxchg32_acc_case(0xffffffffffffffffULL,
                                0xffffffffffffffffULL,
                                0xffffffffffffffffULL,
                                0xffffffff00000000ULL, true);
    test_x86_cmpxchg32_acc_case(0xffffffff00000000ULL,
                                0xffffffffffffffffULL,
                                0x00000000ffffffffULL,
                                0xffffffffffffffffULL, false);
}

static void test_x86_cmpxchg32_reg_case(uint64_t initial_rax,
                                        uint64_t initial_rcx,
                                        uint64_t initial_rbx,
                                        uint64_t expected_rax,
                                        uint64_t expected_rcx,
                                        bool expected_zf)
{
    uc_engine *uc;
    char code[] = "\x0f\xb1\xd9"; /* cmpxchg ecx, ebx */
    uint64_t rax = initial_rax;
    uint64_t rcx = initial_rcx;
    uint64_t rbx = initial_rbx;
    uint64_t rflags;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &rcx));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_read(uc, UC_X86_REG_RCX, &rcx));
    OK(uc_reg_read(uc, UC_X86_REG_RFLAGS, &rflags));

    TEST_CHECK(rax == expected_rax);
    TEST_CHECK(rcx == expected_rcx);
    TEST_CHECK((bool)(rflags & 0x40) == expected_zf);

    OK(uc_close(uc));
}

static void test_x86_cmpxchg32_register(void)
{
    test_x86_cmpxchg32_reg_case(0xeeeeeeeeffffffffULL,
                                0xaaaaaaaaffffffffULL,
                                0x1111111122222222ULL,
                                0xeeeeeeeeffffffffULL,
                                0x0000000022222222ULL, true);
    test_x86_cmpxchg32_reg_case(0x1111111112345678ULL,
                                0xaaaaaaaaffffffffULL,
                                0x1111111122222222ULL,
                                0x00000000ffffffffULL,
                                0xaaaaaaaaffffffffULL, false);
}

static void test_x86_ret_imm16_unsigned(void)
{
    uc_engine *uc;
    char code[] = "\xc2\x00\xff"; /* ret 0xff00 */
    uint64_t stack_address = 0x2000000;
    uint64_t return_address = code_start + sizeof(code) - 1;
    uint64_t rsp = stack_address;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, stack_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, stack_address, &return_address,
                    sizeof(return_address)));
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &rsp));

    OK(uc_emu_start(uc, code_start, return_address, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_RSP, &rsp));

    TEST_CHECK(rsp == stack_address + 8 + 0xff00);

    OK(uc_close(uc));
}

static void test_x86_rorx_rip_relative_imm(void)
{
    uc_engine *uc;
    char code[] = "\xc4\xe3\x7b\xf0\x05\xf6\x14\x00\x00\x00";
    uint64_t expected_address = code_start + sizeof(code) - 1 + 0x14f6;
    uint8_t data[] = {0xaa, 0x11, 0x22, 0x33, 0x44};
    uint64_t rax;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_mem_write(uc, expected_address - 1, data, sizeof(data)));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));

    TEST_CHECK(rax == 0x44332211);

    OK(uc_close(uc));
}

static void test_x86_shiftd_rip_relative_imm(const char *code,
                                             size_t code_size, uint64_t rbx,
                                             uint16_t expected_value)
{
    uc_engine *uc;
    uint64_t expected_address = code_start + code_size + 0x14f7;
    uint8_t data[] = {0xaa, 0x11, 0x22, 0x33, 0x44};
    uint8_t previous;
    uint16_t mem;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, code_size);
    OK(uc_mem_write(uc, expected_address - 1, data, sizeof(data)));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));

    OK(uc_emu_start(uc, code_start, code_start + code_size, 0, 1));
    OK(uc_mem_read(uc, expected_address - 1, &previous, sizeof(previous)));
    OK(uc_mem_read(uc, expected_address, &mem, sizeof(mem)));

    TEST_CHECK(previous == 0xaa);
    TEST_CHECK(mem == expected_value);

    OK(uc_close(uc));
}

static void test_x86_shld_rip_relative_imm(void)
{
    char code[] = "\x66\x0f\xa4\x1d\xf7\x14\x00\x00\x01";

    test_x86_shiftd_rip_relative_imm(code, sizeof(code) - 1, 0x8000, 0x4423);
}

static void test_x86_shrd_rip_relative_imm(void)
{
    char code[] = "\x66\x0f\xac\x1d\xf7\x14\x00\x00\x01";

    test_x86_shiftd_rip_relative_imm(code, sizeof(code) - 1, 1, 0x9108);
}

static void test_x86_pdep32_zero_extend(void)
{
    uc_engine *uc;
    char code[] = "\xc4\xe2\x63\xf5\xc1"; /* pdep eax, ebx, ecx */
    uint64_t rax = 0xffffffffffffffffULL;
    uint64_t rbx = 0xffffffffffffff00ULL;
    uint64_t rcx = 0xffffffffffffff00ULL;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &rcx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));

    TEST_CHECK(rax == 0x00000000ffff0000ULL);

    OK(uc_close(uc));
}

static void test_x86_pext32_zero_extend(void)
{
    uc_engine *uc;
    char code[] = "\xc4\xe2\x62\xf5\xc1"; /* pext eax, ebx, ecx */
    uint64_t rax = 0xffffffffffffffffULL;
    uint64_t rbx = 0xffffffffabcdef00ULL;
    uint64_t rcx = 0xffffffff0000ff00ULL;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &rcx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));

    TEST_CHECK(rax == 0xef);

    OK(uc_close(uc));
}

static void test_x86_nested_emu_start_cb(uc_engine *uc, uint64_t addr,
                                         uint32_t size, void *data)
{
    OK(uc_emu_start(uc, code_start + 1, code_start + 2, 0, 0));
}

static void test_x86_nested_emu_start(void)
{
    uc_engine *uc;
    char code[] = "\x41\x4a"; // INC ecx; DEC edx;
    int r_ecx = 0x1234;
    int r_edx = 0x7890;
    uc_hook h;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));
    // Emulate DEC in the nested hook.
    OK(uc_hook_add(uc, &h, UC_HOOK_CODE, test_x86_nested_emu_start_cb, NULL,
                   code_start, code_start));

    // Emulate INC
    OK(uc_emu_start(uc, code_start, code_start + 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));

    TEST_CHECK(r_ecx == 0x1235);
    TEST_CHECK(r_edx == 0x788f);

    OK(uc_close(uc));
}

typedef struct X86NestedCountData {
    uint64_t inner_address;
    uint32_t count;
} X86NestedCountData;

static void test_x86_nested_count_callback(uc_engine *uc, uint64_t address,
                                           uint32_t size, void *user_data)
{
    X86NestedCountData *data = (X86NestedCountData *)user_data;

    data->count++;
    OK(uc_emu_start(uc, data->inner_address, data->inner_address + 1,
                    0, 1));
}

static void test_x86_nested_count_state(void)
{
    const uint64_t inner_address = code_start + 0x1000;
    const uint8_t outer_code[] = { 0x40, 0x40, 0x40 }; /* inc eax */
    const uint8_t inner_code[] = { 0x4b }; /* dec ebx */
    X86NestedCountData data = { .inner_address = inner_address };
    uint32_t eax = 0;
    uint32_t ebx = 10;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)outer_code, sizeof(outer_code));
    OK(uc_mem_write(uc, inner_address, inner_code, sizeof(inner_code)));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE,
                   test_x86_nested_count_callback, &data,
                   code_start, code_start));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(outer_code),
                    0, 2));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    TEST_CHECK(eax == 2);
    TEST_CHECK(ebx == 9);
    TEST_CHECK(data.count == 1);

    OK(uc_close(uc));
}

static void test_x86_nested_emu_stop_cb(uc_engine *uc, uint64_t addr,
                                        uint32_t size, void *data)
{
    OK(uc_emu_start(uc, code_start + 1, code_start + 2, 0, 0));
    // ecx shouldn't be changed!
    OK(uc_emu_stop(uc));
}

static void test_x86_nested_emu_stop(void)
{
    uc_engine *uc;
    // INC ecx; DEC edx; DEC edx;
    char code[] = "\x41\x4a\x4a";
    int r_ecx = 0x1234;
    int r_edx = 0x7890;
    uc_hook h;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &r_edx));
    // Emulate DEC in the nested hook.
    OK(uc_hook_add(uc, &h, UC_HOOK_CODE, test_x86_nested_emu_stop_cb, NULL,
                   code_start, code_start));

    OK(uc_emu_start(uc, code_start, code_start + 3, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &r_edx));

    TEST_CHECK(r_ecx == 0x1234);
    TEST_CHECK(r_edx == 0x788f);

    OK(uc_close(uc));
}

static void test_x86_nested_emu_start_error_cb(uc_engine *uc, uint64_t addr,
                                               uint32_t size, void *data)
{
    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_emu_start(uc, code_start + 2, 0, 0, 0));
}

static void test_x86_64_nested_emu_start_error(void)
{
    uc_engine *uc;
    // "nop;nop;mov rax, [0x10000]"
    char code[] = "\x90\x90\x48\xa1\x00\x00\x01\x00\x00\x00\x00\x00";
    uc_hook hk;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_hook_add(uc, &hk, UC_HOOK_CODE, test_x86_nested_emu_start_error_cb,
                   NULL, code_start, code_start));

    // This call shouldn't fail!
    OK(uc_emu_start(uc, code_start, code_start + 2, 0, 0));

    OK(uc_close(uc));
}

typedef struct X86NestedDepthData {
    uint32_t callbacks;
    uint32_t successful_starts;
    uint32_t returns;
    uint32_t resource_errors;
} X86NestedDepthData;

static void test_x86_nested_emu_start_max_depth_cb(uc_engine *uc,
                                                    uint64_t address,
                                                    uint32_t size,
                                                    void *user_data)
{
    X86NestedDepthData *data = (X86NestedDepthData *)user_data;
    uc_err err;

    data->callbacks++;
    err = uc_emu_start(uc, code_start, code_start + 1, 0, 0);
    if (err == UC_ERR_RESOURCE) {
        data->resource_errors++;
        OK(uc_ctl_remove_cache(uc, code_start, code_start + 1));
    } else {
        uc_assert_err(UC_ERR_OK, err);
        if (err == UC_ERR_OK) {
            data->successful_starts++;
        }
    }
    data->returns++;
}

static void test_x86_nested_emu_start_max_depth(void)
{
    const uint8_t nop[] = { 0x90 };
    const uint8_t inc_eax[] = { 0x40 };
    X86NestedDepthData data = { 0 };
    uint32_t eax = 0;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)nop, sizeof(nop));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE,
                   test_x86_nested_emu_start_max_depth_cb, &data,
                   code_start, code_start));

    OK(uc_emu_start(uc, code_start, code_start + 1, 0, 0));
    TEST_CHECK(data.resource_errors == 1);
    TEST_CHECK(data.callbacks == data.returns);
    TEST_CHECK(data.successful_starts + 1 == data.callbacks);

    OK(uc_hook_del(uc, hook));
    OK(uc_mem_write(uc, code_start, inc_eax, sizeof(inc_eax)));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_emu_start(uc, code_start, code_start + 1, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 1);

    OK(uc_close(uc));
}

static void test_x86_eflags_reserved_bit(void)
{
    uc_engine *uc;
    uint32_t r_eflags;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));

    OK(uc_reg_read(uc, UC_X86_REG_EFLAGS, &r_eflags));

    TEST_CHECK((r_eflags & 2) != 0);

    OK(uc_reg_write(uc, UC_X86_REG_EFLAGS, &r_eflags));

    OK(uc_reg_read(uc, UC_X86_REG_EFLAGS, &r_eflags));

    TEST_CHECK((r_eflags & 2) != 0);

    OK(uc_close(uc));
}

static void test_x86_blsi_cf_case(uint64_t src, uint64_t expected_dst,
                                  bool expected_cf, bool expected_zf)
{
    uc_engine *uc;
    char code[] = "\xc4\xe2\xf8\xf3\xdb"; /* blsi rax, rbx */
    uint64_t rax;
    uint64_t rflags;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &src));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_read(uc, UC_X86_REG_RFLAGS, &rflags));

    TEST_CHECK(rax == expected_dst);
    TEST_CHECK((bool)(rflags & 1) == expected_cf);
    TEST_CHECK((bool)(rflags & 0x40) == expected_zf);

    OK(uc_close(uc));
}

static void test_x86_blsi_cf(void)
{
    test_x86_blsi_cf_case(1, 1, true, false);
    test_x86_blsi_cf_case(0, 0, false, true);
}

static void test_x86_blsr_flags_case(uint64_t src, uint64_t expected_dst,
                                     bool expected_cf, bool expected_zf)
{
    uc_engine *uc;
    char code[] = "\xc4\xe2\xf8\xf3\xcb"; /* blsr rax, rbx */
    uint64_t rax;
    uint64_t rflags;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &src));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_read(uc, UC_X86_REG_RFLAGS, &rflags));

    TEST_CHECK(rax == expected_dst);
    TEST_CHECK((bool)(rflags & 1) == expected_cf);
    TEST_CHECK((bool)(rflags & 0x40) == expected_zf);

    OK(uc_close(uc));
}

static void test_x86_blsr_flags(void)
{
    test_x86_blsr_flags_case(0x28, 0x20, false, false);
    test_x86_blsr_flags_case(1, 0, false, true);
    test_x86_blsr_flags_case(0, 0, true, true);
}

static void test_x86_blsmsk_flags_case(uint64_t src, uint64_t expected_dst,
                                       bool expected_cf, bool expected_zf,
                                       bool expected_sf)
{
    uc_engine *uc;
    char code[] = "\xc4\xe2\xf8\xf3\xd3"; /* blsmsk rax, rbx */
    uint64_t rax;
    uint64_t rflags;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &src));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_read(uc, UC_X86_REG_RFLAGS, &rflags));

    TEST_CHECK(rax == expected_dst);
    TEST_CHECK((bool)(rflags & 1) == expected_cf);
    TEST_CHECK((bool)(rflags & 0x40) == expected_zf);
    TEST_CHECK((bool)(rflags & 0x80) == expected_sf);

    OK(uc_close(uc));
}

static void test_x86_blsmsk_flags(void)
{
    test_x86_blsmsk_flags_case(0x28, 0x0f, false, false, false);
    test_x86_blsmsk_flags_case(0, UINT64_MAX, true, false, true);
}

static void test_x86_bzhi_index_case(uint64_t index, uint64_t expected_dst,
                                     bool expected_cf, bool expected_sf)
{
    uc_engine *uc;
    char code[] = "\xc4\xe2\xf0\xf5\xc3"; /* bzhi rax, rbx, rcx */
    uint64_t rax;
    uint64_t rflags;
    uint64_t src = 0xffffffffffffffffULL;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &src));
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &index));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_read(uc, UC_X86_REG_RFLAGS, &rflags));

    TEST_CHECK(rax == expected_dst);
    TEST_CHECK((bool)(rflags & 1) == expected_cf);
    TEST_CHECK((bool)(rflags & 0x80) == expected_sf);

    OK(uc_close(uc));
}

static void test_x86_bzhi_index_boundary(void)
{
    test_x86_bzhi_index_case(63, 0x7fffffffffffffffULL, false, false);
    test_x86_bzhi_index_case(255, 0xffffffffffffffffULL, true, true);
}

static void test_x86_nested_uc_emu_start_exits_cb(uc_engine *uc, uint64_t addr,
                                                  uint32_t size, void *data)
{
    OK(uc_emu_start(uc, code_start + 5, code_start + 6, 0, 0));
}

static void test_x86_nested_uc_emu_start_exits(void)
{
    uc_engine *uc;
    //  cmp eax, 0
    //  jnz t
    //  nop <-- nested emu_start
    // t:mov dword ptr [eax], 0
    char code[] = "\x83\xf8\x00\x75\x01\x90\xc7\x00\x00\x00\x00\x00";
    uc_hook hk;
    uint32_t r_pc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    OK(uc_hook_add(uc, &hk, UC_HOOK_CODE, test_x86_nested_uc_emu_start_exits_cb,
                   NULL, code_start, code_start));
    OK(uc_emu_start(uc, code_start, code_start + 5, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &r_pc));

    TEST_CHECK(r_pc == code_start + 5);

    OK(uc_close(uc));
}

static bool test_x86_correct_address_in_small_jump_hook_callback(
    uc_engine *uc, uc_mem_type type, uint64_t address, int size,
    int64_t value, void *user_data)
{
    // Check registers
    uint64_t r_rax = 0x0;
    uint64_t r_rip = 0x0;
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &r_rip));
    TEST_CHECK(r_rax == 0x7F00);
    TEST_CHECK(r_rip == 0x7F00);

    // Check address
    // printf("%lx\n", address);
    TEST_CHECK(address == 0x7F00);

    return false;
}

static void test_x86_correct_address_in_small_jump_hook(void)
{
    uc_engine *uc;
    // movabs $0x7F00, %rax
    // jmp  *%rax
    char code[] = "\x48\xb8\x00\x7F\x00\x00\x00\x00\x00\x00\xff\xe0";

    uint64_t r_rax = 0x0;
    uint64_t r_rip = 0x0;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_UNMAPPED,
                   test_x86_correct_address_in_small_jump_hook_callback, NULL,
                   1, 0));

    uc_assert_err(
        UC_ERR_FETCH_UNMAPPED,
        uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &r_rip));
    TEST_CHECK(r_rax == 0x7F00);
    TEST_CHECK(r_rip == 0x7F00);

    OK(uc_close(uc));
}

static bool test_x86_correct_address_in_long_jump_hook_callback(
    uc_engine *uc, uc_mem_type type, uint64_t address, int size,
    int64_t value, void *user_data)
{
    // Check registers
    uint64_t r_rax = 0x0;
    uint64_t r_rip = 0x0;
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &r_rip));
    TEST_CHECK(r_rax == 0x7FFFFFFFFFFFFF00);
    TEST_CHECK(r_rip == 0x7FFFFFFFFFFFFF00);

    // Check address
    // printf("%lx\n", address);
    TEST_CHECK(address == 0x7FFFFFFFFFFFFF00);

    return false;
}

static void test_x86_correct_address_in_long_jump_hook(void)
{
    uc_engine *uc;
    // movabs $0x7FFFFFFFFFFFFF00, %rax
    // jmp  *%rax
    char code[] = "\x48\xb8\x00\xff\xff\xff\xff\xff\xff\x7f\xff\xe0";

    uint64_t r_rax = 0x0;
    uint64_t r_rip = 0x0;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_UNMAPPED,
                   test_x86_correct_address_in_long_jump_hook_callback, NULL, 1,
                   0));

    uc_assert_err(
        UC_ERR_FETCH_UNMAPPED,
        uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &r_rip));
    TEST_CHECK(r_rax == 0x7FFFFFFFFFFFFF00);
    TEST_CHECK(r_rip == 0x7FFFFFFFFFFFFF00);

    OK(uc_close(uc));
}

static void test_x86_invalid_vex_l(void)
{
    uc_engine *uc;

    /* andn eax, eax, eax with reserved VEX.L set */
    char code[] = {'\xC4', '\xE2', '\x7F', '\xF2', '\xC0'};

    /* initialize memory and run emulation  */
    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, 0, 2 * 1024 * 1024, UC_PROT_ALL));

    OK(uc_mem_write(uc, 0, code, sizeof(code) / sizeof(code[0])));

    uc_assert_err(UC_ERR_INSN_INVALID,
                  uc_emu_start(uc, 0, sizeof(code) / sizeof(code[0]), 0, 0));
    OK(uc_close(uc));
}

typedef struct {
    uint32_t count;
    uint32_t intno;
} X86IntrCapture;

static void test_x86_intr_capture_cb(uc_engine *uc, uint32_t intno, void *data)
{
    X86IntrCapture *capture = (X86IntrCapture *)data;

    capture->count++;
    capture->intno = intno;
    uc_emu_stop(uc);
}

static void test_x86_sse_aligned_access(void)
{
    const uint64_t aligned_stack = 0x200000;
    const uint64_t unaligned_stack = aligned_stack + 8;
    const uint8_t code[] = {
        0x0f, 0x11, 0x04, 0x24, /* movups [rsp], xmm0 */
        0x0f, 0x29, 0x04, 0x24, /* movaps [rsp], xmm0 */
    };
    const uint8_t xmm0[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    const uint8_t sentinel[16] = {
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
    };
    uint8_t memory[16];
    X86IntrCapture capture = { 0 };
    uc_engine *uc;
    uc_hook hook;
    uint64_t rip;
    uint64_t rsp = unaligned_stack;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_mem_map(uc, aligned_stack, 0x1000, UC_PROT_ALL));
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &rsp));
    OK(uc_reg_write(uc, UC_X86_REG_XMM0, &xmm0));

    OK(uc_emu_start(uc, code_start, code_start + 4, 0, 1));
    OK(uc_mem_read(uc, unaligned_stack, memory, sizeof(memory)));
    TEST_CHECK(memcmp(memory, xmm0, sizeof(memory)) == 0);

    OK(uc_mem_write(uc, unaligned_stack, sentinel, sizeof(sentinel)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_INTR, test_x86_intr_capture_cb,
                   &capture, 1, 0));
    OK(uc_emu_start(uc, code_start + 4, code_start + sizeof(code), 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &rip));
    OK(uc_mem_read(uc, unaligned_stack, memory, sizeof(memory)));
    TEST_CHECK(rip == code_start + 4);
    TEST_CHECK(capture.count == 1);
    TEST_CHECK_(capture.intno == 13, "intno=%u", capture.intno);
    TEST_CHECK(memcmp(memory, sentinel, sizeof(memory)) == 0);

    rsp = aligned_stack;
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &rsp));
    OK(uc_mem_write(uc, aligned_stack, sentinel, sizeof(sentinel)));
    OK(uc_emu_start(uc, code_start + 4, code_start + sizeof(code), 0, 1));
    OK(uc_mem_read(uc, aligned_stack, memory, sizeof(memory)));
    TEST_CHECK(capture.count == 1);
    TEST_CHECK(memcmp(memory, xmm0, sizeof(memory)) == 0);

    OK(uc_close(uc));
}

static void test_x86_movdqa_movdqu_alignment(void)
{
    const uint64_t address = 0x200008;
    const uint8_t code[] = {
        0xf3, 0x0f, 0x7f, 0x04, 0x24, /* movdqu [rsp], xmm0 */
        0x66, 0x0f, 0x7f, 0x04, 0x24, /* movdqa [rsp], xmm0 */
    };
    const uint8_t xmm0[16] = {
        0x00, 0x11, 0x22, 0x33, 0x44, 0x55, 0x66, 0x77,
        0x88, 0x99, 0xaa, 0xbb, 0xcc, 0xdd, 0xee, 0xff,
    };
    const uint8_t sentinel[16] = {
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
    };
    uint8_t memory[16];
    uc_engine *uc;
    uint64_t rip;
    uint64_t rsp = address;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_mem_map(uc, address & ~0xfffULL, 0x1000, UC_PROT_ALL));
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &rsp));
    OK(uc_reg_write(uc, UC_X86_REG_XMM0, &xmm0));

    OK(uc_emu_start(uc, code_start, code_start + 5, 0, 1));
    OK(uc_mem_read(uc, address, memory, sizeof(memory)));
    TEST_CHECK(memcmp(memory, xmm0, sizeof(memory)) == 0);

    OK(uc_mem_write(uc, address, sentinel, sizeof(sentinel)));
    uc_assert_err(UC_ERR_EXCEPTION,
                  uc_emu_start(uc, code_start + 5,
                               code_start + sizeof(code), 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &rip));
    OK(uc_mem_read(uc, address, memory, sizeof(memory)));
    TEST_CHECK(rip == code_start + 5);
    TEST_CHECK(memcmp(memory, sentinel, sizeof(memory)) == 0);

    OK(uc_close(uc));
}

static void test_x86_xsave_setup(uc_engine **uc, uc_cpu_x86 cpu_model,
                                 const uint8_t *code, size_t code_size)
{
    const uint8_t empty_header[64] = {0};

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, uc));
    OK(uc_ctl_set_cpu_model(*uc, cpu_model));
    OK(uc_mem_map(*uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(*uc, code_start, code, code_size));
    OK(uc_mem_map(*uc, TEST_X86_XSAVE_AREA, TEST_X86_XSAVE_AREA_SIZE,
                  UC_PROT_ALL));
    OK(uc_mem_write(*uc, TEST_X86_XSAVE_AREA + TEST_X86_XSAVE_HEADER_OFFSET,
                    empty_header, sizeof(empty_header)));
}

static uc_err test_x86_run_xsave_instruction(uc_engine *uc, uint64_t pc,
                                             size_t insn_size, uint64_t area,
                                             uint64_t mask)
{
    uint32_t eax = (uint32_t)mask;
    uint32_t edx = (uint32_t)(mask >> 32);

    OK(uc_reg_write(uc, UC_X86_REG_RDI, &area));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &edx));
    return uc_emu_start(uc, pc, pc + insn_size, 0, 1);
}

static void test_x86_xsave_xrstor_roundtrip(void)
{
    const uint8_t code[] = {
        0x0f, 0xae, 0x27, /* xsave [rdi] */
        0x0f, 0xae, 0x2f, /* xrstor [rdi] */
    };
    const uint8_t initial_st0[10] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0xff, 0x3f,
    };
    const uint8_t changed_st0[10] = {
        0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00, 0x80, 0x00, 0x40,
    };
    const uint64_t initial_ymm0[4] = {
        0x0011223344556677ULL,
        0x8899aabbccddeeffULL,
        0x1021324354657687ULL,
        0x98a9bacbdcedfe0fULL,
    };
    const uint64_t changed_ymm0[4] = {
        0xffffffffffffffffULL,
        0xeeeeeeeeeeeeeeeeULL,
        0xddddddddddddddddULL,
        0xccccccccccccccccULL,
    };
    const uint64_t mask =
        TEST_X86_XSTATE_FP | TEST_X86_XSTATE_SSE | TEST_X86_XSTATE_YMM;
    uint8_t st0[sizeof(initial_st0)];
    uint64_t ymm0[4];
    uint64_t xstate_bv;
    uint16_t fpcw = 0x037f;
    uint32_t mxcsr = 0x1f80;
    uc_engine *uc;

    test_x86_xsave_setup(&uc, UC_CPU_X86_HASWELL, code, sizeof(code));
    OK(uc_reg_write(uc, UC_X86_REG_ST0, initial_st0));
    OK(uc_reg_write(uc, UC_X86_REG_FPCW, &fpcw));
    OK(uc_reg_write(uc, UC_X86_REG_MXCSR, &mxcsr));
    OK(uc_reg_write(uc, UC_X86_REG_YMM0, initial_ymm0));

    OK(test_x86_run_xsave_instruction(uc, code_start, 3, TEST_X86_XSAVE_AREA,
                                      mask));
    OK(uc_mem_read(uc, TEST_X86_XSAVE_AREA + TEST_X86_XSAVE_HEADER_OFFSET,
                   &xstate_bv, sizeof(xstate_bv)));
    TEST_CHECK((xstate_bv & mask) == mask);

    fpcw = 0x077f;
    mxcsr = 0x3f80;
    OK(uc_reg_write(uc, UC_X86_REG_ST0, changed_st0));
    OK(uc_reg_write(uc, UC_X86_REG_FPCW, &fpcw));
    OK(uc_reg_write(uc, UC_X86_REG_MXCSR, &mxcsr));
    OK(uc_reg_write(uc, UC_X86_REG_YMM0, changed_ymm0));

    OK(test_x86_run_xsave_instruction(uc, code_start + 3, 3,
                                      TEST_X86_XSAVE_AREA, mask));
    OK(uc_reg_read(uc, UC_X86_REG_ST0, st0));
    OK(uc_reg_read(uc, UC_X86_REG_FPCW, &fpcw));
    OK(uc_reg_read(uc, UC_X86_REG_MXCSR, &mxcsr));
    OK(uc_reg_read(uc, UC_X86_REG_YMM0, ymm0));

    TEST_CHECK(memcmp(st0, initial_st0, sizeof(st0)) == 0);
    TEST_CHECK(fpcw == 0x037f);
    TEST_CHECK(mxcsr == 0x1f80);
    TEST_CHECK(memcmp(ymm0, initial_ymm0, sizeof(ymm0)) == 0);

    OK(uc_close(uc));
}

static void test_x86_xsaveopt_xrstor_roundtrip(void)
{
    const uint8_t code[] = {
        0x0f, 0xae, 0x37, /* xsaveopt [rdi] */
        0x0f, 0xae, 0x2f, /* xrstor [rdi] */
    };
    const uint64_t initial_ymm2[4] = {
        0x0123456789abcdefULL,
        0xfedcba9876543210ULL,
        0x0f1e2d3c4b5a6978ULL,
        0x8796a5b4c3d2e1f0ULL,
    };
    const uint64_t changed_ymm2[4] = {
        0x1111111111111111ULL,
        0x2222222222222222ULL,
        0x3333333333333333ULL,
        0x4444444444444444ULL,
    };
    const uint64_t mask =
        TEST_X86_XSTATE_FP | TEST_X86_XSTATE_SSE | TEST_X86_XSTATE_YMM;
    X86CpuidResult cpuid;
    uint64_t ymm2[4];
    uc_engine *uc;

    cpuid = test_x86_cpuid(UC_CPU_X86_HASWELL, 0xd, 1);
    TEST_CHECK((cpuid.eax & TEST_X86_CPUID_D_1_EAX_XSAVEOPT) != 0);

    test_x86_xsave_setup(&uc, UC_CPU_X86_HASWELL, code, sizeof(code));
    OK(uc_reg_write(uc, UC_X86_REG_YMM2, initial_ymm2));
    OK(test_x86_run_xsave_instruction(uc, code_start, 3, TEST_X86_XSAVE_AREA,
                                      mask));
    OK(uc_reg_write(uc, UC_X86_REG_YMM2, changed_ymm2));
    OK(test_x86_run_xsave_instruction(uc, code_start + 3, 3,
                                      TEST_X86_XSAVE_AREA, mask));
    OK(uc_reg_read(uc, UC_X86_REG_YMM2, ymm2));
    TEST_CHECK(memcmp(ymm2, initial_ymm2, sizeof(ymm2)) == 0);

    OK(uc_close(uc));
}

static void test_x86_xsave_xcr0_mask(void)
{
    const uint8_t code[] = {
        0x0f, 0xae, 0x27, /* xsave [rdi] */
        0x0f, 0xae, 0x2f, /* xrstor [rdi] */
    };
    const uint64_t initial_ymm0[4] = {
        0x0011223344556677ULL,
        0x8899aabbccddeeffULL,
        0x1021324354657687ULL,
        0x98a9bacbdcedfe0fULL,
    };
    const uint64_t changed_ymm0[4] = {
        0x1111111111111111ULL,
        0x2222222222222222ULL,
        0x3333333333333333ULL,
        0x4444444444444444ULL,
    };
    const uint64_t expected_ymm0[4] = {
        0x0011223344556677ULL,
        0x8899aabbccddeeffULL,
        0x3333333333333333ULL,
        0x4444444444444444ULL,
    };
    const uint64_t requested_mask =
        TEST_X86_XSTATE_FP | TEST_X86_XSTATE_SSE | TEST_X86_XSTATE_YMM;
    const uint64_t xcr0 = TEST_X86_XSTATE_FP | TEST_X86_XSTATE_SSE;
    uint64_t xstate_bv;
    uint64_t ymm0[4];
    uc_engine *uc;

    test_x86_xsave_setup(&uc, UC_CPU_X86_HASWELL, code, sizeof(code));
    OK(uc_reg_write(uc, UC_X86_REG_XCR0, &xcr0));
    OK(uc_reg_write(uc, UC_X86_REG_YMM0, initial_ymm0));
    OK(test_x86_run_xsave_instruction(uc, code_start, 3, TEST_X86_XSAVE_AREA,
                                      requested_mask));
    OK(uc_mem_read(uc, TEST_X86_XSAVE_AREA + TEST_X86_XSAVE_HEADER_OFFSET,
                   &xstate_bv, sizeof(xstate_bv)));
    TEST_CHECK((xstate_bv & requested_mask) == xcr0);

    OK(uc_reg_write(uc, UC_X86_REG_YMM0, changed_ymm0));
    OK(test_x86_run_xsave_instruction(uc, code_start + 3, 3,
                                      TEST_X86_XSAVE_AREA, requested_mask));
    OK(uc_reg_read(uc, UC_X86_REG_YMM0, ymm0));
    TEST_CHECK(memcmp(ymm0, expected_ymm0, sizeof(ymm0)) == 0);

    OK(uc_close(uc));
}

static void test_x86_xsave_model_gating(void)
{
    const uint8_t code[] = {
        0x0f, 0xae, 0x27, /* xsave [rdi] */
        0x0f, 0xae, 0x37, /* xsaveopt [rdi] */
    };
    const uint64_t avx512_xstate = TEST_X86_XSTATE_OPMASK |
                                   TEST_X86_XSTATE_ZMM_HI256 |
                                   TEST_X86_XSTATE_HI16_ZMM;
    X86CpuidResult cpuid;
    uint64_t supported_xstate;
    uc_engine *uc;

    cpuid = test_x86_cpuid(UC_CPU_X86_PENRYN, 1, 0);
    TEST_CHECK((cpuid.ecx & TEST_X86_CPUID_1_ECX_XSAVE) == 0);
    cpuid = test_x86_cpuid(UC_CPU_X86_PENRYN, 0xd, 0);
    TEST_CHECK(cpuid.eax == 0);
    TEST_CHECK(cpuid.ebx == 0);
    TEST_CHECK(cpuid.ecx == 0);
    TEST_CHECK(cpuid.edx == 0);

    test_x86_xsave_setup(&uc, UC_CPU_X86_PENRYN, code, sizeof(code));
    uc_assert_err(UC_ERR_INSN_INVALID,
                  test_x86_run_xsave_instruction(uc, code_start, 3,
                                                 TEST_X86_XSAVE_AREA,
                                                 TEST_X86_XSTATE_FP));
    OK(uc_close(uc));

    cpuid = test_x86_cpuid(UC_CPU_X86_OPTERON_G4, 1, 0);
    TEST_CHECK((cpuid.ecx & TEST_X86_CPUID_1_ECX_XSAVE) != 0);
    cpuid = test_x86_cpuid(UC_CPU_X86_OPTERON_G4, 0xd, 1);
    TEST_CHECK((cpuid.eax & TEST_X86_CPUID_D_1_EAX_XSAVEOPT) == 0);

    test_x86_xsave_setup(&uc, UC_CPU_X86_OPTERON_G4, code, sizeof(code));
    uc_assert_err(UC_ERR_INSN_INVALID,
                  test_x86_run_xsave_instruction(uc, code_start + 3, 3,
                                                 TEST_X86_XSAVE_AREA,
                                                 TEST_X86_XSTATE_FP));
    OK(uc_close(uc));

    cpuid = test_x86_cpuid(UC_CPU_X86_ICELAKE_CLIENT, 0xd, 0);
    supported_xstate = cpuid.eax | ((uint64_t)cpuid.edx << 32);
    /* TCG filters AVX-512, so guest XSAVE must not expose its state. */
    TEST_CHECK((supported_xstate & avx512_xstate) == 0);
}

static void test_x86_xsave_alignment_fault(void)
{
    const uint8_t code[] = {
        0x0f, 0xae, 0x27, /* xsave [rdi] */
        0x0f, 0xae, 0x2f, /* xrstor [rdi] */
    };
    const uint8_t sentinel[32] = {
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
        0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5, 0xa5,
    };
    const uint64_t mask =
        TEST_X86_XSTATE_FP | TEST_X86_XSTATE_SSE | TEST_X86_XSTATE_YMM;
    X86IntrCapture capture = {0};
    uint8_t memory[sizeof(sentinel)];
    uc_engine *uc;
    uc_hook hook;

    test_x86_xsave_setup(&uc, UC_CPU_X86_HASWELL, code, sizeof(code));
    OK(uc_mem_write(uc, TEST_X86_XSAVE_AREA, sentinel, sizeof(sentinel)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_INTR, test_x86_intr_capture_cb, &capture,
                   1, 0));

    OK(test_x86_run_xsave_instruction(uc, code_start, 3,
                                      TEST_X86_XSAVE_AREA + 1, mask));
    TEST_CHECK(capture.count == 1);
    TEST_CHECK(capture.intno == 13);
    OK(uc_mem_read(uc, TEST_X86_XSAVE_AREA, memory, sizeof(memory)));
    TEST_CHECK(memcmp(memory, sentinel, sizeof(memory)) == 0);
    OK(uc_close(uc));

    capture.count = 0;
    capture.intno = 0;
    test_x86_xsave_setup(&uc, UC_CPU_X86_HASWELL, code, sizeof(code));
    OK(uc_hook_add(uc, &hook, UC_HOOK_INTR, test_x86_intr_capture_cb, &capture,
                   1, 0));
    OK(test_x86_run_xsave_instruction(uc, code_start + 3, 3,
                                      TEST_X86_XSAVE_AREA + 1, mask));
    TEST_CHECK(capture.count == 1);
    TEST_CHECK(capture.intno == 13);

    OK(uc_close(uc));
}

static void test_x86_xsave_pkru_roundtrip(void)
{
    const uint8_t code[] = {
        0x0f, 0x01, 0xef, /* wrpkru */
        0x0f, 0xae, 0x27, /* xsave [rdi] */
        0x0f, 0xae, 0x2f, /* xrstor [rdi] */
        0x0f, 0x01, 0xee, /* rdpkru */
    };
    const uint32_t initial_pkru = 0x5a5aa5a5;
    const uint32_t changed_pkru = 0xa5a55a5a;
    X86CpuidResult cpuid;
    X86CpuidResult pkru_leaf;
    uint64_t xcr0;
    uint64_t cr4;
    uint64_t xstate_bv;
    uint32_t eax;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    uint32_t saved_pkru;
    uc_engine *uc;

    cpuid = test_x86_cpuid(UC_CPU_X86_HASWELL, 7, 0);
    TEST_CHECK((cpuid.ecx & TEST_X86_CPUID_7_0_ECX_PKU) == 0);
    cpuid = test_x86_cpuid(UC_CPU_X86_ICELAKE_CLIENT, 7, 0);
    TEST_CHECK((cpuid.ecx & TEST_X86_CPUID_7_0_ECX_PKU) != 0);
    cpuid = test_x86_cpuid(UC_CPU_X86_ICELAKE_CLIENT, 0xd, 0);
    TEST_CHECK((cpuid.eax & TEST_X86_XSTATE_PKRU) != 0);
    pkru_leaf = test_x86_cpuid(UC_CPU_X86_ICELAKE_CLIENT, 0xd, 9);
    TEST_ASSERT(pkru_leaf.eax == sizeof(saved_pkru) * 2);
    TEST_ASSERT(pkru_leaf.ebx + pkru_leaf.eax <= TEST_X86_XSAVE_AREA_SIZE);

    test_x86_xsave_setup(&uc, UC_CPU_X86_ICELAKE_CLIENT, code, sizeof(code));
    OK(uc_reg_read(uc, UC_X86_REG_XCR0, &xcr0));
    TEST_CHECK((xcr0 & TEST_X86_XSTATE_PKRU) != 0);
    OK(uc_reg_read(uc, UC_X86_REG_CR4, &cr4));
    cr4 |= 1ULL << 22;
    OK(uc_reg_write(uc, UC_X86_REG_CR4, &cr4));

    eax = initial_pkru;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &edx));
    OK(uc_emu_start(uc, code_start, code_start + 3, 0, 1));

    OK(test_x86_run_xsave_instruction(
        uc, code_start + 3, 3, TEST_X86_XSAVE_AREA, TEST_X86_XSTATE_PKRU));
    OK(uc_mem_read(uc, TEST_X86_XSAVE_AREA + pkru_leaf.ebx, &saved_pkru,
                   sizeof(saved_pkru)));
    OK(uc_mem_read(uc, TEST_X86_XSAVE_AREA + TEST_X86_XSAVE_HEADER_OFFSET,
                   &xstate_bv, sizeof(xstate_bv)));
    TEST_CHECK(saved_pkru == initial_pkru);
    TEST_CHECK((xstate_bv & TEST_X86_XSTATE_PKRU) != 0);

    eax = changed_pkru;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &edx));
    OK(uc_emu_start(uc, code_start, code_start + 3, 0, 1));

    OK(test_x86_run_xsave_instruction(
        uc, code_start + 6, 3, TEST_X86_XSAVE_AREA, TEST_X86_XSTATE_PKRU));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_emu_start(uc, code_start + 9, code_start + 12, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == initial_pkru);

    OK(uc_close(uc));
}

static void test_x86_data_watchpoint(void)
{
    const uint64_t data_addr = 0x200000;
    const uint8_t code[] = {
        0xc7, 0x00, 0x44, 0x33, 0x22, 0x11, /* mov dword ptr [rax], 0x11223344 */
    };
    const uint64_t dr7_write_len4 = 1 | (1U << 16) | (3U << 18);
    X86IntrCapture capture = { 0 };
    uint32_t memory = 0;
    uint64_t dr6 = 0;
    uint64_t rax = data_addr;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, (const char *)code,
                    sizeof(code));
    OK(uc_mem_map(uc, data_addr, 0x1000, UC_PROT_ALL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_INTR, test_x86_intr_capture_cb,
                   &capture, 1, 0));
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_DR0, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_DR7, &dr7_write_len4));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_mem_read(uc, data_addr, &memory, sizeof(memory)));
    OK(uc_reg_read(uc, UC_X86_REG_DR6, &dr6));
    TEST_CHECK(capture.count == 1);
    TEST_CHECK(capture.intno == 1);
    TEST_CHECK(memory == 0x11223344);
    TEST_CHECK((dr6 & 1) != 0);

    OK(uc_close(uc));
}

static void test_x86_context_check_debug_state(uc_engine *uc,
                                               X86IntrCapture *capture)
{
    uc_hook hook;

    OK(uc_hook_add(uc, &hook, UC_HOOK_INTR, test_x86_intr_capture_cb,
                   capture, 1, 0));

    OK(uc_emu_start(uc, code_start, code_start + 1, 0, 1));
    TEST_CHECK_(capture->count == 1,
                "breakpoint count=%u intno=%u", capture->count,
                capture->intno);

    capture->count = 0;
    capture->intno = 0;
    OK(uc_emu_start(uc, code_start + 1, code_start + 7, 0, 1));
    TEST_CHECK_(capture->count == 1,
                "watchpoint count=%u intno=%u", capture->count,
                capture->intno);
}

static void test_x86_context_debug_lifecycle(void)
{
    const uint64_t data_address = 0x200000;
    const uint8_t code[] = {
        0x90,                                     /* nop */
        0xc7, 0x00, 0x44, 0x33, 0x22, 0x11,     /* mov [rax], 0x11223344 */
    };
    const uint64_t dr7 = 1U | (1U << 2) | (1U << 20) | (3U << 22);
    X86IntrCapture capture = {0};
    uc_engine *source;
    uc_engine *destination;
    uc_context *context;
    uint64_t breakpoint_address = code_start;
    uint64_t disabled = 0;

    uc_common_setup(&source, UC_ARCH_X86, UC_MODE_64, (const char *)code,
                    sizeof(code));
    uc_common_setup(&destination, UC_ARCH_X86, UC_MODE_64,
                    (const char *)code, sizeof(code));
    OK(uc_mem_map(source, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_map(destination, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_reg_write(source, UC_X86_REG_RAX, &data_address));
    OK(uc_reg_write(source, UC_X86_REG_DR0, &breakpoint_address));
    OK(uc_reg_write(source, UC_X86_REG_DR1, &data_address));
    OK(uc_reg_write(source, UC_X86_REG_DR7, &dr7));
    OK(uc_context_alloc(source, &context));
    OK(uc_context_save(source, context));

    OK(uc_reg_write(source, UC_X86_REG_DR7, &disabled));
    OK(uc_context_restore(source, context));
    test_x86_context_check_debug_state(source, &capture);

    capture = (X86IntrCapture){0};
    OK(uc_close(source));
    OK(uc_context_restore(destination, context));
    test_x86_context_check_debug_state(destination, &capture);

    OK(uc_context_free(context));
    OK(uc_close(destination));
}

// AARCH64 inline the read while s390x won't split the access. Though not tested
// on other hosts but we restrict a bit more.
#if !defined(TARGET_READ_INLINED) && defined(BOOST_LITTLE_ENDIAN)

struct writelog_t {
    uint32_t addr, size;
};

static void test_x86_unaligned_access_callback(uc_engine *uc, uc_mem_type type,
                                               uint64_t address, int size,
                                               int64_t value, void *user_data)
{
    TEST_CHECK(size != 0);
    struct writelog_t *write_log = (struct writelog_t *)user_data;

    for (int i = 0; i < 10; i++) {
        if (write_log[i].size == 0) {
            write_log[i].addr = (uint32_t)address;
            write_log[i].size = (uint32_t)size;
            return;
        }
    }
    TEST_ASSERT(false);
}

static void test_x86_unaligned_access(void)
{
    uc_engine *uc;
    uc_hook hook;
    // mov dword ptr [0x200001], eax; mov eax, dword ptr [0x200001]
    char code[] = "\xa3\x01\x00\x20\x00\xa1\x01\x00\x20\x00";
    uint32_t r_eax = LEINT32(0x41424344);
    struct writelog_t write_log[10];
    struct writelog_t read_log[10];
    memset(write_log, 0, sizeof(write_log));
    memset(read_log, 0, sizeof(read_log));

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, 0x200000, 0x1000, UC_PROT_ALL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_WRITE,
                   test_x86_unaligned_access_callback, write_log, 1, 0));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_unaligned_access_callback, read_log, 1, 0));

    OK(uc_reg_write(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    TEST_CHECK(write_log[0].addr == 0x200001);
    TEST_CHECK(write_log[0].size == 4);
    TEST_CHECK(write_log[1].size == 0);

    TEST_CHECK(read_log[0].addr == 0x200001);
    TEST_CHECK(read_log[0].size == 4);
    TEST_CHECK(read_log[1].size == 0);

    char b;
    OK(uc_mem_read(uc, 0x200001, &b, 1));
    TEST_CHECK(b == 0x44);
    OK(uc_mem_read(uc, 0x200002, &b, 1));
    TEST_CHECK(b == 0x43);
    OK(uc_mem_read(uc, 0x200003, &b, 1));
    TEST_CHECK(b == 0x42);
    OK(uc_mem_read(uc, 0x200004, &b, 1));
    TEST_CHECK(b == 0x41);

    OK(uc_close(uc));
}

static void test_x86_64_unaligned_access(void)
{
    uc_engine *uc;
    uc_hook hook;
    char code[] = {"\x48\x89\x01" //   mov         qword ptr [rcx],rax
                   "\x48\x8b\x00" //  mov         rax,qword ptr [rax]
                   "\xcc"};
    uint64_t r_rax = LEINT64(0x2fffff);
    uint64_t r_rcx = LEINT64(0x2fffff);
    struct writelog_t write_log[10];
    struct writelog_t read_log[10];
    memset(write_log, 0, sizeof(write_log));
    memset(read_log, 0, sizeof(read_log));
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, 0x200000, 0x200000, UC_PROT_ALL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_WRITE,
                   test_x86_unaligned_access_callback, write_log, 1, 0));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_unaligned_access_callback, read_log, 1, 0));

    OK(uc_reg_write(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &r_rcx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 2));

    TEST_CHECK(write_log[0].addr == 0x2fffff);
    TEST_CHECK(write_log[0].size == 8);
    TEST_CHECK(write_log[1].size == 0);

    TEST_CHECK(read_log[0].addr == 0x2fffff);
    TEST_CHECK(read_log[0].size == 8);
    TEST_CHECK(read_log[1].size == 0);

    uint64_t b;
    OK(uc_mem_read(uc, 0x2fffff, &b, 8));
    TEST_CHECK(b == 0x2fffff);

    OK(uc_close(uc));
}
#endif

static bool test_x86_lazy_mapping_mem_callback(uc_engine *uc, uc_mem_type type,
                                               uint64_t address, int size,
                                               int64_t value, void *user_data)
{
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x1000, "\x90\x90", 2)); // nop; nop

    // Handled!
    return true;
}

static void test_x86_lazy_mapping_block_callback(uc_engine *uc,
                                                 uint64_t address,
                                                 uint32_t size, void *user_data)
{
    int *block_count = (int *)user_data;
    (*block_count)++;
}

static void test_x86_lazy_mapping(void)
{
    uc_engine *uc;
    uc_hook mem_hook, block_hook;
    int block_count = 0;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_hook_add(uc, &mem_hook, UC_HOOK_MEM_FETCH_UNMAPPED,
                   test_x86_lazy_mapping_mem_callback, NULL, 1, 0));
    OK(uc_hook_add(uc, &block_hook, UC_HOOK_BLOCK,
                   test_x86_lazy_mapping_block_callback, &block_count, 1, 0));

    OK(uc_emu_start(uc, 0x1000, 0x1002, 0, 0));
    TEST_CHECK(block_count == 1);
    OK(uc_close(uc));
}

static void test_x86_16_incorrect_ip_cb(uc_engine *uc, uint64_t address,
                                        uint32_t size, void *data)
{
    uint16_t cs, ip;

    OK(uc_reg_read(uc, UC_X86_REG_CS, &cs));
    OK(uc_reg_read(uc, UC_X86_REG_IP, &ip));

    TEST_CHECK(cs == 0x20);
    TEST_CHECK(address == ((cs << 4) + ip));
}

static void test_x86_16_incorrect_ip(void)
{
    uc_engine *uc;
    uc_hook hk1, hk2;
    uint16_t cs = 0x20;
    char code[] = "\x41"; // INC cx;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_16, code, sizeof(code) - 1);

    OK(uc_hook_add(uc, &hk1, UC_HOOK_BLOCK, test_x86_16_incorrect_ip_cb, NULL,
                   1, 0));
    OK(uc_hook_add(uc, &hk2, UC_HOOK_CODE, test_x86_16_incorrect_ip_cb, NULL, 1,
                   0));

    OK(uc_reg_write(uc, UC_X86_REG_CS, &cs));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

// Porting to BE: Only uc_mem_read/write needs endian fixing
static void test_x86_mmu_prepare_tlb(uc_engine *uc, uint64_t vaddr,
                                     uint64_t tlb_base)
{
    uint64_t cr0;
    uint64_t cr4;
    uc_x86_msr msr = {.rid = 0x0c0000080, .value = 0};
    uint64_t pml4o = ((vaddr & 0x00ff8000000000) >> 39) * 8;
    uint64_t pdpo = ((vaddr & 0x00007fc0000000) >> 30) * 8;
    uint64_t pdo = ((vaddr & 0x0000003fe00000) >> 21) * 8;
    uint64_t pml4e = (tlb_base + 0x1000) | 1 | (1 << 2);
    uint64_t pdpe = (tlb_base + 0x2000) | 1 | (1 << 2);
    uint64_t pde = (tlb_base + 0x3000) | 1 | (1 << 2);
    uint64_t pml4e_mem = LEINT64(pml4e);
    uint64_t pde_mem = LEINT64(pde);
    uint64_t pdpe_mem = LEINT64(pdpe);
    OK(uc_mem_write(uc, tlb_base + pml4o, &pml4e_mem, sizeof(pml4o)));
    OK(uc_mem_write(uc, tlb_base + 0x1000 + pdpo, &pdpe_mem, sizeof(pdpe)));
    OK(uc_mem_write(uc, tlb_base + 0x2000 + pdo, &pde_mem, sizeof(pde)));
    OK(uc_reg_write(uc, UC_X86_REG_CR3, &tlb_base));
    OK(uc_reg_read(uc, UC_X86_REG_CR0, &cr0));
    OK(uc_reg_read(uc, UC_X86_REG_CR4, &cr4));
    OK(uc_reg_read(uc, UC_X86_REG_MSR, &msr));
    cr0 |= 1;
    cr0 |= 1l << 31;
    cr4 |= 1l << 5;
    msr.value |= 1l << 8;
    OK(uc_reg_write(uc, UC_X86_REG_CR0, &cr0));
    OK(uc_reg_write(uc, UC_X86_REG_CR4, &cr4));
    OK(uc_reg_write(uc, UC_X86_REG_MSR, &msr));
}

static void test_x86_mmu_pt_set(uc_engine *uc, uint64_t vaddr, uint64_t paddr,
                                uint64_t tlb_base, bool readwrite)
{
    uint64_t pto = ((vaddr & 0x000000001ff000) >> 12) * 8;
    uint32_t pte;
    if (readwrite)
        pte = (paddr) | 1 | (1 << 2);
    else
        pte = (paddr) | 1;
    pte = LEINT32(pte);

    uc_mem_write(uc, tlb_base + 0x3000 + pto, &pte, sizeof(pte));
}

static void test_x86_mmu_callback(uc_engine *uc, void *userdata)
{
    bool *parrent_done = userdata;
    uint64_t rax;
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    switch (rax) {
    case 57:
        /* fork */
        break;
    case 60:
        /* exit */
        uc_emu_stop(uc);
        return;
    default:
        TEST_CHECK(false);
    }

    if (!(*parrent_done)) {
        *parrent_done = true;
        rax = 27;
        OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
        uc_emu_stop(uc);
    }
}

static void test_x86_mmu(void)
{
    bool parrent_done = false;
    uint64_t tlb_base = 0x3000;
    uint64_t parrent, child;
    uint64_t rax, rip;
    uc_context *context;
    uc_engine *uc;
    uc_hook h1;

    /*
     * mov rax, 57
     * syscall
     * test rax, rax
     * jz child
     * xor rax, rax
     * mov rax, 60
     * mov [0x4000], rax
     * syscall
     *
     * child:
     * xor rcx, rcx
     * mov rcx, 42
     * mov [0x4000], rcx
     * mov rax, 60
     * syscall
     */
    char code[] =
        "\xB8\x39\x00\x00\x00\x0F\x05\x48\x85\xC0\x74\x0F\xB8\x3C\x00\x00\x00"
        "\x48\x89\x04\x25\x00\x40\x00\x00\x0F\x05\xB9\x2A\x00\x00\x00\x48\x89"
        "\x0C\x25\x00\x40\x00\x00\xB8\x3C\x00\x00\x00\x0F\x05";

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_tlb_mode(uc, UC_TLB_CPU));
    OK(uc_hook_add(uc, &h1, UC_HOOK_INSN, &test_x86_mmu_callback, &parrent_done,
                   1, 0, UC_X86_INS_SYSCALL));
    OK(uc_context_alloc(uc, &context));

    OK(uc_mem_map(uc, 0x0, 0x1000, UC_PROT_ALL)); // Code
    OK(uc_mem_write(uc, 0x0, code, sizeof(code) - 1));
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));   // Parrent
    OK(uc_mem_map(uc, 0x2000, 0x1000, UC_PROT_ALL));   // Child
    OK(uc_mem_map(uc, tlb_base, 0x4000, UC_PROT_ALL)); // TLB

    test_x86_mmu_prepare_tlb(uc, 0x0, tlb_base);
    test_x86_mmu_pt_set(uc, 0x2000, 0x0, tlb_base, true);
    test_x86_mmu_pt_set(uc, 0x4000, 0x1000, tlb_base, true);

    OK(uc_ctl_flush_tlb(uc));
    OK(uc_emu_start(uc, 0x2000, 0x0, 0, 0));

    OK(uc_context_save(uc, context));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &rip));

    OK(uc_emu_start(uc, rip, 0x0, 0, 0));

    /* restore for child */
    OK(uc_context_restore(uc, context));
    test_x86_mmu_prepare_tlb(uc, 0x0, tlb_base);
    test_x86_mmu_pt_set(uc, 0x4000, 0x2000, tlb_base, true);
    rax = 0;
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_ctl_flush_tlb(uc));

    OK(uc_emu_start(uc, rip, 0x0, 0, 0));
    OK(uc_mem_read(uc, 0x1000, &parrent, sizeof(parrent)));
    OK(uc_mem_read(uc, 0x2000, &child, sizeof(child)));
    TEST_CHECK(LEINT64(parrent) == 60);
    TEST_CHECK(LEINT64(child) == 42);
    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_x86_read_virtual(void)
{
    bool parrent_done = false;
    uint64_t tlb_base = 0x3000;
    uint64_t parrent, child, tmp;
    uint64_t rax, rip;
    uc_context *context;
    uc_engine *uc;
    uc_hook h1;

    /*
     * mov rax, 57
     * syscall
     * test rax, rax
     * jz child
     * xor rax, rax
     * mov rax, 60
     * mov [0x4000], rax
     * syscall
     *
     * child:
     * xor rcx, rcx
     * mov rcx, 42
     * mov [0x4000], rcx
     * mov rax, 60
     * syscall
     */
    char code[] =
        "\xB8\x39\x00\x00\x00\x0F\x05\x48\x85\xC0\x74\x0F\xB8\x3C\x00\x00\x00"
        "\x48\x89\x04\x25\x00\x40\x00\x00\x0F\x05\xB9\x2A\x00\x00\x00\x48\x89"
        "\x0C\x25\x00\x40\x00\x00\xB8\x3C\x00\x00\x00\x0F\x05";

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_tlb_mode(uc, UC_TLB_CPU));
    OK(uc_hook_add(uc, &h1, UC_HOOK_INSN, &test_x86_mmu_callback, &parrent_done,
                   1, 0, UC_X86_INS_SYSCALL));
    OK(uc_context_alloc(uc, &context));

    OK(uc_mem_map(uc, 0x0, 0x1000, UC_PROT_ALL)); // Code
    OK(uc_mem_write(uc, 0x0, code, sizeof(code) - 1));
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));   // Parrent
    OK(uc_mem_map(uc, 0x2000, 0x1000, UC_PROT_ALL));   // Child
    OK(uc_mem_map(uc, tlb_base, 0x4000, UC_PROT_ALL)); // TLB

    test_x86_mmu_prepare_tlb(uc, 0x0, tlb_base);
    test_x86_mmu_pt_set(uc, 0x2000, 0x0, tlb_base, false);
    test_x86_mmu_pt_set(uc, 0x4000, 0x1000, tlb_base, true);

    OK(uc_ctl_flush_tlb(uc));
    OK(uc_emu_start(uc, 0x2000, 0x0, 0, 0));

    OK(uc_context_save(uc, context));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &rip));

    OK(uc_emu_start(uc, rip, 0x0, 0, 0));
    OK(uc_vmem_read(uc, 0x4000, UC_PROT_READ, &parrent,
                           sizeof(parrent)));

    /* restore for child */
    OK(uc_context_restore(uc, context));
    test_x86_mmu_prepare_tlb(uc, 0x0, tlb_base);
    test_x86_mmu_pt_set(uc, 0x4000, 0x2000, tlb_base, true);
    rax = 0;
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_ctl_flush_tlb(uc));

    OK(uc_emu_start(uc, rip, 0x0, 0, 0));
    OK(uc_vmem_read(uc, 0x4000, UC_PROT_READ, &child, sizeof(child)));
    uc_assert_err(
        UC_ERR_READ_PROT,
        uc_vmem_read(uc, 0x1000, UC_PROT_WRITE, &tmp, sizeof(tmp)));
    TEST_CHECK(parrent == 60);
    TEST_CHECK(child == 42);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static bool test_x86_vtlb_callback(uc_engine *uc, uint64_t addr,
                                   uc_mem_type type, uc_tlb_entry *result,
                                   void *user_data)
{
    if (user_data != NULL) {
        (*(uint32_t *)user_data)++;
    }
    result->paddr = addr;
    result->perms = UC_PROT_ALL;
    return true;
}

typedef struct X86VtlbExitData {
    uc_err stop_error;
    uint32_t data_fill_count;
    bool stop;
} X86VtlbExitData;

static bool test_x86_vtlb_exit_callback(uc_engine *uc, uint64_t addr,
                                        uc_mem_type type,
                                        uc_tlb_entry *result, void *user_data)
{
    X86VtlbExitData *data = (X86VtlbExitData *)user_data;

    result->paddr = addr;
    result->perms = UC_PROT_ALL;
    if (type == UC_MEM_READ) {
        data->data_fill_count++;
        if (data->stop) {
            data->stop_error = uc_emu_stop(uc);
        }
    }
    return true;
}

static void test_x86_cputlb_vtlb_fill_exit(void)
{
    const uint8_t code[] = {
        0xa1, 0x00, 0x00, 0x20, 0x00, /* mov eax,[0x200000] */
        0x43,                         /* inc ebx */
    };
    const uint32_t memory_value = 0x78563412;
    const uint32_t initial_eax = 0xdeadbeef;
    X86VtlbExitData data = {.stop = true};
    uint32_t eax = initial_eax;
    uint32_t ebx = 0;
    uint32_t eip = 0;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_mem_map(uc, 0x200000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x200000, &memory_value, sizeof(memory_value)));
    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL,
                   test_x86_vtlb_exit_callback, &data, 1, 0));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(data.stop_error);
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    TEST_CHECK(data.data_fill_count == 1);
    TEST_CHECK(eip == code_start);
    TEST_CHECK(eax == initial_eax);
    TEST_CHECK(ebx == 0);

    data.stop = false;
    OK(uc_emu_start(uc, eip, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    TEST_CHECK(data.data_fill_count == 1);
    TEST_CHECK(eax == memory_value);
    TEST_CHECK(ebx == 1);

    OK(uc_close(uc));
}

static void test_x86_vtlb(void)
{
    uc_engine *uc;
    uc_hook hook;
    char code[] = "\xeb\x02\x90\x90\x90\x90\x90\x90"; // jmp 4; nop; nop; nop;
                                                      // nop; nop; nop
    uint32_t r_eip = 0;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL, test_x86_vtlb_callback, NULL, 1,
                   0));

    OK(uc_emu_start(uc, code_start, code_start + 4, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EIP, &r_eip));

    TEST_CHECK(r_eip == code_start + 4);

    OK(uc_close(uc));
}

static void test_x86_vtlb_conflict_growth(void)
{
    const uint64_t data_address = 0x200000;
    const uint64_t page_size = 0x1000;
    const uint64_t page_count = 512;
    const uint64_t rounds = 3;
    const uint8_t code[] = {
        0x48, 0x89, 0xde,                   /* mov rsi, rbx */
        0x4c, 0x89, 0xc2,                   /* mov rdx, r8 */
        0x48, 0x03, 0x06,                   /* add rax, [rsi] */
        0x48, 0x81, 0xc6, 0x00, 0x10, 0x00, 0x00,
                                                /* add rsi, 0x1000 */
        0x48, 0xff, 0xca,                   /* dec rdx */
        0x75, 0xf1,                         /* jnz inner */
        0x48, 0xff, 0xc9,                   /* dec rcx */
        0x75, 0xe6,                         /* jnz outer */
    };
    uint64_t rax = 0;
    uint64_t rbx = data_address;
    uint64_t rcx = rounds;
    uint64_t r8 = page_count;
    uint64_t one = 1;
    uint32_t fill_count = 0;
    uc_engine *uc;
    uc_hook hook;
    uint64_t i;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, (const char *)code,
                    sizeof(code));
    OK(uc_mem_map(uc, data_address, page_count * page_size, UC_PROT_ALL));
    for (i = 0; i < page_count; i++) {
        OK(uc_mem_write(uc, data_address + i * page_size, &one,
                        sizeof(one)));
    }

    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL, test_x86_vtlb_callback,
                   &fill_count, 1, 0));
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &rcx));
    OK(uc_reg_write(uc, UC_X86_REG_R8, &r8));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    TEST_CHECK(rax == page_count * rounds);
    TEST_CHECK(fill_count < page_count * rounds);

    OK(uc_close(uc));
}

static void test_x86_vtlb_stride_conflict_growth(void)
{
    const uint64_t data_address = 0x200000;
    const uint64_t stride = 0x100000;
    const uint64_t page_count = 16;
    const uint64_t rounds = 64;
    const uint8_t code[] = {
        0x48, 0x8b, 0x06,                   /* mov rax, [rsi] */
        0x48, 0x81, 0xc6, 0x00, 0x00, 0x10, 0x00,
                                                 /* add rsi, 0x100000 */
        0x48, 0x39, 0xfe,                   /* cmp rsi, rdi */
        0x75, 0x03,                         /* jne no_wrap */
        0x48, 0x89, 0xde,                   /* mov rsi, rbx */
        0x48, 0xff, 0xc9,                   /* dec rcx */
        0x75, 0xe9,                         /* jnz loop */
    };
    uint64_t rax = 0;
    uint64_t rbx = data_address;
    uint64_t rcx = page_count * rounds;
    uint64_t rdi = data_address + stride * page_count;
    uint64_t rsi = data_address;
    uint64_t remaining;
    uint64_t value = 1;
    uint32_t fill_count = 0;
    uint64_t i;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, (const char *)code,
                    sizeof(code));
    OK(uc_mem_map(uc, data_address, stride * page_count, UC_PROT_ALL));
    for (i = 0; i < page_count; i++) {
        OK(uc_mem_write(uc, data_address + i * stride, &value,
                        sizeof(value)));
    }

    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL, test_x86_vtlb_callback,
                   &fill_count, 1, 0));
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &rcx));
    OK(uc_reg_write(uc, UC_X86_REG_RDI, &rdi));
    OK(uc_reg_write(uc, UC_X86_REG_RSI, &rsi));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RCX, &remaining));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));

    TEST_CHECK(remaining == 0);
    TEST_CHECK(rax == value);
    TEST_CHECK_(fill_count < page_count * rounds + 1,
                "fill_count=%u", fill_count);

    OK(uc_close(uc));
}

static void test_x86_vtlb_victim_memory_hook(uc_engine *uc,
                                             uc_mem_type type,
                                             uint64_t address, int size,
                                             int64_t value, void *user_data)
{
    (*(uint32_t *)user_data)++;
}

static void test_x86_vtlb_hooked_victim_hit(void)
{
    const uint64_t address_a = 0x200000;
    const uint64_t address_b = address_a + 0x100000;
    const uint64_t value_a = 7;
    const uint64_t value_b = 11;
    const uint8_t code[] = {
        0x48, 0x8b, 0x03, /* mov rax, [rbx] */
        0x48, 0x8b, 0x11, /* mov rdx, [rcx] */
        0x48, 0x03, 0x03, /* add rax, [rbx] */
    };
    uint64_t rax = 0;
    uint64_t rbx = address_a;
    uint64_t rcx = address_b;
    uint64_t rdx = 0;
    uint32_t fill_count = 0;
    uint32_t hook_count = 0;
    uc_engine *uc;
    uc_hook fill_hook;
    uc_hook read_hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, (const char *)code,
                    sizeof(code));
    OK(uc_mem_map(uc, address_a, 0x1000, UC_PROT_ALL));
    OK(uc_mem_map(uc, address_b, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address_a, &value_a, sizeof(value_a)));
    OK(uc_mem_write(uc, address_b, &value_b, sizeof(value_b)));

    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &fill_hook, UC_HOOK_TLB_FILL,
                   test_x86_vtlb_callback, &fill_count, 1, 0));
    OK(uc_hook_add(uc, &read_hook, UC_HOOK_MEM_READ,
                   test_x86_vtlb_victim_memory_hook, &hook_count,
                   address_a, address_a + 0xfff));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &rcx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_read(uc, UC_X86_REG_RDX, &rdx));

    TEST_CHECK(rax == value_a * 2);
    TEST_CHECK(rdx == value_b);
    TEST_CHECK(hook_count == 2);
    TEST_CHECK(fill_count == 3);

    OK(uc_close(uc));
}

typedef struct {
    uint64_t virtual_address;
    uint64_t physical_address;
} X86HighPaddrTlbData;

static bool test_x86_vtlb_high_paddr_callback(uc_engine *uc, uint64_t address,
                                              uc_mem_type type,
                                              uc_tlb_entry *result,
                                              void *user_data)
{
    X86HighPaddrTlbData *data = (X86HighPaddrTlbData *)user_data;

    if (address == data->virtual_address) {
        result->paddr = data->physical_address;
    } else {
        result->paddr = address;
    }
    result->perms = UC_PROT_ALL;
    return true;
}

static void test_x86_vtlb_32bit_high_paddr(void)
{
    const uint64_t virtual_address = 0x2000;
    const uint64_t physical_address = UINT64_C(0x100000000);
    const char code[] = "\xa1\x00\x20\x00\x00"; /* mov eax, [0x2000] */
    const uint32_t expected = 0x89abcdef;
    X86HighPaddrTlbData data = {
        .virtual_address = virtual_address,
        .physical_address = physical_address,
    };
    uint64_t translated;
    uint32_t eax;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, physical_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, physical_address, &expected, sizeof(expected)));
    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL,
                   test_x86_vtlb_high_paddr_callback, &data, 1, 0));

    OK(uc_vmem_translate(uc, virtual_address, UC_PROT_READ, &translated));
    TEST_CHECK(translated == physical_address);
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == expected);

    OK(uc_close(uc));
}

static void test_x86_segmentation(void)
{
    uc_engine *uc;
    uint16_t fs = 0x53;
    uc_x86_mmr gdtr = {0, 0xfffff8076d962000, 0x57, 0};

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_reg_write(uc, UC_X86_REG_GDTR, &gdtr));
    uc_assert_err(UC_ERR_EXCEPTION, uc_reg_write(uc, UC_X86_REG_FS, &fs));
    OK(uc_close(uc));
}

static void test_x86_0xff_lcall_callback(uc_engine *uc, uint64_t address,
                                         uint32_t size, void *user_data)
{
    // do nothing
    return;
}

// This aborts prior to a7a5d187e77f7853755eff4768658daf8095c3b7
static void test_x86_0xff_lcall(void)
{
    uc_engine *uc;
    uc_hook hk;
    const char code[] =
        "\xB8\x01\x00\x00\x00\xBB\x01\x00\x00\x00\xB9\x01\x00\x00\x00\xFF\xDD"
        "\xBA\x01\x00\x00\x00\xB8\x02\x00\x00\x00\xBB\x02\x00\x00\x00";
    // Taken from #1842
    // 0:  b8 01 00 00 00          mov    eax,0x1
    // 5:  bb 01 00 00 00          mov    ebx,0x1
    // a:  b9 01 00 00 00          mov    ecx,0x1
    // f:  ff                      (bad)
    // 10: dd ba 01 00 00 00       fnstsw WORD PTR [edx+0x1]
    // 16: b8 02 00 00 00          mov    eax,0x2
    // 1b: bb 02 00 00 00          mov    ebx,0x2

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    OK(uc_hook_add(uc, &hk, UC_HOOK_CODE, test_x86_0xff_lcall_callback, NULL, 1,
                   0));

    uc_assert_err(
        UC_ERR_INSN_INVALID,
        uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

static void test_x86_64_not_overwriting_tmp0_for_pc_update_cb(
    uc_engine *uc, uc_mem_type type, uint64_t address, int size, int64_t value,
    void *user_data)
{
}

// https://github.com/unicorn-engine/unicorn/issues/1717
// https://github.com/unicorn-engine/unicorn/issues/1862
static void test_x86_64_not_overwriting_tmp0_for_pc_update(void)
{
    uc_engine *uc;
    uc_hook hk;
    const char code[] = "\x48\xb9\xff\xff\xff\xff\xff\xff\xff\xff\x48\x89\x0c"
                        "\x24\x48\xd3\x24\x24\x73\x0a";
    uint64_t rsp, pc;
    uint32_t eflags;

    // 0x1000: movabs  rcx, 0xffffffffffffffff
    // 0x100a: mov     qword ptr [rsp], rcx
    // 0x100e: shl     qword ptr [rsp], cl ; (Shift to CF=1)
    // 0x1012: jae     0xd ; this jump should not be taken! (CF=1 but jae
    // expects CF=0)
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_hook_add(uc, &hk, UC_HOOK_MEM_READ | UC_HOOK_MEM_WRITE,
                   test_x86_64_not_overwriting_tmp0_for_pc_update_cb, NULL, 1,
                   0));

    rsp = 0x2000;
    OK(uc_reg_write(uc, UC_X86_REG_RSP, (void *)&rsp));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 4));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &pc));
    OK(uc_reg_read(uc, UC_X86_REG_EFLAGS, &eflags));

    TEST_CHECK(pc == 0x1014);
    TEST_CHECK((eflags & 0x1) == 1);

    OK(uc_close(uc));
}

static void test_fxsave_fpip_x86(void)
{
    // note: fxsave was introduced in Pentium II
    uint8_t code_x86[] = {
        // help testing through NOP offset      [disassembly in at&t syntax]
        0x90, 0x90, 0x90, 0x90, // nop nop nop nop
        // run a floating point instruction
        0xdb, 0xc9, // fcmovne %st(1), %st
        // fxsave needs 512 bytes of storage space
        0x81, 0xec, 0x00, 0x02, 0x00, 0x00, // subl $512, %esp
        // fxsave needs a 16-byte aligned address for storage
        0x83, 0xe4, 0xf0, // andl $0xfffffff0, %esp
        // store fxsave data on the stack
        0x0f, 0xae, 0x04, 0x24, // fxsave (%esp)
        // fxsave stores FPIP at an 8-byte offset, move FPIP to eax register
        0x8b, 0x44, 0x24, 0x08 // movl 0x8(%esp), %eax
    };
    uint32_t X86_NOP_OFFSET = 4;
    uint32_t stack_top = (uint32_t)MEM_STACK;
    uint32_t value;
    uc_engine *uc;

    // initialize emulator in X86-32bit mode
    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));

    // map 1MB of memory for this emulation
    OK(uc_mem_map(uc, MEM_BASE, MEM_SIZE, UC_PROT_ALL));
    OK(uc_mem_write(uc, MEM_TEXT, code_x86, sizeof(code_x86)));
    OK(uc_reg_write(uc, UC_X86_REG_ESP, &stack_top));
    OK(uc_emu_start(uc, MEM_TEXT, MEM_TEXT + sizeof(code_x86), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &value));
    TEST_CHECK(value == ((uint32_t)MEM_TEXT + X86_NOP_OFFSET));
    OK(uc_mem_unmap(uc, MEM_BASE, MEM_SIZE));
    OK(uc_close(uc));
}

static void test_fxsave_fpip_x64(void)
{
    uint8_t code_x64[] = {
        // help testing through NOP offset     [disassembly in at&t]
        0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, 0x90, // nops
        // run a floating point instruction
        0xdb, 0xc9, // fcmovne %st(1), %st
        // fxsave64 needs 512 bytes of storage space
        0x48, 0x81, 0xec, 0x00, 0x02, 0x00, 0x00, // subq $512, %rsp
        // fxsave needs a 16-byte aligned address for storage
        0x48, 0x83, 0xe4, 0xf0, // andq 0xfffffffffffffff0, %rsp
        // store fxsave64 data on the stack
        0x48, 0x0f, 0xae, 0x04, 0x24, // fxsave64 (%rsp)
        // fxsave64 stores FPIP at an 8-byte offset, move FPIP to rax register
        0x48, 0x8b, 0x44, 0x24, 0x08, // movq 0x8(%rsp), %rax
    };

    uint64_t stack_top = (uint64_t)MEM_STACK;
    uint64_t X64_NOP_OFFSET = 8;
    uint64_t value;
    uc_engine *uc;

    // initialize emulator in X86-32bit mode
    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    // map 1MB of memory for this emulation
    OK(uc_mem_map(uc, MEM_BASE, MEM_SIZE, UC_PROT_ALL));
    OK(uc_mem_write(uc, MEM_TEXT, code_x64, sizeof(code_x64)));
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &stack_top));
    OK(uc_emu_start(uc, MEM_TEXT, MEM_TEXT + sizeof(code_x64), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &value));
    TEST_CHECK(value == ((uint64_t)MEM_TEXT + X64_NOP_OFFSET));
    OK(uc_mem_unmap(uc, MEM_BASE, MEM_SIZE));
    OK(uc_close(uc));
}

static void test_bswap_ax(void)
{
    // References:
    // - https://gynvael.coldwind.pl/?id=268
    // - https://github.com/JonathanSalwan/Triton/issues/1131
    {
        uint8_t code[] = {
            // bswap ax
            0x66,
            0x0F,
            0xC8,
        };
        TEST_CODE(UC_MODE_32, code);
        TEST_IN_REG(EAX, 0x44332211);
        TEST_OUT_REG(EAX, 0x44330000);
        TEST_RUN();
    }
    {
        uint8_t code[] = {
            // bswap ax
            0x66,
            0x0F,
            0xC8,
        };
        TEST_CODE(UC_MODE_64, code);
        TEST_IN_REG(RAX, 0x8877665544332211);
        TEST_OUT_REG(RAX, 0x8877665544330000);
        TEST_RUN();
    }
    {
        uint8_t code[] = {
            // bswap rax (66h ignored)
            0x66,
            0x48,
            0x0F,
            0xC8,
        };
        TEST_CODE(UC_MODE_64, code);
        TEST_IN_REG(RAX, 0x8877665544332211);
        TEST_OUT_REG(RAX, 0x1122334455667788);
        TEST_RUN();
    }
    {
        uint8_t code[] = {
            // bswap ax (rex ignored)
            0x48,
            0x66,
            0x0F,
            0xC8,
        };
        TEST_CODE(UC_MODE_64, code);
        TEST_IN_REG(RAX, 0x8877665544332211);
        TEST_OUT_REG(RAX, 0x8877665544330000);
        TEST_RUN();
    }
    {
        uint8_t code[] = {
            // bswap eax
            0x0F,
            0xC8,
        };
        TEST_CODE(UC_MODE_32, code);
        TEST_IN_REG(EAX, 0x44332211);
        TEST_OUT_REG(EAX, 0x11223344);
        TEST_RUN();
    }
    {
        uint8_t code[] = {
            // bswap eax
            0x0F,
            0xC8,
        };
        TEST_CODE(UC_MODE_64, code);
        TEST_IN_REG(RAX, 0x8877665544332211);
        TEST_OUT_REG(RAX, 0x0000000011223344);
        TEST_RUN();
    }
}

static void test_rex_x64(void)
{
    {
        uint8_t code[] = {
            // mov ax, bx (rex.w ignored)
            0x48,
            0x66,
            0x89,
            0xD8,
        };
        TEST_CODE(UC_MODE_64, code);
        TEST_IN_REG(RAX, 0x8877665544332211);
        TEST_IN_REG(RBX, 0x1122334455667788);
        TEST_OUT_REG(RAX, 0x8877665544337788);
        TEST_RUN();
    }
    {
        uint8_t code[] = {
            // mov rax, rbx (66h ignored)
            0x66,
            0x48,
            0x89,
            0xD8,
        };
        TEST_CODE(UC_MODE_64, code);
        TEST_IN_REG(RAX, 0x8877665544332211);
        TEST_IN_REG(RBX, 0x1122334455667788);
        TEST_OUT_REG(RAX, 0x1122334455667788);
        TEST_RUN();
    }
    {
        uint8_t code[] = {
            // mov ax, bx (expected encoding)
            0x66,
            0x89,
            0xD8,
        };
        TEST_CODE(UC_MODE_64, code);
        TEST_IN_REG(RAX, 0x8877665544332211);
        TEST_IN_REG(RBX, 0x1122334455667788);
        TEST_OUT_REG(RAX, 0x8877665544337788);
        TEST_RUN();
    }
}

static void test_x86_ro_segfault_cb(uc_engine *uc, uc_mem_type type,
                                    uint64_t address, int size, int64_t value,
                                    void *user_data)
{
    const char code[] = "\xA1\x00\x10\x00\x00\xA1\x00\x10\x00\x00";
    OK(uc_mem_write(uc, address, code, sizeof(code) - 1));
}

static void test_x86_ro_segfault(void)
{
    uc_engine *uc;
    // mov eax, [0x1000]
    // mov eax, [0x1000]
    const char code[] = "\xA1\x00\x10\x00\x00\xA1\x00\x10\x00\x00";
    uint32_t out;
    uc_hook hh;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, 0, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0, code, sizeof(code) - 1));
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_READ));

    OK(uc_hook_add(uc, &hh, UC_HOOK_MEM_READ, test_x86_ro_segfault_cb, NULL, 1,
                   0));
    OK(uc_emu_start(uc, 0, sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, (void *)&out));
    TEST_CHECK(out == 0x001000a1);
    OK(uc_close(uc));
}

static void test_x86_vpermilps_null_ptr_call(void)
{
    char code_modrm_00[] = {
        0x0f, 0x38, 0x0c, 0x00
    };

    char code_modrm_ff[] = {
        0x0f, 0x38, 0x0c, 0xff
    };

    uc_engine *uc = NULL;
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code_modrm_00, sizeof(code_modrm_00));

    uc_assert_err(UC_ERR_INSN_INVALID,
            uc_emu_start(uc, code_start, code_start + sizeof(code_modrm_00), 0, 0));

    OK(uc_close(uc));

    uc = NULL;
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code_modrm_ff, sizeof(code_modrm_ff));

    uc_assert_err(UC_ERR_INSN_INVALID,
            uc_emu_start(uc, code_start, code_start + sizeof(code_modrm_ff), 0, 0));

    OK(uc_close(uc));
}

static int test_x86_hook_insn_rdtsc_cb(uc_engine *uc, void *user_data)
{
    uint64_t h = 0x00000000FEDCBA98;
    OK(uc_reg_write(uc, UC_X86_REG_RDX, &h));

    uint64_t l = 0x0000000076543210;
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &l));

    return true;
}

static void test_x86_hook_insn_rdtsc(void)
{
    char code[] = "\x0F\x31"; // RDTSC

    uc_engine *uc;
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof code - 1);

    uc_hook hook;
    OK(uc_hook_add(uc, &hook, UC_HOOK_INSN, test_x86_hook_insn_rdtsc_cb, NULL,
                   1, 0, UC_X86_INS_RDTSC));

    OK(uc_emu_start(uc, code_start, code_start + sizeof code - 1, 0, 0));

    OK(uc_hook_del(uc, hook));

    uint64_t h = 0;
    OK(uc_reg_read(uc, UC_X86_REG_RDX, &h));
    TEST_CHECK(h == 0x00000000FEDCBA98);

    uint64_t l = 0;
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &l));
    TEST_CHECK(l == 0x0000000076543210);

    OK(uc_close(uc));
}

static int test_x86_hook_insn_rdtscp_cb(uc_engine *uc, void *user_data)
{
    uint64_t h = 0x0000000001234567;
    OK(uc_reg_write(uc, UC_X86_REG_RDX, &h));

    uint64_t l = 0x0000000089ABCDEF;
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &l));

    uint64_t i = 0x00000000DEADBEEF;
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &i));

    return true;
}

static void test_x86_hook_insn_rdtscp(void)
{
    uc_engine *uc;
    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));

    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));

    char code[] = "\x0F\x01\xF9"; // RDTSCP
    OK(uc_mem_write(uc, code_start, code, sizeof code - 1));

    uc_hook hook;
    OK(uc_hook_add(uc, &hook, UC_HOOK_INSN, test_x86_hook_insn_rdtscp_cb, NULL,
                   1, 0, UC_X86_INS_RDTSCP));

    OK(uc_emu_start(uc, code_start, code_start + sizeof code - 1, 0, 0));

    OK(uc_hook_del(uc, hook));

    uint64_t h = 0;
    OK(uc_reg_read(uc, UC_X86_REG_RDX, &h));
    TEST_CHECK(h == 0x0000000001234567);

    uint64_t l = 0;
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &l));
    TEST_CHECK(l == 0x0000000089ABCDEF);

    uint64_t i = 0;
    OK(uc_reg_read(uc, UC_X86_REG_RCX, &i));
    TEST_CHECK(i == 0x00000000DEADBEEF);

    OK(uc_close(uc));
}

static int test_x86_hook_insn_wrmsr_cb(uc_engine *uc, void *user_data)
{
    *(int *)user_data = 1;
    return 0;
}

static void test_x86_hook_insn_wrmsr(void)
{
    /* WRMSR (0f 30) */
    char code[] = "\x0F\x30";
    int fired = 0;

    uc_engine *uc;
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof code - 1);

    uint64_t rcx = 0x174; /* MSR_IA32_SYSENTER_CS */
    uint64_t rax = 0x10;
    uint64_t rdx = 0;
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &rcx));
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_RDX, &rdx));

    uc_hook hook;
    OK(uc_hook_add(uc, &hook, UC_HOOK_INSN, test_x86_hook_insn_wrmsr_cb,
                   &fired, 1, 0, UC_X86_INS_WRMSR));

    OK(uc_emu_start(uc, code_start, code_start + sizeof code - 1, 0, 0));

    OK(uc_hook_del(uc, hook));
    TEST_CHECK(fired == 1);
    OK(uc_close(uc));
}

static int test_x86_hook_insn_rdmsr_cb(uc_engine *uc, void *user_data)
{
    uint64_t eax = 0xDEAD;
    uint64_t edx = 0xBEEF;
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_RDX, &edx));
    *(int *)user_data = 1;
    return 1; /* skip underlying rdmsr */
}

static void test_x86_hook_insn_rdmsr(void)
{
    /* RDMSR (0f 32) */
    char code[] = "\x0F\x32";
    int fired = 0;

    uc_engine *uc;
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof code - 1);

    uint64_t rcx = 0x174; /* MSR_IA32_SYSENTER_CS */
    OK(uc_reg_write(uc, UC_X86_REG_RCX, &rcx));

    uc_hook hook;
    OK(uc_hook_add(uc, &hook, UC_HOOK_INSN, test_x86_hook_insn_rdmsr_cb,
                   &fired, 1, 0, UC_X86_INS_RDMSR));

    OK(uc_emu_start(uc, code_start, code_start + sizeof code - 1, 0, 0));

    OK(uc_hook_del(uc, hook));
    TEST_CHECK(fired == 1);

    uint64_t eax = 0;
    uint64_t edx = 0;
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_RDX, &edx));
    TEST_CHECK(eax == 0xDEAD);
    TEST_CHECK(edx == 0xBEEF);

    OK(uc_close(uc));
}

static int test_x86_count_filtered_insn_hook(uc_engine *uc, void *user_data)
{
    uint32_t *count = (uint32_t *)user_data;

    (*count)++;
    return 1;
}

static void test_x86_filtered_insn_hooks(void)
{
    static const struct {
        uint8_t code[3];
        size_t size;
        int insn;
    } cases[] = {
        {{0x0f, 0xa2, 0x00}, 2, UC_X86_INS_CPUID},
        {{0x0f, 0x31, 0x00}, 2, UC_X86_INS_RDTSC},
        {{0x0f, 0x01, 0xf9}, 3, UC_X86_INS_RDTSCP},
        {{0x0f, 0x30, 0x00}, 2, UC_X86_INS_WRMSR},
        {{0x0f, 0x32, 0x00}, 2, UC_X86_INS_RDMSR},
    };
    static const int insns[] = {
        UC_X86_INS_CPUID,
        UC_X86_INS_RDTSC,
        UC_X86_INS_RDTSCP,
        UC_X86_INS_WRMSR,
        UC_X86_INS_RDMSR,
    };
    uint32_t counts[sizeof(insns) / sizeof(insns[0])] = { 0 };
    uc_hook hooks[sizeof(insns) / sizeof(insns[0])];
    uc_engine *uc;
    size_t i;
    size_t j;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_set_cpu_model(uc, UC_CPU_X86_HASWELL));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        OK(uc_mem_write(uc, code_start + i * 0x10,
                        cases[i].code, cases[i].size));
    }
    for (i = 0; i < sizeof(insns) / sizeof(insns[0]); i++) {
        OK(uc_hook_add(uc, &hooks[i], UC_HOOK_INSN,
                       test_x86_count_filtered_insn_hook, &counts[i],
                       1, 0, insns[i]));
    }

    for (i = 0; i < sizeof(cases) / sizeof(cases[0]); i++) {
        uint64_t address = code_start + i * 0x10;

        OK(uc_emu_start(uc, address, address + cases[i].size, 0, 0));
        for (j = 0; j < sizeof(counts) / sizeof(counts[0]); j++) {
            TEST_CHECK_(counts[j] == (j <= i ? 1u : 0u),
                        "case=%zu hook=%zu count=%u", i, j, counts[j]);
        }
    }

    OK(uc_close(uc));
}

static void test_x86_count_filtered_system_hook(uc_engine *uc,
                                                void *user_data)
{
    uint32_t *count = (uint32_t *)user_data;

    (*count)++;
}

static void test_x86_filtered_system_hooks_one(const uint8_t code[2],
                                               int expected_insn,
                                               int unrelated_insn)
{
    uint32_t expected_count = 0;
    uint32_t unrelated_count = 0;
    uc_engine *uc;
    uc_hook expected_hook;
    uc_hook unrelated_hook;

    uc_common_setup(&uc, UC_ARCH_X86,
                    expected_insn == UC_X86_INS_SYSCALL ?
                        UC_MODE_64 : UC_MODE_32,
                    (const char *)code, 2);
    OK(uc_hook_add(uc, &expected_hook, UC_HOOK_INSN,
                   test_x86_count_filtered_system_hook, &expected_count,
                   1, 0, expected_insn));
    OK(uc_hook_add(uc, &unrelated_hook, UC_HOOK_INSN,
                   test_x86_count_filtered_system_hook, &unrelated_count,
                   1, 0, unrelated_insn));

    OK(uc_emu_start(uc, code_start, code_start + 2, 0, 0));
    TEST_CHECK(expected_count == 1);
    TEST_CHECK(unrelated_count == 0);

    OK(uc_close(uc));
}

static void test_x86_filtered_system_hooks(void)
{
    const uint8_t syscall_code[] = {0x0f, 0x05};
    const uint8_t sysenter_code[] = {0x0f, 0x34};

    test_x86_filtered_system_hooks_one(syscall_code, UC_X86_INS_SYSCALL,
                                       UC_X86_INS_SYSENTER);
    test_x86_filtered_system_hooks_one(sysenter_code, UC_X86_INS_SYSENTER,
                                       UC_X86_INS_SYSCALL);
}

static void test_x86_dr7(void)
{
    uc_engine *uc;
    char code[] =
        "\x48\xC7\xC0\x05\x00\x01\x00\x0F\x23\xF8"; // mov rax, 0x10005
                                                    // mov dr7, rax
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

static void test_x86_hook_block_cb(uc_engine *uc, uint64_t address,
                                   uint32_t size, void *user_data)
{
    uint32_t pc;

    OK(uc_reg_read(uc, UC_X86_REG_EIP, (void *)&pc));

    TEST_CHECK(pc == address);
    *((uint64_t *)user_data) += 1;
}

static void test_x86_hook_block(void)
{
    uc_engine *uc;
    char code[] = "\xeb\x02\x90\x90\x90\x90\x90\x90"; // jmp 4; nop; nop; nop;
                                                      // nop; nop; nop
    uint64_t cnt = 0;
    uc_hook hk;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    OK(uc_hook_add(uc, &hk, UC_HOOK_BLOCK, test_x86_hook_block_cb, (void *)&cnt,
                   1, 0));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    TEST_CHECK(cnt == 2);
    OK(uc_close(uc));
}

static void test_x86_count_memory_hook(uc_engine *uc, uc_mem_type type,
                                       uint64_t address, int size,
                                       int64_t value, void *user_data)
{
    uint32_t *count = (uint32_t *)user_data;

    (*count)++;
}

typedef struct {
    uc_hook hook;
    uint32_t count;
} X86SelfDeletingHookData;

static void test_x86_self_deleting_memory_hook(uc_engine *uc,
                                               uc_mem_type type,
                                               uint64_t address, int size,
                                               int64_t value, void *user_data)
{
    X86SelfDeletingHookData *data =
        (X86SelfDeletingHookData *)user_data;

    data->count++;
    OK(uc_hook_del(uc, data->hook));
}

static void test_x86_memory_hook_add_delete_after_translation(void)
{
    const uint64_t data_address = 0x200000;
    const char code[] = "\x8b\x03"; /* mov eax, [ebx] */
    const uint32_t value = 0x44332211;
    uint32_t hook_count = 0;
    uint32_t ebx = (uint32_t)data_address;
    uint32_t eax;
    uint32_t virtual_value;
    X86SelfDeletingHookData self_deleting = { 0 };
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &value, sizeof(value)));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == value);

    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_count_memory_hook, &hook_count, data_address,
                   data_address + sizeof(value) - 1));
    OK(uc_vmem_read(uc, data_address, UC_PROT_READ, &virtual_value,
                    sizeof(virtual_value)));
    TEST_CHECK(virtual_value == value);
    TEST_CHECK(hook_count == 0);
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    TEST_CHECK(hook_count == 1);

    OK(uc_hook_del(uc, hook));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    TEST_CHECK(hook_count == 1);

    OK(uc_hook_add(uc, &self_deleting.hook, UC_HOOK_MEM_READ,
                   test_x86_self_deleting_memory_hook, &self_deleting,
                   data_address, data_address + sizeof(value) - 1));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    TEST_CHECK(self_deleting.count == 1);

    OK(uc_close(uc));
}

typedef struct X86MemoryHookMutationData {
    int hook_type;
    uint64_t address;
    uc_hook added_hook;
    uc_hook deleted_hook;
    uint32_t log[8];
    uint32_t log_count;
    bool mutated;
} X86MemoryHookMutationData;

static void test_x86_log_added_memory_hook(uc_engine *uc, uc_mem_type type,
                                           uint64_t address, int size,
                                           int64_t value, void *user_data)
{
    X86MemoryHookMutationData *data =
        (X86MemoryHookMutationData *)user_data;

    data->log[data->log_count++] = 3;
}

static void test_x86_unreachable_memory_hook(uc_engine *uc,
                                             uc_mem_type type,
                                             uint64_t address, int size,
                                             int64_t value, void *user_data)
{
    X86MemoryHookMutationData *data =
        (X86MemoryHookMutationData *)user_data;

    data->log[data->log_count++] = 9;
}

static void test_x86_mutate_memory_hooks(uc_engine *uc, uc_mem_type type,
                                         uint64_t address, int size,
                                         int64_t value, void *user_data)
{
    X86MemoryHookMutationData *data =
        (X86MemoryHookMutationData *)user_data;

    data->log[data->log_count++] = 1;
    if (!data->mutated) {
        data->mutated = true;
        OK(uc_hook_del(uc, data->deleted_hook));
        OK(uc_hook_add(uc, &data->added_hook, data->hook_type,
                       test_x86_log_added_memory_hook, data,
                       data->address, data->address + 3));
    }
}

static void test_x86_memory_hook_mutation_same_dispatch_one(int hook_type)
{
    const uint64_t data_address = 0x200000;
    const uint8_t read_code[] = {
        0xa1, 0x00, 0x00, 0x20, 0x00,
        0xa1, 0x00, 0x00, 0x20, 0x00,
    };
    const uint8_t write_code[] = {
        0xa3, 0x00, 0x00, 0x20, 0x00,
        0xa3, 0x00, 0x00, 0x20, 0x00,
    };
    const uint8_t *code = hook_type == UC_HOOK_MEM_WRITE ?
                              write_code : read_code;
    uint32_t value = 0x44332211;
    X86MemoryHookMutationData data = {
        .hook_type = hook_type,
        .address = data_address,
    };
    uc_engine *uc;
    uc_hook first_hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(read_code));
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &value, sizeof(value)));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &value));
    OK(uc_hook_add(uc, &first_hook, hook_type,
                   test_x86_mutate_memory_hooks, &data,
                   data_address, data_address + 3));
    OK(uc_hook_add(uc, &data.deleted_hook, hook_type,
                   test_x86_unreachable_memory_hook, &data,
                   data_address, data_address + 3));

    OK(uc_emu_start(uc, code_start,
                    code_start + sizeof(read_code), 0, 0));
    TEST_CHECK(data.log_count == 4);
    TEST_CHECK(data.log[0] == 1 && data.log[1] == 3 &&
               data.log[2] == 1 && data.log[3] == 3);

    OK(uc_close(uc));
}

static void test_x86_memory_hook_mutation_same_dispatch(void)
{
    test_x86_memory_hook_mutation_same_dispatch_one(UC_HOOK_MEM_READ);
    test_x86_memory_hook_mutation_same_dispatch_one(UC_HOOK_MEM_READ_AFTER);
    test_x86_memory_hook_mutation_same_dispatch_one(UC_HOOK_MEM_WRITE);
}

typedef struct X86MemoryHookUserData {
    uc_hook hook;
    struct X86MemoryHookUserData *replacement;
    uint32_t count;
} X86MemoryHookUserData;

static void test_x86_memory_hook_user_data_callback(
    uc_engine *uc, uc_mem_type type, uint64_t address, int size,
    int64_t value, void *user_data)
{
    X86MemoryHookUserData *data =
        (X86MemoryHookUserData *)user_data;

    data->count++;
    if (data->replacement != NULL) {
        OK(uc_hook_set_user_data(uc, data->hook, data->replacement));
        data->replacement = NULL;
    }
}

static void test_x86_memory_hook_user_data(void)
{
    const uint64_t data_address = 0x200000;
    const uint8_t code[] = {
        0xa1, 0x00, 0x00, 0x20, 0x00,
        0xa1, 0x00, 0x00, 0x20, 0x00,
    };
    const uint32_t value = 0x11223344;
    X86MemoryHookUserData replacement = { 0 };
    X86MemoryHookUserData initial = {
        .replacement = &replacement,
    };
    uc_engine *uc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)code, sizeof(code));
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &value, sizeof(value)));
    OK(uc_hook_add(uc, &initial.hook, UC_HOOK_MEM_READ,
                   test_x86_memory_hook_user_data_callback, &initial,
                   data_address, data_address + sizeof(value) - 1));
    replacement.hook = initial.hook;

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    TEST_CHECK(initial.count == 1);
    TEST_CHECK(replacement.count == 1);

    OK(uc_close(uc));
}

typedef struct X86MemoryHookFallbackData {
    uint32_t count;
} X86MemoryHookFallbackData;

static void test_x86_memory_hook_fallback_callback(
    uc_engine *uc, uc_mem_type type, uint64_t address, int size,
    int64_t value, void *user_data)
{
    X86MemoryHookFallbackData *data =
        (X86MemoryHookFallbackData *)user_data;
    uint32_t eip;

    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    TEST_CHECK(type == UC_MEM_READ);
    TEST_CHECK(size == 4);
    TEST_CHECK(eip == code_start + data->count * 5);
    data->count++;
}

static void test_x86_memory_hook_fallback_accesses(void)
{
    const uint64_t data_address = 0x200000;
    const uint8_t code[] = {
        0xa1, 0x01, 0x00, 0x20, 0x00,
        0xa1, 0xfe, 0x0f, 0x20, 0x00,
    };
    const uint32_t unaligned_value = 0x11223344;
    const uint32_t cross_page_value = 0xaabbccdd;
    X86MemoryHookFallbackData data = { 0 };
    uint32_t eax;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)code, sizeof(code));
    OK(uc_mem_map(uc, data_address, 0x2000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address + 1, &unaligned_value,
                    sizeof(unaligned_value)));
    OK(uc_mem_write(uc, data_address + 0xffe, &cross_page_value,
                    sizeof(cross_page_value)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_memory_hook_fallback_callback, &data, 1, 0));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(data.count == 2);
    TEST_CHECK(eax == cross_page_value);

    OK(uc_close(uc));
}

typedef struct {
    uint32_t expected_pc;
    uint32_t count;
} X86RestoreCacheHookData;

static void test_x86_restore_cache_memory_hook(uc_engine *uc,
                                               uc_mem_type type,
                                               uint64_t address, int size,
                                               int64_t value, void *user_data)
{
    X86RestoreCacheHookData *data =
        (X86RestoreCacheHookData *)user_data;
    uint32_t pc;

    OK(uc_reg_read(uc, UC_X86_REG_EIP, &pc));
    TEST_CHECK(pc == data->expected_pc);
    data->count++;
}

static void test_x86_memory_hook_restore_cache_tb_flush(void)
{
    const uint64_t second_code_address = code_start + 0x1000;
    const uint64_t data_address = 0x200000;
    const uint8_t code[] = { 0x8b, 0x03 }; /* mov eax, [ebx] */
    const uint32_t value = 0x44332211;
    X86RestoreCacheHookData data = {
        .expected_pc = (uint32_t)code_start,
    };
    uint32_t ebx = (uint32_t)data_address;
    uint32_t eax;
    size_t i;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_mem_write(uc, second_code_address, code, sizeof(code)));
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &value, sizeof(value)));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_restore_cache_memory_hook, &data,
                   data_address, data_address + sizeof(value) - 1));

    for (i = 0; i < 8; i++) {
        OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    }

    OK(uc_ctl_flush_tb(uc));
    data.expected_pc = (uint32_t)second_code_address;
    for (i = 0; i < 8; i++) {
        OK(uc_emu_start(uc, second_code_address,
                        second_code_address + sizeof(code), 0, 0));
    }

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == value);
    TEST_CHECK(data.count == 16);

    OK(uc_close(uc));
}

typedef struct X86CodeHookDeleteData {
    uc_hook hook;
    uint32_t count;
} X86CodeHookDeleteData;

static void test_x86_count_code_hook(uc_engine *uc, uint64_t address,
                                     uint32_t size, void *user_data)
{
    X86CodeHookDeleteData *data =
        (X86CodeHookDeleteData *)user_data;

    data->count++;
}

static void test_x86_delete_code_hook(uc_engine *uc, uint64_t address,
                                      uint32_t size, void *user_data)
{
    X86CodeHookDeleteData *data =
        (X86CodeHookDeleteData *)user_data;

    data->count++;
    if (data->count == 1) {
        OK(uc_hook_del(uc, data->hook));
    }
}

static void test_x86_bounded_mid_tb_code_hook_delete(void)
{
    const uint64_t sink_address = code_start + 0x1000;
    const uint8_t code[] = {
        0x40,                         /* inc eax */
        0x40,                         /* inc eax */
        0xe9, 0xf9, 0x0f, 0x00, 0x00, /* jmp sink */
    };
    const uint8_t sink_code[] = { 0x90 }; /* nop */
    X86CodeHookDeleteData data = { 0 };
    uint32_t eax = 0;
    uc_engine *uc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_mem_write(uc, sink_address, sink_code, sizeof(sink_code)));
    OK(uc_hook_add(uc, &data.hook, UC_HOOK_CODE,
                   test_x86_count_code_hook, &data,
                   code_start + 1, code_start + 1));

    OK(uc_emu_start(uc, code_start,
                    sink_address + sizeof(sink_code), 0, 0));
    TEST_CHECK(data.count == 1);
    OK(uc_hook_del(uc, data.hook));

    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_emu_start(uc, code_start,
                    sink_address + sizeof(sink_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 2);
    TEST_CHECK(data.count == 1);

    OK(uc_close(uc));
}

static void test_x86_self_deleting_single_code_hook(void)
{
    const uint8_t code[] = { 0x40, 0x40, 0x40, 0x40 };
    X86CodeHookDeleteData data = { 0 };
    uint32_t eax;
    uc_engine *uc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_hook_add(uc, &data.hook, UC_HOOK_CODE,
                   test_x86_delete_code_hook, &data, 1, 0));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 4);
    TEST_CHECK(data.count == 1);

    OK(uc_close(uc));
}

typedef struct X86CodeHookUserData {
    uc_hook hook;
    struct X86CodeHookUserData *replacement;
    uc_err callback_update_error;
    uint32_t count;
} X86CodeHookUserData;

static void test_x86_code_hook_user_data_callback(uc_engine *uc,
                                                  uint64_t address,
                                                  uint32_t size,
                                                  void *user_data)
{
    X86CodeHookUserData *data = (X86CodeHookUserData *)user_data;

    data->count++;
    if (data->replacement != NULL) {
        data->callback_update_error =
            uc_hook_set_user_data(uc, data->hook, data->replacement);
    }
}

static void test_x86_bounded_code_hook_user_data(void)
{
    const uint8_t code[] = { 0x40, 0x40 };
    X86CodeHookUserData replacement = { 0 };
    X86CodeHookUserData initial = {
        .replacement = &replacement,
        .callback_update_error = UC_ERR_OK,
    };
    uint32_t eax = 0;
    uc_engine *uc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)code, sizeof(code));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_hook_add(uc, &initial.hook, UC_HOOK_CODE,
                   test_x86_code_hook_user_data_callback, &initial,
                   code_start + 1, code_start + 1));
    replacement.hook = initial.hook;

    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    TEST_CHECK(initial.count == 1);
    TEST_CHECK(initial.callback_update_error == UC_ERR_ARG);
    TEST_CHECK(replacement.count == 0);

    OK(uc_hook_set_user_data(uc, initial.hook, &replacement));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    TEST_CHECK(initial.count == 1);
    TEST_CHECK(replacement.count == 1);

    OK(uc_hook_del(uc, initial.hook));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    TEST_CHECK(replacement.count == 1);

    OK(uc_close(uc));
}

typedef struct X86SelfDeletingBlockData {
    uc_hook hook;
    uint32_t count;
} X86SelfDeletingBlockData;

static void test_x86_self_deleting_block_callback(uc_engine *uc,
                                                  uint64_t address,
                                                  uint32_t size,
                                                  void *user_data)
{
    X86SelfDeletingBlockData *data =
        (X86SelfDeletingBlockData *)user_data;

    data->count++;
    OK(uc_hook_del(uc, data->hook));
}

static void test_x86_self_deleting_single_block_hook(void)
{
    const uint64_t target = code_start + 0x1000;
    const uint8_t first_code[] = {
        0x40,
        0xe9, 0xfa, 0x0f, 0x00, 0x00,
    };
    const uint8_t target_code[] = { 0x40 };
    X86SelfDeletingBlockData data = { 0 };
    uint32_t eax;
    uc_engine *uc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)first_code, sizeof(first_code));
    OK(uc_mem_write(uc, target, target_code, sizeof(target_code)));
    OK(uc_hook_add(uc, &data.hook, UC_HOOK_BLOCK,
                   test_x86_self_deleting_block_callback, &data, 1, 0));

    OK(uc_emu_start(uc, code_start,
                    target + sizeof(target_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 2);
    TEST_CHECK(data.count == 1);

    OK(uc_close(uc));
}

typedef struct X86CodeHookMutationData {
    uc_hook added_hook;
    uc_hook deleted_hook;
    uint32_t log[8];
    uint32_t log_count;
    bool mutated;
} X86CodeHookMutationData;

static void test_x86_log_added_code_hook(uc_engine *uc, uint64_t address,
                                         uint32_t size, void *user_data)
{
    X86CodeHookMutationData *data =
        (X86CodeHookMutationData *)user_data;

    data->log[data->log_count++] = 3;
}

static void test_x86_add_code_hook_from_callback(uc_engine *uc,
                                                 uint64_t address,
                                                 uint32_t size,
                                                 void *user_data)
{
    X86CodeHookMutationData *data =
        (X86CodeHookMutationData *)user_data;

    data->log[data->log_count++] = 2;
    if (!data->mutated) {
        data->mutated = true;
        OK(uc_hook_add(uc, &data->added_hook, UC_HOOK_CODE,
                       test_x86_log_added_code_hook, data,
                       address, address));
    }
}

static void test_x86_delete_later_code_hook(uc_engine *uc,
                                            uint64_t address,
                                            uint32_t size,
                                            void *user_data)
{
    X86CodeHookMutationData *data =
        (X86CodeHookMutationData *)user_data;

    data->log[data->log_count++] = 1;
    if (!data->mutated) {
        data->mutated = true;
        OK(uc_hook_del(uc, data->deleted_hook));
    }
}

static void test_x86_unreachable_code_hook(uc_engine *uc, uint64_t address,
                                           uint32_t size, void *user_data)
{
    X86CodeHookMutationData *data =
        (X86CodeHookMutationData *)user_data;

    data->log[data->log_count++] = 9;
}

static void test_x86_code_hook_append_same_dispatch(void)
{
    const uint8_t code[] = { 0x40 }; /* inc eax */
    X86CodeHookMutationData data = { 0 };
    uc_engine *uc;
    uc_hook nonmatching_hook;
    uc_hook matching_hook;
    uint32_t eax;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_hook_add(uc, &nonmatching_hook, UC_HOOK_CODE,
                   test_x86_unreachable_code_hook, &data,
                   code_start + 0x100, code_start + 0x100));
    OK(uc_hook_add(uc, &matching_hook, UC_HOOK_CODE,
                   test_x86_add_code_hook_from_callback, &data,
                   code_start, code_start));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 1);
    TEST_CHECK(data.log_count == 4);
    TEST_CHECK(data.log[0] == 2 && data.log[1] == 3 &&
               data.log[2] == 2 && data.log[3] == 3);

    OK(uc_close(uc));
}

static void test_x86_code_hook_delete_later_same_dispatch(void)
{
    const uint8_t code[] = { 0x40 }; /* inc eax */
    X86CodeHookMutationData data = { 0 };
    uc_engine *uc;
    uc_hook first_hook;
    uint32_t eax;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_hook_add(uc, &first_hook, UC_HOOK_CODE,
                   test_x86_delete_later_code_hook, &data,
                   code_start, code_start));
    OK(uc_hook_add(uc, &data.deleted_hook, UC_HOOK_CODE,
                   test_x86_unreachable_code_hook, &data,
                   code_start, code_start));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 1);
    TEST_CHECK(data.log_count == 1 && data.log[0] == 1);

    OK(uc_close(uc));
}

typedef struct X86EdgeFlushData {
    uint32_t count;
    uc_tb first_current;
    uc_tb first_previous;
} X86EdgeFlushData;

static void test_x86_edge_flush_callback(uc_engine *uc,
                                         uc_tb *current,
                                         uc_tb *previous,
                                         void *user_data)
{
    X86EdgeFlushData *data = (X86EdgeFlushData *)user_data;

    if (data->count == 0) {
        data->first_current = *current;
        data->first_previous = *previous;
        OK(uc_ctl_flush_tb(uc));
    }
    data->count++;
}

static void test_x86_edge_hook_tb_flush_lifetime(void)
{
    const uint64_t second_address = code_start + 0x1000;
    const uint64_t sink_address = code_start + 0x2000;
    const uint8_t first_code[] = {
        0xe9, 0xfb, 0x0f, 0x00, 0x00, /* jmp second */
    };
    const uint8_t second_code[] = {
        0x40,                         /* inc eax */
        0xe9, 0xfa, 0x0f, 0x00, 0x00, /* jmp sink */
    };
    const uint8_t sink_code[] = { 0x90 };
    X86EdgeFlushData data = { 0 };
    uc_engine *uc;
    uc_hook hook;
    uint32_t eax;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)first_code, sizeof(first_code));
    OK(uc_mem_write(uc, second_address, second_code, sizeof(second_code)));
    OK(uc_mem_write(uc, sink_address, sink_code, sizeof(sink_code)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_EDGE_GENERATED,
                   test_x86_edge_flush_callback, &data, 1, 0));

    OK(uc_emu_start(uc, code_start, second_address, 0, 0));
    OK(uc_ctl_remove_cache(uc, second_address, second_address + 1));
    OK(uc_emu_start(uc, second_address,
                    sink_address + sizeof(sink_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 1);
    TEST_CHECK_(data.count == 3, "count=%u", data.count);
    TEST_CHECK_(data.first_previous.pc == code_start,
                "previous=0x%" PRIx64, data.first_previous.pc);
    TEST_CHECK_(data.first_current.pc == second_address,
                "current=0x%" PRIx64, data.first_current.pc);

    OK(uc_close(uc));
}

typedef struct X86NestedEdgeData {
    uint64_t inner_start;
    uint64_t inner_end;
    uint64_t previous[8];
    uint64_t current[8];
    uint32_t count;
    bool nested_started;
} X86NestedEdgeData;

static void test_x86_nested_edge_callback(uc_engine *uc,
                                          uc_tb *current,
                                          uc_tb *previous,
                                          void *user_data)
{
    X86NestedEdgeData *data = (X86NestedEdgeData *)user_data;

    TEST_CHECK(data->count < 8);
    data->previous[data->count] = previous->pc;
    data->current[data->count] = current->pc;
    data->count++;
    if (!data->nested_started) {
        uint32_t resume_pc = (uint32_t)current->pc;

        data->nested_started = true;
        OK(uc_emu_start(uc, data->inner_start, data->inner_end, 0, 0));
        OK(uc_reg_write(uc, UC_X86_REG_EIP, &resume_pc));
    }
}

static void test_x86_nested_edge_history(void)
{
    const uint64_t outer_target = code_start + 0x1000;
    const uint64_t inner_start = code_start + 0x2000;
    const uint64_t inner_end = code_start + 0x3000;
    const uint8_t jump[] = {
        0xe9, 0xfb, 0x0f, 0x00, 0x00,
    };
    const uint8_t nop[] = { 0x90 };
    X86NestedEdgeData data = {
        .inner_start = inner_start,
        .inner_end = inner_end,
    };
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)jump, sizeof(jump));
    OK(uc_mem_write(uc, outer_target, nop, sizeof(nop)));
    OK(uc_mem_write(uc, inner_start, jump, sizeof(jump)));
    OK(uc_mem_write(uc, inner_end, nop, sizeof(nop)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_EDGE_GENERATED,
                   test_x86_nested_edge_callback, &data, 1, 0));

    OK(uc_emu_start(uc, code_start, outer_target, 0, 0));
    TEST_CHECK(data.count >= 2);
    TEST_CHECK(data.previous[0] == code_start);
    TEST_CHECK(data.current[0] == outer_target);
    TEST_CHECK(data.previous[1] == inner_start);
    TEST_CHECK(data.current[1] == inner_end);

    OK(uc_close(uc));
}

typedef struct X86EdgeHistoryResetData {
    uint64_t previous[4];
    uint64_t current[4];
    uint32_t count;
} X86EdgeHistoryResetData;

static void test_x86_edge_history_reset_callback(uc_engine *uc,
                                                 uc_tb *current,
                                                 uc_tb *previous,
                                                 void *user_data)
{
    X86EdgeHistoryResetData *data =
        (X86EdgeHistoryResetData *)user_data;

    TEST_CHECK(data->count < 4);
    data->previous[data->count] = previous->pc;
    data->current[data->count] = current->pc;
    data->count++;
}

static void test_x86_edge_hook_add_resets_history(void)
{
    const uint64_t middle = code_start + 0x1000;
    const uint64_t sink = code_start + 0x2000;
    const uint8_t jump[] = {
        0xe9, 0xfb, 0x0f, 0x00, 0x00,
    };
    const uint8_t sink_code[] = { 0x90 };
    X86EdgeHistoryResetData data = { 0 };
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)jump, sizeof(jump));
    OK(uc_mem_write(uc, middle, jump, sizeof(jump)));
    OK(uc_mem_write(uc, sink, sink_code, sizeof(sink_code)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_EDGE_GENERATED,
                   test_x86_edge_history_reset_callback, &data, 1, 0));
    OK(uc_emu_start(uc, code_start, middle, 0, 0));
    TEST_CHECK(data.count == 1);
    OK(uc_hook_del(uc, hook));
    memset(&data, 0, sizeof(data));

    OK(uc_hook_add(uc, &hook, UC_HOOK_EDGE_GENERATED,
                   test_x86_edge_history_reset_callback, &data, 1, 0));
    OK(uc_emu_start(uc, middle,
                    sink + sizeof(sink_code), 0, 0));
    TEST_CHECK_(data.count == 2, "count=%u", data.count);
    TEST_CHECK_(data.previous[0] == middle, "previous=0x%" PRIx64,
                data.previous[0]);
    TEST_CHECK_(data.current[0] == sink, "current=0x%" PRIx64,
                data.current[0]);
    TEST_CHECK(data.previous[1] == sink);
    TEST_CHECK(data.current[1] == sink + sizeof(sink_code));

    OK(uc_close(uc));
}

typedef struct {
    uint32_t read_after_count;
    uint32_t write_count;
} X86ReadAfterWriteHookData;

static void test_x86_count_read_after_write_hook(uc_engine *uc,
                                                 uc_mem_type type,
                                                 uint64_t address, int size,
                                                 int64_t value,
                                                 void *user_data)
{
    X86ReadAfterWriteHookData *data =
        (X86ReadAfterWriteHookData *)user_data;

    if (type == UC_MEM_READ_AFTER) {
        data->read_after_count++;
    } else {
        TEST_CHECK(type == UC_MEM_WRITE);
        data->write_count++;
    }
}

static void test_x86_memory_write_read_after_hook_add_delete(void)
{
    const uint64_t data_address = 0x200000;
    const uint8_t code[] = {
        0xa1, 0x00, 0x00, 0x20, 0x00, /* mov eax, [0x200000] */
        0xa3, 0x04, 0x00, 0x20, 0x00, /* mov [0x200004], eax */
    };
    const uint32_t value = 0x44332211;
    X86ReadAfterWriteHookData data = { 0 };
    uint32_t stored = 0;
    uc_engine *uc;
    uc_hook read_after_hook;
    uc_hook write_hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &value, sizeof(value)));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_hook_add(uc, &read_after_hook, UC_HOOK_MEM_READ_AFTER,
                   test_x86_count_read_after_write_hook, &data,
                   data_address, data_address + sizeof(value) - 1));
    OK(uc_hook_add(uc, &write_hook, UC_HOOK_MEM_WRITE,
                   test_x86_count_read_after_write_hook, &data,
                   data_address + sizeof(value),
                   data_address + 2 * sizeof(value) - 1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    TEST_CHECK(data.read_after_count == 1);
    TEST_CHECK(data.write_count == 1);

    OK(uc_hook_del(uc, read_after_hook));
    OK(uc_hook_del(uc, write_hook));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    TEST_CHECK(data.read_after_count == 1);
    TEST_CHECK(data.write_count == 1);
    OK(uc_mem_read(uc, data_address + sizeof(value), &stored,
                   sizeof(stored)));
    TEST_CHECK(stored == value);

    OK(uc_close(uc));
}

typedef struct {
    uint64_t address;
    uint32_t value;
    uint32_t count;
} X86RemapHookData;

static void test_x86_remap_memory_hook(uc_engine *uc, uc_mem_type type,
                                       uint64_t address, int size,
                                       int64_t value, void *user_data)
{
    X86RemapHookData *data = (X86RemapHookData *)user_data;

    data->count++;
    OK(uc_mem_unmap(uc, data->address, 0x1000));
    OK(uc_mem_map(uc, data->address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data->address, &data->value, sizeof(data->value)));
}

static void test_x86_memory_hook_remap_revalidation(void)
{
    const uint64_t data_address = 0x200000;
    const char code[] = "\x8b\x03"; /* mov eax, [ebx] */
    const uint32_t initial_value = 0x11223344;
    X86RemapHookData data = {
        .address = data_address,
        .value = 0xaabbccdd,
    };
    uint32_t ebx = (uint32_t)data_address;
    uint32_t eax;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &initial_value, sizeof(initial_value)));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_remap_memory_hook, &data, data_address,
                   data_address + sizeof(initial_value) - 1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(data.count == 1);
    TEST_CHECK(eax == data.value);

    OK(uc_close(uc));
}

typedef struct {
    uint64_t nested_address;
    uint32_t count;
    bool active;
} X86NestedMemoryHookData;

static void test_x86_nested_memory_hook(uc_engine *uc, uc_mem_type type,
                                        uint64_t address, int size,
                                        int64_t value, void *user_data)
{
    X86NestedMemoryHookData *data = (X86NestedMemoryHookData *)user_data;

    data->count++;
    if (!data->active) {
        data->active = true;
        OK(uc_emu_start(uc, data->nested_address,
                        data->nested_address + 2, 0, 1));
        data->active = false;
    }
}

static void test_x86_memory_hook_nested_tlb_revalidation(void)
{
    const uint64_t outer_data_address = 0x200000;
    const uint64_t nested_data_address = 0x300000;
    const uint64_t nested_code_address = code_start + 0x1000;
    const char outer_code[] = "\x8b\x03"; /* mov eax, [ebx] */
    const char nested_code[] = "\x8b\x0a"; /* mov ecx, [edx] */
    const uint32_t outer_value = 0x12345678;
    const uint32_t nested_value = 0x87654321;
    X86NestedMemoryHookData data = {
        .nested_address = nested_code_address,
    };
    uint32_t ebx = (uint32_t)outer_data_address;
    uint32_t edx = (uint32_t)nested_data_address;
    uint32_t eax;
    uint32_t ecx;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, outer_code,
                    sizeof(outer_code) - 1);
    OK(uc_mem_write(uc, nested_code_address, nested_code,
                    sizeof(nested_code) - 1));
    OK(uc_mem_map(uc, outer_data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_map(uc, nested_data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, outer_data_address, &outer_value,
                    sizeof(outer_value)));
    OK(uc_mem_write(uc, nested_data_address, &nested_value,
                    sizeof(nested_value)));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &edx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_nested_memory_hook, &data, outer_data_address,
                   outer_data_address + sizeof(outer_value) - 1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(outer_code) - 1, 0,
                    1));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK(data.count == 1);
    TEST_CHECK(eax == outer_value);
    TEST_CHECK(ecx == nested_value);

    OK(uc_close(uc));
}

typedef struct X86NestedFlushHookData {
    uint64_t nested_address;
    uint32_t count;
    bool nested_started;
} X86NestedFlushHookData;

static void test_x86_memory_hook_nested_flush_callback(
    uc_engine *uc, uc_mem_type type, uint64_t address, int size,
    int64_t value, void *user_data)
{
    X86NestedFlushHookData *data =
        (X86NestedFlushHookData *)user_data;

    data->count++;
    if (!data->nested_started) {
        data->nested_started = true;
        OK(uc_ctl_flush_tb(uc));
        OK(uc_emu_start(uc, data->nested_address,
                        data->nested_address + 5, 0, 0));
    }
}

static void test_x86_memory_hook_nested_tb_flush(void)
{
    const uint64_t data_address = 0x200000;
    const uint64_t nested_address = code_start + 0x1000;
    const uint8_t outer_code[] = {
        0x8b, 0x03, /* mov eax, [ebx] */
        0x40,       /* inc eax */
    };
    const uint8_t nested_code[] = {
        0xb9, 0x78, 0x56, 0x34, 0x12, /* mov ecx, 0x12345678 */
    };
    const uint32_t memory_value = 0x11223344;
    X86NestedFlushHookData data = {
        .nested_address = nested_address,
    };
    uint32_t ebx = (uint32_t)data_address;
    uint32_t eax;
    uint32_t ecx;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)outer_code, sizeof(outer_code));
    OK(uc_mem_write(uc, nested_address, nested_code, sizeof(nested_code)));
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &memory_value,
                    sizeof(memory_value)));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_memory_hook_nested_flush_callback, &data,
                   data_address,
                   data_address + sizeof(memory_value) - 1));

    OK(uc_emu_start(uc, code_start,
                    code_start + sizeof(outer_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK_(data.count == 1, "count=%u", data.count);
    TEST_CHECK_(eax == memory_value + 1, "eax=0x%x", eax);
    TEST_CHECK(ecx == 0x12345678);

    OK(uc_close(uc));
}

typedef struct X86NestedOuterPatchData {
    uint64_t nested_address;
    uint64_t patch_address;
    uint32_t outer_count;
    uint32_t nested_count;
    bool nested_started;
    bool patched;
} X86NestedOuterPatchData;

static void test_x86_nested_patch_inner_callback(uc_engine *uc,
                                                 uint64_t address,
                                                 uint32_t size,
                                                 void *user_data)
{
    X86NestedOuterPatchData *data =
        (X86NestedOuterPatchData *)user_data;
    const uint8_t inc_ecx = 0x41;

    data->nested_count++;
    if (!data->patched) {
        data->patched = true;
        OK(uc_mem_write(uc, data->patch_address, &inc_ecx,
                        sizeof(inc_ecx)));
    }
}

static void test_x86_nested_patch_outer_callback(uc_engine *uc,
                                                 uint64_t address,
                                                 uint32_t size,
                                                 void *user_data)
{
    X86NestedOuterPatchData *data =
        (X86NestedOuterPatchData *)user_data;

    data->outer_count++;
    if (!data->nested_started) {
        data->nested_started = true;
        OK(uc_emu_start(uc, data->nested_address,
                        data->nested_address + 1, 0, 0));
    }
}

static void test_x86_nested_patch_following_outer_instruction(void)
{
    const uint64_t nested_address = code_start + 0x2000;
    const uint8_t outer_code[] = {
        0x40, /* inc eax */
        0x40, /* inc eax; patched to inc ecx by nested execution */
    };
    const uint8_t nested_code[] = { 0x90 }; /* nop */
    X86NestedOuterPatchData data = {
        .nested_address = nested_address,
        .patch_address = code_start + 1,
    };
    uint32_t eax = 0;
    uint32_t ecx = 0;
    uc_engine *uc;
    uc_hook inner_hook;
    uc_hook outer_hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)outer_code, sizeof(outer_code));
    OK(uc_mem_write(uc, nested_address, nested_code, sizeof(nested_code)));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_hook_add(uc, &outer_hook, UC_HOOK_CODE,
                   test_x86_nested_patch_outer_callback, &data,
                   code_start, code_start));
    OK(uc_hook_add(uc, &inner_hook, UC_HOOK_CODE,
                   test_x86_nested_patch_inner_callback, &data,
                   nested_address, nested_address));

    OK(uc_emu_start(uc, code_start,
                    code_start + sizeof(outer_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK_(data.outer_count == 2, "outer_count=%u", data.outer_count);
    TEST_CHECK_(data.nested_count == 1, "nested_count=%u", data.nested_count);
    TEST_CHECK(data.patched);
    TEST_CHECK_(eax == 1, "eax=0x%x", eax);
    TEST_CHECK_(ecx == 1, "ecx=0x%x", ecx);

    OK(uc_close(uc));
}

typedef struct X86LinkedSuccessorPatchData {
    uint64_t nested_address;
    uint64_t patch_address;
    uint32_t successor_count;
    uint32_t nested_count;
    bool patched;
} X86LinkedSuccessorPatchData;

static void test_x86_linked_successor_patch_inner_callback(
    uc_engine *uc, uint64_t address, uint32_t size, void *user_data)
{
    X86LinkedSuccessorPatchData *data =
        (X86LinkedSuccessorPatchData *)user_data;
    const uint8_t inc_ecx = 0x41;

    data->nested_count++;
    if (!data->patched) {
        data->patched = true;
        OK(uc_mem_write(uc, data->patch_address, &inc_ecx,
                        sizeof(inc_ecx)));
    }
}

static uint64_t test_x86_linked_successor_patch_mmio_read(
    uc_engine *uc, uint64_t offset, unsigned int size, void *user_data)
{
    X86LinkedSuccessorPatchData *data =
        (X86LinkedSuccessorPatchData *)user_data;

    data->successor_count++;
    if (data->successor_count == 2) {
        OK(uc_emu_start(uc, data->nested_address,
                        data->nested_address + 1, 0, 0));
    }
    return 7;
}

static void test_x86_nested_patch_direct_linked_successor(void)
{
    const uint64_t successor_address = code_start + 0x100;
    const uint64_t nested_address = code_start + 0x2000;
    const uint64_t data_address = 0x6000;
    const uint8_t predecessor_code[] = {
        0xe9, 0xfb, 0x00, 0x00, 0x00, /* jmp successor_address */
    };
    const uint8_t successor_code[] = {
        0xa1, 0x00, 0x60, 0x00, 0x00, /* mov eax, [data_address] */
        0x43,                         /* inc ebx */
    };
    const uint8_t nested_code[] = { 0x90 }; /* nop */
    const uint32_t memory_value = 7;
    X86LinkedSuccessorPatchData data = {
        .nested_address = nested_address,
        .patch_address = successor_address + sizeof(successor_code) - 1,
    };
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uc_engine *uc;
    uc_hook inner_hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)predecessor_code,
                    sizeof(predecessor_code));
    OK(uc_mem_write(uc, successor_address, successor_code,
                    sizeof(successor_code)));
    OK(uc_mem_write(uc, nested_address, nested_code,
                    sizeof(nested_code)));
    OK(uc_mmio_map(uc, data_address, 0x1000,
                   test_x86_linked_successor_patch_mmio_read, &data,
                   NULL, NULL));
    OK(uc_hook_add(uc, &inner_hook, UC_HOOK_CODE,
                   test_x86_linked_successor_patch_inner_callback, &data,
                   nested_address, nested_address));

    /* The first run installs the predecessor-to-successor direct link. */
    OK(uc_emu_start(uc, code_start,
                    successor_address + sizeof(successor_code), 0, 0));
    OK(uc_emu_start(uc, code_start,
                    successor_address + sizeof(successor_code), 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK_(data.successor_count == 3, "successor_count=%u",
                data.successor_count);
    TEST_CHECK_(data.nested_count == 1, "nested_count=%u",
                data.nested_count);
    TEST_CHECK(data.patched);
    TEST_CHECK_(eax == memory_value, "eax=0x%x", eax);
    TEST_CHECK_(ebx == 1, "ebx=0x%x", ebx);
    TEST_CHECK_(ecx == 1, "ecx=0x%x", ecx);

    OK(uc_close(uc));
}

typedef struct X86LinkedBlockPatchData {
    uint64_t patch_address;
    uint32_t block_count;
    bool patched;
} X86LinkedBlockPatchData;

static void test_x86_linked_block_patch_outer_callback(
    uc_engine *uc, uint64_t address, uint32_t size, void *user_data)
{
    X86LinkedBlockPatchData *data =
        (X86LinkedBlockPatchData *)user_data;
    const uint8_t inc_ecx = 0x41;

    data->block_count++;
    if (data->block_count == 2 && !data->patched) {
        data->patched = true;
        OK(uc_mem_write(uc, data->patch_address, &inc_ecx,
                        sizeof(inc_ecx)));
    }
}

static void test_x86_nested_patch_direct_linked_block_hook(void)
{
    const uint64_t successor_address = code_start + 0x100;
    const uint8_t predecessor_code[] = {
        0xe9, 0xfb, 0x00, 0x00, 0x00, /* jmp successor_address */
    };
    const uint8_t successor_code[] = {
        0x43,                         /* inc ebx */
        0x4a,                         /* dec edx */
        0x0f, 0x85, 0xf8, 0xfe, 0xff, 0xff, /* jnz code_start */
    };
    X86LinkedBlockPatchData data = {
        .patch_address = successor_address,
    };
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 2;
    uc_engine *uc;
    uc_hook block_hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)predecessor_code,
                    sizeof(predecessor_code));
    OK(uc_mem_write(uc, successor_address, successor_code,
                    sizeof(successor_code)));
    OK(uc_hook_add(uc, &block_hook, UC_HOOK_BLOCK,
                   test_x86_linked_block_patch_outer_callback, &data,
                   successor_address, successor_address));

    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &edx));
    OK(uc_emu_start(uc, code_start,
                    successor_address + sizeof(successor_code), 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK_(data.block_count == 3, "block_count=%u", data.block_count);
    TEST_CHECK(data.patched);
    TEST_CHECK_(ebx == 1, "ebx=0x%x", ebx);
    TEST_CHECK_(ecx == 1, "ecx=0x%x", ecx);

    OK(uc_close(uc));
}

typedef struct X86VtlbActivePatchData {
    uint64_t patch_address;
    uint32_t callback_count;
    bool patched;
} X86VtlbActivePatchData;

static void test_x86_vtlb_active_patch_callback(uc_engine *uc,
                                                uint64_t address,
                                                uint32_t size,
                                                void *user_data)
{
    X86VtlbActivePatchData *data = (X86VtlbActivePatchData *)user_data;
    const uint8_t inc_ecx = 0x41;

    data->callback_count++;
    if (!data->patched) {
        data->patched = true;
        OK(uc_mem_write(uc, data->patch_address, &inc_ecx,
                        sizeof(inc_ecx)));
    }
}

static bool test_x86_vtlb_allow_fetch_prot(uc_engine *uc,
                                           uc_mem_type type,
                                           uint64_t address, int size,
                                           int64_t value, void *user_data)
{
    return true;
}

static void test_x86_vtlb_active_code_rw_backing_patch(void)
{
    const uint8_t code[] = {
        0x40, /* inc eax */
        0x40, /* inc eax; patched to inc ecx by callback */
    };
    X86VtlbActivePatchData data = {
        .patch_address = code_start + 1,
    };
    uint32_t eax = 0;
    uint32_t ecx = 0;
    uc_engine *uc;
    uc_hook code_hook;
    uc_hook fetch_prot_hook;
    uc_hook tlb_hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, code_start, code_len,
                  UC_PROT_READ | UC_PROT_WRITE));
    OK(uc_mem_write(uc, code_start, code, sizeof(code)));
    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &tlb_hook, UC_HOOK_TLB_FILL,
                   test_x86_vtlb_callback, NULL, 1, 0));
    OK(uc_hook_add(uc, &fetch_prot_hook, UC_HOOK_MEM_FETCH_PROT,
                   test_x86_vtlb_allow_fetch_prot, NULL, 1, 0));
    OK(uc_hook_add(uc, &code_hook, UC_HOOK_CODE,
                   test_x86_vtlb_active_patch_callback, &data,
                   code_start, code_start));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK_(data.callback_count == 2, "callback_count=%u",
                data.callback_count);
    TEST_CHECK_(eax == 1, "eax=0x%x", eax);
    TEST_CHECK_(ecx == 1, "ecx=0x%x", ecx);

    OK(uc_close(uc));
}

typedef struct X86TbFlushSelfLinkData {
    uint8_t *code;
    uint32_t count;
} X86TbFlushSelfLinkData;

static void test_x86_memory_hook_tb_flush_self_link_callback(
    uc_engine *uc, uc_mem_type type, uint64_t address, int size,
    int64_t value, void *user_data)
{
    X86TbFlushSelfLinkData *data =
        (X86TbFlushSelfLinkData *)user_data;

    data->count++;
    if (data->count == 1) {
        data->code[2] = 0x42; /* inc edx */
        OK(uc_ctl_flush_tb(uc));
    }
}

static void test_x86_memory_hook_tb_flush_self_link(void)
{
    const uint64_t data_address = 0x200000;
    const uint8_t original_code[] = {
        0x8b, 0x33, /* mov esi, [ebx] */
        0x40,       /* inc eax */
        0x49,       /* dec ecx */
        0x75, 0xfa, /* jne loop */
    };
    const uint32_t memory_value = 0x11223344;
    uint8_t *code_page = calloc(1, 0x1000);
    X86TbFlushSelfLinkData data = {
        .code = code_page,
    };
    uint32_t ebx = (uint32_t)data_address;
    uint32_t ecx = 4;
    uint32_t eax = 0;
    uint32_t edx = 0;
    uc_engine *uc;
    uc_hook hook;

    TEST_CHECK(code_page != NULL);
    memcpy(code_page, original_code, sizeof(original_code));
    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map_ptr(uc, code_start, 0x1000, UC_PROT_ALL, code_page));
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &memory_value,
                    sizeof(memory_value)));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));

    OK(uc_emu_start(uc, code_start,
                    code_start + sizeof(original_code), 0, 0));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_memory_hook_tb_flush_self_link_callback, &data,
                   data_address,
                   data_address + sizeof(memory_value) - 1));

    ecx = 2;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_reg_write(uc, UC_X86_REG_EDX, &edx));
    OK(uc_emu_start(uc, code_start,
                    code_start + sizeof(original_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &edx));
    TEST_CHECK_(data.count == 2, "count=%u", data.count);
    TEST_CHECK_(eax == 1, "eax=0x%x", eax);
    TEST_CHECK_(edx == 1, "edx=0x%x", edx);

    OK(uc_close(uc));
    free(code_page);
}

static void test_x86_code_hook_nested_flush_callback(
    uc_engine *uc, uint64_t address, uint32_t size, void *user_data)
{
    X86NestedFlushHookData *data =
        (X86NestedFlushHookData *)user_data;

    data->count++;
    if (!data->nested_started) {
        data->nested_started = true;
        OK(uc_ctl_flush_tb(uc));
        if (data->nested_address != 0) {
            OK(uc_emu_start(uc, data->nested_address,
                            data->nested_address + 5, 0, 0));
        }
    }
}

static void test_x86_code_hook_nested_tb_flush_run(unsigned int hook_count,
                                                   bool nested)
{
    const uint64_t nested_address = code_start + 0x1000;
    const uint8_t outer_code[] = { 0x40 }; /* inc eax */
    const uint8_t nested_code[] = {
        0xb9, 0x78, 0x56, 0x34, 0x12, /* mov ecx, 0x12345678 */
    };
    X86NestedFlushHookData data = {
        .nested_address = nested ? nested_address : 0,
    };
    uint32_t eax;
    uint32_t ecx;
    unsigned int i;
    uc_engine *uc;
    uc_hook hooks[2];

    TEST_CHECK(hook_count <= sizeof(hooks) / sizeof(hooks[0]));

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)outer_code, sizeof(outer_code));
    OK(uc_mem_write(uc, nested_address, nested_code, sizeof(nested_code)));
    for (i = 0; i < hook_count; i++) {
        OK(uc_hook_add(uc, &hooks[i], UC_HOOK_CODE,
                       test_x86_code_hook_nested_flush_callback, &data,
                       code_start, code_start));
    }

    OK(uc_emu_start(uc, code_start,
                    code_start + sizeof(outer_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK_(data.count == hook_count, "count=%u", data.count);
    TEST_CHECK_(eax == 1, "eax=0x%x", eax);
    TEST_CHECK(ecx == (nested ? 0x12345678 : 0));

    OK(uc_close(uc));
}

static void test_x86_code_hook_nested_tb_flush(void)
{
    test_x86_code_hook_nested_tb_flush_run(1, true);
}

static void test_x86_multi_code_hook_nested_tb_flush(void)
{
    test_x86_code_hook_nested_tb_flush_run(2, false);
}

static void test_x86_nested_tb_flush_retranslation(void)
{
    const uint64_t nested_address = 0x400000;
    const uint8_t outer_code[] = { 0x40 }; /* inc eax */
    const uint8_t old_nested_code[] = {
        0xb9, 0x11, 0x11, 0x11, 0x11, /* mov ecx, 0x11111111 */
    };
    const uint8_t new_nested_code[] = {
        0xb9, 0x78, 0x56, 0x34, 0x12, /* mov ecx, 0x12345678 */
    };
    X86NestedFlushHookData data = {
        .nested_address = nested_address,
    };
    uint8_t *nested_page = calloc(1, 0x1000);
    uint32_t ecx;
    uc_engine *uc;
    uc_hook hook;

    TEST_CHECK(nested_page != NULL);
    memcpy(nested_page, old_nested_code, sizeof(old_nested_code));
    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32,
                    (const char *)outer_code, sizeof(outer_code));
    OK(uc_mem_map_ptr(uc, nested_address, 0x1000, UC_PROT_ALL,
                      nested_page));

    OK(uc_emu_start(uc, nested_address,
                    nested_address + sizeof(old_nested_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK(ecx == 0x11111111);

    memcpy(nested_page, new_nested_code, sizeof(new_nested_code));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE,
                   test_x86_code_hook_nested_flush_callback, &data,
                   code_start, code_start));
    OK(uc_emu_start(uc, code_start,
                    code_start + sizeof(outer_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK_(data.count == 1, "count=%u", data.count);
    TEST_CHECK_(ecx == 0x12345678, "ecx=0x%x", ecx);

    OK(uc_close(uc));
    free(nested_page);
}

typedef struct X86NestedFlushFaultData {
    uint64_t nested_address;
    uc_err nested_error;
    uint32_t count;
} X86NestedFlushFaultData;

static void test_x86_nested_flush_fault_callback(
    uc_engine *uc, uc_mem_type type, uint64_t address, int size,
    int64_t value, void *user_data)
{
    X86NestedFlushFaultData *data =
        (X86NestedFlushFaultData *)user_data;

    data->count++;
    OK(uc_ctl_flush_tb(uc));
    data->nested_error =
        uc_emu_start(uc, data->nested_address,
                     data->nested_address + 6, 0, 0);
}

static void test_x86_nested_tb_flush_fault_recovery(void)
{
    const uint64_t nested_address = code_start + 0x100;
    const uint64_t data_address = 0x200000;
    const uint8_t outer_code[] = {
        0x8b, 0x03,
        0x40,
    };
    const uint8_t nested_code[] = {
        0x8b, 0x0d, 0x00, 0x00, 0x30, 0x00,
    };
    const uint32_t memory_value = 0x11223344;
    uint8_t *code_page = calloc(1, 0x1000);
    X86NestedFlushFaultData data = {
        .nested_address = nested_address,
    };
    uint32_t ebx = (uint32_t)data_address;
    uint32_t eax;
    uc_engine *uc;
    uc_hook hook;

    TEST_CHECK(code_page != NULL);
    memcpy(code_page, outer_code, sizeof(outer_code));
    memcpy(code_page + nested_address - code_start,
           nested_code, sizeof(nested_code));
    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map_ptr(uc, code_start, 0x1000, UC_PROT_ALL, code_page));
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, data_address, &memory_value,
                    sizeof(memory_value)));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                   test_x86_nested_flush_fault_callback, &data,
                   data_address, data_address + sizeof(memory_value) - 1));

    OK(uc_emu_start(uc, code_start,
                    code_start + sizeof(outer_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(data.count == 1);
    TEST_CHECK(data.nested_error == UC_ERR_READ_UNMAPPED);
    TEST_CHECK(eax == memory_value + 1);

    OK(uc_hook_del(uc, hook));
    code_page[2] = 0x48;
    OK(uc_emu_start(uc, code_start,
                    code_start + sizeof(outer_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == memory_value - 1);

    OK(uc_close(uc));
    free(code_page);
}

static void test_x86_mem_hooks_pc_guarante_mem(uc_engine *uc, uc_mem_type type,
                                              uint64_t addr, int size,
                                              int64_t val, void *data)
{
    if (addr >= code_start + code_len) {
        uint32_t eip;
        OK(uc_reg_read(uc, UC_X86_REG_EIP, (void*)&eip));
        TEST_CHECK(eip == code_start + 1);
    }
}

static void test_x86_mem_hooks_pc_guarantee(void)
{
    uc_engine *uc;
    // bs, _ = ks.asm("inc edx; t: mov eax, [ebx]; inc ebx; cmp ebx, ecx; jnz t;")
    char code[] = "\x42\x8b\x03\x43\x39\xcb\x75\xf9";
    uint32_t ebx=code_start + code_len, ecx = code_start + code_len + 0x10;
    uc_hook hk;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    OK(uc_mem_map(uc, code_start + code_len, 0x1000, UC_PROT_ALL));
    OK(uc_hook_add(uc, &hk, UC_HOOK_MEM_READ, test_x86_mem_hooks_pc_guarante_mem, NULL,
                   1, 0));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, (void*)&ebx));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, (void*)&ecx));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

// Test that AAA sets PF, ZF, SF based on the result AL value.
// Bug: AAA previously left these flags stale from the prior instruction.
static void test_x86_aaa_flags(void)
{
    uc_engine *uc;

    // INC ECX sets PF from ECX result (to create a conflicting PF state).
    // AAA then adjusts AL and must set PF = parity(AL), ZF, SF independently.
    //
    // Case: EAX = 0x030A  (AH=3, AL=0x0A)
    //   AL low nibble (0x0A & 0x0F = 0x0A) > 9, so adjustment fires:
    //     AL = (0x0A + 6) & 0x0F = 0x00
    //     AH = 3 + 1 = 4
    //     CF = 1, AF = 1
    //   Result AL = 0x00: PF=1 (even parity), ZF=1, SF=0
    //
    // INC ECX (0x41) ; AAA (0x37)
    char code[] = "\x41\x37";

    uint32_t r_eax = 0x030A;
    uint32_t r_ecx = 0x0000; // INC 0 -> 1, PF=0 (odd parity of 1)
    uint32_t r_eflags;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_read(uc, UC_X86_REG_EFLAGS, &r_eflags));

    // EAX should be 0x0400 (AH=4, AL=0)
    TEST_CHECK(r_eax == 0x0400);
    // PF (bit 2) = 1 (AL=0x00 has even parity)
    TEST_CHECK((r_eflags & 0x04) != 0);
    // ZF (bit 6) = 1 (AL == 0)
    TEST_CHECK((r_eflags & 0x40) != 0);
    // SF (bit 7) = 0 (AL bit 7 clear)
    TEST_CHECK((r_eflags & 0x80) == 0);
    // CF (bit 0) = 1 (adjustment fired)
    TEST_CHECK((r_eflags & 0x01) != 0);

    OK(uc_close(uc));
}

// Test that AAS sets PF, ZF, SF based on the result AL value.
static void test_x86_aas_flags(void)
{
    uc_engine *uc;

    // Case: EAX = 0x030A  (AH=3, AL=0x0A)
    //   AL low nibble > 9, so adjustment fires:
    //     AL = (0x0A - 6) & 0x0F = 0x04
    //     AH = 3 - 1 = 2
    //     CF = 1, AF = 1
    //   Result AL = 0x04: PF=0 (odd parity of 0x04), ZF=0, SF=0
    //
    // INC ECX (0x41) ; AAS (0x3F)
    char code[] = "\x41\x3f";

    uint32_t r_eax = 0x030A;
    uint32_t r_ecx = 0x0002; // INC 2 -> 3, PF=1 (even parity of 3)
    uint32_t r_eflags;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_start, code, sizeof(code) - 1));

    OK(uc_reg_write(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &r_ecx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_read(uc, UC_X86_REG_EFLAGS, &r_eflags));

    // EAX should be 0x0204 (AH=2, AL=4)
    TEST_CHECK(r_eax == 0x0204);
    // PF (bit 2) = 0 (AL=0x04 has odd parity: one bit set)
    TEST_CHECK((r_eflags & 0x04) == 0);
    // ZF (bit 6) = 0 (AL != 0)
    TEST_CHECK((r_eflags & 0x40) == 0);
    // SF (bit 7) = 0 (AL bit 7 clear)
    TEST_CHECK((r_eflags & 0x80) == 0);
    // CF (bit 0) = 1 (adjustment fired)
    TEST_CHECK((r_eflags & 0x01) != 0);

    OK(uc_close(uc));
}

static void test_x86_group_1a(void)
{
    uc_engine *uc;

    char code[] = {
        // pop rax (ModRM.reg == 0)
        0x8f, 0xc0,
        // reserved group 1a opcode (ModRM.reg == 7)
        0x8f, 0xc0 | (7 << 3)
    };

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code));

    OK(uc_mem_map(uc, code_start + code_len, 0x1000, UC_PROT_ALL));

    uint8_t stack_data[] = {0x12, 0x34, 0x56, 0x78, 0x90, 0xab, 0xcd, 0xef};

    uint64_t rsp = code_start + code_len + 0x800;
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &rsp));
    OK(uc_mem_write(uc, rsp, stack_data, sizeof(stack_data)));

    uc_assert_err(UC_ERR_INSN_INVALID,
            uc_emu_start(uc, code_start, code_start + sizeof(code), 0, 0));

    uint64_t rip = 0;
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &rip));
    TEST_CHECK(rip == code_start + 2);

    OK(uc_reg_read(uc, UC_X86_REG_RSP, &rsp));
    TEST_CHECK(rsp == code_start + code_len + 0x800 + 8);

    uint64_t rax = 0;
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    TEST_CHECK(rax == 0xefcdab9078563412);

    OK(uc_close(uc));
}

static void test_x86_lock_bt_mem(void)
{
    uc_engine *uc;

    // lock bt [rax], eax
    char code[] = "\xf0\x0f\xa3\x00";

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);

    OK(uc_mem_map(uc, code_start + code_len, 0x1000, UC_PROT_ALL));
    uc_assert_err(UC_ERR_INSN_INVALID, uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

static void test_x86_lock_bt_reg(void)
{
    uc_engine *uc;

    // lock bt eax, eax
    char code[] = "\xf0\x0f\xa3\xc0";

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);

    OK(uc_mem_map(uc, code_start + code_len, 0x1000, UC_PROT_ALL));
    uc_assert_err(UC_ERR_INSN_INVALID, uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

static void test_x86_lock_btc_mem(void)
{
    uc_engine *uc;

    // lock btc [rax], ebx
    char code[] = "\xf0\x0f\xbb\x18";

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);

    OK(uc_mem_map(uc, code_start + code_len, 0x1000, UC_PROT_ALL));

    char data[1] = "\x80";
    OK(uc_mem_write(uc, code_start + code_len + 0x800, data, sizeof(data)));

    uint64_t rax = code_start + code_len;
    uint64_t rbx = 8*0x800+7;
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &rax));
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &rbx));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    uint64_t rflags = 0;
    OK(uc_reg_read(uc, UC_X86_REG_RFLAGS, &rflags));
    TEST_CHECK((rflags & 1) != 0); // RFLAGS.CF must be set

    OK(uc_mem_read(uc, code_start + code_len + 0x800, data, sizeof(data)));
    TEST_CHECK(data[0] == 0);

    OK(uc_close(uc));
}

static void test_x86_lock_btc_reg(void)
{
    uc_engine *uc;

    // lock btc eax, eax
    char code[] = "\xf0\x0f\xbb\xc0";

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);

    OK(uc_mem_map(uc, code_start + code_len, 0x1000, UC_PROT_ALL));
    uc_assert_err(UC_ERR_INSN_INVALID, uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

TEST_LIST = {
    {"test_x86_in", test_x86_in},
    {"test_x86_out", test_x86_out},
    {"test_x86_mem_hook_all", test_x86_mem_hook_all},
    {"test_x86_invalid_decode_fetch_size",
     test_x86_invalid_decode_fetch_size},
    {"test_x86_inc_dec_pxor", test_x86_inc_dec_pxor},
    {"test_x86_avx_vpxor_ymm", test_x86_avx_vpxor_ymm},
    {"test_x86_avx_vex128_zero_upper", test_x86_avx_vex128_zero_upper},
    {"test_x86_avx_scalar_zero_upper", test_x86_avx_scalar_zero_upper},
    {"test_x86_avx_fma_ps", test_x86_avx_fma_ps},
    {"test_x86_fma_scalar_variants", test_x86_fma_scalar_variants},
    {"test_x86_avx2_broadcast_permute", test_x86_avx2_broadcast_permute},
    {"test_x86_avx2_variable_shifts", test_x86_avx2_variable_shifts},
    {"test_x86_avx2_mask_gather", test_x86_avx2_mask_gather},
    {"test_x86_avx_vzeroall", test_x86_avx_vzeroall},
    {"test_x86_aes_pclmul", test_x86_aes_pclmul},
    {"test_x86_avx512_tcg_mask", test_x86_avx512_tcg_mask},
    {"test_x86_vaes_vex_gating", test_x86_vaes_vex_gating},
    {"test_x86_vpclmulqdq_tcg_mask", test_x86_vpclmulqdq_tcg_mask},
    {"test_x86_relative_jump", test_x86_relative_jump},
    {"test_x86_loop", test_x86_loop},
    {"test_x86_invalid_mem_read", test_x86_invalid_mem_read},
    {"test_x86_invalid_mem_write", test_x86_invalid_mem_write},
    {"test_x86_invalid_jump", test_x86_invalid_jump},
    {"test_x86_64_syscall", test_x86_64_syscall},
    {"test_x86_16_add", test_x86_16_add},
    {"test_x86_reg_save", test_x86_reg_save},
    {"test_x86_invalid_mem_read_stop_in_cb",
     test_x86_invalid_mem_read_stop_in_cb},
    {"test_x86_x87_fnstenv", test_x86_x87_fnstenv},
    {"test_x86_mmio", test_x86_mmio},
    {"test_x86_cputlb_read_after_exit", test_x86_cputlb_read_after_exit},
    {"test_x86_cputlb_split_mmio_exit", test_x86_cputlb_split_mmio_exit},
    {"test_x86_missing_code", test_x86_missing_code},
    {"test_x86_smc_xor", test_x86_smc_xor},
    {"test_x86_smc_add", test_x86_smc_add},
    {"test_x86_smc_mem_hook", test_x86_smc_mem_hook},
    {"test_x86_mmio_uc_mem_rw", test_x86_mmio_uc_mem_rw},
    {"test_x86_sysenter", test_x86_sysenter},
    {"test_x86_hook_cpuid", test_x86_hook_cpuid},
    {"test_x86_486_cpuid", test_x86_486_cpuid},
    {"test_x86_qemu72_xsave_cpuid", test_x86_qemu72_xsave_cpuid},
    {"test_x86_xsave_xrstor_roundtrip", test_x86_xsave_xrstor_roundtrip},
    {"test_x86_xsaveopt_xrstor_roundtrip", test_x86_xsaveopt_xrstor_roundtrip},
    {"test_x86_xsave_xcr0_mask", test_x86_xsave_xcr0_mask},
    {"test_x86_xsave_model_gating", test_x86_xsave_model_gating},
    {"test_x86_xsave_alignment_fault", test_x86_xsave_alignment_fault},
    {"test_x86_xsave_pkru_roundtrip", test_x86_xsave_pkru_roundtrip},
    {"test_x86_opmask_registers", test_x86_opmask_registers},
    {"test_x86_qemu72_msr_state", test_x86_qemu72_msr_state},
    {"test_x86_clear_tb_cache", test_x86_clear_tb_cache},
    {"test_x86_clear_empty_tb", test_x86_clear_empty_tb},
    {"test_x86_self_linked_tb_guest_smc", test_x86_self_linked_tb_guest_smc},
    {"test_x86_two_page_tb_invalidation",
     test_x86_two_page_tb_invalidation},
    {"test_x86_tb_cache_engine_isolation",
     test_x86_tb_cache_engine_isolation},
    {"test_x86_hook_tcg_op", test_x86_hook_tcg_op},
    {"test_x86_cmpxchg", test_x86_cmpxchg},
    {"test_x86_cmpxchg32_accumulator", test_x86_cmpxchg32_accumulator},
    {"test_x86_cmpxchg32_register", test_x86_cmpxchg32_register},
    {"test_x86_ret_imm16_unsigned", test_x86_ret_imm16_unsigned},
    {"test_x86_rorx_rip_relative_imm", test_x86_rorx_rip_relative_imm},
    {"test_x86_shld_rip_relative_imm", test_x86_shld_rip_relative_imm},
    {"test_x86_shrd_rip_relative_imm", test_x86_shrd_rip_relative_imm},
    {"test_x86_pdep32_zero_extend", test_x86_pdep32_zero_extend},
    {"test_x86_pext32_zero_extend", test_x86_pext32_zero_extend},
    {"test_x86_nested_emu_start", test_x86_nested_emu_start},
    {"test_x86_nested_count_state", test_x86_nested_count_state},
    {"test_x86_nested_emu_stop", test_x86_nested_emu_stop},
    {"test_x86_64_nested_emu_start_error", test_x86_64_nested_emu_start_error},
    {"test_x86_nested_emu_start_max_depth",
     test_x86_nested_emu_start_max_depth},
    {"test_x86_eflags_reserved_bit", test_x86_eflags_reserved_bit},
    {"test_x86_blsi_cf", test_x86_blsi_cf},
    {"test_x86_blsr_flags", test_x86_blsr_flags},
    {"test_x86_blsmsk_flags", test_x86_blsmsk_flags},
    {"test_x86_bzhi_index_boundary", test_x86_bzhi_index_boundary},
    {"test_x86_nested_uc_emu_start_exits", test_x86_nested_uc_emu_start_exits},
    {"test_x86_clear_count_cache", test_x86_clear_count_cache},
    {"test_x86_large_instruction_count", test_x86_large_instruction_count},
    {"test_x86_instruction_count_pc_change_refund",
     test_x86_instruction_count_pc_change_refund},
    {"test_x86_correct_address_in_small_jump_hook",
     test_x86_correct_address_in_small_jump_hook},
    {"test_x86_correct_address_in_long_jump_hook",
     test_x86_correct_address_in_long_jump_hook},
    {"test_x86_invalid_vex_l", test_x86_invalid_vex_l},
    {"test_x86_sse_aligned_access", test_x86_sse_aligned_access},
    {"test_x86_movdqa_movdqu_alignment",
     test_x86_movdqa_movdqu_alignment},
    {"test_x86_data_watchpoint", test_x86_data_watchpoint},
    {"test_x86_context_debug_lifecycle",
     test_x86_context_debug_lifecycle},
#if !defined(TARGET_READ_INLINED) && defined(BOOST_LITTLE_ENDIAN)
    {"test_x86_unaligned_access", test_x86_unaligned_access},
    {"test_x86_64_unaligned_access", test_x86_64_unaligned_access},

#endif
    {"test_x86_lazy_mapping", test_x86_lazy_mapping},
    {"test_x86_16_incorrect_ip", test_x86_16_incorrect_ip},
    {"test_x86_mmu", test_x86_mmu},
    {"test_x86_read_virtual", test_x86_read_virtual},
    {"test_x86_vtlb", test_x86_vtlb},
    {"test_x86_cputlb_vtlb_fill_exit", test_x86_cputlb_vtlb_fill_exit},
    {"test_x86_vtlb_conflict_growth", test_x86_vtlb_conflict_growth},
    {"test_x86_vtlb_stride_conflict_growth",
     test_x86_vtlb_stride_conflict_growth},
    {"test_x86_vtlb_hooked_victim_hit", test_x86_vtlb_hooked_victim_hit},
    {"test_x86_vtlb_32bit_high_paddr", test_x86_vtlb_32bit_high_paddr},
    {"test_x86_segmentation", test_x86_segmentation},
    {"test_x86_0xff_lcall", test_x86_0xff_lcall},
    {"test_x86_64_not_overwriting_tmp0_for_pc_update",
     test_x86_64_not_overwriting_tmp0_for_pc_update},
    {"test_fxsave_fpip_x86", test_fxsave_fpip_x86},
    {"test_fxsave_fpip_x64", test_fxsave_fpip_x64},
    {"test_bswap_x64", test_bswap_ax},
    {"test_rex_x64", test_rex_x64},
    {"test_x86_ro_segfault", test_x86_ro_segfault},
    {"test_x86_vpermilps_null_ptr_call", test_x86_vpermilps_null_ptr_call},
    {"test_x86_hook_insn_rdtsc", test_x86_hook_insn_rdtsc},
    {"test_x86_hook_insn_rdtscp", test_x86_hook_insn_rdtscp},
    {"test_x86_hook_insn_wrmsr", test_x86_hook_insn_wrmsr},
    {"test_x86_hook_insn_rdmsr", test_x86_hook_insn_rdmsr},
    {"test_x86_filtered_insn_hooks", test_x86_filtered_insn_hooks},
    {"test_x86_filtered_system_hooks", test_x86_filtered_system_hooks},
    {"test_x86_dr7", test_x86_dr7},
    {"test_x86_hook_block", test_x86_hook_block},
    {"test_x86_memory_hook_add_delete_after_translation",
     test_x86_memory_hook_add_delete_after_translation},
    {"test_x86_memory_hook_mutation_same_dispatch",
     test_x86_memory_hook_mutation_same_dispatch},
    {"test_x86_memory_hook_user_data",
     test_x86_memory_hook_user_data},
    {"test_x86_memory_hook_fallback_accesses",
     test_x86_memory_hook_fallback_accesses},
    {"test_x86_memory_hook_restore_cache_tb_flush",
     test_x86_memory_hook_restore_cache_tb_flush},
    {"test_x86_bounded_mid_tb_code_hook_delete",
     test_x86_bounded_mid_tb_code_hook_delete},
    {"test_x86_self_deleting_single_code_hook",
     test_x86_self_deleting_single_code_hook},
    {"test_x86_bounded_code_hook_user_data",
     test_x86_bounded_code_hook_user_data},
    {"test_x86_self_deleting_single_block_hook",
     test_x86_self_deleting_single_block_hook},
    {"test_x86_code_hook_append_same_dispatch",
     test_x86_code_hook_append_same_dispatch},
    {"test_x86_code_hook_delete_later_same_dispatch",
     test_x86_code_hook_delete_later_same_dispatch},
    {"test_x86_edge_hook_tb_flush_lifetime",
     test_x86_edge_hook_tb_flush_lifetime},
    {"test_x86_nested_edge_history", test_x86_nested_edge_history},
    {"test_x86_edge_hook_add_resets_history",
     test_x86_edge_hook_add_resets_history},
    {"test_x86_memory_write_read_after_hook_add_delete",
     test_x86_memory_write_read_after_hook_add_delete},
    {"test_x86_memory_hook_remap_revalidation",
     test_x86_memory_hook_remap_revalidation},
    {"test_x86_memory_hook_nested_tlb_revalidation",
     test_x86_memory_hook_nested_tlb_revalidation},
    {"test_x86_memory_hook_nested_tb_flush",
     test_x86_memory_hook_nested_tb_flush},
    {"test_x86_nested_patch_following_outer_instruction",
     test_x86_nested_patch_following_outer_instruction},
    {"test_x86_nested_patch_direct_linked_successor",
     test_x86_nested_patch_direct_linked_successor},
    {"test_x86_nested_patch_direct_linked_block_hook",
     test_x86_nested_patch_direct_linked_block_hook},
    {"test_x86_vtlb_active_code_rw_backing_patch",
     test_x86_vtlb_active_code_rw_backing_patch},
    {"test_x86_memory_hook_tb_flush_self_link",
     test_x86_memory_hook_tb_flush_self_link},
    {"test_x86_code_hook_nested_tb_flush",
     test_x86_code_hook_nested_tb_flush},
    {"test_x86_multi_code_hook_nested_tb_flush",
     test_x86_multi_code_hook_nested_tb_flush},
    {"test_x86_nested_tb_flush_retranslation",
     test_x86_nested_tb_flush_retranslation},
    {"test_x86_nested_tb_flush_fault_recovery",
     test_x86_nested_tb_flush_fault_recovery},
    {"test_x86_mem_hooks_pc_guarantee", test_x86_mem_hooks_pc_guarantee},
    {"test_x86_aaa_flags", test_x86_aaa_flags},
    {"test_x86_aas_flags", test_x86_aas_flags},
    {"test_x86_group_1a", test_x86_group_1a},
    {"test_x86_lock_bt_mem", test_x86_lock_bt_mem},
    {"test_x86_lock_bt_reg", test_x86_lock_bt_reg},
    {"test_x86_lock_btc_mem", test_x86_lock_btc_mem},
    {"test_x86_lock_btc_reg", test_x86_lock_btc_reg},
    {NULL, NULL}};
