#include "unicorn_test.h"

static void test_map_correct(void)
{
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, 0x40000, 0x1000 * 16, UC_PROT_ALL)); // [0x40000, 0x50000]
    OK(uc_mem_map(uc, 0x60000, 0x1000 * 16, UC_PROT_ALL)); // [0x60000, 0x70000]
    OK(uc_mem_map(uc, 0x20000, 0x1000 * 16, UC_PROT_ALL)); // [0x20000, 0x30000]
    uc_assert_err(UC_ERR_MAP,
                  uc_mem_map(uc, 0x10000, 0x2000 * 16, UC_PROT_ALL));
    uc_assert_err(UC_ERR_MAP,
                  uc_mem_map(uc, 0x25000, 0x1000 * 16, UC_PROT_ALL));
    uc_assert_err(UC_ERR_MAP,
                  uc_mem_map(uc, 0x35000, 0x1000 * 16, UC_PROT_ALL));
    uc_assert_err(UC_ERR_MAP,
                  uc_mem_map(uc, 0x45000, 0x1000 * 16, UC_PROT_ALL));
    uc_assert_err(UC_ERR_MAP,
                  uc_mem_map(uc, 0x55000, 0x2000 * 16, UC_PROT_ALL));
    OK(uc_mem_map(uc, 0x35000, 0x5000, UC_PROT_ALL));
    OK(uc_mem_map(uc, 0x50000, 0x5000, UC_PROT_ALL));

    OK(uc_close(uc));
}

static void test_map_wrapping(void)
{
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    uc_assert_err(UC_ERR_ARG, uc_mem_map(uc, (~0ll - 0x4000) & ~0xfff, 0x8000,
                                         UC_PROT_ALL));

    OK(uc_close(uc));
}

static void test_mem_protect(void)
{
    uc_engine *qc;
    int r_eax = 0x2000;
    int r_esi = 0xdeadbeef;
    uint32_t mem;
    // add [eax + 4], esi
    char code[] = {0x01, 0x70, 0x04};

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &qc));
    OK(uc_reg_write(qc, UC_X86_REG_EAX, &r_eax));
    OK(uc_reg_write(qc, UC_X86_REG_ESI, &r_esi));
    OK(uc_mem_map(qc, 0x1000, 0x1000, UC_PROT_READ | UC_PROT_EXEC));
    OK(uc_mem_map(qc, 0x2000, 0x1000, UC_PROT_READ));
    OK(uc_mem_protect(qc, 0x2000, 0x1000, UC_PROT_READ | UC_PROT_WRITE));
    OK(uc_mem_write(qc, 0x1000, code, sizeof(code)));

    OK(uc_emu_start(qc, 0x1000, 0x1000 + sizeof(code) - 1, 0, 1));
    OK(uc_mem_read(qc, 0x2000 + 4, &mem, 4));

    TEST_CHECK(LEINT32(mem) == 0xdeadbeef);

    OK(uc_close(qc));
}

static void test_splitting_mem_unmap(void)
{
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));

    OK(uc_mem_map(uc, 0x20000, 0x1000, UC_PROT_NONE));
    OK(uc_mem_map(uc, 0x21000, 0x2000, UC_PROT_NONE));

    OK(uc_mem_unmap(uc, 0x21000, 0x1000));

    OK(uc_close(uc));
}

static uint64_t test_splitting_mmio_unmap_read_callback(uc_engine *uc,
                                                        uint64_t offset,
                                                        unsigned size,
                                                        void *user_data)
{
    TEST_CHECK(offset == 4);
    TEST_CHECK(size == 4);

    return 0x19260817;
}

static void test_splitting_mmio_unmap(void)
{
    uc_engine *uc;
    // mov ecx, [0x3004] <-- normal read
    // mov ebx, [0x4004] <-- mmio read
    char code[] = "\x8b\x0d\x04\x30\x00\x00\x8b\x1d\x04\x40\x00\x00";
    int r_ecx, r_ebx;
    int bytes = LEINT32(0xdeadbeef);

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));

    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x1000, code, sizeof(code) - 1));

    OK(uc_mmio_map(uc, 0x3000, 0x2000, test_splitting_mmio_unmap_read_callback,
                   NULL, NULL, NULL));

    // Map a ram area instead
    OK(uc_mem_unmap(uc, 0x3000, 0x1000));
    OK(uc_mem_map(uc, 0x3000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x3004, &bytes, 4));

    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_ECX, &r_ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &r_ebx));

    TEST_CHECK(r_ecx == 0xdeadbeef);
    TEST_CHECK(r_ebx == 0x19260817);

    OK(uc_close(uc));
}

static void test_mem_protect_map_ptr(void)
{
    uc_engine *uc;
    uint64_t val = 0x114514;
    uint8_t *data1 = NULL;
    uint8_t *data2 = NULL;
    uint64_t mem;

    data1 = calloc(sizeof(*data1), 0x4000);
    data2 = calloc(sizeof(*data2), 0x2000);

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    OK(uc_mem_map_ptr(uc, 0x4000, 0x4000, UC_PROT_ALL, data1));
    OK(uc_mem_unmap(uc, 0x6000, 0x2000));
    OK(uc_mem_map_ptr(uc, 0x6000, 0x2000, UC_PROT_ALL, data2));

    OK(uc_mem_write(uc, 0x6004, &val, 8));
    OK(uc_mem_protect(uc, 0x6000, 0x1000, UC_PROT_READ));
    OK(uc_mem_read(uc, 0x6004, (void *)&mem, 8));

    TEST_CHECK(val == mem);

    OK(uc_close(uc));

    free(data2);
    free(data1);
}

static void test_mem_cross_region_access(void)
{
    const uint64_t first_address = 0x1000;
    const uint64_t hole_address = 0x4000;
    uint8_t input[16];
    uint8_t output[16];
    uint8_t *first = calloc(1, 0x1000);
    uint8_t *second = calloc(1, 0x1000);
    uint8_t *before_hole = calloc(1, 0x1000);
    uc_engine *uc;
    size_t i;

    TEST_CHECK(first != NULL);
    TEST_CHECK(second != NULL);
    TEST_CHECK(before_hole != NULL);
    for (i = 0; i < sizeof(input); i++) {
        input[i] = (uint8_t)(0x40 + i);
    }

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map_ptr(uc, first_address, 0x1000, UC_PROT_ALL, first));
    OK(uc_mem_map_ptr(uc, first_address + 0x1000, 0x1000, UC_PROT_ALL, second));
    OK(uc_mem_map_ptr(uc, hole_address, 0x1000, UC_PROT_ALL, before_hole));

    OK(uc_mem_write(uc, first_address + 0xff8, input, sizeof(input)));
    TEST_CHECK(memcmp(first + 0xff8, input, 8) == 0);
    TEST_CHECK(memcmp(second, input + 8, 8) == 0);
    memset(output, 0, sizeof(output));
    OK(uc_mem_read(uc, first_address + 0xff8, output, sizeof(output)));
    TEST_CHECK(memcmp(output, input, sizeof(input)) == 0);

    memset(before_hole + 0xff8, 0x5a, 8);
    uc_assert_err(UC_ERR_WRITE_UNMAPPED,
                  uc_mem_write(uc, hole_address + 0xff8, input, sizeof(input)));
    for (i = 0; i < 8; i++) {
        TEST_CHECK(before_hole[0xff8 + i] == 0x5a);
    }
    memset(output, 0xa5, sizeof(output));
    uc_assert_err(UC_ERR_READ_UNMAPPED, uc_mem_read(uc, hole_address + 0xff8,
                                                    output, sizeof(output)));
    for (i = 0; i < sizeof(output); i++) {
        TEST_CHECK(output[i] == 0xa5);
    }

    OK(uc_close(uc));
    free(before_hole);
    free(second);
    free(first);
}

static void test_mem_regions_topology(void)
{
    const uint64_t address = 0x8000;
    uint8_t *memory = calloc(1, 0x3000);
    uc_mem_region *regions;
    uint32_t count;
    uc_engine *uc;

    TEST_CHECK(memory != NULL);
    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map_ptr(uc, address, 0x3000, UC_PROT_ALL, memory));
    OK(uc_mem_protect(uc, address + 0x1000, 0x1000, UC_PROT_READ));

    OK(uc_mem_regions(uc, &regions, &count));
    TEST_CHECK(count == 3);
    TEST_CHECK(regions[0].begin == address);
    TEST_CHECK(regions[0].end == address + 0xfff);
    TEST_CHECK(regions[0].perms == UC_PROT_ALL);
    TEST_CHECK(regions[1].begin == address + 0x1000);
    TEST_CHECK(regions[1].end == address + 0x1fff);
    TEST_CHECK(regions[1].perms == UC_PROT_READ);
    TEST_CHECK(regions[2].begin == address + 0x2000);
    TEST_CHECK(regions[2].end == address + 0x2fff);
    TEST_CHECK(regions[2].perms == UC_PROT_ALL);
    OK(uc_free(regions));

    OK(uc_mem_unmap(uc, address + 0x1000, 0x1000));
    OK(uc_mem_regions(uc, &regions, &count));
    TEST_CHECK(count == 2);
    TEST_CHECK(regions[0].begin == address);
    TEST_CHECK(regions[0].end == address + 0xfff);
    TEST_CHECK(regions[1].begin == address + 0x2000);
    TEST_CHECK(regions[1].end == address + 0x2fff);
    OK(uc_free(regions));

    OK(uc_close(uc));
    free(memory);
}

static void test_map_at_the_end(void)
{
    const uint64_t address = UINT64_MAX - 0xfff;
    uc_engine *uc;
    uint8_t mem[0x1000];
    uint8_t actual[sizeof(mem)];
    uc_mem_region *regions;
    uint32_t count;

    memset(mem, 0xff, sizeof(mem));

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    uc_assert_err(UC_ERR_MAP,
                  uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    uc_assert_err(UC_ERR_MAP,
                  uc_mem_map(uc, address - 0x1000, 0x2000, UC_PROT_ALL));

    OK(uc_mem_regions(uc, &regions, &count));
    TEST_CHECK(count == 1);
    TEST_CHECK(regions[0].begin == address);
    TEST_CHECK(regions[0].end == UINT64_MAX);
    TEST_CHECK(regions[0].perms == UC_PROT_ALL);
    OK(uc_free(regions));

    OK(uc_mem_write(uc, address, mem, sizeof(mem)));
    OK(uc_mem_read(uc, address, actual, sizeof(actual)));
    TEST_CHECK(memcmp(actual, mem, sizeof(actual)) == 0);

    uc_assert_err(UC_ERR_WRITE_UNMAPPED,
                  uc_mem_write(uc, 0xffffffffffffff00, mem, sizeof(mem)));
    uc_assert_err(UC_ERR_WRITE_UNMAPPED, uc_mem_write(uc, 0, mem, sizeof(mem)));

    OK(uc_mem_unmap(uc, address, 0x1000));
    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_mem_read(uc, address, actual, sizeof(actual)));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));

    OK(uc_close(uc));
}

