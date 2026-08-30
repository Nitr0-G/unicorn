#include "unicorn_test.h"
#include <string.h>

const uint64_t code_start = 0x1000;
const uint64_t code_len = 0x4000;

static void uc_common_setup(uc_engine **uc, uc_arch arch, uc_mode mode,
                            const char *code, uint64_t size)
{
    OK(uc_open(arch, mode, uc));
    OK(uc_mem_map(*uc, code_start, code_len, UC_PROT_ALL));
    OK(uc_mem_write(*uc, code_start, code, size));
}

#define GEN_SIMPLE_READ_TEST(field, ctl_type, arg_type, expected)              \
    static void test_uc_ctl_##field(void)                                      \
    {                                                                          \
        uc_engine *uc;                                                         \
        arg_type arg;                                                          \
        OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));                             \
        OK(uc_ctl(uc, UC_CTL_READ(ctl_type, 1), &arg));                        \
        TEST_CHECK(arg == expected);                                           \
        OK(uc_close(uc));                                                      \
    }

GEN_SIMPLE_READ_TEST(mode, UC_CTL_UC_MODE, int, 4)
GEN_SIMPLE_READ_TEST(arch, UC_CTL_UC_ARCH, int, 4)
GEN_SIMPLE_READ_TEST(page_size, UC_CTL_UC_PAGE_SIZE, uint32_t, 4096)
GEN_SIMPLE_READ_TEST(time_out, UC_CTL_UC_TIMEOUT, uint64_t, 0)

static void test_uc_ctl_exits(void)
{
    uc_engine *uc;
    //   cmp eax, 0;
    //   jg lb;
    //   inc eax;
    //   nop;       <---- exit1
    // lb:
    //   inc ebx;
    //   nop;      <---- exit2
    char code[] = "\x83\xf8\x00\x7f\x02\x40\x90\x43\x90";
    int r_eax;
    int r_ebx;
    uint64_t exits[] = {code_start + 6, code_start + 8};

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);
    OK(uc_ctl_exits_enable(uc));
    OK(uc_ctl_set_exits(uc, exits, 2));
    r_eax = 0;
    r_ebx = 0;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &r_ebx));

    // Run two times.
    OK(uc_emu_start(uc, code_start, 0, 0, 0));
    OK(uc_emu_start(uc, code_start, 0, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &r_ebx));

    TEST_CHECK(r_eax == 1);
    TEST_CHECK(r_ebx == 1);

    OK(uc_close(uc));
}

static void test_uc_ctl_exits_boundaries(void)
{
    const char code[] = "\x90";
    uint64_t exits[] = {
        code_start + 0x30,
        code_start + 0x10,
        code_start + 0x30,
        code_start + 0x20,
    };
    uint64_t output[4] = {
        UINT64_MAX, UINT64_MAX, UINT64_MAX, UINT64_MAX,
    };
    size_t count;
    uc_engine *uc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code,
                    sizeof(code) - 1);
    uc_assert_err(UC_ERR_ARG, uc_ctl_get_exits_cnt(uc, &count));
    uc_assert_err(UC_ERR_ARG, uc_ctl_set_exits(uc, exits, 4));

    OK(uc_ctl_exits_enable(uc));
    OK(uc_ctl_set_exits(uc, exits, 4));
    OK(uc_ctl_get_exits_cnt(uc, &count));
    TEST_CHECK(count == 3);
    uc_assert_err(UC_ERR_ARG, uc_ctl_get_exits(uc, output, 2));
    TEST_CHECK(output[0] == UINT64_MAX && output[1] == UINT64_MAX);
    OK(uc_ctl_get_exits(uc, output, 4));
    TEST_CHECK(output[0] == code_start + 0x10);
    TEST_CHECK(output[1] == code_start + 0x20);
    TEST_CHECK(output[2] == code_start + 0x30);
    TEST_CHECK(output[3] == UINT64_MAX);

    OK(uc_ctl_exits_disable(uc));
    uc_assert_err(UC_ERR_ARG, uc_ctl_get_exits_cnt(uc, &count));
    OK(uc_ctl_exits_enable(uc));
    OK(uc_ctl_get_exits_cnt(uc, &count));
    TEST_CHECK(count == 0);

    OK(uc_close(uc));
}

static void test_uc_timeout_reuse(void)
{
    const char loop[] = "\xeb\xfe";
    const char nop[] = "\x90";
    size_t timed_out;
    uint32_t eip;
    uc_engine *uc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, loop,
                    sizeof(loop) - 1);
    OK(uc_mem_write(uc, code_start + 0x100, nop, sizeof(nop) - 1));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(loop) - 1,
                    UC_SECOND_SCALE / 10, 0));
    OK(uc_query(uc, UC_QUERY_TIMEOUT, &timed_out));
    TEST_CHECK(timed_out == 1);

    OK(uc_emu_start(uc, code_start + 0x100,
                    code_start + 0x100 + sizeof(nop) - 1, 0, 1));
    OK(uc_query(uc, UC_QUERY_TIMEOUT, &timed_out));
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    TEST_CHECK(timed_out == 0);
    TEST_CHECK(eip == code_start + 0x100 + sizeof(nop) - 1);

    OK(uc_close(uc));
}

static void test_uc_timeout_max_tb(void)
{
    enum {
        max_tb_insns = 512,
        nop_insns = max_tb_insns - 1,
        loop_size = nop_insns + 5,
    };
    uint8_t code[loop_size];
    const int32_t displacement = -(int32_t)sizeof(code);
    size_t timed_out;
    uint32_t eip;
    uc_engine *uc;

    memset(code, 0x90, nop_insns);
    code[nop_insns] = 0xe9;
    memcpy(&code[nop_insns + 1], &displacement, sizeof(displacement));

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, (const char *)code,
                    sizeof(code));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code),
                    UC_SECOND_SCALE / 20, 0));
    OK(uc_query(uc, UC_QUERY_TIMEOUT, &timed_out));
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    TEST_CHECK(timed_out == 1);
    TEST_CHECK_(eip == code_start, "eip=0x%x", eip);

    OK(uc_close(uc));
}

