#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <time.h>
#endif

#include <unicorn/unicorn.h>
#include <unicorn/x86.h>

#define CODE_ADDRESS UINT64_C(0x00100000)
#define DATA_ADDRESS UINT64_C(0x02001000)
#define PAGE_SIZE UINT64_C(0x1000)
#define DENSE_SIZE (PAGE_SIZE * UINT64_C(16))
#define CONFLICT_STRIDE (PAGE_SIZE * UINT64_C(256))
#define CONFLICT_PAGES UINT64_C(16)
#define DATA_MAP_SIZE (CONFLICT_STRIDE * CONFLICT_PAGES)
#define DENSE_LOADS_PER_ITERATION UINT64_C(8)

#define DEFAULT_WARMUP 2
#define DEFAULT_REPEATS 7
#define DEFAULT_LOADS UINT64_C(1048576)

typedef enum ReadHookMode {
    READ_HOOK_NONE,
    READ_HOOK_GLOBAL,
    READ_HOOK_BOUNDED,
} ReadHookMode;

typedef struct BenchmarkDefinition {
    const char *name;
    ReadHookMode read_hook;
    bool virtual_tlb;
    bool conflict_access;
    bool recreate_engine;
} BenchmarkDefinition;

typedef struct CallbackCounts {
    uint64_t hook_reads;
    uint64_t tlb_fills;
    uint64_t read_fills;
    uint64_t fetch_fills;
} CallbackCounts;

typedef struct RunSample {
    double milliseconds;
    CallbackCounts counts;
} RunSample;

typedef struct Options {
    const char *mode;
    size_t warmup;
    size_t repeats;
    uint64_t loads;
} Options;

typedef struct CounterRange {
    uint64_t minimum;
    uint64_t maximum;
} CounterRange;

static const BenchmarkDefinition benchmark_definitions[] = {
    { "dense", READ_HOOK_NONE, false, false, false },
    { "hook-global", READ_HOOK_GLOBAL, false, false, false },
    { "hook-bounded", READ_HOOK_BOUNDED, false, false, false },
    { "vtlb-dense", READ_HOOK_NONE, true, false, false },
    { "vtlb-conflict", READ_HOOK_NONE, true, true, false },
    { "vtlb-conflict-cold", READ_HOOK_NONE, true, true, true },
};

/*
 * Eight adjacent 64-bit loads, followed by a 64-byte step through a 64 KiB
 * ring. RCX contains the number of eight-load groups.
 */
static const uint8_t dense_code[] = {
    0x48, 0x8b, 0x06,
    0x48, 0x8b, 0x46, 0x08,
    0x48, 0x8b, 0x46, 0x10,
    0x48, 0x8b, 0x46, 0x18,
    0x48, 0x8b, 0x46, 0x20,
    0x48, 0x8b, 0x46, 0x28,
    0x48, 0x8b, 0x46, 0x30,
    0x48, 0x8b, 0x46, 0x38,
    0x48, 0x83, 0xc6, 0x40,
    0x48, 0x39, 0xfe,
    0x75, 0x03,
    0x48, 0x89, 0xde,
    0x48, 0xff, 0xc9,
    0x75, 0xd0,
};

/*
 * One load from each of 16 pages separated by 1 MiB. The pages have the same
 * low virtual-page index and outnumber the QEMU victim TLB entries.
 */
static const uint8_t conflict_code[] = {
    0x48, 0x8b, 0x06,
    0x48, 0x81, 0xc6, 0x00, 0x00, 0x10, 0x00,
    0x48, 0x39, 0xfe,
    0x75, 0x03,
    0x48, 0x89, 0xde,
    0x48, 0xff, 0xc9,
    0x75, 0xe9,
};

