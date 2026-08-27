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
    OK(uc_mem_map_ptr(uc, first_address + 0x1000, 0x1000,
                      UC_PROT_ALL, second));
    OK(uc_mem_map_ptr(uc, hole_address, 0x1000, UC_PROT_ALL,
                      before_hole));

    OK(uc_mem_write(uc, first_address + 0xff8, input, sizeof(input)));
    TEST_CHECK(memcmp(first + 0xff8, input, 8) == 0);
    TEST_CHECK(memcmp(second, input + 8, 8) == 0);
    memset(output, 0, sizeof(output));
    OK(uc_mem_read(uc, first_address + 0xff8, output, sizeof(output)));
    TEST_CHECK(memcmp(output, input, sizeof(input)) == 0);

    memset(before_hole + 0xff8, 0x5a, 8);
    uc_assert_err(UC_ERR_WRITE_UNMAPPED,
                  uc_mem_write(uc, hole_address + 0xff8,
                               input, sizeof(input)));
    for (i = 0; i < 8; i++) {
        TEST_CHECK(before_hole[0xff8 + i] == 0x5a);
    }
    memset(output, 0xa5, sizeof(output));
    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_mem_read(uc, hole_address + 0xff8,
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
    uc_engine *uc;
    uint8_t mem[0x1000];

    memset(mem, 0xff, 0x100);

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));

    OK(uc_mem_map(uc, 0xfffffffffffff000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0xfffffffffffff000, mem, sizeof(mem)));

    uc_assert_err(UC_ERR_WRITE_UNMAPPED,
                  uc_mem_write(uc, 0xffffffffffffff00, mem, sizeof(mem)));
    uc_assert_err(UC_ERR_WRITE_UNMAPPED, uc_mem_write(uc, 0, mem, sizeof(mem)));

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
                                                  size_t size, void *data)
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
    TestActiveCodePageData *data =
        (TestActiveCodePageData *)user_data;

    data->count++;
    if (data->action == TEST_ACTIVE_CODE_PROTECT) {
        OK(uc_mem_protect(uc, 0x1000, 0x1000,
                          UC_PROT_READ | UC_PROT_WRITE));
    } else {
        OK(uc_mem_unmap(uc, 0x1000, 0x1000));
    }
}