static void test_uc_reg_sized(void)
{
    const uint64_t initial_rax = UINT64_C(0x0123456789abcdef);
    const uint64_t rejected_rax = UINT64_C(0xfedcba9876543210);
    const uint64_t initial_xmm[2] = {
        UINT64_C(0x0011223344556677),
        UINT64_C(0x8899aabbccddeeff),
    };
    const uint64_t rejected_xmm[2] = {
        UINT64_C(0xffeeddccbbaa9988),
        UINT64_C(0x7766554433221100),
    };
    uint64_t rax;
    uint64_t xmm[2];
    size_t size;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    size = sizeof(initial_rax);
    OK(uc_reg_write2(uc, UC_X86_REG_RAX, &initial_rax, &size));
    TEST_CHECK(size == sizeof(initial_rax));
    size = sizeof(initial_xmm);
    OK(uc_reg_write2(uc, UC_X86_REG_XMM0, initial_xmm, &size));
    TEST_CHECK(size == sizeof(initial_xmm));

    rax = 0;
    size = sizeof(rax);
    OK(uc_reg_read2(uc, UC_X86_REG_RAX, &rax, &size));
    TEST_CHECK(size == sizeof(rax));
    TEST_CHECK(rax == initial_rax);
    memset(xmm, 0, sizeof(xmm));
    size = sizeof(xmm);
    OK(uc_reg_read2(uc, UC_X86_REG_XMM0, xmm, &size));
    TEST_CHECK(size == sizeof(xmm));
    TEST_CHECK(memcmp(xmm, initial_xmm, sizeof(xmm)) == 0);

    rax = UINT64_MAX;
    size = sizeof(rax) - 1;
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_reg_read2(uc, UC_X86_REG_RAX, &rax, &size));
    TEST_CHECK(rax == UINT64_MAX);
    size = sizeof(rejected_rax) - 1;
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_reg_write2(uc, UC_X86_REG_RAX, &rejected_rax, &size));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
    TEST_CHECK(rax == initial_rax);

    xmm[0] = UINT64_MAX;
    xmm[1] = UINT64_MAX;
    size = sizeof(xmm) - 1;
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_reg_read2(uc, UC_X86_REG_XMM0, xmm, &size));
    TEST_CHECK(xmm[0] == UINT64_MAX && xmm[1] == UINT64_MAX);
    size = sizeof(rejected_xmm) - 1;
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_reg_write2(uc, UC_X86_REG_XMM0, rejected_xmm, &size));
    OK(uc_reg_read(uc, UC_X86_REG_XMM0, xmm));
    TEST_CHECK(memcmp(xmm, initial_xmm, sizeof(xmm)) == 0);

    OK(uc_close(uc));
}

static void test_uc_reg_batch(void)
{
    const int regs[] = {
        UC_X86_REG_RAX,
        UC_X86_REG_XMM0,
        UC_X86_REG_RIP,
    };
    uint64_t rax = UINT64_C(0x1122334455667788);
    uint64_t xmm[2] = {
        UINT64_C(0x1020304050607080),
        UINT64_C(0x90a0b0c0d0e0f000),
    };
    uint64_t rip = code_start + 0x20;
    void *write_values[] = {&rax, xmm, &rip};
    uint64_t read_rax = 0;
    uint64_t read_xmm[2] = {0};
    uint64_t read_rip = 0;
    void *read_values[] = {&read_rax, read_xmm, &read_rip};
    const void *const_values[] = {&rax, xmm, &rip};
    size_t sizes[] = {sizeof(rax), sizeof(xmm), sizeof(rip)};
    size_t read_sizes[] = {
        sizeof(read_rax),
        sizeof(read_xmm),
        sizeof(read_rip),
    };
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    OK(uc_reg_write_batch(uc, regs, write_values, 3));
    OK(uc_reg_read_batch(uc, regs, read_values, 3));
    TEST_CHECK(read_rax == rax);
    TEST_CHECK(memcmp(read_xmm, xmm, sizeof(xmm)) == 0);
    TEST_CHECK(read_rip == rip);

    rax = UINT64_C(0x8877665544332211);
    xmm[0] = UINT64_C(0xf0e0d0c0b0a09080);
    xmm[1] = UINT64_C(0x7060504030201000);
    rip = code_start + 0x40;
    OK(uc_reg_write_batch2(uc, regs, const_values, sizes, 3));
    TEST_CHECK(sizes[0] == sizeof(rax));
    TEST_CHECK(sizes[1] == sizeof(xmm));
    TEST_CHECK(sizes[2] == sizeof(rip));
    memset(read_values[0], 0, sizeof(read_rax));
    memset(read_values[1], 0, sizeof(read_xmm));
    memset(read_values[2], 0, sizeof(read_rip));
    OK(uc_reg_read_batch2(uc, regs, read_values, read_sizes, 3));
    TEST_CHECK(read_sizes[0] == sizeof(read_rax));
    TEST_CHECK(read_sizes[1] == sizeof(read_xmm));
    TEST_CHECK(read_sizes[2] == sizeof(read_rip));
    TEST_CHECK(read_rax == rax);
    TEST_CHECK(memcmp(read_xmm, xmm, sizeof(xmm)) == 0);
    TEST_CHECK(read_rip == rip);

    OK(uc_close(uc));
}