static void test_terminal_two_page_partial_change(bool protect,
                                                  bool last_page)
{
    const uint64_t address = UINT64_MAX - 0x1fff;
    const uint64_t target = address + (last_page ? 0x1000 : 0);
    const uint8_t first_value = 0x41;
    const uint8_t last_value = 0x52;
    const uint32_t expected_count = protect ? 2 : 1;
    uint8_t actual = 0;
    uc_mem_region *regions;
    uint32_t count;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, address, 0x2000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address, &first_value, sizeof(first_value)));
    OK(uc_mem_write(uc, address + 0x1000, &last_value,
                    sizeof(last_value)));

    if (protect) {
        OK(uc_mem_protect(uc, target, 0x1000, UC_PROT_READ));
    } else {
        OK(uc_mem_unmap(uc, target, 0x1000));
    }

    OK(uc_mem_regions(uc, &regions, &count));
    if (TEST_CHECK(count == expected_count)) {
        if (protect) {
            TEST_CHECK(regions[0].begin == address);
            TEST_CHECK(regions[0].end == address + 0xfff);
            TEST_CHECK(regions[0].perms ==
                       (last_page ? UC_PROT_ALL : UC_PROT_READ));
            TEST_CHECK(regions[1].begin == address + 0x1000);
            TEST_CHECK(regions[1].end == UINT64_MAX);
            TEST_CHECK(regions[1].perms ==
                       (last_page ? UC_PROT_READ : UC_PROT_ALL));
        } else if (last_page) {
            TEST_CHECK(regions[0].begin == address);
            TEST_CHECK(regions[0].end == address + 0xfff);
            TEST_CHECK(regions[0].perms == UC_PROT_ALL);
        } else {
            TEST_CHECK(regions[0].begin == address + 0x1000);
            TEST_CHECK(regions[0].end == UINT64_MAX);
            TEST_CHECK(regions[0].perms == UC_PROT_ALL);
        }
    }
    OK(uc_free(regions));

    if (protect || last_page) {
        OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
        TEST_CHECK(actual == first_value);
    } else {
        uc_assert_err(UC_ERR_READ_UNMAPPED,
                      uc_mem_read(uc, address, &actual, sizeof(actual)));
    }
    if (protect || !last_page) {
        OK(uc_mem_read(uc, address + 0x1000, &actual, sizeof(actual)));
        TEST_CHECK(actual == last_value);
    } else {
        uc_assert_err(
            UC_ERR_READ_UNMAPPED,
            uc_mem_read(uc, address + 0x1000, &actual, sizeof(actual)));
    }

    OK(uc_close(uc));
}

static void test_terminal_two_page_unmap_first(void)
{
    test_terminal_two_page_partial_change(false, false);
}

static void test_terminal_two_page_unmap_last(void)
{
    test_terminal_two_page_partial_change(false, true);
}

static void test_terminal_two_page_protect_first(void)
{
    test_terminal_two_page_partial_change(true, false);
}

static void test_terminal_two_page_protect_last(void)
{
    test_terminal_two_page_partial_change(true, true);
}

typedef struct TestTerminalPageUnmapData {
    uint64_t address;
    uint32_t count;
} TestTerminalPageUnmapData;

static void test_terminal_page_unmap_callback(uc_engine *uc, uint64_t address,
                                              uint32_t size, void *user_data)
{
    TestTerminalPageUnmapData *data =
        (TestTerminalPageUnmapData *)user_data;

    (void)address;
    (void)size;
    data->count++;
    OK(uc_mem_unmap(uc, data->address, 0x1000));
}

static void test_terminal_page_active_unmap(void)
{
    const uint64_t address = UINT64_MAX - 0xfff;
    const uint64_t code_address = 0x1000;
    const uint8_t code[] = {
        0xff, 0xc0, /* inc eax */
        0xff, 0xc0, /* inc eax */
    };
    TestTerminalPageUnmapData data = {.address = address};
    uint8_t actual = 0;
    uint32_t eax = 0;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, code_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_address, code, sizeof(code)));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE,
                   test_terminal_page_unmap_callback, &data, code_address,
                   code_address));

    OK(uc_emu_start(uc, code_address, code_address + sizeof(code), 0, 2));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(data.count == 1);
    TEST_CHECK(eax == 2);
    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_mem_read(uc, address, &actual, sizeof(actual)));

    OK(uc_hook_del(uc, hook));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));

    OK(uc_close(uc));
}

static void test_map_wrap(void)
{
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    uc_assert_err(UC_ERR_ARG,
                  uc_mem_map(uc, 0xfffffffffffff000, 0x2000, UC_PROT_ALL));

    OK(uc_close(uc));
}

static void test_map_big_memory(void)
{
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

#if defined(_WIN32) || defined(__WIN32__) || defined(__WINDOWS__)
    uint64_t requested_size = 0xfffffffffffff000; // assume 4K page size
#else
    long ps = sysconf(_SC_PAGESIZE);
    uint64_t requested_size = (uint64_t)(-ps);
#endif

    uc_assert_err(UC_ERR_NOMEM,
                  uc_mem_map(uc, 0x0, requested_size, UC_PROT_ALL));

    OK(uc_close(uc));
}

static void test_mem_protect_remove_exec_callback(uc_engine *uc, uint64_t addr,
                                                  uint32_t size, void *data)
{
    uint64_t *p = (uint64_t *)data;
    (*p)++;

    OK(uc_mem_protect(uc, 0x2000, 0x1000, UC_PROT_READ));
}

static void test_mem_protect_remove_exec(void)
{
    uc_engine *uc;
    char code[] = "\x90\xeb\x00\x90";
    uc_hook hk;
    uint64_t called_count = 0;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_map(uc, 0x2000, 0x1000, UC_PROT_ALL));

    OK(uc_mem_write(uc, 0x1000, code, sizeof(code) - 1));
    OK(uc_hook_add(uc, &hk, UC_HOOK_BLOCK,
                   test_mem_protect_remove_exec_callback, (void *)&called_count,
                   1, 0));

    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(code) - 1, 0, 0));

    TEST_CHECK(called_count == 2);

    OK(uc_close(uc));
}

typedef enum TestActiveCodePageAction {
    TEST_ACTIVE_CODE_PROTECT,
    TEST_ACTIVE_CODE_UNMAP,
} TestActiveCodePageAction;

typedef struct TestActiveCodePageData {
    TestActiveCodePageAction action;
    uint32_t count;
} TestActiveCodePageData;

static void test_active_code_page_callback(uc_engine *uc, uint64_t address,
                                           uint32_t size, void *user_data)
{
    TestActiveCodePageData *data = (TestActiveCodePageData *)user_data;

    data->count++;
    if (data->action == TEST_ACTIVE_CODE_PROTECT) {
        OK(uc_mem_protect(uc, 0x1000, 0x1000, UC_PROT_READ | UC_PROT_WRITE));
    } else {
        OK(uc_mem_unmap(uc, 0x1000, 0x1000));
    }
}

static void test_active_code_page_change_one(TestActiveCodePageAction action)
{
    const char code[] = "\x40\x40";
    TestActiveCodePageData data = {.action = action};
    uint32_t eax = 0;
    uint32_t eip;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x1000, code, sizeof(code) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE, test_active_code_page_callback,
                   &data, 0x1000, 0x1000));

    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    TEST_CHECK(data.count == 1);
    TEST_CHECK(eax == 0);
    TEST_CHECK(eip == 0x1000);

    if (action == TEST_ACTIVE_CODE_PROTECT) {
        uc_assert_err(
            UC_ERR_FETCH_PROT,
            uc_emu_start(uc, 0x1000, 0x1000 + sizeof(code) - 1, 0, 0));
    } else {
        uc_assert_err(
            UC_ERR_FETCH_UNMAPPED,
            uc_emu_start(uc, 0x1000, 0x1000 + sizeof(code) - 1, 0, 0));
    }

    OK(uc_close(uc));
}

static void test_active_code_page_change(void)
{
    test_active_code_page_change_one(TEST_ACTIVE_CODE_PROTECT);
    test_active_code_page_change_one(TEST_ACTIVE_CODE_UNMAP);
}

static void test_inactive_code_page_change_one(TestActiveCodePageAction action)
{
    const char code[] = "\x40";
    uint32_t eax = 0;
    uint32_t eip = 0x1000;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, 0x1000, 0x2000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x2000, code, sizeof(code) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_EIP, &eip));
    if (action == TEST_ACTIVE_CODE_PROTECT) {
        OK(uc_mem_protect(uc, 0x1000, 0x1000, UC_PROT_READ | UC_PROT_WRITE));
    } else {
        OK(uc_mem_unmap(uc, 0x1000, 0x1000));
    }

    OK(uc_emu_start(uc, 0x2000, 0x2000 + sizeof(code) - 1, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 1);

    OK(uc_close(uc));
}

static void test_inactive_code_page_change(void)
{
    test_inactive_code_page_change_one(TEST_ACTIVE_CODE_PROTECT);
    test_inactive_code_page_change_one(TEST_ACTIVE_CODE_UNMAP);
}

typedef struct TestMmioExecWriteData {
    uint64_t ram_address;
    uint32_t read_count;
    uint32_t code_count;
} TestMmioExecWriteData;

static uint64_t test_mmio_exec_read_callback(uc_engine *uc, uint64_t offset,
                                             unsigned int size, void *user_data)
{
    TestMmioExecWriteData *data = (TestMmioExecWriteData *)user_data;
    uint64_t value = 0;
    unsigned int i;

    data->read_count++;
    for (i = 0; i < size; i++) {
        if (offset + i == 1) {
            value |= (uint64_t)0x40 << (i * 8); /* inc eax */
        }
    }
    return value;
}

static void test_mmio_exec_write_callback(uc_engine *uc, uint64_t address,
                                          uint32_t size, void *user_data)
{
    TestMmioExecWriteData *data = (TestMmioExecWriteData *)user_data;
    const uint8_t value = 0x5a;

    TEST_CHECK(address == 0x2001);
    TEST_CHECK(size == 1);
    data->code_count++;
    OK(uc_mem_write(uc, data->ram_address, &value, sizeof(value)));
}

static void test_mmio_exec_write_unrelated_ram(void)
{
    TestMmioExecWriteData data = {.ram_address = 0x1000};
    uint32_t eax = 0;
    uint8_t value = 0;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, data.ram_address, 0x1000, UC_PROT_ALL));
    OK(uc_mmio_map(uc, 0x2000, 0x1000, test_mmio_exec_read_callback, &data,
                   NULL, NULL));
    OK(uc_mem_protect(uc, 0x2000, 0x1000, UC_PROT_ALL));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE, test_mmio_exec_write_callback,
                   &data, 0x2001, 0x2001));

    OK(uc_emu_start(uc, 0x2001, 0x2002, 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_mem_read(uc, data.ram_address, &value, sizeof(value)));
    TEST_CHECK(data.read_count != 0);
    TEST_CHECK(data.code_count == 1);
    TEST_CHECK(eax == 1);
    TEST_CHECK(value == 0x5a);

    OK(uc_close(uc));
}

static uint64_t test_mem_protect_mmio_read_cb(struct uc_struct *uc,
                                              uint64_t addr, unsigned size,
                                              void *user_data)
{
    TEST_CHECK(addr == 0x20); // note, it's not 0x1020

    *(uint64_t *)user_data += 1;
    return 0x114514;
}

static void test_mem_protect_mmio_write_cb(struct uc_struct *uc, uint64_t addr,
                                           unsigned size, uint64_t data,
                                           void *user_data)
{
    TEST_CHECK(false);
    return;
}

static void test_mem_protect_mmio(void)
{
    uc_engine *uc;
    // mov eax, [0x2020]; mov [0x2020], eax
    char code[] = "\xa1\x20\x20\x00\x00\x00\x00\x00\x00\xa3\x20\x20\x00\x00\x00"
                  "\x00\x00\x00";
    uint64_t called = 0;
    uint64_t r_eax;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, 0x8000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x8000, code, sizeof(code) - 1));

    OK(uc_mmio_map(uc, 0x1000, 0x3000, test_mem_protect_mmio_read_cb,
                   (void *)&called, test_mem_protect_mmio_write_cb,
                   (void *)&called));
    OK(uc_mem_protect(uc, 0x2000, 0x1000, UC_PROT_READ));

    uc_assert_err(UC_ERR_WRITE_PROT,
                  uc_emu_start(uc, 0x8000, 0x8000 + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &r_eax));

    TEST_CHECK(called == 1);
    TEST_CHECK(r_eax == 0x114514);

    OK(uc_close(uc));
}