static double monotonic_seconds(void)
{
#ifdef _WIN32
    static LARGE_INTEGER frequency;
    static bool initialized;
    LARGE_INTEGER counter;

    if (!initialized) {
        QueryPerformanceFrequency(&frequency);
        initialized = true;
    }
    QueryPerformanceCounter(&counter);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec now;

    clock_gettime(CLOCK_MONOTONIC, &now);
    return (double)now.tv_sec + (double)now.tv_nsec / 1000000000.0;
#endif
}

static void mem_read_callback(uc_engine *uc, uc_mem_type type,
                              uint64_t address, int size, int64_t value,
                              void *user_data)
{
    CallbackCounts *counts = user_data;

    (void)uc;
    (void)type;
    (void)address;
    (void)size;
    (void)value;
    counts->hook_reads++;
}

static bool tlb_fill_callback(uc_engine *uc, uint64_t address,
                              uc_mem_type type, uc_tlb_entry *result,
                              void *user_data)
{
    CallbackCounts *counts = user_data;

    (void)uc;
    counts->tlb_fills++;
    if (type == UC_MEM_READ) {
        counts->read_fills++;
    } else if (type == UC_MEM_FETCH) {
        counts->fetch_fills++;
    }

    result->paddr = address;
    result->perms = UC_PROT_ALL;
    return true;
}

static bool report_uc_error(const char *operation, uc_err err)
{
    if (err == UC_ERR_OK) {
        return true;
    }

    fprintf(stderr, "%s failed: %s\n", operation, uc_strerror(err));
    return false;
}

static const BenchmarkDefinition *find_benchmark(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(benchmark_definitions) /
                        sizeof(benchmark_definitions[0]); i++) {
        if (strcmp(name, benchmark_definitions[i].name) == 0) {
            return &benchmark_definitions[i];
        }
    }
    return NULL;
}

static bool parse_u64(const char *text, uint64_t *value)
{
    char *end;
    unsigned long long parsed;

    if (*text == '-') {
        return false;
    }

    errno = 0;
    end = NULL;
    parsed = strtoull(text, &end, 10);
    if (errno != 0 || end == text || *end != '\0') {
        return false;
    }
    *value = (uint64_t)parsed;
    return true;
}

static void print_usage(const char *program)
{
    fprintf(stderr,
            "usage: %s [all|dense|hook-global|hook-bounded|vtlb-dense|"
            "vtlb-conflict|vtlb-conflict-cold]\n"
            "          [--warmup N] [--repeats N] [--loads N]\n",
            program);
}

static int parse_arguments(int argc, char **argv, Options *options)
{
    int argument;

    options->mode = "all";
    options->warmup = DEFAULT_WARMUP;
    options->repeats = DEFAULT_REPEATS;
    options->loads = DEFAULT_LOADS;
    argument = 1;

    if (argument < argc && argv[argument][0] != '-') {
        options->mode = argv[argument++];
    }

    while (argument < argc) {
        const char *name = argv[argument++];
        uint64_t parsed;

        if (strcmp(name, "--help") == 0 || strcmp(name, "-h") == 0) {
            print_usage(argv[0]);
            return 1;
        }
        if (argument >= argc) {
            fprintf(stderr, "missing value after %s\n", name);
            return -1;
        }
        if (!parse_u64(argv[argument++], &parsed)) {
            fprintf(stderr, "invalid numeric value for %s\n", name);
            return -1;
        }

        if (strcmp(name, "--warmup") == 0) {
            if (parsed > SIZE_MAX) {
                fprintf(stderr, "--warmup is too large\n");
                return -1;
            }
            options->warmup = (size_t)parsed;
        } else if (strcmp(name, "--repeats") == 0) {
            if (parsed == 0 || parsed > SIZE_MAX) {
                fprintf(stderr, "--repeats must be between 1 and SIZE_MAX\n");
                return -1;
            }
            options->repeats = (size_t)parsed;
        } else if (strcmp(name, "--loads") == 0) {
            if (parsed == 0) {
                fprintf(stderr, "--loads must be greater than zero\n");
                return -1;
            }
            options->loads = parsed;
        } else {
            fprintf(stderr, "unknown option: %s\n", name);
            return -1;
        }
    }

    if (strcmp(options->mode, "all") != 0 &&
        find_benchmark(options->mode) == NULL) {
        fprintf(stderr, "unknown mode: %s\n", options->mode);
        return -1;
    }
    return 0;
}