static void test_uc_reg_batch_partial_failure(void)
{
    const int invalid_regs[] = {
        UC_X86_REG_RAX,
        UC_X86_REG_ENDING,
        UC_X86_REG_RBX,
    };
    const int overflow_regs[] = {
        UC_X86_REG_RAX,
        UC_X86_REG_XMM0,
        UC_X86_REG_RBX,
    };
    uint64_t rax = 1;
    uint64_t invalid = 2;
    uint64_t rbx = 3;
    void *write_values[] = {&rax, &invalid, &rbx};
    uint64_t read_rax = UINT64_MAX;
    uint64_t read_invalid = UINT64_MAX;
    uint64_t read_rbx = UINT64_MAX;
    void *read_values[] = {&read_rax, &read_invalid, &read_rbx};
    uint64_t xmm[2] = {5, 6};
    const void *overflow_values[] = {&rax, xmm, &rbx};
    uint64_t read_xmm[2] = {UINT64_MAX, UINT64_MAX};
    void *overflow_read_values[] = {&read_rax, read_xmm, &read_rbx};
    size_t sizes[] = {sizeof(rax), sizeof(xmm) - 1, sizeof(rbx)};
    uint64_t value;
    uc_err error;
    bool strict_errors;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    value = 10;
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &value));
    value = 20;
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &value));

    strict_errors = getenv("UC_IGNORE_REG_BREAK") != NULL;
    error = uc_reg_write_batch(uc, invalid_regs, write_values, 3);
    TEST_CHECK(error == (strict_errors ? UC_ERR_ARG : UC_ERR_OK));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &value));
    TEST_CHECK(value == rax);
    OK(uc_reg_read(uc, UC_X86_REG_RBX, &value));
    TEST_CHECK(value == (strict_errors ? 20 : rbx));
    error = uc_reg_read_batch(uc, invalid_regs, read_values, 3);
    TEST_CHECK(error == (strict_errors ? UC_ERR_ARG : UC_ERR_OK));
    TEST_CHECK(read_rax == rax);
    TEST_CHECK(read_invalid == UINT64_MAX);
    TEST_CHECK(read_rbx == (strict_errors ? UINT64_MAX : value));

    rax = 4;
    value = 30;
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &value));
    value = 40;
    OK(uc_reg_write(uc, UC_X86_REG_RBX, &value));
    uc_assert_err(
        UC_ERR_OVERFLOW,
        uc_reg_write_batch2(uc, overflow_regs, overflow_values, sizes, 3));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &value));
    TEST_CHECK(value == rax);
    OK(uc_reg_read(uc, UC_X86_REG_RBX, &value));
    TEST_CHECK(value == 40);

    read_rax = UINT64_MAX;
    read_xmm[0] = UINT64_MAX;
    read_xmm[1] = UINT64_MAX;
    read_rbx = UINT64_MAX;
    sizes[0] = sizeof(read_rax);
    sizes[1] = sizeof(read_xmm) - 1;
    sizes[2] = sizeof(read_rbx);
    uc_assert_err(
        UC_ERR_OVERFLOW,
        uc_reg_read_batch2(uc, overflow_regs, overflow_read_values, sizes, 3));
    TEST_CHECK(read_rax == rax);
    TEST_CHECK(read_xmm[0] == UINT64_MAX && read_xmm[1] == UINT64_MAX);
    TEST_CHECK(read_rbx == UINT64_MAX);

    OK(uc_close(uc));
}