static void test_snapshot(void)
{
    uc_engine *uc;
    uc_context *c0, *c1;
    uint32_t mem;
    uint8_t code_data;
    // mov eax, [0x2020]; inc eax; mov [0x2020], eax
    char code[] = "\xa1\x20\x20\x00\x00\x00\x00\x00\x00\xff\xc0\xa3\x20\x20\x00"
                  "\x00\x00\x00\x00\x00";

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_context_alloc(uc, &c0));
    OK(uc_context_alloc(uc, &c1));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x1000, code, sizeof(code) - 1));

    OK(uc_mem_map(uc, 0x2000, 0x1000, UC_PROT_ALL));
    OK(uc_context_save(uc, c0));

    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(code) - 1, 0, 0));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 1);
    OK(uc_context_save(uc, c1));
    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(code) - 1, 0, 0));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 2);
    OK(uc_context_restore(uc, c1));

    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 1);
    OK(uc_context_restore(uc, c0));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 0);

    OK(uc_mem_read(uc, 0x1000, &code_data, sizeof(code_data)));
    TEST_CHECK(code_data == 0xa1);

    OK(uc_context_free(c0));
    OK(uc_context_free(c1));
    OK(uc_close(uc));
}

typedef struct TestSnapshotCodeRestoreData {
    uc_context *context;
    uint32_t count;
    bool restored;
} TestSnapshotCodeRestoreData;

static void test_snapshot_code_restore_callback(uc_engine *uc, uint64_t address,
                                                uint32_t size, void *user_data)
{
    TestSnapshotCodeRestoreData *data =
        (TestSnapshotCodeRestoreData *)user_data;

    data->count++;
    if (!data->restored) {
        data->restored = true;
        OK(uc_context_restore(uc, data->context));
    }
}

static void test_snapshot_code_restore_from_callback(void)
{
    const uint8_t snapshot_code[] = {0x90, 0x43};
    const uint8_t live_code[] = {0x90, 0x40};
    uint8_t restored_code[sizeof(snapshot_code)];
    TestSnapshotCodeRestoreData data = {0};
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x1000, snapshot_code, sizeof(snapshot_code)));
    OK(uc_context_alloc(uc, &data.context));
    OK(uc_context_save(uc, data.context));
    OK(uc_mem_write(uc, 0x1000, live_code, sizeof(live_code)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE, test_snapshot_code_restore_callback,
                   &data, 0x1000, 0x1000));

    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(live_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_mem_read(uc, 0x1000, restored_code, sizeof(restored_code)));
    TEST_CHECK(data.count == 2);
    TEST_CHECK(eax == 0);
    TEST_CHECK(ebx == 1);
    TEST_CHECK(memcmp(restored_code, snapshot_code, sizeof(snapshot_code)) ==
               0);

    OK(uc_context_free(data.context));
    OK(uc_close(uc));
}

typedef struct TestSnapshotCowNestedPatchData {
    uint64_t nested_address;
    uint64_t cow_address;
    uint64_t patch_address;
    uint32_t outer_count;
    uint32_t inner_count;
    bool nested_started;
    bool patched;
} TestSnapshotCowNestedPatchData;

static void test_snapshot_cow_nested_patch_inner(uc_engine *uc,
                                                 uint64_t address,
                                                 uint32_t size, void *user_data)
{
    TestSnapshotCowNestedPatchData *data =
        (TestSnapshotCowNestedPatchData *)user_data;
    const uint8_t disjoint_value = 0x5a;
    const uint8_t inc_ecx = 0x41;

    data->inner_count++;
    if (!data->patched) {
        data->patched = true;
        OK(uc_mem_write(uc, data->cow_address, &disjoint_value,
                        sizeof(disjoint_value)));
        OK(uc_mem_write(uc, data->patch_address, &inc_ecx, sizeof(inc_ecx)));
    }
}

static void test_snapshot_cow_nested_patch_outer(uc_engine *uc,
                                                 uint64_t address,
                                                 uint32_t size, void *user_data)
{
    TestSnapshotCowNestedPatchData *data =
        (TestSnapshotCowNestedPatchData *)user_data;

    data->outer_count++;
    if (!data->nested_started) {
        data->nested_started = true;
        OK(uc_emu_start(uc, data->nested_address, data->nested_address + 1, 0,
                        0));
    }
}

static void test_snapshot_cow_disjoint_nested_patch(void)
{
    const uint64_t outer_address = 0x1000;
    const uint64_t nested_address = 0x3000;
    const uint8_t outer_code[] = {
        0x40, /* inc eax */
        0x40, /* inc eax; patched to inc ecx after COW */
    };
    const uint8_t nested_code[] = {0x42}; /* inc edx */
    TestSnapshotCowNestedPatchData data = {
        .nested_address = nested_address,
        .cow_address = outer_address + 0x100,
        .patch_address = outer_address + 1,
    };
    uint32_t eax = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    uc_context *context;
    uc_engine *uc;
    uc_hook inner_hook;
    uc_hook outer_hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, outer_address, 0x3000, UC_PROT_ALL));
    OK(uc_mem_write(uc, outer_address, outer_code, sizeof(outer_code)));
    OK(uc_mem_write(uc, nested_address, nested_code, sizeof(nested_code)));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));
    OK(uc_hook_add(uc, &outer_hook, UC_HOOK_CODE,
                   test_snapshot_cow_nested_patch_outer, &data, outer_address,
                   outer_address));
    OK(uc_hook_add(uc, &inner_hook, UC_HOOK_CODE,
                   test_snapshot_cow_nested_patch_inner, &data, nested_address,
                   nested_address));

    OK(uc_emu_start(uc, outer_address, outer_address + sizeof(outer_code), 0,
                    0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_reg_read(uc, UC_X86_REG_EDX, &edx));
    TEST_CHECK_(data.outer_count == 2, "outer_count=%u", data.outer_count);
    TEST_CHECK_(data.inner_count == 1, "inner_count=%u", data.inner_count);
    TEST_CHECK_(eax == 1, "eax=0x%x", eax);
    TEST_CHECK_(ecx == 1, "ecx=0x%x", ecx);
    TEST_CHECK_(edx == 1, "edx=0x%x", edx);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

typedef struct TestSnapshotNestedRestoreData {
    uc_context *context;
    uint64_t nested_address;
    uint32_t outer_count;
    bool nested_started;
    bool restored;
} TestSnapshotNestedRestoreData;

static void test_snapshot_nested_restore_inner(uc_engine *uc, uint64_t address,
                                               uint32_t size, void *user_data)
{
    TestSnapshotNestedRestoreData *data =
        (TestSnapshotNestedRestoreData *)user_data;

    if (!data->restored) {
        data->restored = true;
        OK(uc_context_restore(uc, data->context));
    }
}

static void test_snapshot_nested_restore_outer(uc_engine *uc, uint64_t address,
                                               uint32_t size, void *user_data)
{
    TestSnapshotNestedRestoreData *data =
        (TestSnapshotNestedRestoreData *)user_data;

    data->outer_count++;
    if (!data->nested_started) {
        data->nested_started = true;
        OK(uc_emu_start(uc, data->nested_address, data->nested_address + 1, 0,
                        0));
    }
}

static void test_snapshot_nested_restore_outer_instruction(void)
{
    const uint64_t outer_address = 0x1000;
    const uint64_t nested_address = 0x3000;
    const uint8_t snapshot_code[] = {
        0x40, /* inc eax */
        0x41, /* inc ecx */
    };
    const uint8_t live_code[] = {
        0x40, /* inc eax */
        0x40, /* inc eax; restored to inc ecx */
    };
    const uint8_t nested_code[] = {0x90}; /* nop */
    TestSnapshotNestedRestoreData data = {
        .nested_address = nested_address,
    };
    uint8_t restored_code[sizeof(snapshot_code)];
    uint32_t eax = 0;
    uint32_t ecx = 0;
    uc_engine *uc;
    uc_hook inner_hook;
    uc_hook outer_hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, outer_address, 0x3000, UC_PROT_ALL));
    OK(uc_mem_write(uc, outer_address, snapshot_code, sizeof(snapshot_code)));
    OK(uc_mem_write(uc, nested_address, nested_code, sizeof(nested_code)));
    OK(uc_context_alloc(uc, &data.context));
    OK(uc_context_save(uc, data.context));
    OK(uc_mem_write(uc, outer_address, live_code, sizeof(live_code)));
    OK(uc_hook_add(uc, &outer_hook, UC_HOOK_CODE,
                   test_snapshot_nested_restore_outer, &data, outer_address,
                   outer_address));
    OK(uc_hook_add(uc, &inner_hook, UC_HOOK_CODE,
                   test_snapshot_nested_restore_inner, &data, nested_address,
                   nested_address));

    OK(uc_emu_start(uc, outer_address, outer_address + sizeof(live_code), 0,
                    0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_ECX, &ecx));
    OK(uc_mem_read(uc, outer_address, restored_code, sizeof(restored_code)));
    TEST_CHECK_(data.outer_count == 2, "outer_count=%u", data.outer_count);
    TEST_CHECK_(eax == 1, "eax=0x%x", eax);
    TEST_CHECK_(ecx == 1, "ecx=0x%x", ecx);
    TEST_CHECK(memcmp(restored_code, snapshot_code, sizeof(snapshot_code)) ==
               0);

    OK(uc_context_free(data.context));
    OK(uc_close(uc));
}

static bool test_snapshot_with_vtlb_callback(uc_engine *uc, uint64_t addr,
                                             uc_mem_type type,
                                             uc_tlb_entry *result,
                                             void *user_data)
{
    result->paddr = addr - 0x400000000;
    result->perms = UC_PROT_ALL;
    return true;
}

static void test_snapshot_with_vtlb(void)
{
    uc_engine *uc;
    uc_context *c0, *c1;
    uint32_t mem;
    uint8_t code_data;
    uc_hook hook;

    // mov eax, [0x2020]; inc eax; mov [0x2020], eax
    char code[] = "\xA1\x20\x20\x00\x00\x04\x00\x00\x00\xFF\xC0\xA3\x20\x20\x00"
                  "\x00\x04\x00\x00\x00";

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    // Allocate contexts
    OK(uc_context_alloc(uc, &c0));
    OK(uc_context_alloc(uc, &c1));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));

    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL,
                   test_snapshot_with_vtlb_callback, NULL, 1, 0));

    // Map physical memory
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_EXEC | UC_PROT_READ));
    OK(uc_mem_write(uc, 0x1000, code, sizeof(code) - 1));
    OK(uc_mem_map(uc, 0x2000, 0x1000, UC_PROT_ALL));

    // Initial context save
    OK(uc_context_save(uc, c0));

    OK(uc_emu_start(uc, 0x400000000 + 0x1000,
                    0x400000000 + 0x1000 + sizeof(code) - 1, 0, 0));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 1);
    OK(uc_context_save(uc, c1));
    OK(uc_emu_start(uc, 0x400000000 + 0x1000,
                    0x400000000 + 0x1000 + sizeof(code) - 1, 0, 0));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 2);
    mem = LEINT32(0xdeadbeef);
    OK(uc_mem_write(uc, 0x2020, &mem, sizeof(mem)));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 0xdeadbeef);
    OK(uc_context_restore(uc, c1));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 1);
    OK(uc_mem_read(uc, 0x1000, &code_data, sizeof(code_data)));
    TEST_CHECK(code_data == 0xa1);

    mem = LEINT32(0xcafebabe);
    OK(uc_mem_write(uc, 0x2020, &mem, sizeof(mem)));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 0xcafebabe);
    OK(uc_context_restore(uc, c0));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 0);
    OK(uc_mem_read(uc, 0x1000, &code_data, sizeof(code_data)));
    TEST_CHECK(code_data == 0xa1);

    OK(uc_context_free(c0));
    OK(uc_context_free(c1));
    OK(uc_close(uc));
}

static void test_context_snapshot(void)
{
    uc_engine *uc;
    uc_context *ctx;
    uint64_t baseaddr = 0xfffff1000;
    uint64_t offset = 0x10;
    uint64_t tmp = 1;
    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY | UC_CTL_CONTEXT_CPU));
    OK(uc_mem_map(uc, baseaddr, 0x1000, UC_PROT_ALL));
    OK(uc_context_alloc(uc, &ctx));
    OK(uc_context_save(uc, ctx));

    OK(uc_mem_write(uc, baseaddr + offset, &tmp, sizeof(tmp)));
    OK(uc_mem_read(uc, baseaddr + offset, &tmp, sizeof(tmp)));
    TEST_CHECK(tmp == 1);
    OK(uc_context_restore(uc, ctx));
    OK(uc_mem_read(uc, baseaddr + offset, &tmp, sizeof(tmp)));
    TEST_CHECK(tmp == 0);

    tmp = 2;
    OK(uc_mem_write(uc, baseaddr + offset, &tmp, sizeof(tmp)));
    OK(uc_mem_read(uc, baseaddr + offset, &tmp, sizeof(tmp)));
    TEST_CHECK(tmp == 2);
    OK(uc_context_restore(uc, ctx));
    OK(uc_mem_read(uc, baseaddr + offset, &tmp, sizeof(tmp)));
    TEST_CHECK(tmp == 0);

    OK(uc_context_free(ctx));
    OK(uc_close(uc));
}