static bool setup_engine(const BenchmarkDefinition *definition,
                         CallbackCounts *counts, uc_engine **result)
{
    const uint8_t *code;
    size_t code_size;
    uint64_t hook_begin;
    uint64_t hook_end;
    uc_engine *uc;
    uc_hook hook;
    uc_err err;

    uc = NULL;
    err = uc_open(UC_ARCH_X86, UC_MODE_64, &uc);
    if (!report_uc_error("uc_open", err)) {
        return false;
    }

    if (definition->virtual_tlb) {
        err = uc_ctl_tlb_mode(uc, UC_TLB_VIRTUAL);
        if (!report_uc_error("uc_ctl_tlb_mode", err)) {
            uc_close(uc);
            return false;
        }
        err = uc_hook_add(uc, &hook, UC_HOOK_TLB_FILL,
                          (void *)tlb_fill_callback, counts, 1, 0);
        if (!report_uc_error("uc_hook_add(UC_HOOK_TLB_FILL)", err)) {
            uc_close(uc);
            return false;
        }
    }

    err = uc_mem_map(uc, CODE_ADDRESS, PAGE_SIZE,
                     UC_PROT_READ | UC_PROT_EXEC);
    if (!report_uc_error("uc_mem_map(code)", err)) {
        uc_close(uc);
        return false;
    }
    err = uc_mem_map(uc, DATA_ADDRESS, DATA_MAP_SIZE, UC_PROT_READ);
    if (!report_uc_error("uc_mem_map(data)", err)) {
        uc_close(uc);
        return false;
    }

    if (definition->conflict_access) {
        code = conflict_code;
        code_size = sizeof(conflict_code);
    } else {
        code = dense_code;
        code_size = sizeof(dense_code);
    }
    err = uc_mem_write(uc, CODE_ADDRESS, code, code_size);
    if (!report_uc_error("uc_mem_write(code)", err)) {
        uc_close(uc);
        return false;
    }

    if (definition->read_hook != READ_HOOK_NONE) {
        if (definition->read_hook == READ_HOOK_GLOBAL) {
            hook_begin = 1;
            hook_end = 0;
        } else {
            hook_begin = DATA_ADDRESS;
            hook_end = DATA_ADDRESS + PAGE_SIZE - 1;
        }
        err = uc_hook_add(uc, &hook, UC_HOOK_MEM_READ,
                          (void *)mem_read_callback, counts, hook_begin,
                          hook_end);
        if (!report_uc_error("uc_hook_add(UC_HOOK_MEM_READ)", err)) {
            uc_close(uc);
            return false;
        }
    }

    *result = uc;
    return true;
}

