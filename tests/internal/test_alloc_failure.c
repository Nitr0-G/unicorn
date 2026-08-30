#include "unicorn_test.h"
#include "uc_priv.h"

#include <string.h>

typedef enum TestMapKind {
    TEST_MAP_RAM,
    TEST_MAP_HOST,
    TEST_MAP_MMIO,
} TestMapKind;

static void fail_next_allocation(uc_engine *uc, UcTestAllocFailSite site)
{
    TEST_CHECK(uc->test_alloc_fail_site == UC_TEST_ALLOC_FAIL_NONE);
    uc->test_alloc_fail_site = site;
}

static void check_failure_consumed(uc_engine *uc,
                                   UcTestAllocFailSite site)
{
    if (!TEST_CHECK(uc->test_alloc_fail_site == UC_TEST_ALLOC_FAIL_NONE)) {
        TEST_MSG("failure site %d was not reached", (int)site);
    }
}

static bool fail_flatview_copy(struct uc_struct *uc, FlatView *dst,
                               FlatView *src, bool update_dispatcher)
{
    (void)uc;
    (void)dst;
    (void)src;
    (void)update_dispatcher;
    return false;
}

static bool fail_flatview_reserve(FlatView *view, unsigned int count)
{
    (void)view;
    (void)count;
    return false;
}

static MemoryRegion *fail_guest_memory_cow(struct uc_struct *uc,
                                           UcMapping *mapping,
                                           MemoryRegion *current,
                                           hwaddr begin, size_t size)
{
    (void)uc;
    (void)mapping;
    (void)current;
    (void)begin;
    (void)size;
    return NULL;
}

static void check_regions(uc_engine *uc, const uc_mem_region *expected,
                          uint32_t expected_count)
{
    uc_mem_region *actual = NULL;
    uint32_t actual_count = 0;
    uc_err err = uc_mem_regions(uc, &actual, &actual_count);
    uint32_t i;

    if (!TEST_CHECK(err == UC_ERR_OK)) {
        TEST_MSG("uc_mem_regions failed: %s", uc_strerror(err));
        return;
    }
    if (TEST_CHECK(actual_count == expected_count)) {
        for (i = 0; i < expected_count; i++) {
            TEST_CHECK(actual[i].begin == expected[i].begin);
            TEST_CHECK(actual[i].end == expected[i].end);
            TEST_CHECK(actual[i].perms == expected[i].perms);
        }
    }
    OK(uc_free(actual));
}

static void check_byte(uc_engine *uc, uint64_t address, uint8_t expected)
{
    uint8_t actual = 0;

    OK(uc_mem_read(uc, address, &actual, sizeof(actual)));
    TEST_CHECK(actual == expected);
}

static void check_unmapped(uc_engine *uc, uint64_t address)
{
    uint8_t value = 0;

    uc_assert_err(UC_ERR_READ_UNMAPPED,
                  uc_mem_read(uc, address, &value, sizeof(value)));
}

static uc_err map_memory(uc_engine *uc, TestMapKind kind, uint64_t address,
                         void *host_memory)
{
    switch (kind) {
    case TEST_MAP_RAM:
        return uc_mem_map(uc, address, 0x1000, UC_PROT_ALL);
    case TEST_MAP_HOST:
        return uc_mem_map_ptr(uc, address, 0x1000, UC_PROT_ALL, host_memory);
    case TEST_MAP_MMIO:
        return uc_mmio_map(uc, address, 0x1000, NULL, NULL, NULL, NULL);
    default:
        return UC_ERR_ARG;
    }
}

static void run_map_failure(UcTestAllocFailSite site)
{
    const uint64_t address = 0x1000;
    TestMapKind kind;

    for (kind = TEST_MAP_RAM; kind <= TEST_MAP_MMIO; kind++) {
        const uint32_t perms = kind == TEST_MAP_MMIO ? 0 : UC_PROT_ALL;
        const uc_mem_region expected = {address, address + 0xfff, perms};
        uint8_t host_memory[0x1000];
        uc_engine *uc;
        uc_err err;

        memset(host_memory, 0, sizeof(host_memory));
        OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
        fail_next_allocation(uc, site);
        err = map_memory(uc, kind, address, host_memory);
        if (!TEST_CHECK(err == UC_ERR_NOMEM)) {
            TEST_MSG("map kind %d returned %s", (int)kind, uc_strerror(err));
        }
        check_failure_consumed(uc, site);
        check_regions(uc, NULL, 0);
        check_unmapped(uc, address);

        OK(map_memory(uc, kind, address, host_memory));
        check_regions(uc, &expected, 1);
        OK(uc_mem_unmap(uc, address, 0x1000));
        OK(uc_close(uc));
    }
}

