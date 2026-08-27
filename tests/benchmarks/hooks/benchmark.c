#ifndef _WIN32
#define _POSIX_C_SOURCE 200809L
#endif

#include <inttypes.h>
#include <stdbool.h>
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

#define CODE_ADDRESS UINT64_C(0x100000)
#define DATA_ADDRESS UINT64_C(0x200000)
#define DEFAULT_ITERATIONS UINT64_C(1000000)
#define DEFAULT_REPEATS 7
#define MAX_HOOKS 8

typedef enum BenchmarkMode {
    MODE_NONE,
    MODE_COUNT,
    MODE_CODE_ONE,
    MODE_CODE_EIGHT,
    MODE_CODE_TAIL,
    MODE_BLOCK_ONE,
    MODE_MEMORY_NONE,
    MODE_MEMORY_ONE,
    MODE_MEMORY_EIGHT,
    MODE_MEMORY_TAIL,
} BenchmarkMode;

typedef struct BenchmarkDefinition {
    const char *name;
    BenchmarkMode mode;
} BenchmarkDefinition;

static const BenchmarkDefinition definitions[] = {
    { "none", MODE_NONE },
    { "count", MODE_COUNT },
    { "code-one", MODE_CODE_ONE },
    { "code-eight", MODE_CODE_EIGHT },
    { "code-tail", MODE_CODE_TAIL },
    { "block-one", MODE_BLOCK_ONE },
    { "memory-none", MODE_MEMORY_NONE },
    { "memory-one", MODE_MEMORY_ONE },
    { "memory-eight", MODE_MEMORY_EIGHT },
    { "memory-tail", MODE_MEMORY_TAIL },
};

static volatile uint64_t callback_count;

static void fail_uc(uc_engine *uc, const char *operation, uc_err error)
{
    fprintf(stderr, "%s: %s\n", operation, uc_strerror(error));
    if (uc != NULL) {
        uc_close(uc);
    }
    exit(1);
}

static void check_uc(uc_engine *uc, const char *operation, uc_err error)
{
    if (error != UC_ERR_OK) {
        fail_uc(uc, operation, error);
    }
}

static void code_callback(uc_engine *uc, uint64_t address, uint32_t size,
                          void *user_data)
{
    (void)uc;
    (void)address;
    (void)size;
    (void)user_data;
    callback_count++;
}

static void memory_callback(uc_engine *uc, uc_mem_type type,
                            uint64_t address, int size, int64_t value,
                            void *user_data)
{
    (void)uc;
    (void)type;
    (void)address;
    (void)size;
    (void)value;
    (void)user_data;
    callback_count++;
}

static double monotonic_seconds(void)
{
#ifdef _WIN32
    LARGE_INTEGER counter;
    LARGE_INTEGER frequency;

    QueryPerformanceCounter(&counter);
    QueryPerformanceFrequency(&frequency);
    return (double)counter.QuadPart / (double)frequency.QuadPart;
#else
    struct timespec time;

    clock_gettime(CLOCK_MONOTONIC, &time);
    return (double)time.tv_sec + (double)time.tv_nsec / 1000000000.0;
#endif
}

static int compare_double(const void *lhs, const void *rhs)
{
    double a = *(const double *)lhs;
    double b = *(const double *)rhs;

    return (a > b) - (a < b);
}

static const BenchmarkDefinition *find_definition(const char *name)
{
    size_t i;

    for (i = 0; i < sizeof(definitions) / sizeof(definitions[0]); i++) {
        if (strcmp(name, definitions[i].name) == 0) {
            return &definitions[i];
        }
    }
    return NULL;
}

static bool is_memory_mode(BenchmarkMode mode)
{
    return mode >= MODE_MEMORY_NONE;
}

static uint64_t expected_callbacks(BenchmarkMode mode, uint64_t iterations,
                                   size_t repeats)
{
    uint64_t per_run;

    switch (mode) {
    case MODE_CODE_ONE:
    case MODE_CODE_TAIL:
        per_run = iterations * 2;
        break;
    case MODE_CODE_EIGHT:
        per_run = iterations * 16;
        break;
    case MODE_BLOCK_ONE:
    case MODE_MEMORY_ONE:
    case MODE_MEMORY_TAIL:
        per_run = iterations;
        break;
    case MODE_MEMORY_EIGHT:
        per_run = iterations * 8;
        break;
    default:
        per_run = 0;
        break;
    }
    return per_run * repeats;
}

static void add_code_hooks(uc_engine *uc, BenchmarkMode mode,
                           uc_hook hooks[MAX_HOOKS])
{
    int count = mode == MODE_CODE_ONE || mode == MODE_BLOCK_ONE ? 1 : 8;
    int type = mode == MODE_BLOCK_ONE ? UC_HOOK_BLOCK : UC_HOOK_CODE;
    int i;

    if (mode == MODE_NONE || mode == MODE_COUNT) {
        return;
    }
    for (i = 0; i < count; i++) {
        uint64_t begin = 1;
        uint64_t end = 0;

        if (mode == MODE_CODE_TAIL && i != count - 1) {
            begin = CODE_ADDRESS + 0x1000 + (uint64_t)i * 0x10;
            end = begin;
        }
        check_uc(uc, "uc_hook_add(code)",
                 uc_hook_add(uc, &hooks[i], type, code_callback, NULL,
                             begin, end));
    }
}