static void test_context_cpu_resave_preserves_memory_snapshot(void)
{
    const uint64_t address = 0x12000;
    const uint8_t original = 0x11;
    const uint8_t saved = 0x22;
    const uint8_t changed = 0x33;
    uint8_t actual = 0;
    uc_context *base_context;
    uc_context *saved_context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address, &original, sizeof(original)));
    OK(uc_context_alloc(uc, &base_context));
    OK(uc_context_alloc(uc, &saved_context));
    OK(uc_context_save(uc, base_context));

    OK(uc_mem_write(uc, address, &saved, sizeof(saved)));
    OK(uc_context_save(uc, saved_context));
    OK(uc_context_restore(uc, base_context));

    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_CPU));
    OK(uc_context_save(uc, saved_context));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_context_restore(uc, saved_context));
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == saved);

    OK(uc_mem_write(uc, address, &changed, sizeof(changed)));
    OK(uc_context_restore(uc, base_context));
    OK(uc_context_restore(uc, saved_context));
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == saved);

    OK(uc_context_free(base_context));
    OK(uc_context_free(saved_context));
    OK(uc_close(uc));
}

static void test_snapshot_unmap(void)
{
    uc_engine *uc;
    uc_context *ctx;
    uint64_t tmp;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY | UC_CTL_CONTEXT_CPU));
    OK(uc_mem_map(uc, 0x1000, 0x2000, UC_PROT_ALL));

    tmp = 1;
    OK(uc_mem_write(uc, 0x1000, &tmp, sizeof(tmp)));
    tmp = 2;
    OK(uc_mem_write(uc, 0x2000, &tmp, sizeof(tmp)));

    OK(uc_context_alloc(uc, &ctx));
    OK(uc_context_save(uc, ctx));

    uc_assert_err(UC_ERR_ARG, uc_mem_unmap(uc, 0x1000, 0x1000));
    OK(uc_mem_unmap(uc, 0x1000, 0x2000));
    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_mem_read(uc, 0x1000, &tmp, sizeof(tmp)));
    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_mem_read(uc, 0x2000, &tmp, sizeof(tmp)));

    OK(uc_context_restore(uc, ctx));
    OK(uc_mem_read(uc, 0x1000, &tmp, sizeof(tmp)));
    TEST_CHECK(tmp == 1);
    OK(uc_mem_read(uc, 0x2000, &tmp, sizeof(tmp)));
    TEST_CHECK(tmp == 2);

    OK(uc_context_free(ctx));
    OK(uc_close(uc));
}

static void test_snapshot_check_regions(uc_engine *uc,
                                        const uc_mem_region *expected,
                                        uint32_t expected_count)
{
    uc_mem_region *regions;
    uint32_t count;
    uint32_t i;

    OK(uc_mem_regions(uc, &regions, &count));
    TEST_CHECK(count == expected_count);
    for (i = 0; i < count && i < expected_count; i++) {
        TEST_CHECK(regions[i].begin == expected[i].begin);
        TEST_CHECK(regions[i].end == expected[i].end);
        TEST_CHECK(regions[i].perms == expected[i].perms);
    }
    OK(uc_free(regions));
}

static void test_terminal_page_snapshot_restore(void)
{
    const uint64_t address = UINT64_MAX - 0xfff;
    const uint8_t original = 0x41;
    const uint8_t changed = 0x52;
    const uint8_t replacement = 0x63;
    const uc_mem_region expected_region = {
        address,
        UINT64_MAX,
        UC_PROT_ALL,
    };
    uint8_t actual = 0;
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address, &original, sizeof(original)));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_mem_write(uc, address, &changed, sizeof(changed)));
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == changed);
    OK(uc_mem_unmap(uc, address, 0x1000));
    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_mem_read(uc, address, &actual, sizeof(actual)));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address, &replacement, sizeof(replacement)));

    OK(uc_context_restore(uc, context));
    test_snapshot_check_regions(uc, &expected_region, 1);
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == original);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_snapshot_restore_skips_later_unmapped_regions(void)
{
    const uint64_t address = 0x30000;
    const uint8_t original = 0x11;
    const uint8_t replacement = 0x22;
    const uc_mem_region original_region = {
        address,
        address + 0xfff,
        UC_PROT_READ | UC_PROT_WRITE,
    };
    const uc_mem_region current_region = {
        address,
        address + 0xfff,
        UC_PROT_READ,
    };
    uint8_t actual = 0;
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x1000, original_region.perms));
    OK(uc_mem_write(uc, address, &original, sizeof(original)));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_mem_unmap(uc, address, 0x1000));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address, &replacement, sizeof(replacement)));
    OK(uc_mem_unmap(uc, address, 0x1000));
    OK(uc_mem_map(uc, address, 0x1000, current_region.perms));
    test_snapshot_check_regions(uc, &current_region, 1);

    OK(uc_context_restore(uc, context));
    test_snapshot_check_regions(uc, &original_region, 1);
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == original);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_snapshot_empty_flatview(void)
{
    const uint64_t address = 0x40000;
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_context_restore(uc, context));
    test_snapshot_check_regions(uc, NULL, 0);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_snapshot_replace_with_ram(void)
{
    const uint64_t address = 0x10000;
    const uint8_t original_first[] = {0x10, 0x11, 0x12, 0x13};
    const uint8_t original_second[] = {0x20, 0x21, 0x22, 0x23};
    const uint8_t replacement[] = {0xa0, 0xa1, 0xa2, 0xa3};
    const uc_mem_region original_regions[] = {
        {address, address + 0xfff, UC_PROT_READ | UC_PROT_WRITE},
        {address + 0x1000, address + 0x1fff, UC_PROT_READ | UC_PROT_EXEC},
    };
    const uc_mem_region replacement_regions[] = {
        {address, address + 0x1fff, UC_PROT_ALL},
    };
    uint8_t actual[sizeof(original_first)];
    uc_context *older_context;
    uc_context *newer_context;
    uc_engine *uc;
    uc_err err;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_READ | UC_PROT_WRITE));
    OK(uc_mem_map(uc, address + 0x1000, 0x1000, UC_PROT_READ | UC_PROT_EXEC));
    OK(uc_mem_write(uc, address + 0x20, original_first,
                    sizeof(original_first)));
    OK(uc_mem_write(uc, address + 0x1020, original_second,
                    sizeof(original_second)));
    test_snapshot_check_regions(uc, original_regions,
                                sizeof(original_regions) /
                                    sizeof(original_regions[0]));

    OK(uc_context_alloc(uc, &older_context));
    OK(uc_context_alloc(uc, &newer_context));
    OK(uc_context_save(uc, older_context));
    OK(uc_mem_unmap(uc, address, 0x1000));
    OK(uc_mem_unmap(uc, address + 0x1000, 0x1000));
    OK(uc_mem_map(uc, address, 0x2000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address + 0x20, replacement, sizeof(replacement)));
    OK(uc_mem_write(uc, address + 0x1020, replacement, sizeof(replacement)));

    test_snapshot_check_regions(uc, replacement_regions,
                                sizeof(replacement_regions) /
                                    sizeof(replacement_regions[0]));
    OK(uc_context_save(uc, newer_context));

    err = uc_context_restore(uc, older_context);
    if (!TEST_CHECK(err == UC_ERR_OK)) {
        TEST_MSG("%s", uc_strerror(err));
        goto cleanup;
    }
    test_snapshot_check_regions(uc, original_regions,
                                sizeof(original_regions) /
                                    sizeof(original_regions[0]));
    OK(uc_mem_read(uc, address + 0x20, actual, sizeof(actual)));
    TEST_CHECK(memcmp(actual, original_first, sizeof(actual)) == 0);
    OK(uc_mem_read(uc, address + 0x1020, actual, sizeof(actual)));
    TEST_CHECK(memcmp(actual, original_second, sizeof(actual)) == 0);

cleanup:
    OK(uc_context_free(older_context));
    OK(uc_context_free(newer_context));
    OK(uc_close(uc));
}

static void test_snapshot_cow_coordinates_and_remap(void)
{
    const uint64_t address = 0x50000;
    const uint64_t size = 0x3000;
    const uint64_t value_address = address + 0x1020;
    const uint8_t original = 0x11;
    const uint8_t changed = 0x22;
    const uint8_t replacement = 0x33;
    const uc_mem_region expected_region = {
        address,
        address + size - 1,
        UC_PROT_ALL,
    };
    uint8_t actual = 0;
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, size, UC_PROT_ALL));
    OK(uc_mem_write(uc, value_address, &original, sizeof(original)));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_mem_write(uc, value_address, &changed, sizeof(changed)));
    test_snapshot_check_regions(uc, &expected_region, 1);
    OK(uc_mem_unmap(uc, address, size));
    OK(uc_mem_map(uc, address, size, UC_PROT_ALL));
    OK(uc_mem_write(uc, value_address, &replacement, sizeof(replacement)));
    OK(uc_mem_read(uc, value_address, &actual, sizeof(actual)));
    TEST_CHECK(actual == replacement);

    OK(uc_context_restore(uc, context));
    test_snapshot_check_regions(uc, &expected_region, 1);
    OK(uc_mem_read(uc, value_address, &actual, sizeof(actual)));
    TEST_CHECK(actual == original);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_snapshot_cow_replacement_restore(void)
{
    const uint64_t address = 0x54000;
    const uint64_t value_address = address + 0x20;
    const uint8_t original = 0x41;
    const uint8_t cow_value = 0x52;
    const uint8_t replacement = 0x63;
    uint8_t actual = 0;
    uc_context *original_context;
    uc_context *cow_context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, value_address, &original, sizeof(original)));
    OK(uc_context_alloc(uc, &original_context));
    OK(uc_context_alloc(uc, &cow_context));
    OK(uc_context_save(uc, original_context));

    OK(uc_mem_write(uc, value_address, &cow_value, sizeof(cow_value)));
    OK(uc_context_save(uc, cow_context));
    OK(uc_mem_unmap(uc, address, 0x1000));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, value_address, &replacement, sizeof(replacement)));

    OK(uc_context_restore(uc, cow_context));
    OK(uc_mem_read(uc, value_address, &actual, sizeof(actual)));
    TEST_CHECK(actual == cow_value);
    OK(uc_context_restore(uc, original_context));
    OK(uc_mem_read(uc, value_address, &actual, sizeof(actual)));
    TEST_CHECK(actual == original);
    OK(uc_context_restore(uc, cow_context));
    OK(uc_mem_read(uc, value_address, &actual, sizeof(actual)));
    TEST_CHECK(actual == cow_value);

    OK(uc_context_free(original_context));
    OK(uc_context_free(cow_context));
    OK(uc_close(uc));
}

static void test_snapshot_forward_restore_same_level_aba(void)
{
    const uint64_t address = 0x58000;
    const uint8_t value_a = 0x71;
    const uint8_t value_b = 0x82;
    const uint8_t value_c = 0x93;
    uint8_t actual = 0;
    uc_context *context_a;
    uc_context *context_b;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address, &value_a, sizeof(value_a)));
    OK(uc_context_alloc(uc, &context_a));
    OK(uc_context_alloc(uc, &context_b));
    OK(uc_context_save(uc, context_a));

    OK(uc_mem_unmap(uc, address, 0x1000));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address, &value_b, sizeof(value_b)));
    OK(uc_context_save(uc, context_b));

    OK(uc_context_restore(uc, context_a));
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == value_a);
    OK(uc_mem_unmap(uc, address, 0x1000));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address, &value_c, sizeof(value_c)));

    OK(uc_context_restore(uc, context_b));
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == value_b);
    OK(uc_context_restore(uc, context_a));
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == value_a);
    OK(uc_context_restore(uc, context_b));
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == value_b);

    OK(uc_context_free(context_a));
    OK(uc_context_free(context_b));
    OK(uc_close(uc));
}