static void test_uc_context_reg_apis(void)
{
    const int regs[] = {
        UC_X86_REG_RAX,
        UC_X86_REG_XMM0,
        UC_X86_REG_RIP,
    };
    uint64_t rax = UINT64_C(0x0123456789abcdef);
    uint64_t xmm[2] = {
        UINT64_C(0x1111222233334444),
        UINT64_C(0x5555666677778888),
    };
    uint64_t rip = code_start + 0x20;
    void *write_values[] = {&rax, xmm, &rip};
    const void *const_values[] = {&rax, xmm, &rip};
    uint64_t read_rax = 0;
    uint64_t read_xmm[2] = {0};
    uint64_t read_rip = 0;
    void *read_values[] = {&read_rax, read_xmm, &read_rip};
    size_t sizes[] = {sizeof(rax), sizeof(xmm), sizeof(rip)};
    size_t read_sizes[] = {
        sizeof(read_rax),
        sizeof(read_xmm),
        sizeof(read_rip),
    };
    size_t context_size;
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    context_size = uc_context_size(uc);
    TEST_CHECK(context_size > sizeof(rax) + sizeof(xmm));
    TEST_CHECK(uc_context_size(uc) == context_size);
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    sizes[0] = sizeof(rax);
    OK(uc_context_reg_write2(context, UC_X86_REG_RAX, &rax, &sizes[0]));
    TEST_CHECK(sizes[0] == sizeof(rax));
    sizes[1] = sizeof(xmm);
    OK(uc_context_reg_write2(context, UC_X86_REG_XMM0, xmm, &sizes[1]));
    TEST_CHECK(sizes[1] == sizeof(xmm));
    read_rax = 0;
    read_sizes[0] = sizeof(read_rax);
    OK(uc_context_reg_read2(context, UC_X86_REG_RAX, &read_rax,
                            &read_sizes[0]));
    TEST_CHECK(read_sizes[0] == sizeof(read_rax));
    TEST_CHECK(read_rax == rax);
    memset(read_xmm, 0, sizeof(read_xmm));
    read_sizes[1] = sizeof(read_xmm);
    OK(uc_context_reg_read2(context, UC_X86_REG_XMM0, read_xmm,
                            &read_sizes[1]));
    TEST_CHECK(read_sizes[1] == sizeof(read_xmm));
    TEST_CHECK(memcmp(read_xmm, xmm, sizeof(xmm)) == 0);

    read_rax = UINT64_MAX;
    read_sizes[0] = sizeof(read_rax) - 1;
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_context_reg_read2(context, UC_X86_REG_RAX, &read_rax,
                                       &read_sizes[0]));
    TEST_CHECK(read_rax == UINT64_MAX);
    read_sizes[0] = sizeof(rax) - 1;
    read_rax = rax + 1;
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_context_reg_write2(context, UC_X86_REG_RAX, &read_rax,
                                        &read_sizes[0]));
    OK(uc_context_reg_read(context, UC_X86_REG_RAX, &read_rax));
    TEST_CHECK(read_rax == rax);
    read_xmm[0] = UINT64_MAX;
    read_xmm[1] = UINT64_MAX;
    read_sizes[1] = sizeof(read_xmm) - 1;
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_context_reg_read2(context, UC_X86_REG_XMM0, read_xmm,
                                       &read_sizes[1]));
    TEST_CHECK(read_xmm[0] == UINT64_MAX && read_xmm[1] == UINT64_MAX);
    read_sizes[1] = sizeof(read_xmm) - 1;
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_context_reg_write2(context, UC_X86_REG_XMM0, read_xmm,
                                        &read_sizes[1]));
    OK(uc_context_reg_read(context, UC_X86_REG_XMM0, read_xmm));
    TEST_CHECK(memcmp(read_xmm, xmm, sizeof(xmm)) == 0);

    rax = UINT64_C(0x8877665544332211);
    xmm[0] = UINT64_C(0x9999aaaabbbbcccc);
    xmm[1] = UINT64_C(0xddddeeeeffff0000);
    rip = code_start + 0x40;
    OK(uc_context_reg_write_batch(context, regs, write_values, 3));
    OK(uc_context_reg_read_batch(context, regs, read_values, 3));
    TEST_CHECK(read_rax == rax);
    TEST_CHECK(memcmp(read_xmm, xmm, sizeof(xmm)) == 0);
    TEST_CHECK(read_rip == rip);
    OK(uc_context_restore(uc, context));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &read_rip));
    TEST_CHECK(read_rip == rip);

    rax = UINT64_C(0x1020304050607080);
    xmm[0] = UINT64_C(0x0f1e2d3c4b5a6978);
    xmm[1] = UINT64_C(0x8796a5b4c3d2e1f0);
    rip = code_start + 0x60;
    sizes[0] = sizeof(rax);
    sizes[1] = sizeof(xmm);
    sizes[2] = sizeof(rip);
    OK(uc_context_reg_write_batch2(context, regs, const_values, sizes, 3));
    TEST_CHECK(sizes[0] == sizeof(rax));
    TEST_CHECK(sizes[1] == sizeof(xmm));
    TEST_CHECK(sizes[2] == sizeof(rip));
    read_rax = 0;
    memset(read_xmm, 0, sizeof(read_xmm));
    read_rip = 0;
    read_sizes[0] = sizeof(read_rax);
    read_sizes[1] = sizeof(read_xmm);
    read_sizes[2] = sizeof(read_rip);
    OK(uc_context_reg_read_batch2(context, regs, read_values, read_sizes, 3));
    TEST_CHECK(read_sizes[0] == sizeof(read_rax));
    TEST_CHECK(read_sizes[1] == sizeof(read_xmm));
    TEST_CHECK(read_sizes[2] == sizeof(read_rip));
    TEST_CHECK(read_rax == rax);
    TEST_CHECK(memcmp(read_xmm, xmm, sizeof(xmm)) == 0);
    TEST_CHECK(read_rip == rip);
    OK(uc_context_restore(uc, context));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &read_rip));
    TEST_CHECK(read_rip == rip);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_uc_context_batch_partial_failure(void)
{
    const int invalid_regs[] = {
        UC_X86_REG_RAX,
        UC_X86_REG_ENDING,
        UC_X86_REG_RBX,
    };
    const int overflow_regs[] = {
        UC_X86_REG_RAX,
        UC_X86_REG_XMM0,
        UC_X86_REG_RBX,
    };
    uint64_t rax = 1;
    uint64_t invalid = 2;
    uint64_t rbx = 3;
    void *write_values[] = {&rax, &invalid, &rbx};
    uint64_t read_rax = UINT64_MAX;
    uint64_t read_invalid = UINT64_MAX;
    uint64_t read_rbx = UINT64_MAX;
    void *read_values[] = {&read_rax, &read_invalid, &read_rbx};
    uint64_t xmm[2] = {5, 6};
    const void *overflow_values[] = {&rax, xmm, &rbx};
    uint64_t read_xmm[2] = {UINT64_MAX, UINT64_MAX};
    void *overflow_read_values[] = {&read_rax, read_xmm, &read_rbx};
    size_t sizes[] = {sizeof(rax), sizeof(xmm) - 1, sizeof(rbx)};
    uint64_t value;
    uc_err error;
    bool strict_errors;
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));
    value = 10;
    OK(uc_context_reg_write(context, UC_X86_REG_RAX, &value));
    value = 20;
    OK(uc_context_reg_write(context, UC_X86_REG_RBX, &value));

    strict_errors = getenv("UC_IGNORE_REG_BREAK") != NULL;
    error = uc_context_reg_write_batch(context, invalid_regs, write_values, 3);
    TEST_CHECK(error == (strict_errors ? UC_ERR_ARG : UC_ERR_OK));
    OK(uc_context_reg_read(context, UC_X86_REG_RAX, &value));
    TEST_CHECK(value == rax);
    OK(uc_context_reg_read(context, UC_X86_REG_RBX, &value));
    TEST_CHECK(value == (strict_errors ? 20 : rbx));
    error = uc_context_reg_read_batch(context, invalid_regs, read_values, 3);
    TEST_CHECK(error == (strict_errors ? UC_ERR_ARG : UC_ERR_OK));
    TEST_CHECK(read_rax == rax);
    TEST_CHECK(read_invalid == UINT64_MAX);
    TEST_CHECK(read_rbx == (strict_errors ? UINT64_MAX : value));

    rax = 4;
    value = 30;
    OK(uc_context_reg_write(context, UC_X86_REG_RAX, &value));
    value = 40;
    OK(uc_context_reg_write(context, UC_X86_REG_RBX, &value));
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_context_reg_write_batch2(context, overflow_regs,
                                              overflow_values, sizes, 3));
    OK(uc_context_reg_read(context, UC_X86_REG_RAX, &value));
    TEST_CHECK(value == rax);
    OK(uc_context_reg_read(context, UC_X86_REG_RBX, &value));
    TEST_CHECK(value == 40);

    read_rax = UINT64_MAX;
    read_xmm[0] = UINT64_MAX;
    read_xmm[1] = UINT64_MAX;
    read_rbx = UINT64_MAX;
    sizes[0] = sizeof(read_rax);
    sizes[1] = sizeof(read_xmm) - 1;
    sizes[2] = sizeof(read_rbx);
    uc_assert_err(UC_ERR_OVERFLOW,
                  uc_context_reg_read_batch2(context, overflow_regs,
                                             overflow_read_values, sizes, 3));
    TEST_CHECK(read_rax == rax);
    TEST_CHECK(read_xmm[0] == UINT64_MAX && read_xmm[1] == UINT64_MAX);
    TEST_CHECK(read_rbx == UINT64_MAX);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

typedef struct TestNestedTimeoutData {
    uint64_t inner_address;
    uint64_t inner_size;
    uint64_t inner_timeout;
    uc_err inner_error;
    uc_err query_error;
    size_t timed_out;
    uint32_t calls;
} TestNestedTimeoutData;

static void test_uc_nested_timeout_cb(uc_engine *uc, uint64_t address,
                                      uint32_t size, void *user_data)
{
    TestNestedTimeoutData *data = (TestNestedTimeoutData *)user_data;

    data->calls++;
    data->inner_error =
        uc_emu_start(uc, data->inner_address,
                     data->inner_address + data->inner_size,
                     data->inner_timeout, 0);
    data->query_error = uc_query(uc, UC_QUERY_TIMEOUT, &data->timed_out);
}