static void add_memory_hooks(uc_engine *uc, BenchmarkMode mode,
                             uc_hook hooks[MAX_HOOKS])
{
    int count = mode == MODE_MEMORY_ONE ? 1 : 8;
    int i;

    if (mode == MODE_MEMORY_NONE) {
        return;
    }
    for (i = 0; i < count; i++) {
        uint64_t begin = DATA_ADDRESS;
        uint64_t end = DATA_ADDRESS + 7;

        if (mode == MODE_MEMORY_TAIL && i != count - 1) {
            begin += 0x1000 + (uint64_t)i * 0x10;
            end = begin + 7;
        }
        check_uc(uc, "uc_hook_add(memory)",
                 uc_hook_add(uc, &hooks[i], UC_HOOK_MEM_READ,
                             memory_callback, NULL, begin, end));
    }
}

static uc_engine *create_engine(const BenchmarkDefinition *definition,
                                uint64_t iterations)
{
    static const uint8_t code_loop[] = {
        0x48, 0xff, 0xc9, /* dec rcx */
        0x75, 0xfb,       /* jne loop */
    };
    static const uint8_t memory_loop[] = {
        0x48, 0x8b, 0x03, /* mov rax, [rbx] */
        0x48, 0xff, 0xc9, /* dec rcx */
        0x75, 0xf8,       /* jne loop */
    };
    const uint8_t *code = code_loop;
    size_t code_size = sizeof(code_loop);
    uint64_t data = UINT64_C(0x1122334455667788);
    uint64_t rbx = DATA_ADDRESS;
    uc_hook hooks[MAX_HOOKS];
    uc_engine *uc = NULL;

    if (is_memory_mode(definition->mode)) {
        code = memory_loop;
        code_size = sizeof(memory_loop);
    }
    check_uc(uc, "uc_open", uc_open(UC_ARCH_X86, UC_MODE_64, &uc));
    check_uc(uc, "uc_mem_map(code)",
             uc_mem_map(uc, CODE_ADDRESS, 0x1000, UC_PROT_ALL));
    check_uc(uc, "uc_mem_write(code)",
             uc_mem_write(uc, CODE_ADDRESS, code, code_size));
    check_uc(uc, "uc_mem_map(data)",
             uc_mem_map(uc, DATA_ADDRESS, 0x1000, UC_PROT_ALL));
    check_uc(uc, "uc_mem_write(data)",
             uc_mem_write(uc, DATA_ADDRESS, &data, sizeof(data)));
    check_uc(uc, "uc_reg_write(rbx)",
             uc_reg_write(uc, UC_X86_REG_RBX, &rbx));
    if (is_memory_mode(definition->mode)) {
        add_memory_hooks(uc, definition->mode, hooks);
    } else {
        add_code_hooks(uc, definition->mode, hooks);
    }
    (void)iterations;
    return uc;
}

static double run_once(uc_engine *uc, const BenchmarkDefinition *definition,
                       uint64_t iterations)
{
    const uint64_t instruction_count = iterations * 2;
    const size_t code_size = is_memory_mode(definition->mode) ? 8 : 5;
    uint64_t rcx = iterations;
    double begin;
    double end;

    check_uc(uc, "uc_reg_write(rcx)",
             uc_reg_write(uc, UC_X86_REG_RCX, &rcx));
    begin = monotonic_seconds();
    check_uc(uc, "uc_emu_start",
             uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + code_size, 0,
                          definition->mode == MODE_COUNT ?
                              instruction_count : 0));
    end = monotonic_seconds();
    return (end - begin) * 1000.0;
}

int main(int argc, char **argv)
{
    const BenchmarkDefinition *definition;
    uint64_t iterations = DEFAULT_ITERATIONS;
    size_t repeats = DEFAULT_REPEATS;
    double *samples;
    uc_engine *uc;
    size_t i;

    if (argc < 2 || argc > 4 ||
        (definition = find_definition(argv[1])) == NULL) {
        fprintf(stderr, "usage: hook_bench MODE [ITERATIONS] [REPEATS]\n");
        return 2;
    }
    if (argc >= 3) {
        iterations = strtoull(argv[2], NULL, 0);
    }
    if (argc >= 4) {
        repeats = strtoul(argv[3], NULL, 0);
    }
    if (iterations == 0 || repeats == 0) {
        return 2;
    }
    samples = calloc(repeats, sizeof(*samples));
    if (samples == NULL) {
        return 1;
    }
    uc = create_engine(definition, iterations);
    (void)run_once(uc, definition, 1000);
    callback_count = 0;
    for (i = 0; i < repeats; i++) {
        samples[i] = run_once(uc, definition, iterations);
    }
    if (callback_count !=
        expected_callbacks(definition->mode, iterations, repeats)) {
        fprintf(stderr, "unexpected callback count: %" PRIu64 "\n",
                callback_count);
        return 1;
    }
    qsort(samples, repeats, sizeof(*samples), compare_double);
    printf("mode=%s iterations=%" PRIu64 " repeats=%zu median_ms=%.3f "
           "callbacks=%" PRIu64 "\n", definition->name, iterations,
           repeats, samples[repeats / 2], callback_count);
    check_uc(NULL, "uc_close", uc_close(uc));
    free(samples);
    return 0;
}