static void test_snapshot_equal_priority_cow_leaves(void)
{
    const uint64_t address = 0x5c000;
    const uint8_t original[] = {0x11, 0x22};
    const uint8_t changed[] = {0xa1, 0xb2};
    const uint64_t offsets[] = {0x20, 0x1020};
    uint8_t actual;
    uc_context *base_context;
    uc_context *cow_context;
    uc_engine *uc;
    size_t i;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x3000, UC_PROT_ALL));
    for (i = 0; i < 2; i++) {
        OK(uc_mem_write(uc, address + offsets[i], &original[i], 1));
    }
    OK(uc_context_alloc(uc, &base_context));
    OK(uc_context_alloc(uc, &cow_context));
    OK(uc_context_save(uc, base_context));

    for (i = 0; i < 2; i++) {
        OK(uc_mem_write(uc, address + offsets[i], &changed[i], 1));
    }
    OK(uc_context_save(uc, cow_context));

    OK(uc_context_restore(uc, base_context));
    for (i = 0; i < 2; i++) {
        OK(uc_mem_read(uc, address + offsets[i], &actual, 1));
        TEST_CHECK(actual == original[i]);
    }
    OK(uc_context_restore(uc, cow_context));
    for (i = 0; i < 2; i++) {
        OK(uc_mem_read(uc, address + offsets[i], &actual, 1));
        TEST_CHECK(actual == changed[i]);
    }

    OK(uc_context_free(base_context));
    OK(uc_context_free(cow_context));
    OK(uc_close(uc));
}

static void test_snapshot_repeated_root_leaf_forward_restore(void)
{
    const uint64_t address = 0x60000;
    const uint8_t original = 0x31;
    const uint8_t changed = 0x42;
    uint8_t actual;
    uc_context *leaf_context;
    uc_context *cow_context;
    uc_engine *uc;
    unsigned int i;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, address, &original, 1));
    OK(uc_context_alloc(uc, &leaf_context));
    OK(uc_context_alloc(uc, &cow_context));
    OK(uc_context_save(uc, leaf_context));
    OK(uc_mem_write(uc, address, &changed, 1));
    OK(uc_context_save(uc, cow_context));

    for (i = 0; i < 32; i++) {
        OK(uc_context_restore(uc, leaf_context));
        OK(uc_mem_read(uc, address, &actual, 1));
        TEST_CHECK(actual == original);
        OK(uc_context_restore(uc, cow_context));
        OK(uc_mem_read(uc, address, &actual, 1));
        TEST_CHECK(actual == changed);
    }

    OK(uc_context_free(leaf_context));
    OK(uc_context_free(cow_context));
    OK(uc_close(uc));
}

static void test_snapshot_context_resave_free_ownership(void)
{
    const uint64_t old_address = 0x64000;
    const uint64_t new_address = 0x68000;
    const uint8_t old_value = 0x51;
    const uint8_t new_value = 0x62;
    uint8_t actual;
    uc_context *context;
    uc_engine *uc;
    unsigned int i;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, old_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, old_address, &old_value, 1));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));
    OK(uc_mem_unmap(uc, old_address, 0x1000));

    OK(uc_mem_map(uc, new_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, new_address, &new_value, 1));
    OK(uc_context_save(uc, context));
    OK(uc_mem_unmap(uc, new_address, 0x1000));
    OK(uc_context_restore(uc, context));
    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_mem_read(uc, old_address, &actual, 1));
    OK(uc_mem_read(uc, new_address, &actual, 1));
    TEST_CHECK(actual == new_value);

    OK(uc_mem_unmap(uc, new_address, 0x1000));
    OK(uc_context_free(context));
    for (i = 0; i < 64; i++) {
        OK(uc_mem_map(uc, new_address, 0x1000, UC_PROT_ALL));
        OK(uc_mem_unmap(uc, new_address, 0x1000));
    }
    OK(uc_close(uc));
}

static void test_snapshot_final_release_restores_memory_apis(void)
{
    const uint64_t address = 0x7c000;
    const uint8_t value = 0x5a;
    uint8_t actual = 0;
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x2000, UC_PROT_ALL));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));
    OK(uc_mem_write(uc, address, &value, sizeof(value)));

    OK(uc_context_free(context));
    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == value);
    OK(uc_mem_protect(uc, address, 0x1000, UC_PROT_READ));
    OK(uc_mem_unmap(uc, address + 0x1000, 0x1000));
    OK(uc_mem_unmap(uc, address, 0x1000));

    OK(uc_close(uc));
}

static void test_snapshot_close_with_retained_mappings(void)
{
    const uint64_t ram_address = 0x6c000;
    const uint64_t host_address = 0x70000;
    const uint64_t mmio_address = 0x74000;
    uint8_t *host_memory = calloc(1, 0x1000);
    uc_context *context;
    uc_engine *uc;

    TEST_ASSERT(host_memory != NULL);
    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, ram_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_map_ptr(uc, host_address, 0x1000, UC_PROT_ALL, host_memory));
    OK(uc_mmio_map(uc, mmio_address, 0x1000, NULL, NULL, NULL, NULL));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_mem_unmap(uc, ram_address, 0x1000));
    OK(uc_mem_unmap(uc, host_address, 0x1000));
    OK(uc_mem_unmap(uc, mmio_address, 0x1000));
    OK(uc_close(uc));
    OK(uc_context_free(context));
    free(host_memory);
}

static void test_snapshot_replace_with_host_memory(void)
{
    const uint64_t address = 0x18000;
    const uint8_t original[] = {0x51, 0x52, 0x53, 0x54};
    const uint8_t replacement[] = {0xa1, 0xa2, 0xa3, 0xa4};
    const uc_mem_region original_region = {
        address,
        address + 0xfff,
        UC_PROT_READ | UC_PROT_WRITE,
    };
    uint8_t actual[sizeof(original)];
    uint8_t *host_memory = calloc(1, 0x1000);
    uc_context *context;
    uc_engine *uc;

    TEST_ASSERT(host_memory != NULL);
    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x1000, original_region.perms));
    OK(uc_mem_write(uc, address + 0x20, original, sizeof(original)));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_mem_unmap(uc, address, 0x1000));
    memcpy(host_memory + 0x20, replacement, sizeof(replacement));
    OK(uc_mem_map_ptr(uc, address, 0x1000, UC_PROT_ALL, host_memory));
    OK(uc_mem_read(uc, address + 0x20, actual, sizeof(actual)));
    TEST_CHECK(memcmp(actual, replacement, sizeof(actual)) == 0);

    OK(uc_context_restore(uc, context));
    test_snapshot_check_regions(uc, &original_region, 1);
    OK(uc_mem_read(uc, address + 0x20, actual, sizeof(actual)));
    TEST_CHECK(memcmp(actual, original, sizeof(actual)) == 0);
    TEST_CHECK(memcmp(host_memory + 0x20, replacement, sizeof(replacement)) ==
               0);

    OK(uc_context_free(context));
    OK(uc_close(uc));
    free(host_memory);
}

typedef struct TestSnapshotMmioData {
    uint64_t read_value;
    uint64_t write_value;
    uint64_t read_offset;
    uint64_t write_offset;
    unsigned int read_size;
    unsigned int write_size;
    uint32_t read_count;
    uint32_t write_count;
} TestSnapshotMmioData;

static uint64_t test_snapshot_mmio_read(uc_engine *uc, uint64_t offset,
                                        unsigned int size, void *user_data)
{
    TestSnapshotMmioData *data = (TestSnapshotMmioData *)user_data;

    (void)uc;
    data->read_count++;
    data->read_offset = offset;
    data->read_size = size;
    return data->read_value;
}

static void test_snapshot_mmio_write(uc_engine *uc, uint64_t offset,
                                     unsigned int size, uint64_t value,
                                     void *user_data)
{
    TestSnapshotMmioData *data = (TestSnapshotMmioData *)user_data;

    (void)uc;
    data->write_count++;
    data->write_offset = offset;
    data->write_size = size;
    data->write_value = value;
}

static void test_snapshot_existing_mmio_write(void)
{
    const uint64_t address = 0x20000;
    const uint32_t value = LEINT32(0x76543210);
    TestSnapshotMmioData data = {0};
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mmio_map(uc, address, 0x1000, NULL, NULL,
                   test_snapshot_mmio_write, &data));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_mem_write(uc, address + 0x20, &value, sizeof(value)));
    TEST_CHECK(data.write_count == 1);
    TEST_CHECK(data.write_offset == 0x20);
    TEST_CHECK(data.write_size == sizeof(value));
    TEST_CHECK(data.write_value == 0x76543210);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_snapshot_replace_with_mmio(void)
{
    const uint64_t address = 0x20000;
    const uint8_t original_first[] = {0x30, 0x31, 0x32, 0x33};
    const uint8_t original_second[] = {0x40, 0x41, 0x42, 0x43};
    /* mov rax, 0x21020; mov dword ptr [rax], 0x76543210 */
    const uint8_t mmio_write_code[] = {
        0x48, 0xc7, 0xc0, 0x20, 0x10, 0x02, 0x00,
        0xc7, 0x00, 0x10, 0x32, 0x54, 0x76,
    };
    const uc_mem_region original_regions[] = {
        {0x1000, 0x1fff, UC_PROT_ALL},
        {address, address + 0xfff, UC_PROT_READ | UC_PROT_WRITE},
        {address + 0x1000, address + 0x1fff, UC_PROT_READ | UC_PROT_EXEC},
    };
    TestSnapshotMmioData data = {.read_value = 0x89abcdef};
    uint8_t actual[sizeof(original_first)];
    uint32_t mmio_value = 0;
    uint32_t write_value;
    uc_context *context;
    uc_engine *uc;
    uc_err err;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_mem_map(uc, address, 0x1000, UC_PROT_READ | UC_PROT_WRITE));
    OK(uc_mem_map(uc, address + 0x1000, 0x1000, UC_PROT_READ | UC_PROT_EXEC));
    OK(uc_mem_write(uc, address + 0x20, original_first,
                    sizeof(original_first)));
    OK(uc_mem_write(uc, address + 0x1020, original_second,
                    sizeof(original_second)));
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x1000, mmio_write_code, sizeof(mmio_write_code)));
    test_snapshot_check_regions(uc, original_regions,
                                sizeof(original_regions) /
                                    sizeof(original_regions[0]));

    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));
    OK(uc_mem_unmap(uc, address, 0x1000));
    OK(uc_mem_unmap(uc, address + 0x1000, 0x1000));
    OK(uc_mmio_map(uc, address, 0x2000, test_snapshot_mmio_read, &data,
                   test_snapshot_mmio_write, &data));

    OK(uc_mem_read(uc, address + 0x20, &mmio_value, sizeof(mmio_value)));
    TEST_CHECK(LEINT32(mmio_value) == 0x89abcdef);
    TEST_CHECK(data.read_count == 1);
    TEST_CHECK(data.read_offset == 0x20);
    TEST_CHECK(data.read_size == sizeof(mmio_value));
    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(mmio_write_code), 0, 0));
    TEST_CHECK(data.write_count == 1);
    TEST_CHECK(data.write_offset == 0x1020);
    TEST_CHECK(data.write_size == sizeof(uint32_t));
    TEST_CHECK(data.write_value == 0x76543210);

    err = uc_context_restore(uc, context);
    if (!TEST_CHECK(err == UC_ERR_OK)) {
        TEST_MSG("%s", uc_strerror(err));
        goto cleanup;
    }
    test_snapshot_check_regions(uc, original_regions,
                                sizeof(original_regions) /
                                    sizeof(original_regions[0]));
    OK(uc_mem_read(uc, address + 0x20, actual, sizeof(actual)));
    TEST_CHECK(memcmp(actual, original_first, sizeof(actual)) == 0);
    OK(uc_mem_read(uc, address + 0x1020, actual, sizeof(actual)));
    TEST_CHECK(memcmp(actual, original_second, sizeof(actual)) == 0);
    TEST_CHECK(data.read_count == 1);

    write_value = LEINT32(0x12345678);
    OK(uc_mem_write(uc, address + 0x20, &write_value, sizeof(write_value)));
    OK(uc_mem_read(uc, address + 0x20, &mmio_value, sizeof(mmio_value)));
    TEST_CHECK(LEINT32(mmio_value) == 0x12345678);
    TEST_CHECK(data.read_count == 1);
    TEST_CHECK(data.write_count == 1);