static void test_uc_nested_timeout_case(uint64_t inner_timeout,
                                        uint64_t outer_timeout,
                                        bool outer_continues)
{
    const char outer_code[] = "\x40\x43"; /* inc eax; inc ebx */
    const char inner_code[] = "\xeb\xfe"; /* jmp inner_code */
    const char reuse_code[] = "\x41";     /* inc ecx */
    const uint64_t inner_address = code_start + 0x100;
    const uint64_t reuse_address = code_start + 0x200;
    TestNestedTimeoutData data = {
        .inner_address = inner_address,
        .inner_size = sizeof(inner_code) - 1,
        .inner_timeout = inner_timeout,
    };
    size_t timed_out;
    uint32_t value;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, outer_code,
                    sizeof(outer_code) - 1);
    OK(uc_mem_write(uc, inner_address, inner_code, sizeof(inner_code) - 1));
    OK(uc_mem_write(uc, reuse_address, reuse_code, sizeof(reuse_code) - 1));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE, test_uc_nested_timeout_cb, &data,
                   code_start, code_start));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(outer_code) - 1,
                    outer_timeout, 0));
    TEST_CHECK(data.calls == 1);
    TEST_CHECK(data.inner_error == UC_ERR_OK);
    TEST_CHECK(data.query_error == UC_ERR_OK);
    TEST_CHECK(data.timed_out == 1);
    OK(uc_query(uc, UC_QUERY_TIMEOUT, &timed_out));
    TEST_CHECK(timed_out == 1);
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &value));
    TEST_CHECK(value == (outer_continues ? 1 : 0));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &value));
    TEST_CHECK(value == (outer_continues ? 1 : 0));

    OK(uc_hook_del(uc, hook));
    OK(uc_emu_start(uc, reuse_address, reuse_address + sizeof(reuse_code) - 1,
                    0, 1));
    OK(uc_query(uc, UC_QUERY_TIMEOUT, &timed_out));
    TEST_CHECK(timed_out == 0);
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &value));
    TEST_CHECK(value == 1);

    OK(uc_close(uc));
}

static void test_uc_nested_timeout(void)
{
    test_uc_nested_timeout_case(UC_SECOND_SCALE / 20, 0, true);
    test_uc_nested_timeout_case(0, UC_SECOND_SCALE / 20, false);
    test_uc_nested_timeout_case(UC_SECOND_SCALE / 50,
                                UC_SECOND_SCALE / 2, true);
    test_uc_nested_timeout_case(UC_SECOND_SCALE / 2,
                                UC_SECOND_SCALE / 50, false);
}

static void test_uc_nested_timeout_completion(void)
{
    const char outer_code[] = "\x40\x43"; /* inc eax; inc ebx */
    const char inner_code[] = "\x42";     /* inc edx */
    const char reuse_code[] = "\x41";     /* inc ecx */
    const uint64_t inner_address = code_start + 0x100;
    const uint64_t reuse_address = code_start + 0x200;
    TestNestedTimeoutData data = {
        .inner_address = inner_address,
        .inner_size = sizeof(inner_code) - 1,
        .inner_timeout = UC_SECOND_SCALE / 2,
    };
    size_t timed_out;
    uint32_t value;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, outer_code,
                    sizeof(outer_code) - 1);
    OK(uc_mem_write(uc, inner_address, inner_code, sizeof(inner_code) - 1));
    OK(uc_mem_write(uc, reuse_address, reuse_code, sizeof(reuse_code) - 1));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE, test_uc_nested_timeout_cb, &data,
                   code_start, code_start));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(outer_code) - 1,
                    UC_SECOND_SCALE / 2, 0));
    TEST_CHECK(data.calls == 1);
    TEST_CHECK(data.inner_error == UC_ERR_OK);
    TEST_CHECK(data.query_error == UC_ERR_OK);
    TEST_CHECK(data.timed_out == 0);
    OK(uc_query(uc, UC_QUERY_TIMEOUT, &timed_out));
    TEST_CHECK(timed_out == 0);
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &value));
    TEST_CHECK(value == 1);
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &value));
    TEST_CHECK(value == 1);
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &value));
    TEST_CHECK(value == 1);

    OK(uc_hook_del(uc, hook));
    OK(uc_emu_start(uc, reuse_address, reuse_address + sizeof(reuse_code) - 1,
                    0, 1));
    OK(uc_query(uc, UC_QUERY_TIMEOUT, &timed_out));
    TEST_CHECK(timed_out == 0);
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &value));
    TEST_CHECK(value == 1);

    OK(uc_close(uc));
}

static void test_uc_invalid_hook_cb(uc_engine *uc, uint64_t address,
                                    uint32_t size, void *user_data)
{
    uint32_t *calls = (uint32_t *)user_data;

    (*calls)++;
}

static void test_uc_invalid_hook_types(void)
{
    const char code[] = "\x90";
    const int unknown_type = 1U << 30;
    uint32_t calls = 0;
    uc_hook hook;
    uc_engine *uc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    hook = UINTPTR_MAX;
    uc_assert_err(
        UC_ERR_HOOK,
        uc_hook_add(uc, &hook, 0, test_uc_invalid_hook_cb, &calls, 1, 0));
    TEST_CHECK(hook == UINTPTR_MAX);
    hook = UINTPTR_MAX;
    uc_assert_err(UC_ERR_HOOK,
                  uc_hook_add(uc, &hook, unknown_type, test_uc_invalid_hook_cb,
                              &calls, 1, 0));
    TEST_CHECK(hook == UINTPTR_MAX);
    hook = UINTPTR_MAX;
    uc_assert_err(UC_ERR_HOOK,
                  uc_hook_add(uc, &hook, UC_HOOK_CODE | unknown_type,
                              test_uc_invalid_hook_cb, &calls, 1, 0));
    TEST_CHECK(hook == UINTPTR_MAX);

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    TEST_CHECK(calls == 0);
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE, test_uc_invalid_hook_cb, &calls, 1,
                   0));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 1));
    TEST_CHECK(calls == 1);

    OK(uc_close(uc));
}

static void test_uc_query_and_cpu_model(void)
{
    const char code[] = "\x90";
    size_t result;
    int model;
    uc_engine *uc;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code,
                    sizeof(code) - 1);
    uc_assert_err(UC_ERR_ARG, uc_query(uc, UC_QUERY_MODE + 100, &result));
    OK(uc_query(uc, UC_QUERY_ARCH, &result));
    TEST_CHECK(result == UC_ARCH_X86);
    OK(uc_query(uc, UC_QUERY_MODE, &result));
    TEST_CHECK(result == UC_MODE_32);
    OK(uc_query(uc, UC_QUERY_PAGE_SIZE, &result));
    TEST_CHECK(result == 4096);
    OK(uc_query(uc, UC_QUERY_TIMEOUT, &result));
    TEST_CHECK(result == 0);
    OK(uc_ctl_get_cpu_model(uc, &model));
    TEST_CHECK(model >= 0 && model < UC_CPU_X86_ENDING);
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1,
                    0, 0));

    OK(uc_close(uc));
}