static void test_active_code_page_change_one(TestActiveCodePageAction action)
{
    const char code[] = "\x40\x40";
    TestActiveCodePageData data = { .action = action };
    uint32_t eax = 0;
    uint32_t eip;
    uc_engine *uc;
    uc_hook hook;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, 0x1000, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, 0x1000, code, sizeof(code) - 1));
    OK(uc_reg_write(uc, UC_X86_REG_EAX, &eax));
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE,
                   test_active_code_page_callback, &data,
                   0x1000, 0x1000));

    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(code) - 1, 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    TEST_CHECK(data.count == 1);
    TEST_CHECK(eax == 0);
    TEST_CHECK(eip == 0x1000);

    if (action == TEST_ACTIVE_CODE_PROTECT) {
        uc_assert_err(UC_ERR_FETCH_PROT,
                      uc_emu_start(uc, 0x1000,
                                   0x1000 + sizeof(code) - 1, 0, 0));
    } else {
        uc_assert_err(UC_ERR_FETCH_UNMAPPED,
                      uc_emu_start(uc, 0x1000,
                                   0x1000 + sizeof(code) - 1, 0, 0));
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
        OK(uc_mem_protect(uc, 0x1000, 0x1000,
                          UC_PROT_READ | UC_PROT_WRITE));
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

static void test_snapshot_code_restore_callback(uc_engine *uc,
                                                uint64_t address,
                                                uint32_t size,
                                                void *user_data)
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
    const uint8_t snapshot_code[] = { 0x90, 0x43 };
    const uint8_t live_code[] = { 0x90, 0x40 };
    uint8_t restored_code[sizeof(snapshot_code)];
    TestSnapshotCodeRestoreData data = { 0 };
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
    OK(uc_hook_add(uc, &hook, UC_HOOK_CODE,
                   test_snapshot_code_restore_callback, &data,
                   0x1000, 0x1000));

    OK(uc_emu_start(uc, 0x1000, 0x1000 + sizeof(live_code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_reg_read(uc, UC_X86_REG_EBX, &ebx));
    OK(uc_mem_read(uc, 0x1000, restored_code, sizeof(restored_code)));
    TEST_CHECK(data.count == 2);
    TEST_CHECK(eax == 0);
    TEST_CHECK(ebx == 1);
    TEST_CHECK(memcmp(restored_code, snapshot_code,
                      sizeof(snapshot_code)) == 0);

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
    OK(uc_context_restore(uc, c1));
    // TODO check mem
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 1);
    OK(uc_context_restore(uc, c0));
    OK(uc_mem_read(uc, 0x2020, &mem, sizeof(mem)));
    TEST_CHECK(LEINT32(mem) == 0);
    // TODO check mem

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

static bool test_virtual_write_tlb_fill(uc_engine *uc, uint64_t addr, uc_mem_type type,
                                        uc_tlb_entry *result, void *user_data)
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
    char code[] = { 0x48, 0x8B, 0x04, 0x25, 0x00, 0x20, 0x00, 0x00 };

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL));
    OK(uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL, test_virtual_write_tlb_fill, NULL, 1, 0));
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
    OK(uc_mem_map(uc, 0,                     0x1000, UC_PROT_ALL));
    OK(uc_mem_map(uc, 0xFFFFFFFFFFFFF000ULL, 0x1000, UC_PROT_ALL));

    uc_assert_err(UC_ERR_READ_UNMAPPED,
        uc_mem_read(uc, 0xFFFFFFFFFFFFF000ULL, buf, 0x2000));
    uc_assert_err(UC_ERR_WRITE_UNMAPPED,
        uc_mem_write(uc, 0xFFFFFFFFFFFFF000ULL, buf, 0x2000));
    uc_assert_err(UC_ERR_NOMEM,
        uc_mem_unmap(uc, 0xFFFFFFFFFFFFF000ULL, 0x2000));
    uc_assert_err(UC_ERR_NOMEM,
        uc_mem_protect(uc, 0xFFFFFFFFFFFFF000ULL, 0x2000, UC_PROT_READ));

    // The non-wrapping single-page case must still work for both regions.
    OK(uc_mem_read(uc, 0xFFFFFFFFFFFFF000ULL, buf, 0x1000));
    OK(uc_mem_read(uc, 0,                     buf, 0x1000));

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
    OK(uc_mem_map  (uc, 0x0,    0x1000, UC_PROT_ALL));                // text
    OK(uc_mem_map  (uc, 0x4000, 0x1000, UC_PROT_READ|UC_PROT_WRITE)); // stack
    OK(uc_mem_write(uc, 0x0,    code,   sizeof(code)-1));
    OK(uc_reg_write(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_reg_write(uc, UC_X86_REG_RSP, &r_rsp));
    OK(uc_emu_start(uc, 0x4, sizeof(code)-1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_RAX, &r_rax));
    OK(uc_mem_read(uc, 0x0, code, sizeof(code)-1));
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
    OK(uc_reg_write  (uc, UC_X86_REG_EAX, &eax));
    OK(uc_mem_map    (uc, 0x0, 0x1000, UC_PROT_READ|UC_PROT_WRITE));
    OK(uc_mem_write  (uc, 0x0, code,   sizeof(code)-1));
    OK(uc_mem_protect(uc, 0x0, 0x1000, UC_PROT_READ|UC_PROT_EXEC));
    OK(uc_emu_start  (uc, 0x0, sizeof(code)-1, 0, 0));

    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    TEST_CHECK(eax == 42);

    OK(uc_close(uc));
}

TEST_LIST = {{"test_map_correct", test_map_correct},
             {"test_map_wrapping", test_map_wrapping},
             {"test_mem_protect", test_mem_protect},
             {"test_splitting_mem_unmap", test_splitting_mem_unmap},
             {"test_splitting_mmio_unmap", test_splitting_mmio_unmap},
             {"test_mem_protect_map_ptr", test_mem_protect_map_ptr},
             {"test_mem_cross_region_access", test_mem_cross_region_access},
             {"test_mem_regions_topology", test_mem_regions_topology},
             {"test_map_at_the_end", test_map_at_the_end},
             {"test_map_wrap", test_map_wrap},
             {"test_map_big_memory", test_map_big_memory},
             {"test_mem_protect_remove_exec", test_mem_protect_remove_exec},
             {"test_active_code_page_change", test_active_code_page_change},
             {"test_inactive_code_page_change",
              test_inactive_code_page_change},
             {"test_mem_protect_mmio", test_mem_protect_mmio},
             {"test_snapshot", test_snapshot},
             {"test_snapshot_code_restore_from_callback",
              test_snapshot_code_restore_from_callback},
             {"test_snapshot_with_vtlb", test_snapshot_with_vtlb},
             {"test_context_snapshot", test_context_snapshot},
             {"test_snapshot_unmap", test_snapshot_unmap},
             {"test_mem_read_and_write_large_memory_block",
              test_mem_read_and_write_large_memory_block},
             {"test_virtual_to_physical", test_virtual_to_physical},
             {"test_virtual_write", test_virtual_write},
             {"test_mem_addr_size_wraparound", test_mem_addr_size_wraparound},
             {"test_smc", test_smc},
             {"test_tlbdirty_exec", test_tlbdirty_exec},
             {NULL, NULL}};