cleanup:
    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void parts_increment(size_t idx, char parts[3])
{
    if (idx && idx % 3 == 0) {
        if (++parts[2] > '9') {
            parts[2] = '0';
            if (++parts[1] > 'z') {
                parts[1] = 'a';
                if (++parts[0] > 'Z')
                    parts[0] = 'A';
            }
        }
    }
}

// Create a pattern string. It works the same as
// https://github.com/rapid7/metasploit-framework/blob/master/tools/exploit/pattern_create.rb
static void pattern_create(char *buf, size_t len)
{
    char parts[] = {'A', 'a', '0'};
    size_t i;

    for (i = 0; i < len; i++) {
        buf[i] = parts[i % 3];
        parts_increment(i, parts);
    }
}

static bool pattern_verify(const char *buf, size_t len)
{
    char parts[] = {'A', 'a', '0'};
    size_t i;

    for (i = 0; i < len; i++) {
        if (buf[i] != parts[i % 3])
            return false;
        parts_increment(i, parts);
    }

    return true;
}

// Test for reading and writing memory block that are bigger than INT_MAX.
static void test_mem_read_and_write_large_memory_block(void)
{
    uc_engine *uc;
    uint64_t mem_addr = 0x1000000;
    uint64_t mem_size = 0x9f000000;
    char *pmem = NULL;

    if (sizeof(void *) < 8) {
        // Don't perform the test on a 32-bit platforms since we may not have
        // enough memory space.
        return;
    }
    // Android CI/CD services do not have enough memory capacity for this
    // test to work. Executing it will result in a permanent loop with the
    // low memory killer daemon.
#ifdef __ANDROID__
    return;
#endif

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, mem_addr, mem_size, UC_PROT_ALL));

    pmem = malloc(mem_size);
    if (TEST_CHECK(pmem != NULL)) {
        pattern_create(pmem, mem_size);

        OK(uc_mem_write(uc, mem_addr, pmem, mem_size));
        memset(pmem, 'a', mem_size);
        OK(uc_mem_read(uc, mem_addr, pmem, mem_size));
        TEST_CHECK(pattern_verify(pmem, mem_size));
        free(pmem);
    }

    OK(uc_mem_unmap(uc, mem_addr, mem_size));
    OK(uc_close(uc));
}

static bool test_v2p_tlb_fill(uc_engine *uc, uint64_t addr, uc_mem_type type,
                              uc_tlb_entry *result, void *user_data)
{
    if (type != UC_MEM_READ)
        return false;
    result->paddr = addr;
    result->perms = UC_PROT_READ;
    return true;
}

static void test_virtual_to_physical(void)
{
    uc_engine *uc;
    uc_hook hook;
    uint64_t res;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL, test_v2p_tlb_fill, NULL, 1, 0));

    OK(uc_vmem_translate(uc, 0x1000, UC_PROT_READ, &res));
    uc_assert_err(UC_ERR_WRITE_PROT,
                  uc_vmem_translate(uc, 0x1000, UC_PROT_WRITE, &res));
    OK(uc_close(uc));
}

static bool test_virtual_write_tlb_fill(uc_engine *uc, uint64_t addr,
                                        uc_mem_type type, uc_tlb_entry *result,
                                        void *user_data)
{
    if (addr < 0x1000)
        return false;
    result->paddr = addr - 0x1000;
    result->perms = UC_PROT_ALL;
    return true;
}

static void test_virtual_write(void)
{
    uc_engine *uc;
    uc_hook hook;
    uint64_t rax = 21;
    uint64_t res = 0;
    /*
     * mov rax, [0x2000]
     */
    char code[] = {0x48, 0x8B, 0x04, 0x25, 0x00, 0x20, 0x00, 0x00};

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL, test_virtual_write_tlb_fill,
                   NULL, 1, 0));
    OK(uc_mem_map(uc, 0x0, 0x2000, UC_PROT_ALL));

    OK(uc_vmem_write(uc, 0x1000, UC_PROT_EXEC, code, sizeof(code)));
    OK(uc_vmem_write(uc, 0x2000, UC_PROT_READ, &rax, sizeof(rax)));

    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(code), 0, 1));
    OK(uc_reg_read(uc, UC_X86_REG_RAX, &res));
    TEST_CHECK(rax == res);

    OK(uc_close(uc));
}

// Regression test for issue #2321. With two legal mappings (top page and
// page 0), an address+size that wraps used to let check_mem_area() walk
// from the top of the address space back to 0 and treat the wrapping
// range as fully mapped. uc_mem_read then memcpy'd 0x2000 bytes into a
// 0x1000 buffer; uc_mem_unmap silently dropped both regions. After the
// fix, check_mem_area() rejects the wrap up front so the four entry
// points fall back to their existing not-mapped error codes.
static void test_mem_addr_size_wraparound(void)
{
    uc_engine *uc;
    uint8_t buf[0x1000];

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, 0, 0x1000, UC_PROT_ALL));
    OK(uc_mem_map(uc, 0xFFFFFFFFFFFFF000ULL, 0x1000, UC_PROT_ALL));

    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_mem_read(uc, 0xFFFFFFFFFFFFF000ULL, buf, 0x2000));
    uc_assert_err(UC_ERR_WRITE_UNMAPPED,
                  uc_mem_write(uc, 0xFFFFFFFFFFFFF000ULL, buf, 0x2000));
    uc_assert_err(UC_ERR_NOMEM,
                  uc_mem_unmap(uc, 0xFFFFFFFFFFFFF000ULL, 0x2000));
    uc_assert_err(UC_ERR_NOMEM, uc_mem_protect(uc, 0xFFFFFFFFFFFFF000ULL,
                                               0x2000, UC_PROT_READ));

    // The non-wrapping single-page case must still work for both regions.
    OK(uc_mem_read(uc, 0xFFFFFFFFFFFFF000ULL, buf, 0x1000));
    OK(uc_mem_read(uc, 0, buf, 0x1000));

    OK(uc_close(uc));
}

/* SMC test: verifies that a store which overwrites code on
 * the same page invalidates any cached TBs translated from that
 * page, so subsequent execution sees the new instructions.
 */
static void test_smc(void)
{
    uc_engine *uc;
    uint64_t r_rax;
    uint64_t r_rsp;

    char code[] = (                    // 00: do_inc_dec:
        "\x48\xff\xc0"                 // 00:    inc %rax
        "\xc3"                         // 03:    ret
        "\xe8\xf7\xff\xff\xff"         // 04: call do_inc_dec
        "\xc6\x05\xf2\xff\xff\xff\xc8" // 09: movb $0xc8, -0xe(%rip)
        "\xe8\xeb\xff\xff\xff"         // 16: call do_inc_dec
    );

    r_rax = 0x1234;
    r_rsp = 0x5000;
    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, 0x0, 0x1000, UC_PROT_ALL));                     // text
    OK(uc_mem_map(uc, 0x4000, 0x1000, UC_PROT_READ | UC_PROT_WRITE)); // stack
    OK(uc_mem_write(uc, 0x0, code, sizeof(code) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &r_rsp));
    OK(uc_emu_start(uc, 0x4, sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_mem_read(uc, 0x0, code, sizeof(code) - 1));
    TEST_CHECK(r_rax == 0x1234);
    TEST_CHECK((code[2] & 0xFF) == 0xC8);

    OK(uc_close(uc));
}

/*
 * Ensures that a code section initially as RW, if marked later as RX
 * still works as expected
 */
static void test_tlbdirty_exec(void)
{
    uc_engine *uc;
    uint32_t eax;

    char code[] = ("\x40"); // inc eax
    eax = 41;
    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_mem_map(uc, 0x0, 0x1000, UC_PROT_READ | UC_PROT_WRITE));
    OK(uc_mem_write(uc, 0x0, code, sizeof(code) - 1));
    OK(uc_mem_protect(uc, 0x0, 0x1000, UC_PROT_READ | UC_PROT_EXEC));
    OK(uc_emu_start(uc, 0x0, sizeof(code) - 1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 42);

    OK(uc_close(uc));
}

#define TEST_MEM_HOOK_CODE 0x1000
#define TEST_MEM_HOOK_DATA 0x3000
#define TEST_MEM_HOOK_EVENT_CAPACITY 16

typedef struct TestMemoryHookEvent {
    uc_mem_type type;
    uint64_t address;
    uint64_t pc;
    int size;
    int64_t value;
} TestMemoryHookEvent;

typedef struct TestMemoryHookData {
    TestMemoryHookEvent events[TEST_MEM_HOOK_EVENT_CAPACITY];
    size_t event_count;
    uint32_t read_value;
    bool stop_on_invalid;
    bool stopped;
} TestMemoryHookData;

static void test_memory_hook_record(uc_engine *uc, uc_mem_type type,
                                    uint64_t address, int size, int64_t value,
                                    TestMemoryHookData *data)
{
    TestMemoryHookEvent *event;
    uint32_t eip;

    if (!TEST_CHECK(data->event_count < TEST_MEM_HOOK_EVENT_CAPACITY)) {
        return;
    }

    event = &data->events[data->event_count++];
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    event->type = type;
    event->address = address;
    event->pc = eip;
    event->size = size;
    event->value = value;
}

static void test_memory_hook_callback(uc_engine *uc, uc_mem_type type,
                                      uint64_t address, int size, int64_t value,
                                      void *user_data)
{
    test_memory_hook_record(uc, type, address, size, value,
                            (TestMemoryHookData *)user_data);
}

static bool test_memory_invalid_hook_callback(uc_engine *uc, uc_mem_type type,
                                              uint64_t address, int size,
                                              int64_t value, void *user_data)
{
    TestMemoryHookData *data = (TestMemoryHookData *)user_data;
    const uint8_t fetch_code[] = {0x40};
    uint64_t page = address & ~0xfffULL;

    test_memory_hook_record(uc, type, address, size, value, data);

    switch (type) {
    case UC_MEM_READ_UNMAPPED:
        OK(uc_mem_map(uc, page, 0x1000, UC_PROT_ALL));
        OK(uc_mem_write(uc, address, &data->read_value,
                        sizeof(data->read_value)));
        break;
    case UC_MEM_WRITE_UNMAPPED:
        OK(uc_mem_map(uc, page, 0x1000, UC_PROT_ALL));
        break;
    case UC_MEM_FETCH_UNMAPPED:
        OK(uc_mem_map(uc, page, 0x1000, UC_PROT_ALL));
        OK(uc_mem_write(uc, address, fetch_code, sizeof(fetch_code)));
        break;
    case UC_MEM_READ_PROT:
    case UC_MEM_WRITE_PROT:
    case UC_MEM_FETCH_PROT:
        OK(uc_mem_protect(uc, page, 0x1000, UC_PROT_ALL));
        break;
    default:
        TEST_CHECK(false);
        return false;
    }

    if (data->stop_on_invalid && !data->stopped) {
        data->stopped = true;
        OK(uc_emu_stop(uc));
    }

    return true;
}

static void test_memory_hook_assert_event(const TestMemoryHookData *data,
                                          size_t index, uc_mem_type type,
                                          uint64_t address,
                                          uint64_t expected_pc, int size)
{
    TEST_CHECK(index < data->event_count);
    if (index >= data->event_count) {
        return;
    }

    TEST_CHECK(data->events[index].type == type);
    TEST_CHECK(data->events[index].address == address);
    TEST_CHECK(data->events[index].pc == expected_pc);
    TEST_CHECK(data->events[index].size == size);
}