#define TB_COUNT (8)
#define TCG_MAX_INSNS (512) // from tcg.h
#define CODE_LEN TB_COUNT *TCG_MAX_INSNS

static void test_uc_ctl_tb_cache(void)
{
    uc_engine *uc;
    char code[CODE_LEN + 1];

    memset(code, 0x90, CODE_LEN);
    code[CODE_LEN] = 0;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    for (int i = 0; i < TB_COUNT; i++) {
        uc_tb tb;
        uint64_t pc = code_start + i * TCG_MAX_INSNS;

        OK(uc_ctl_request_cache(uc, pc, &tb));
        TEST_CHECK(tb.pc == pc);
        TEST_CHECK(tb.size > 0);
        TEST_CHECK(tb.icount > 0);
    }

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    for (int i = 0; i < TB_COUNT; i++) {
        OK(uc_ctl_remove_cache(uc, code_start + i * TCG_MAX_INSNS,
                               code_start + i * TCG_MAX_INSNS + 1));
    }

    for (int i = 0; i < TB_COUNT; i++) {
        uc_tb tb;
        uint64_t pc = code_start + i * TCG_MAX_INSNS;

        OK(uc_ctl_request_cache(uc, pc, &tb));
        TEST_CHECK(tb.pc == pc);
        TEST_CHECK(tb.size > 0);
        TEST_CHECK(tb.icount > 0);
    }

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

// Test requires UC_ARCH_ARM.
#ifdef UNICORN_HAS_ARM
static void test_uc_ctl_change_page_size(void)
{
    uc_engine *uc;
    uc_engine *uc2;
    size_t mode;
    uint32_t pg = 0;

    OK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc));
    OK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc2));

    OK(uc_ctl_set_page_size(uc, 4096));
    uc_assert_err(UC_ERR_ARG, uc_ctl_set_page_size(uc, 0));
    uc_assert_err(UC_ERR_ARG, uc_ctl_set_page_size(uc, 1536));
    OK(uc_ctl_get_page_size(uc, &pg));
    TEST_CHECK(pg == 4096);
    OK(uc_query(uc, UC_QUERY_MODE, &mode));
    TEST_CHECK((mode & UC_MODE_THUMB) == 0);
    uc_assert_err(UC_ERR_ARG, uc_ctl_set_page_size(uc, 1024));
    OK(uc_ctl_get_page_size(uc, &pg));
    TEST_CHECK(pg == 4096);

    OK(uc_mem_map(uc2, 1 << 10, 1 << 10, UC_PROT_ALL));
    uc_assert_err(UC_ERR_ARG, uc_mem_map(uc, 1 << 10, 1 << 10, UC_PROT_ALL));

    OK(uc_close(uc));
    OK(uc_close(uc2));
}

static void test_uc_ctl_arm_page_size_per_engine(void)
{
    uc_engine *uc4k;
    uc_engine *uc1k;
    uint32_t page_size;
    uint32_t r0 = 0;
    const char code[] = "\x01\x00\xa0\xe3";

    OK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc4k));
    OK(uc_ctl_set_page_size(uc4k, 4096));
    OK(uc_mem_map(uc4k, 0x1000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc4k, 0x1000, code, sizeof(code) - 1));

    OK(uc_open(UC_ARCH_ARM, UC_MODE_ARM, &uc1k));
    OK(uc_ctl_set_page_size(uc1k, 1024));
    OK(uc_mem_map(uc1k, 0x400, 0x400, UC_PROT_ALL));

    OK(uc_ctl_get_page_size(uc4k, &page_size));
    TEST_CHECK(page_size == 4096);
    OK(uc_emu_start(uc4k, 0x1000, 0x1000 + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc4k, UC_ARM_REG_R0, &r0));
    TEST_CHECK(r0 == 1);

    OK(uc_close(uc1k));
    OK(uc_close(uc4k));
}
#endif

// Test requires UC_ARCH_ARM64.
#ifdef UNICORN_HAS_ARM64
static void test_uc_ctl_change_page_size_arm64(void)
{
    uc_engine *uc;
    uc_engine *uc2;
    uint32_t pg = 0;

    OK(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc));
    OK(uc_open(UC_ARCH_ARM64, UC_MODE_ARM, &uc2));

    OK(uc_ctl_set_page_size(uc, 16384));
    OK(uc_ctl_get_page_size(uc, &pg));
    TEST_CHECK(pg == 16384);

    OK(uc_mem_map(uc2, 1 << 10, 1 << 10, UC_PROT_ALL));
    uc_assert_err(UC_ERR_ARG, uc_mem_map(uc, 1 << 10, 1 << 10, UC_PROT_ALL));

    OK(uc_close(uc));
    OK(uc_close(uc2));
}
#endif

// Test requires UC_ARCH_ARM.
#ifdef UNICORN_HAS_ARM
// Copy from test_arm.c but with new API.
static void test_uc_ctl_arm_cpu(void)
{
    uc_engine *uc;
    int r_control, r_msp, r_psp;

    OK(uc_open(UC_ARCH_ARM, UC_MODE_THUMB, &uc));

    OK(uc_ctl_set_cpu_model(uc, UC_CPU_ARM_CORTEX_M7));

    r_control = 0; // Make sure we are using MSP.
    OK(uc_reg_write(uc, UC_ARM_REG_CONTROL, &r_control));

    r_msp = 0x1000;
    OK(uc_reg_write(uc, UC_ARM_REG_R13, &r_msp));

    r_control = 0b10; // Make the switch.
    OK(uc_reg_write(uc, UC_ARM_REG_CONTROL, &r_control));

    OK(uc_reg_read(uc, UC_ARM_REG_R13, &r_psp));
    TEST_CHECK(r_psp != r_msp);

    r_psp = 0x2000;
    OK(uc_reg_write(uc, UC_ARM_REG_R13, &r_psp));

    r_control = 0; // Switch again
    OK(uc_reg_write(uc, UC_ARM_REG_CONTROL, &r_control));

    OK(uc_reg_read(uc, UC_ARM_REG_R13, &r_msp));
    TEST_CHECK(r_psp != r_msp);
    TEST_CHECK(r_msp == 0x1000);

    OK(uc_close(uc));
}
#endif