static bool run_once(uc_engine *uc, const BenchmarkDefinition *definition,
                     uint64_t loads, double *milliseconds)
{
    uint64_t base;
    uint64_t current;
    uint64_t end;
    uint64_t iterations;
    uint64_t remaining;
    size_t code_size;
    double start;
    double finish;
    uc_err err;

    base = DATA_ADDRESS;
    current = base;
    if (definition->conflict_access) {
        end = base + CONFLICT_STRIDE * CONFLICT_PAGES;
        iterations = loads;
        code_size = sizeof(conflict_code);
    } else {
        end = base + DENSE_SIZE;
        iterations = loads / DENSE_LOADS_PER_ITERATION;
        code_size = sizeof(dense_code);
    }

    err = uc_reg_write(uc, UC_X86_REG_RBX, &base);
    if (!report_uc_error("uc_reg_write(RBX)", err)) {
        return false;
    }
    err = uc_reg_write(uc, UC_X86_REG_RSI, &current);
    if (!report_uc_error("uc_reg_write(RSI)", err)) {
        return false;
    }
    err = uc_reg_write(uc, UC_X86_REG_RDI, &end);
    if (!report_uc_error("uc_reg_write(RDI)", err)) {
        return false;
    }
    err = uc_reg_write(uc, UC_X86_REG_RCX, &iterations);
    if (!report_uc_error("uc_reg_write(RCX)", err)) {
        return false;
    }

    start = monotonic_seconds();
    err = uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + code_size, 0, 0);
    finish = monotonic_seconds();
    if (!report_uc_error("uc_emu_start", err)) {
        return false;
    }
    err = uc_reg_read(uc, UC_X86_REG_RCX, &remaining);
    if (!report_uc_error("uc_reg_read(RCX)", err)) {
        return false;
    }
    if (remaining != 0) {
        fprintf(stderr, "%s stopped with RCX=%" PRIu64 "\n",
                definition->name, remaining);
        return false;
    }

    *milliseconds = (finish - start) * 1000.0;
    return true;
}

static int compare_sample_time(const void *left, const void *right)
{
    const RunSample *a = left;
    const RunSample *b = right;

    return (a->milliseconds > b->milliseconds) -
           (a->milliseconds < b->milliseconds);
}

static CounterRange counter_range(const RunSample *samples, size_t count,
                                  size_t member_offset)
{
    CounterRange range;
    size_t i;

    range.minimum = UINT64_MAX;
    range.maximum = 0;
    for (i = 0; i < count; i++) {
        const uint8_t *counts = (const uint8_t *)&samples[i].counts;
        const uint64_t *value = (const uint64_t *)(counts + member_offset);

        if (*value < range.minimum) {
            range.minimum = *value;
        }
        if (*value > range.maximum) {
            range.maximum = *value;
        }
    }
    return range;
}

static void print_summary(const BenchmarkDefinition *definition,
                          RunSample *samples, size_t count,
                          uint64_t loads)
{
    CounterRange hook_reads;
    CounterRange tlb_fills;
    CounterRange read_fills;
    CounterRange fetch_fills;
    double median;
    double minimum;
    double maximum;
    double rate;
    size_t i;

    minimum = samples[0].milliseconds;
    maximum = samples[0].milliseconds;
    for (i = 1; i < count; i++) {
        if (samples[i].milliseconds < minimum) {
            minimum = samples[i].milliseconds;
        }
        if (samples[i].milliseconds > maximum) {
            maximum = samples[i].milliseconds;
        }
    }
    hook_reads = counter_range(samples, count,
                               offsetof(CallbackCounts, hook_reads));
    tlb_fills = counter_range(samples, count,
                              offsetof(CallbackCounts, tlb_fills));
    read_fills = counter_range(samples, count,
                               offsetof(CallbackCounts, read_fills));
    fetch_fills = counter_range(samples, count,
                                offsetof(CallbackCounts, fetch_fills));

    qsort(samples, count, sizeof(*samples), compare_sample_time);
    if ((count & 1) != 0) {
        median = samples[count / 2].milliseconds;
    } else {
        median = (samples[count / 2 - 1].milliseconds +
                  samples[count / 2].milliseconds) / 2.0;
    }
    rate = (double)loads / (median * 1000.0);

    printf("%-14s median=%9.3f ms  min=%9.3f  max=%9.3f  "
           "rate=%9.3f Mload/s\n",
           definition->name, median, minimum, maximum, rate);
    printf("               hook_reads=%" PRIu64 "..%" PRIu64
           "  tlb_fills=%" PRIu64 "..%" PRIu64
           "  read=%" PRIu64 "..%" PRIu64
           "  fetch=%" PRIu64 "..%" PRIu64 "\n",
           hook_reads.minimum, hook_reads.maximum, tlb_fills.minimum,
           tlb_fills.maximum, read_fills.minimum, read_fills.maximum,
           fetch_fills.minimum, fetch_fills.maximum);
}