typedef enum TestInsnTraceType {
    TEST_INSN_TRACE_FETCH,
    TEST_INSN_TRACE_CODE,
} TestInsnTraceType;

typedef struct TestInsnTraceEvent {
    TestInsnTraceType type;
    uint64_t address;
    uint32_t pc;
    uint32_t size;
    uint32_t eax;
    uint32_t ebx;
} TestInsnTraceEvent;

typedef struct TestInsnTrace {
    TestInsnTraceEvent events[16];
    size_t count;
    bool stop_on_code;
} TestInsnTrace;

static void test_insn_trace_record(uc_engine *uc, TestInsnTraceType type,
                                   uint64_t address, uint32_t size,
                                   TestInsnTrace *trace)
{
    TestInsnTraceEvent *event;

    if (!TEST_CHECK(trace->count <
                    sizeof(trace->events) / sizeof(trace->events[0]))) {
        return;
    }

    event = &trace->events[trace->count++];
    event->type = type;
    event->address = address;
    event->size = size;
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &event->pc));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &event->eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &event->ebx));
}

static void test_insn_fetch_callback(uc_engine *uc, uc_mem_type type,
                                     uint64_t address, int size, int64_t value,
                                     void *user_data)
{
    TEST_CHECK(type == UC_MEM_FETCH);
    TEST_CHECK(value == 0);
    test_insn_trace_record(uc, TEST_INSN_TRACE_FETCH, address, size,
                           (TestInsnTrace *)user_data);
}

static void test_insn_code_callback(uc_engine *uc, uint64_t address,
                                    uint32_t size, void *user_data)
{
    TestInsnTrace *trace = (TestInsnTrace *)user_data;

    test_insn_trace_record(uc, TEST_INSN_TRACE_CODE, address, size, trace);
    if (trace->stop_on_code) {
        OK(uc_emu_stop(uc));
    }
}

static void test_insn_trace_assert_event(const TestInsnTrace *trace,
                                         size_t index, TestInsnTraceType type,
                                         uint64_t address, uint32_t size,
                                         uint32_t eax, uint32_t ebx)
{
    TEST_CHECK(index < trace->count);
    if (index >= trace->count) {
        return;
    }

    TEST_CHECK(trace->events[index].type == type);
    TEST_CHECK(trace->events[index].address == address);
    TEST_CHECK(trace->events[index].pc == address);
    TEST_CHECK(trace->events[index].size == size);
    TEST_CHECK(trace->events[index].eax == eax);
    TEST_CHECK(trace->events[index].ebx == ebx);
}

static void test_mem_fetch_hook(void)
{
    const uint8_t code[] = {
        0x40,             /* inc eax */
        0x83, 0xc3, 0x02, /* add ebx,2 */
    };
    TestInsnTrace trace = {0};
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uc_engine *uc;
    uc_hook code_hook;
    uc_hook fetch_hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_CODE, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_CODE, code, sizeof(code)));

    /* Populate the TB cache before installing either hook. */
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));

    eax = 0;
    ebx = 0;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_hook_add(uc, &fetch_hook, UC_HOOK_MEM_FETCH, test_insn_fetch_callback,
                   &trace, 1, 0));
    OK(uc_hook_add(uc, &code_hook, UC_HOOK_CODE, test_insn_code_callback,
                   &trace, 1, 0));

    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));
    TEST_CHECK(trace.count == 4);
    test_insn_trace_assert_event(&trace, 0, TEST_INSN_TRACE_FETCH,
                                 TEST_MEM_HOOK_CODE, 1, 0, 0);
    test_insn_trace_assert_event(&trace, 1, TEST_INSN_TRACE_CODE,
                                 TEST_MEM_HOOK_CODE, 1, 0, 0);
    test_insn_trace_assert_event(&trace, 2, TEST_INSN_TRACE_FETCH,
                                 TEST_MEM_HOOK_CODE + 1, 3, 1, 0);
    test_insn_trace_assert_event(&trace, 3, TEST_INSN_TRACE_CODE,
                                 TEST_MEM_HOOK_CODE + 1, 3, 1, 0);
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    TEST_CHECK(eax == 1);
    TEST_CHECK(ebx == 2);

    eax = 0;
    ebx = 0;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));
    TEST_CHECK(trace.count == 8);
    test_insn_trace_assert_event(&trace, 4, TEST_INSN_TRACE_FETCH,
                                 TEST_MEM_HOOK_CODE, 1, 0, 0);
    test_insn_trace_assert_event(&trace, 5, TEST_INSN_TRACE_CODE,
                                 TEST_MEM_HOOK_CODE, 1, 0, 0);
    test_insn_trace_assert_event(&trace, 6, TEST_INSN_TRACE_FETCH,
                                 TEST_MEM_HOOK_CODE + 1, 3, 1, 0);
    test_insn_trace_assert_event(&trace, 7, TEST_INSN_TRACE_CODE,
                                 TEST_MEM_HOOK_CODE + 1, 3, 1, 0);

    OK(uc_hook_del(uc, fetch_hook));

    eax = 0;
    ebx = 0;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));
    TEST_CHECK(trace.count == 10);
    test_insn_trace_assert_event(&trace, 8, TEST_INSN_TRACE_CODE,
                                 TEST_MEM_HOOK_CODE, 1, 0, 0);
    test_insn_trace_assert_event(&trace, 9, TEST_INSN_TRACE_CODE,
                                 TEST_MEM_HOOK_CODE + 1, 3, 1, 0);

    OK(uc_hook_del(uc, code_hook));
    OK(uc_close(uc));
}

static void test_mem_fetch_precedes_code_stop(void)
{
    const uint8_t code[] = {0x40};
    TestInsnTrace trace = {
        .stop_on_code = true,
    };
    uint32_t eax = 0;
    uc_engine *uc;
    uc_hook code_hook;
    uc_hook fetch_hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_CODE, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_CODE, code, sizeof(code)));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_hook_add(uc, &code_hook, UC_HOOK_CODE, test_insn_code_callback,
                   &trace, 1, 0));
    OK(uc_hook_add(uc, &fetch_hook, UC_HOOK_MEM_FETCH, test_insn_fetch_callback,
                   &trace, 1, 0));

    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 1));
    TEST_CHECK(trace.count == 2);
    test_insn_trace_assert_event(&trace, 0, TEST_INSN_TRACE_FETCH,
                                 TEST_MEM_HOOK_CODE, 1, 0, 0);
    test_insn_trace_assert_event(&trace, 1, TEST_INSN_TRACE_CODE,
                                 TEST_MEM_HOOK_CODE, 1, 0, 0);
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0);

    OK(uc_hook_del(uc, fetch_hook));
    OK(uc_hook_del(uc, code_hook));
    OK(uc_close(uc));
}

static void test_mem_fetch_range_and_user_data(void)
{
    const uint8_t code[] = {0x40, 0x43};
    TestMemoryHookData initial = {0};
    TestMemoryHookData replacement = {0};
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_CODE, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_CODE, code, sizeof(code)));
    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));

    eax = 0;
    ebx = 0;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_FETCH, test_memory_hook_callback,
                   &initial, TEST_MEM_HOOK_CODE + 1, TEST_MEM_HOOK_CODE + 1));
    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));
    TEST_CHECK(initial.event_count == 1);
    test_memory_hook_assert_event(&initial, 0, UC_MEM_FETCH,
                                  TEST_MEM_HOOK_CODE + 1,
                                  TEST_MEM_HOOK_CODE + 1, 1);

    OK(uc_hook_set_user_data(uc, hook, &replacement));
    eax = 0;
    ebx = 0;
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_write(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));
    TEST_CHECK(initial.event_count == 1);
    TEST_CHECK(replacement.event_count == 1);
    test_memory_hook_assert_event(&replacement, 0, UC_MEM_FETCH,
                                  TEST_MEM_HOOK_CODE + 1,
                                  TEST_MEM_HOOK_CODE + 1, 1);

    OK(uc_hook_del(uc, hook));
    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));
    TEST_CHECK(replacement.event_count == 1);

    OK(uc_close(uc));
}

typedef struct TestSelfDeletingFetch {
    uc_hook hook;
    size_t count;
} TestSelfDeletingFetch;

static void test_mem_fetch_self_delete_callback(uc_engine *uc, uc_mem_type type,
                                                uint64_t address, int size,
                                                int64_t value, void *user_data)
{
    TestSelfDeletingFetch *data = (TestSelfDeletingFetch *)user_data;

    TEST_CHECK(type == UC_MEM_FETCH);
    data->count++;
    if (data->count == 1) {
        OK(uc_hook_del(uc, data->hook));
    }
}

static void test_mem_fetch_self_delete(void)
{
    const uint8_t code[] = {0x40, 0x43};
    TestSelfDeletingFetch data = {0};
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_CODE, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_CODE, code, sizeof(code)));
    OK(uc_hook_add(uc, &data.hook, UC_HOOK_MEM_FETCH,
                   test_mem_fetch_self_delete_callback, &data, 1, 0));

    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));
    TEST_CHECK(data.count == 1);
    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 2));
    TEST_CHECK(data.count == 1);

    OK(uc_close(uc));
}

static void test_mem_read_prot_stop_resume(void)
{
    const uint8_t code[] = {0xa1, 0x00, 0x30, 0x00, 0x00};
    TestMemoryHookData data = {
        .read_value = LEINT32(0x78563412),
        .stop_on_invalid = true,
    };
    uint32_t eax = 0xaaaaaaaa;
    uint32_t eip;
    uc_engine *uc;
    uc_hook read_hook;
    uc_hook prot_hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_CODE, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_CODE, code, sizeof(code)));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_DATA, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_DATA, &data.read_value,
                    sizeof(data.read_value)));
    OK(uc_mem_protect(uc, TEST_MEM_HOOK_DATA, 0x1000, UC_PROT_WRITE));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_hook_add(uc, &read_hook, UC_HOOK_MEM_READ, test_memory_hook_callback,
                   &data, 1, 0));
    OK(uc_hook_add(uc, &prot_hook, UC_HOOK_MEM_READ_PROT,
                   test_memory_invalid_hook_callback, &data, 1, 0));

    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 1));
    TEST_CHECK(data.event_count == 2);
    test_memory_hook_assert_event(&data, 0, UC_MEM_READ, TEST_MEM_HOOK_DATA,
                                  TEST_MEM_HOOK_CODE, 4);
    test_memory_hook_assert_event(&data, 1, UC_MEM_READ_PROT,
                                  TEST_MEM_HOOK_DATA, TEST_MEM_HOOK_CODE, 4);
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    TEST_CHECK(eip == TEST_MEM_HOOK_CODE);
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0xaaaaaaaa);

    OK(uc_emu_start(uc, eip, TEST_MEM_HOOK_CODE + sizeof(code), 0, 1));
    TEST_CHECK(data.event_count == 3);
    test_memory_hook_assert_event(&data, 2, UC_MEM_READ, TEST_MEM_HOOK_DATA,
                                  TEST_MEM_HOOK_CODE, 4);
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0x78563412);

    OK(uc_hook_del(uc, prot_hook));
    OK(uc_hook_del(uc, read_hook));
    OK(uc_mem_protect(uc, TEST_MEM_HOOK_DATA, 0x1000, UC_PROT_WRITE));
    uc_assert_err(UC_ERR_READ_PROT,
                  uc_emu_start(uc, TEST_MEM_HOOK_CODE,
                               TEST_MEM_HOOK_CODE + sizeof(code), 0, 1));
    TEST_CHECK(data.event_count == 3);

    OK(uc_close(uc));
}