static void test_uc_hook_cached_cb(uc_engine *uc, uint64_t addr, uint32_t size,
                                   void *user_data)
{
    uint64_t *p = (uint64_t *)user_data;
    (*p)++;
    return;
}

static void test_uc_hook_cached_uaf(void)
{
    uc_engine *uc;
    // "INC ecx; DEC edx; jmp t; t: nop"
    char code[] = "\x41\x4a\xeb\x00\x90";
    uc_hook h;
    uint64_t count = 0;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code) - 1);

    OK(uc_hook_add(uc, &h, UC_HOOK_CODE, (void *)test_uc_hook_cached_cb,
                   (void *)&count, 1, 0));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    // Move the hook to the deleted hooks list.
    OK(uc_hook_del(uc, h));

    // This will clear deleted hooks and SHOULD clear cache.
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    // Now hooks are deleted and thus this _should not_ call
    // test_uc_hook_cached_cb anymore. If the hook is allocated like from
    // malloc, and the code region is free-ed, this call _shall not_ call the
    // hook anymore to avoid UAF.
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    // Only 4 calls
    TEST_CHECK(count == 4);

    OK(uc_close(uc));
}

static void test_uc_emu_stop_set_ip_callback(uc_engine *uc, uint64_t address,
                                             uint32_t size, void *userdata)
{
    uint64_t rip = code_start + 0xb;

    if (address == code_start + 0x7) {
        uc_emu_stop(uc);
        uc_reg_write(uc, UC_X86_REG_RIP, &rip);
    }
}

static void test_uc_emu_stop_set_ip(void)
{
    uc_engine *uc;
    uc_hook h;
    uint64_t rip;

    char code[] =
        "\x48\x31\xc0" // 0x0    xor rax, rax    : rax = 0
        "\x90"         // 0x3    nop             :
        "\x48\xff\xc0" // 0x4    inc rax         : rax++
        "\x90"         // 0x7    nop             : <-- going to stop here
        "\x48\xff\xc0" // 0x8    inc rax         : rax++
        "\x90"         // 0xb    nop             :
        "\x0f\x0b"     // 0xc    ud2             : <-- will raise
                       // UC_ERR_INSN_INVALID, but should not never be reached
        "\x90"         // 0xe    nop             :
        "\x90";        // 0xf    nop             :

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_hook_add(uc, &h, UC_HOOK_CODE, test_uc_emu_stop_set_ip_callback, NULL,
                   1, 0));
    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RIP, &rip));
    TEST_CHECK(rip == code_start + 0xb);
    OK(uc_close(uc));
}

typedef enum TestPcWriteMethod {
    TEST_PC_WRITE_SINGLE,
    TEST_PC_WRITE_BATCH,
    TEST_PC_WRITE_BATCH2,
    TEST_PC_WRITE_SIZED,
    TEST_PC_WRITE_METHOD_COUNT,
} TestPcWriteMethod;

typedef struct TestPcWriteData {
    TestPcWriteMethod method;
    uint32_t count;
} TestPcWriteData;

static void test_uc_set_ip_callback(uc_engine *uc, uint64_t address,
                                    uint32_t size, void *user_data)
{
    TestPcWriteData *data = (TestPcWriteData *)user_data;
    uint64_t rip = code_start + 0xb;
    int regs[] = { UC_X86_REG_RIP };
    void *values[] = { &rip };
    const void *const_values[] = { &rip };
    size_t sizes[] = { sizeof(rip) };

    if (address != code_start + 0x7) {
        return;
    }
    data->count++;
    switch (data->method) {
    case TEST_PC_WRITE_SINGLE:
        OK(uc_reg_write(uc, UC_X86_REG_RIP, &rip));
        break;
    case TEST_PC_WRITE_BATCH:
        OK(uc_reg_write_batch(uc, regs, values, 1));
        break;
    case TEST_PC_WRITE_BATCH2:
        OK(uc_reg_write_batch2(uc, regs, const_values, sizes, 1));
        break;
    case TEST_PC_WRITE_SIZED:
        OK(uc_reg_write2(uc, UC_X86_REG_RIP, &rip, sizes));
        break;
    default:
        TEST_CHECK(false);
        break;
    }
}

static void test_uc_set_ip_write_apis(void)
{
    const char code[] =
        "\x48\x31\xc0" /* xor rax, rax */
        "\x90"         /* nop */
        "\x48\xff\xc0" /* inc rax */
        "\x90"         /* callback changes RIP */
        "\x48\xff\xc0" /* must not execute */
        "\x90";        /* destination */
    TestPcWriteMethod method;

    for (method = TEST_PC_WRITE_SINGLE;
         method < TEST_PC_WRITE_METHOD_COUNT; method++) {
        TestPcWriteData data = { .method = method };
        uc_engine *uc;
        uc_hook hook;
        uint64_t rax;
        uint64_t rip;

        uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code,
                        sizeof(code) - 1);
        OK(uc_hook_add(uc, &hook, UC_HOOK_CODE,
                       test_uc_set_ip_callback, &data, 1, 0));
        OK(uc_emu_start(uc, code_start, code_start + 0xb, 0, 0));
        OK(uc_reg_read(uc, UC_X86_REG_RAX, &rax));
        OK(uc_reg_read(uc, UC_X86_REG_RIP, &rip));
        TEST_CHECK(rax == 1);
        TEST_CHECK(rip == code_start + 0xb);
        TEST_CHECK(data.count == 1);
        OK(uc_close(uc));
    }
}

typedef struct TestContextRestoreData {
    uc_context *context;
    uint32_t count;
} TestContextRestoreData;

static void test_uc_context_restore_callback(uc_engine *uc,
                                             uint64_t address,
                                             uint32_t size,
                                             void *user_data)
{
    TestContextRestoreData *data =
        (TestContextRestoreData *)user_data;

    data->count++;
    OK(uc_context_restore(uc, data->context));
}

