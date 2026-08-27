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

static void test_uc_hook_cached_cb(uc_engine *uc, uint64_t addr, size_t size,
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