static void test_mem_write_prot_recovery(void)
{
    const uint8_t code[] = {
        0xc7, 0x05, 0x00, 0x30, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12,
    };
    TestMemoryHookData data = {0};
    uint32_t memory = 0;
    uc_engine *uc;
    uc_hook write_hook;
    uc_hook prot_hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_CODE, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_CODE, code, sizeof(code)));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_DATA, 0x1000, UC_PROT_READ));
    OK(uc_hook_add(uc, &write_hook, UC_HOOK_MEM_WRITE,
                   test_memory_hook_callback, &data, 1, 0));
    OK(uc_hook_add(uc, &prot_hook, UC_HOOK_MEM_WRITE_PROT,
                   test_memory_invalid_hook_callback, &data, 1, 0));

    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 1));
    TEST_CHECK(data.event_count == 2);
    test_memory_hook_assert_event(&data, 0, UC_MEM_WRITE, TEST_MEM_HOOK_DATA,
                                  TEST_MEM_HOOK_CODE, 4);
    test_memory_hook_assert_event(&data, 1, UC_MEM_WRITE_PROT,
                                  TEST_MEM_HOOK_DATA, TEST_MEM_HOOK_CODE, 4);
    TEST_CHECK(data.events[0].value == 0x12345678);
    TEST_CHECK(data.events[1].value == 0x12345678);
    OK(uc_mem_read(uc, TEST_MEM_HOOK_DATA, &memory, sizeof(memory)));
    TEST_CHECK(LEINT32(memory) == 0x12345678);

    OK(uc_hook_del(uc, prot_hook));
    OK(uc_hook_del(uc, write_hook));
    OK(uc_mem_protect(uc, TEST_MEM_HOOK_DATA, 0x1000, UC_PROT_READ));
    uc_assert_err(UC_ERR_WRITE_PROT,
                  uc_emu_start(uc, TEST_MEM_HOOK_CODE,
                               TEST_MEM_HOOK_CODE + sizeof(code), 0, 1));
    TEST_CHECK(data.event_count == 2);
    OK(uc_close(uc));
}

static void test_mem_fetch_prot_recovery(void)
{
    const uint8_t code[] = {0x40};
    TestMemoryHookData data = {0};
    uint32_t eax = 0;
    uc_engine *uc;
    uc_hook fetch_hook;
    uc_hook prot_hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_CODE, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_CODE, code, sizeof(code)));
    OK(uc_mem_protect(uc, TEST_MEM_HOOK_CODE, 0x1000,
                      UC_PROT_READ | UC_PROT_WRITE));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_hook_add(uc, &fetch_hook, UC_HOOK_MEM_FETCH,
                   test_memory_hook_callback, &data, 1, 0));
    OK(uc_hook_add(uc, &prot_hook, UC_HOOK_MEM_FETCH_PROT,
                   test_memory_invalid_hook_callback, &data, 1, 0));

    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 1));
    TEST_CHECK(data.event_count == 2);
    test_memory_hook_assert_event(&data, 0, UC_MEM_FETCH_PROT,
                                  TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE, 1);
    test_memory_hook_assert_event(&data, 1, UC_MEM_FETCH, TEST_MEM_HOOK_CODE,
                                  TEST_MEM_HOOK_CODE, 1);
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 1);

    OK(uc_hook_del(uc, prot_hook));
    OK(uc_hook_del(uc, fetch_hook));
    OK(uc_mem_protect(uc, TEST_MEM_HOOK_CODE, 0x1000,
                      UC_PROT_READ | UC_PROT_WRITE));
    uc_assert_err(UC_ERR_FETCH_PROT,
                  uc_emu_start(uc, TEST_MEM_HOOK_CODE,
                               TEST_MEM_HOOK_CODE + sizeof(code), 0, 1));
    TEST_CHECK(data.event_count == 2);

    OK(uc_close(uc));
}

static void test_mem_read_unmapped_recovery(void)
{
    const uint8_t code[] = {0xa1, 0x00, 0x30, 0x00, 0x00};
    TestMemoryHookData data = {.read_value = LEINT32(0x78563412)};
    uint32_t eax = 0;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_CODE, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_CODE, code, sizeof(code)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_READ_UNMAPPED,
                   test_memory_invalid_hook_callback, &data, 1, 0));

    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 1));
    TEST_CHECK(data.event_count == 1);
    test_memory_hook_assert_event(&data, 0, UC_MEM_READ_UNMAPPED,
                                  TEST_MEM_HOOK_DATA, TEST_MEM_HOOK_CODE, 4);
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 0x78563412);

    OK(uc_hook_del(uc, hook));
    OK(uc_mem_unmap(uc, TEST_MEM_HOOK_DATA, 0x1000));
    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_emu_start(uc, TEST_MEM_HOOK_CODE,
                               TEST_MEM_HOOK_CODE + sizeof(code), 0, 1));
    TEST_CHECK(data.event_count == 1);
    OK(uc_close(uc));
}

static void test_mem_write_unmapped_recovery(void)
{
    const uint8_t code[] = {
        0xc7, 0x05, 0x00, 0x30, 0x00, 0x00, 0x78, 0x56, 0x34, 0x12,
    };
    TestMemoryHookData data = {0};
    uint32_t memory = 0;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, TEST_MEM_HOOK_CODE, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, TEST_MEM_HOOK_CODE, code, sizeof(code)));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_WRITE_UNMAPPED,
                   test_memory_invalid_hook_callback, &data, 1, 0));

    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + sizeof(code),
                    0, 1));
    TEST_CHECK(data.event_count == 1);
    test_memory_hook_assert_event(&data, 0, UC_MEM_WRITE_UNMAPPED,
                                  TEST_MEM_HOOK_DATA, TEST_MEM_HOOK_CODE, 4);
    TEST_CHECK(data.events[0].value == 0x12345678);
    OK(uc_mem_read(uc, TEST_MEM_HOOK_DATA, &memory, sizeof(memory)));
    TEST_CHECK(LEINT32(memory) == 0x12345678);

    OK(uc_hook_del(uc, hook));
    OK(uc_mem_unmap(uc, TEST_MEM_HOOK_DATA, 0x1000));
    uc_assert_err(UC_ERR_WRITE_UNMAPPED,
                  uc_emu_start(uc, TEST_MEM_HOOK_CODE,
                               TEST_MEM_HOOK_CODE + sizeof(code), 0, 1));
    TEST_CHECK(data.event_count == 1);
    OK(uc_close(uc));
}

static void test_mem_fetch_unmapped_recovery(void)
{
    TestMemoryHookData data = {0};
    uint32_t eax = 0;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_hook_add(uc, &hook, UC_HOOK_MEM_FETCH_UNMAPPED,
                   test_memory_invalid_hook_callback, &data, 1, 0));

    OK(uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + 1, 0, 1));
    TEST_CHECK(data.event_count == 1);
    test_memory_hook_assert_event(&data, 0, UC_MEM_FETCH_UNMAPPED,
                                  TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE, 1);
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 1);

    OK(uc_hook_del(uc, hook));
    OK(uc_mem_unmap(uc, TEST_MEM_HOOK_CODE, 0x1000));
    uc_assert_err(
        UC_ERR_FETCH_UNMAPPED,
        uc_emu_start(uc, TEST_MEM_HOOK_CODE, TEST_MEM_HOOK_CODE + 1, 0, 1));
    TEST_CHECK(data.event_count == 1);
    OK(uc_close(uc));
}

TEST_LIST = {
    {"test_map_correct", test_map_correct},
    {"test_map_wrapping", test_map_wrapping},
    {"test_mem_protect", test_mem_protect},
    {"test_splitting_mem_unmap", test_splitting_mem_unmap},
    {"test_splitting_mmio_unmap", test_splitting_mmio_unmap},
    {"test_mem_protect_map_ptr", test_mem_protect_map_ptr},
    {"test_mem_cross_region_access", test_mem_cross_region_access},
    {"test_mem_regions_topology", test_mem_regions_topology},
    {"test_map_at_the_end", test_map_at_the_end},
    {"test_terminal_two_page_unmap_first",
     test_terminal_two_page_unmap_first},
    {"test_terminal_two_page_unmap_last", test_terminal_two_page_unmap_last},
    {"test_terminal_two_page_protect_first",
     test_terminal_two_page_protect_first},
    {"test_terminal_two_page_protect_last",
     test_terminal_two_page_protect_last},
    {"test_terminal_page_active_unmap", test_terminal_page_active_unmap},
    {"test_map_wrap", test_map_wrap},
    {"test_map_big_memory", test_map_big_memory},
    {"test_mem_protect_remove_exec", test_mem_protect_remove_exec},
    {"test_active_code_page_change", test_active_code_page_change},
    {"test_inactive_code_page_change", test_inactive_code_page_change},
    {"test_mmio_exec_write_unrelated_ram", test_mmio_exec_write_unrelated_ram},
    {"test_mem_protect_mmio", test_mem_protect_mmio},
    {"test_snapshot", test_snapshot},
    {"test_snapshot_code_restore_from_callback",
     test_snapshot_code_restore_from_callback},
    {"test_snapshot_cow_disjoint_nested_patch",
     test_snapshot_cow_disjoint_nested_patch},
    {"test_snapshot_nested_restore_outer_instruction",
     test_snapshot_nested_restore_outer_instruction},
    {"test_snapshot_with_vtlb", test_snapshot_with_vtlb},
    {"test_context_snapshot", test_context_snapshot},
    {"test_context_cpu_resave_preserves_memory_snapshot",
     test_context_cpu_resave_preserves_memory_snapshot},
    {"test_snapshot_unmap", test_snapshot_unmap},
    {"test_terminal_page_snapshot_restore",
     test_terminal_page_snapshot_restore},
    {"test_snapshot_restore_skips_later_unmapped_regions",
     test_snapshot_restore_skips_later_unmapped_regions},
    {"test_snapshot_empty_flatview", test_snapshot_empty_flatview},
    {"test_snapshot_replace_with_ram", test_snapshot_replace_with_ram},
    {"test_snapshot_cow_coordinates_and_remap",
     test_snapshot_cow_coordinates_and_remap},
    {"test_snapshot_cow_replacement_restore",
     test_snapshot_cow_replacement_restore},
    {"test_snapshot_forward_restore_same_level_aba",
     test_snapshot_forward_restore_same_level_aba},
    {"test_snapshot_equal_priority_cow_leaves",
     test_snapshot_equal_priority_cow_leaves},
    {"test_snapshot_repeated_root_leaf_forward_restore",
     test_snapshot_repeated_root_leaf_forward_restore},
    {"test_snapshot_context_resave_free_ownership",
     test_snapshot_context_resave_free_ownership},
    {"test_snapshot_final_release_restores_memory_apis",
     test_snapshot_final_release_restores_memory_apis},
    {"test_snapshot_close_with_retained_mappings",
     test_snapshot_close_with_retained_mappings},
    {"test_snapshot_replace_with_host_memory",
     test_snapshot_replace_with_host_memory},
    {"test_snapshot_existing_mmio_write", test_snapshot_existing_mmio_write},
    {"test_snapshot_replace_with_mmio", test_snapshot_replace_with_mmio},
    {"test_mem_read_and_write_large_memory_block",
     test_mem_read_and_write_large_memory_block},
    {"test_virtual_to_physical", test_virtual_to_physical},
    {"test_virtual_write", test_virtual_write},
    {"test_mem_addr_size_wraparound", test_mem_addr_size_wraparound},
    {"test_smc", test_smc},
    {"test_tlbdirty_exec", test_tlbdirty_exec},
    {"test_mem_fetch_hook", test_mem_fetch_hook},
    {"test_mem_fetch_precedes_code_stop", test_mem_fetch_precedes_code_stop},
    {"test_mem_fetch_range_and_user_data", test_mem_fetch_range_and_user_data},
    {"test_mem_fetch_self_delete", test_mem_fetch_self_delete},
    {"test_mem_read_prot_stop_resume", test_mem_read_prot_stop_resume},
    {"test_mem_write_prot_recovery", test_mem_write_prot_recovery},
    {"test_mem_fetch_prot_recovery", test_mem_fetch_prot_recovery},
    {"test_mem_read_unmapped_recovery", test_mem_read_unmapped_recovery},
    {"test_mem_write_unmapped_recovery", test_mem_write_unmapped_recovery},
    {"test_mem_fetch_unmapped_recovery", test_mem_fetch_unmapped_recovery},
    {NULL, NULL}};