static void test_uc_context_restore_from_callback(void)
{
    const char code[] = {
        0x40, /* inc eax */
        0x43, /* callback before inc ebx */
        0x41, /* inc ecx */
        0x42, /* destination */
    };
    TestContextRestoreData data = { 0 };
    uint32_t destination = (uint32_t)code_start + 3;
    uint32_t eax = 0x10;
    uint32_t ebx = 0x20;
    uint32_t ecx = 0x30;
    uint32_t eip;
    uc_engine *uc;
    uc_hook hook;

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_32, code, sizeof(code));
    OK(uc_context_alloc(uc, &data.context));
    OK(uc_reg_write(uc, UC_X86_REG_EIP, &destination));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_context_save(uc, data.context));

    eax = 0;
    ebx = 0;
    ecx = 0;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_write(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE,
                   test_uc_context_restore_callback, &data,
                   code_start + 1, code_start + 1));

    OK(uc_emu_start(uc, code_start, destination, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    TEST_CHECK(data.count == 1);
    TEST_CHECK(eip == destination);
    TEST_CHECK(eax == 0x10);
    TEST_CHECK(ebx == 0x20);
    TEST_CHECK(ecx == 0x30);

    OK(uc_context_free(data.context));
    OK(uc_close(uc));
}

static bool test_tlb_clear_tlb(uc_engine *uc, uint64_t addr, uc_mem_type type,
                               uc_tlb_entry *result, void *user_data)
{
    size_t *tlbcount = (size_t *)user_data;
    *tlbcount += 1;
    result->paddr = addr;
    result->perms = UC_PROT_ALL;
    return true;
}

static void test_tlb_clear_syscall(uc_engine *uc, void *user_data)
{
    OK(uc_ctl_flush_tlb(uc));
}

static void test_tlb_clear(void)
{
    uc_engine *uc;
    uc_hook hook1, hook2;
    size_t tlbcount = 0;
    char code[] =
        "\xa3\x00\x00\x20\x00\x00\x00\x00\x00\x0f\x05\xa3\x00\x00\x20\x00\x00"
        "\x00\x00\x00"; // movabs  dword ptr [0x200000], eax; syscall; movabs
                        // dword ptr [0x200000], eax

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_mem_map(uc, 0x200000, 0x1000, UC_PROT_ALL));

    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook1, UC_HOOK_TLB_FILL, test_tlb_clear_tlb, &tlbcount,
                   1, 0));
    OK(uc_hook_add(uc, &hook2, UC_HOOK_INSN, test_tlb_clear_syscall, NULL, 1, 0,
                   UC_X86_INS_SYSCALL));

    OK(uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    TEST_CHECK(tlbcount == 4);

    OK(uc_close(uc));
}

static void test_noexec(void)
{
    uc_engine *uc;
    /* mov al, byte ptr[rip]
     * nop
     */
    char code[] = "\x8a\x05\x00\x00\x00\x00\x90";

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_mem_protect(uc, code_start, code_start + 0x1000, UC_PROT_EXEC));

    uc_assert_err(
        UC_ERR_READ_PROT,
        uc_emu_start(uc, code_start, code_start + sizeof(code) - 1, 0, 0));

    OK(uc_close(uc));
}

static void test_add_block_hook_syscall_cb(uc_engine *uc, void *userdata)
{
    OK(uc_emu_stop(uc));
}

static void test_add_block_hook_block_cb(uc_engine *uc, uint64_t address, uint32_t size, void *user_data)
{
    uint64_t *block_counter = user_data;
    *block_counter += 1;
}

static void test_add_block_hook(void)
{
    uc_engine *uc;
    uint64_t block_counter = 0;
    uc_hook syscall_hook;
    uc_hook block_hook;
    /* nop
     * syscall
     */
    char code[] = "\x90\x0F\x05";

    uc_common_setup(&uc, UC_ARCH_X86, UC_MODE_64, code, sizeof(code) - 1);
    OK(uc_hook_add(uc, &syscall_hook, UC_HOOK_INSN, &test_add_block_hook_syscall_cb, NULL, 1, 0, UC_X86_INS_SYSCALL));
    OK(uc_emu_start(uc, code_start, 0, 0, 0));
    OK(uc_hook_add(uc, &block_hook, UC_HOOK_BLOCK, &test_add_block_hook_block_cb, &block_counter, code_start, code_start+0x1000));
    OK(uc_emu_start(uc, code_start, 0, 0, 0));
    TEST_CHECK(block_counter == 1);
    OK(uc_close(uc));
}

TEST_LIST = {
    {"test_uc_ctl_mode", test_uc_ctl_mode},
    {"test_uc_ctl_page_size", test_uc_ctl_page_size},
    {"test_uc_ctl_arch", test_uc_ctl_arch},
    {"test_uc_ctl_time_out", test_uc_ctl_time_out},
    {"test_uc_ctl_exits", test_uc_ctl_exits},
    {"test_uc_ctl_exits_boundaries", test_uc_ctl_exits_boundaries},
    {"test_uc_timeout_reuse", test_uc_timeout_reuse},
    {"test_uc_timeout_max_tb", test_uc_timeout_max_tb},
    {"test_uc_reg_sized", test_uc_reg_sized},
    {"test_uc_reg_batch", test_uc_reg_batch},
    {"test_uc_reg_batch_partial_failure", test_uc_reg_batch_partial_failure},
    {"test_uc_context_reg_apis", test_uc_context_reg_apis},
    {"test_uc_context_batch_partial_failure",
     test_uc_context_batch_partial_failure},
    {"test_uc_nested_timeout", test_uc_nested_timeout},
    {"test_uc_nested_timeout_completion",
     test_uc_nested_timeout_completion},
    {"test_uc_invalid_hook_types", test_uc_invalid_hook_types},
    {"test_uc_query_and_cpu_model", test_uc_query_and_cpu_model},
    {"test_uc_ctl_tb_cache", test_uc_ctl_tb_cache},
#ifdef UNICORN_HAS_ARM
    {"test_uc_ctl_change_page_size", test_uc_ctl_change_page_size},
    {"test_uc_ctl_arm_page_size_per_engine",
     test_uc_ctl_arm_page_size_per_engine},
    {"test_uc_ctl_arm_cpu", test_uc_ctl_arm_cpu},
#endif
#ifdef UNICORN_HAS_ARM64
    {"test_uc_ctl_change_page_size_arm64", test_uc_ctl_change_page_size_arm64},
#endif
    {"test_uc_hook_cached_uaf", test_uc_hook_cached_uaf},
    {"test_uc_emu_stop_set_ip", test_uc_emu_stop_set_ip},
    {"test_uc_set_ip_write_apis", test_uc_set_ip_write_apis},
    {"test_uc_context_restore_from_callback",
     test_uc_context_restore_from_callback},
    {"test_tlb_clear", test_tlb_clear},
    {"test_noexec", test_noexec},
    {"test_add_block_hook", test_add_block_hook},
    {NULL, NULL}};