static bool run_benchmark(const BenchmarkDefinition *definition,
                          const Options *options)
{
    CallbackCounts counts;
    RunSample *samples;
    uc_engine *uc;
    uc_err err;
    size_t i;
    bool success;

    if (!definition->conflict_access &&
        options->loads % DENSE_LOADS_PER_ITERATION != 0) {
        fprintf(stderr,
                "%s requires --loads to be a multiple of %" PRIu64 "\n",
                definition->name, DENSE_LOADS_PER_ITERATION);
        return false;
    }
    if (options->repeats > SIZE_MAX / sizeof(*samples)) {
        fprintf(stderr, "--repeats is too large\n");
        return false;
    }

    memset(&counts, 0, sizeof(counts));
    uc = NULL;
    samples = calloc(options->repeats, sizeof(*samples));
    if (samples == NULL) {
        fprintf(stderr, "failed to allocate sample storage\n");
        return false;
    }
    if (!definition->recreate_engine &&
        !setup_engine(definition, &counts, &uc)) {
        free(samples);
        return false;
    }

    success = false;
    for (i = 0; i < options->warmup; i++) {
        memset(&counts, 0, sizeof(counts));
        if (definition->recreate_engine) {
            if (uc != NULL) {
                uc_close(uc);
                uc = NULL;
            }
            if (!setup_engine(definition, &counts, &uc)) {
                goto cleanup;
            }
            err = UC_ERR_OK;
        } else {
            err = uc_ctl_flush_tlb(uc);
        }
        if (!report_uc_error("prepare warmup", err) ||
            !run_once(uc, definition, options->loads,
                      &samples[0].milliseconds)) {
            goto cleanup;
        }
    }

    for (i = 0; i < options->repeats; i++) {
        memset(&counts, 0, sizeof(counts));
        if (definition->recreate_engine) {
            if (uc != NULL) {
                uc_close(uc);
                uc = NULL;
            }
            if (!setup_engine(definition, &counts, &uc)) {
                goto cleanup;
            }
            err = UC_ERR_OK;
        } else {
            err = uc_ctl_flush_tlb(uc);
        }
        if (!report_uc_error("prepare repeat", err) ||
            !run_once(uc, definition, options->loads,
                      &samples[i].milliseconds)) {
            goto cleanup;
        }
        samples[i].counts = counts;
    }

    print_summary(definition, samples, options->repeats, options->loads);
    success = true;

cleanup:
    if (uc != NULL) {
        err = uc_close(uc);
        if (!report_uc_error("uc_close", err)) {
            success = false;
        }
    }
    free(samples);
    return success;
}

int main(int argc, char **argv)
{
    Options options;
    const BenchmarkDefinition *definition;
    unsigned int major;
    unsigned int minor;
    size_t i;
    int parse_result;

    parse_result = parse_arguments(argc, argv, &options);
    if (parse_result != 0) {
        if (parse_result < 0) {
            print_usage(argv[0]);
            return EXIT_FAILURE;
        }
        return EXIT_SUCCESS;
    }

    uc_version(&major, &minor);
    printf("Unicorn %u.%u, loads=%" PRIu64 ", warmup=%zu, repeats=%zu\n",
           major, minor, options.loads, options.warmup, options.repeats);
    printf("dense=64 KiB, bounded hook=first 4 KiB, "
           "conflicts=16 pages x 1 MiB stride\n");

    if (strcmp(options.mode, "all") != 0) {
        definition = find_benchmark(options.mode);
        return run_benchmark(definition, &options) ?
               EXIT_SUCCESS : EXIT_FAILURE;
    }

    for (i = 0; i < sizeof(benchmark_definitions) /
                        sizeof(benchmark_definitions[0]); i++) {
        if (!run_benchmark(&benchmark_definitions[i], &options)) {
            return EXIT_FAILURE;
        }
    }
    return EXIT_SUCCESS;
}