static void test_map_block_reservation_failure(void)
{
    run_map_failure(UC_TEST_ALLOC_FAIL_MAPPED_BLOCKS);
}

static void test_mapping_record_allocation_failure(void)
{
    run_map_failure(UC_TEST_ALLOC_FAIL_MAPPING_RECORD);
}

static void run_context_save_failure(UcTestAllocFailSite site,
                                     bool fail_ranges)
{
    const uint64_t saved_address = 0x1000;
    const uint64_t current_address = 0x3000;
    const uint8_t saved_value = 0x11;
    const uint8_t current_value = 0x22;
    const uint8_t extra_value = 0x33;
    const uc_mem_region current_regions[] = {
        {saved_address, saved_address + 0xfff, UC_PROT_ALL},
        {current_address, current_address + 0xfff, UC_PROT_ALL},
    };
    const uc_mem_region saved_region = {
        saved_address, saved_address + 0xfff, UC_PROT_ALL};
    uc_flatview_copy_t flatview_copy = NULL;
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, saved_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, saved_address, &saved_value, sizeof(saved_value)));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_mem_write(uc, saved_address, &current_value,
                    sizeof(current_value)));
    OK(uc_mem_map(uc, current_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, current_address, &extra_value, sizeof(extra_value)));

    if (fail_ranges) {
        flatview_copy = uc->flatview_copy;
        uc->flatview_copy = fail_flatview_copy;
    } else {
        fail_next_allocation(uc, site);
    }
    uc_assert_err(UC_ERR_NOMEM, uc_context_save(uc, context));
    if (fail_ranges) {
        uc->flatview_copy = flatview_copy;
    } else {
        check_failure_consumed(uc, site);
    }
    check_regions(uc, current_regions, 2);
    check_byte(uc, saved_address, current_value);
    check_byte(uc, current_address, extra_value);

    OK(uc_context_restore(uc, context));
    check_regions(uc, &saved_region, 1);
    check_byte(uc, saved_address, saved_value);
    check_unmapped(uc, current_address);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_context_save_reservation_failures(void)
{
    static const UcTestAllocFailSite sites[] = {
        UC_TEST_ALLOC_FAIL_CONTEXT_VIEW,
        UC_TEST_ALLOC_FAIL_CONTEXT_MAPPINGS,
        UC_TEST_ALLOC_FAIL_CONTEXT_REGIONS,
    };
    size_t i;

    for (i = 0; i < sizeof(sites) / sizeof(sites[0]); i++) {
        run_context_save_failure(sites[i], false);
    }
    run_context_save_failure(UC_TEST_ALLOC_FAIL_NONE, true);
}

static void run_context_restore_failure(UcTestAllocFailSite site,
                                        bool fail_ranges)
{
    const uint64_t first_saved_address = 0x1000;
    const uint64_t second_saved_address = 0x3000;
    const uint64_t current_address = 0x5000;
    const uint8_t first_saved_value = 0x41;
    const uint8_t second_saved_value = 0x52;
    const uint8_t current_value = 0x63;
    const uc_mem_region saved_regions[] = {
        {first_saved_address, first_saved_address + 0xfff, UC_PROT_ALL},
        {second_saved_address, second_saved_address + 0xfff, UC_PROT_ALL},
    };
    const uc_mem_region current_region = {
        current_address, current_address + 0xfff, UC_PROT_ALL};
    uc_flatview_reserve_t flatview_reserve = NULL;
    uc_context *context;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    OK(uc_mem_map(uc, first_saved_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_map(uc, second_saved_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, first_saved_address, &first_saved_value,
                    sizeof(first_saved_value)));
    OK(uc_mem_write(uc, second_saved_address, &second_saved_value,
                    sizeof(second_saved_value)));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    OK(uc_mem_unmap(uc, first_saved_address, 0x1000));
    OK(uc_mem_unmap(uc, second_saved_address, 0x1000));
    OK(uc_mem_map(uc, current_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, current_address, &current_value,
                    sizeof(current_value)));
    check_regions(uc, &current_region, 1);

    if (site == UC_TEST_ALLOC_FAIL_MAPPED_BLOCKS) {
        TEST_CHECK(uc->mapped_block_count == 1);
        /* Force restore to grow an otherwise valid active-mapping array. */
        uc->mapped_block_capacity = uc->mapped_block_count;
    }
    if (fail_ranges) {
        flatview_reserve = uc->flatview_reserve;
        uc->flatview_reserve = fail_flatview_reserve;
    } else {
        fail_next_allocation(uc, site);
    }
    uc_assert_err(UC_ERR_NOMEM, uc_context_restore(uc, context));
    if (fail_ranges) {
        uc->flatview_reserve = flatview_reserve;
    } else {
        check_failure_consumed(uc, site);
    }
    check_regions(uc, &current_region, 1);
    check_byte(uc, current_address, current_value);
    check_unmapped(uc, first_saved_address);
    check_unmapped(uc, second_saved_address);

    OK(uc_context_restore(uc, context));
    check_regions(uc, saved_regions, 2);
    check_byte(uc, first_saved_address, first_saved_value);
    check_byte(uc, second_saved_address, second_saved_value);
    check_unmapped(uc, current_address);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

static void test_context_restore_view_failure(void)
{
    run_context_restore_failure(UC_TEST_ALLOC_FAIL_RESTORE_VIEW, false);
}

static void test_context_restore_mapping_reservation_failure(void)
{
    run_context_restore_failure(UC_TEST_ALLOC_FAIL_MAPPED_BLOCKS, false);
}

static void test_context_restore_range_reservation_failure(void)
{
    run_context_restore_failure(UC_TEST_ALLOC_FAIL_NONE, true);
}

static void test_guest_cow_failure_exit(void)
{
    const uint64_t code_address = 0x1000;
    const uint64_t data_address = 0x3000;
    const uint8_t code[] = {
        0xc7, 0x05, 0x00, 0x30, 0x00, 0x00,
        0x78, 0x56, 0x34, 0x12, /* mov dword [0x3000],0x12345678 */
        0x40,                   /* inc eax */
    };
    const uint32_t initial_value = 0x44332211;
    const uint32_t stored_value = 0x12345678;
    uc_mem_cow_t memory_cow;
    uc_context *context;
    uint32_t actual = 0;
    uint32_t eax = 0;
    uint32_t eip = 0;
    uc_engine *uc;

    OK(uc_open(UC_ARCH_X86, UC_MODE_32, &uc));
    OK(uc_mem_map(uc, code_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_map(uc, data_address, 0x1000, UC_PROT_ALL));
    OK(uc_mem_write(uc, code_address, code, sizeof(code)));
    OK(uc_mem_write(uc, data_address, &initial_value,
                    sizeof(initial_value)));
    OK(uc_ctl_context_mode(uc, UC_CTL_CONTEXT_MEMORY));
    OK(uc_context_alloc(uc, &context));
    OK(uc_context_save(uc, context));

    memory_cow = uc->memory_cow;
    uc->memory_cow = fail_guest_memory_cow;
    uc_assert_err(UC_ERR_NOMEM,
                  uc_emu_start(uc, code_address,
                               code_address + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EIP, &eip));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_mem_read(uc, data_address, &actual, sizeof(actual)));
    TEST_CHECK(eip == code_address);
    TEST_CHECK(eax == 0);
    TEST_CHECK(actual == initial_value);

    uc->memory_cow = memory_cow;
    OK(uc_emu_start(uc, eip, code_address + sizeof(code), 0, 0));
    OK(uc_reg_read(uc, UC_X86_REG_EAX, &eax));
    OK(uc_mem_read(uc, data_address, &actual, sizeof(actual)));
    TEST_CHECK(eax == 1);
    TEST_CHECK(actual == stored_value);

    OK(uc_context_free(context));
    OK(uc_close(uc));
}

TEST_LIST = {
    {"test_map_block_reservation_failure",
     test_map_block_reservation_failure},
    {"test_mapping_record_allocation_failure",
     test_mapping_record_allocation_failure},
    {"test_context_save_reservation_failures",
     test_context_save_reservation_failures},
    {"test_context_restore_view_failure",
     test_context_restore_view_failure},
    {"test_context_restore_mapping_reservation_failure",
     test_context_restore_mapping_reservation_failure},
    {"test_context_restore_range_reservation_failure",
     test_context_restore_range_reservation_failure},
    {"test_guest_cow_failure_exit", test_guest_cow_failure_exit},
    {NULL, NULL}};
