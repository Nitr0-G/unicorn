#include <errno.h>
#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
#include <process.h>
#include <windows.h>
typedef DWORD SystemError;
#else
#include <pthread.h>
#include <sys/time.h>
#include <unistd.h>
typedef int SystemError;
#endif

#include <unicorn/unicorn.h>

#define STOP_STRESS_ITERATIONS 256
#define STOP_COALESCE_CALLS 32
#define WAIT_TIMEOUT_MS 10000
#define EXTERNAL_RUN_TIMEOUT_US 20000000
#define TIMER_STOP_TIMEOUT_US 1000

static const uint64_t code_address = 0x100000;
static const uint8_t code[] = {
    0x01, 0x00, 0x00, 0x10, /* beq $zero,$zero,code_address + 8 */
    0x01, 0x00, 0x42, 0x24, /* addiu $v0,$v0,1 */
    0x01, 0x00, 0x63, 0x24, /* addiu $v1,$v1,1 */
};

typedef struct StartSignal {
#ifdef _WIN32
    HANDLE event;
    volatile LONG started;
#else
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool started;
#endif
} StartSignal;

typedef enum StopSource {
    STOP_FROM_EXTERNAL_THREAD,
    STOP_FROM_TIMEOUT_THREAD,
} StopSource;

typedef struct StopContext {
    uc_engine *uc;
    StopSource source;
    uc_err emu_error;
    uc_err stop_error;
    uc_err query_error;
    StartSignal entered_signal;
    StartSignal stop_done_signal;
    SystemError signal_error;
    SystemError wait_error;
    size_t timed_out;
    uint32_t calls;
    uint32_t stop_calls;
} StopContext;

#ifdef _WIN32
static SystemError start_signal_init(StartSignal *signal)
{
    signal->started = 0;
    signal->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    return signal->event == NULL ? GetLastError() : ERROR_SUCCESS;
}

static SystemError start_signal_set(StartSignal *signal)
{
    if (InterlockedCompareExchange(&signal->started, 1, 0) == 0 &&
        !SetEvent(signal->event)) {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

static SystemError start_signal_wait(StartSignal *signal)
{
    DWORD result = WaitForSingleObject(signal->event, WAIT_TIMEOUT_MS);

    if (result == WAIT_OBJECT_0) {
        return ERROR_SUCCESS;
    }
    if (result == WAIT_TIMEOUT) {
        return ERROR_TIMEOUT;
    }
    return GetLastError();
}

static SystemError start_signal_destroy(StartSignal *signal)
{
    return CloseHandle(signal->event) ? ERROR_SUCCESS : GetLastError();
}

static SystemError timeout_error(void)
{
    return ERROR_TIMEOUT;
}

static void sleep_one_millisecond(void)
{
    Sleep(1);
}
#else
static SystemError start_signal_init(StartSignal *signal)
{
    int error;

    signal->started = false;
    error = pthread_mutex_init(&signal->mutex, NULL);
    if (error != 0) {
        return error;
    }

    error = pthread_cond_init(&signal->condition, NULL);
    if (error != 0) {
        pthread_mutex_destroy(&signal->mutex);
    }
    return error;
}

static SystemError start_signal_set(StartSignal *signal)
{
    int result = 0;
    int error = pthread_mutex_lock(&signal->mutex);

    if (error != 0) {
        return error;
    }

    if (!signal->started) {
        signal->started = true;
        result = pthread_cond_signal(&signal->condition);
    }

    error = pthread_mutex_unlock(&signal->mutex);
    return result != 0 ? result : error;
}

static SystemError start_signal_wait(StartSignal *signal)
{
    struct timeval now;
    struct timespec deadline;
    int result = 0;
    int error;

    if (gettimeofday(&now, NULL) != 0) {
        return errno;
    }

    deadline.tv_sec = now.tv_sec + WAIT_TIMEOUT_MS / 1000;
    deadline.tv_nsec =
        now.tv_usec * 1000 + (WAIT_TIMEOUT_MS % 1000) * 1000000;
    if (deadline.tv_nsec >= 1000000000) {
        deadline.tv_sec++;
        deadline.tv_nsec -= 1000000000;
    }

    error = pthread_mutex_lock(&signal->mutex);
    if (error != 0) {
        return error;
    }

    while (!signal->started && result == 0) {
        result = pthread_cond_timedwait(&signal->condition, &signal->mutex,
                                        &deadline);
    }

    error = pthread_mutex_unlock(&signal->mutex);
    return result != 0 ? result : error;
}

static SystemError start_signal_destroy(StartSignal *signal)
{
    int condition_error = pthread_cond_destroy(&signal->condition);
    int mutex_error = pthread_mutex_destroy(&signal->mutex);

    return condition_error != 0 ? condition_error : mutex_error;
}

static SystemError timeout_error(void)
{
    return ETIMEDOUT;
}

static void sleep_one_millisecond(void)
{
    usleep(1000);
}
#endif

static void report_uc_error(const char *operation, uc_err error,
                            unsigned int iteration)
{
    fprintf(stderr, "%s failed at iteration %u with %u: %s\n", operation,
            iteration + 1, (unsigned int)error, uc_strerror(error));
}

static void report_system_error(const char *operation, SystemError error,
                                unsigned int iteration)
{
    fprintf(stderr, "%s failed at iteration %u with %lu\n", operation,
            iteration + 1, (unsigned long)error);
}

static void wait_for_timeout(StopContext *context)
{
    unsigned int elapsed;

    for (elapsed = 0; elapsed < WAIT_TIMEOUT_MS; elapsed++) {
        context->query_error =
            uc_query(context->uc, UC_QUERY_TIMEOUT, &context->timed_out);
        if (context->query_error != UC_ERR_OK || context->timed_out) {
            break;
        }
        sleep_one_millisecond();
    }
    if (context->query_error == UC_ERR_OK && !context->timed_out) {
        context->wait_error = timeout_error();
    } else if (context->timed_out) {
        /* The timer publishes cpu_exit immediately after timed_out. */
        sleep_one_millisecond();
    }
}

static void stop_after_tb_entry(uc_engine *uc, uint64_t address, uint32_t size,
                                void *user_data)
{
    StopContext *context = user_data;

    (void)uc;
    (void)size;

    if (address != code_address + 4) {
        return;
    }

    context->calls++;
    if (context->source == STOP_FROM_EXTERNAL_THREAD) {
        context->signal_error = start_signal_set(&context->entered_signal);
        if (context->signal_error == 0) {
            context->wait_error =
                start_signal_wait(&context->stop_done_signal);
        }
    } else {
        wait_for_timeout(context);
    }
}

static void run_external_emulation(StopContext *context)
{
    context->emu_error =
        uc_emu_start(context->uc, code_address, code_address + sizeof(code),
                     EXTERNAL_RUN_TIMEOUT_US, 0);
}

#ifdef _WIN32
static unsigned int __stdcall emulation_thread(void *opaque)
{
    run_external_emulation(opaque);
    return 0;
}
#else
static void *emulation_thread(void *opaque)
{
    run_external_emulation(opaque);
    return NULL;
}
#endif

static bool reset_guest(uc_engine *uc, unsigned int iteration)
{
    uint32_t value = 0;
    uc_err error;

    error = uc_reg_write(uc, UC_MIPS_REG_V0, &value);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_write(V0)", error, iteration);
        return false;
    }
    error = uc_reg_write(uc, UC_MIPS_REG_V1, &value);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_write(V1)", error, iteration);
        return false;
    }
    return true;
}

static bool verify_stop(StopContext *context, size_t expected_timeout,
                        uint32_t expected_stop_calls,
                        unsigned int iteration)
{
    uint32_t pc = 0;
    uint32_t v0 = 0;
    uint32_t v1 = 0;
    size_t timed_out = 0;
    uc_err error;

    if (context->emu_error != UC_ERR_OK) {
        report_uc_error("uc_emu_start", context->emu_error, iteration);
        return false;
    }
    if (context->query_error != UC_ERR_OK) {
        report_uc_error("uc_query in callback", context->query_error,
                        iteration);
        return false;
    }
    if (context->stop_error != UC_ERR_OK) {
        report_uc_error("uc_emu_stop", context->stop_error, iteration);
        return false;
    }
    if (context->signal_error != 0 || context->wait_error != 0) {
        report_system_error("callback synchronization",
                            context->signal_error != 0 ? context->signal_error :
                                                         context->wait_error,
                            iteration);
        return false;
    }
    if (context->calls != 1) {
        fprintf(stderr, "callback count %u at iteration %u\n", context->calls,
                iteration + 1);
        return false;
    }
    if (context->stop_calls != expected_stop_calls) {
        fprintf(stderr, "stop call count %u at iteration %u\n",
                context->stop_calls, iteration + 1);
        return false;
    }

    error = uc_query(context->uc, UC_QUERY_TIMEOUT, &timed_out);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_query", error, iteration);
        return false;
    }
    error = uc_reg_read(context->uc, UC_MIPS_REG_PC, &pc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_read(PC)", error, iteration);
        return false;
    }
    error = uc_reg_read(context->uc, UC_MIPS_REG_V0, &v0);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_read(V0)", error, iteration);
        return false;
    }
    error = uc_reg_read(context->uc, UC_MIPS_REG_V1, &v1);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_read(V1)", error, iteration);
        return false;
    }

    if (timed_out != expected_timeout || pc != code_address + 8 || v0 != 1 ||
        v1 != 0) {
        fprintf(stderr,
                "unexpected state at iteration %u: timeout=%zu pc=0x%08" PRIx32
                " v0=%" PRIu32 " v1=%" PRIu32 "\n",
                iteration + 1, timed_out, pc, v0, v1);
        return false;
    }
    return true;
}

static bool run_external_stop_iteration(StopContext *context,
                                        unsigned int iteration)
{
    bool entered_initialized = false;
    bool done_initialized = false;
    bool thread_started = false;
    bool passed = true;
    SystemError error;
#ifdef _WIN32
    uintptr_t thread_value;
    HANDLE thread = NULL;
    DWORD wait_result;
#else
    pthread_t thread;
#endif

    memset(&context->entered_signal, 0, sizeof(context->entered_signal));
    memset(&context->stop_done_signal, 0, sizeof(context->stop_done_signal));
    context->source = STOP_FROM_EXTERNAL_THREAD;
    context->emu_error = UC_ERR_OK;
    context->stop_error = UC_ERR_OK;
    context->query_error = UC_ERR_OK;
    context->signal_error = 0;
    context->wait_error = 0;
    context->timed_out = 0;
    context->calls = 0;
    context->stop_calls = 0;

    if (!reset_guest(context->uc, iteration)) {
        return false;
    }
    error = start_signal_init(&context->entered_signal);
    if (error != 0) {
        report_system_error("entered signal initialization", error, iteration);
        return false;
    }
    entered_initialized = true;
    error = start_signal_init(&context->stop_done_signal);
    if (error != 0) {
        report_system_error("done signal initialization", error, iteration);
        passed = false;
        goto cleanup;
    }
    done_initialized = true;

#ifdef _WIN32
    thread_value =
        _beginthreadex(NULL, 0, emulation_thread, context, 0, NULL);
    if (thread_value == 0) {
        fprintf(stderr, "_beginthreadex failed at iteration %u with %d: %s\n",
                iteration + 1, errno, strerror(errno));
        passed = false;
        goto cleanup;
    }
    thread = (HANDLE)thread_value;
#else
    error = pthread_create(&thread, NULL, emulation_thread, context);
    if (error != 0) {
        report_system_error("pthread_create", error, iteration);
        passed = false;
        goto cleanup;
    }
#endif
    thread_started = true;

    error = start_signal_wait(&context->entered_signal);
    if (error != 0) {
        report_system_error("waiting for TB entry", error, iteration);
        passed = false;
    }
    for (context->stop_calls = 0;
         context->stop_calls < STOP_COALESCE_CALLS;
         context->stop_calls++) {
        context->stop_error = uc_emu_stop(context->uc);
        if (context->stop_error != UC_ERR_OK) {
            report_uc_error("uc_emu_stop", context->stop_error, iteration);
            passed = false;
            break;
        }
    }
    error = start_signal_set(&context->stop_done_signal);
    if (error != 0) {
        report_system_error("publishing stop completion", error, iteration);
        passed = false;
    }

#ifdef _WIN32
    wait_result = WaitForSingleObject(thread, WAIT_TIMEOUT_MS * 3);
    if (wait_result != WAIT_OBJECT_0) {
        report_system_error("waiting for emulation thread",
                            wait_result == WAIT_TIMEOUT ? ERROR_TIMEOUT :
                                                          GetLastError(),
                            iteration);
        return false;
    }
    if (!CloseHandle(thread)) {
        report_system_error("CloseHandle", GetLastError(), iteration);
        passed = false;
    }
    thread = NULL;
#else
    error = pthread_join(thread, NULL);
    if (error != 0) {
        report_system_error("pthread_join", error, iteration);
        return false;
    }
#endif
    thread_started = false;
    if (passed &&
        !verify_stop(context, 0, STOP_COALESCE_CALLS, iteration)) {
        passed = false;
    }

cleanup:
    if (thread_started) {
        start_signal_set(&context->stop_done_signal);
#ifdef _WIN32
        WaitForSingleObject(thread, WAIT_TIMEOUT_MS * 3);
        CloseHandle(thread);
#else
        pthread_join(thread, NULL);
#endif
    }
    if (done_initialized) {
        error = start_signal_destroy(&context->stop_done_signal);
        if (error != 0) {
            report_system_error("done signal cleanup", error, iteration);
            passed = false;
        }
    }
    if (entered_initialized) {
        error = start_signal_destroy(&context->entered_signal);
        if (error != 0) {
            report_system_error("entered signal cleanup", error, iteration);
            passed = false;
        }
    }
    return passed;
}

static bool run_timeout_stop_iteration(StopContext *context,
                                       unsigned int iteration)
{
    context->source = STOP_FROM_TIMEOUT_THREAD;
    context->emu_error = UC_ERR_OK;
    context->stop_error = UC_ERR_OK;
    context->query_error = UC_ERR_OK;
    context->signal_error = 0;
    context->wait_error = 0;
    context->timed_out = 0;
    context->calls = 0;
    context->stop_calls = 0;

    if (!reset_guest(context->uc, iteration)) {
        return false;
    }
    context->emu_error =
        uc_emu_start(context->uc, code_address, code_address + sizeof(code),
                     TIMER_STOP_TIMEOUT_US, 0);
    return verify_stop(context, 1, 0, iteration);
}

int main(void)
{
    StopContext context = {0};
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_err error;
    unsigned int iteration;
    bool passed = true;

    error = uc_open(UC_ARCH_MIPS, UC_MODE_MIPS32 | UC_MODE_LITTLE_ENDIAN, &uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_open", error, 0);
        return EXIT_FAILURE;
    }
    context.uc = uc;

    error = uc_mem_map(uc, code_address, 0x1000, UC_PROT_ALL);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_map", error, 0);
        passed = false;
        goto cleanup;
    }
    error = uc_mem_write(uc, code_address, code, sizeof(code));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_write", error, 0);
        passed = false;
        goto cleanup;
    }
    error = uc_hook_add(uc, &hook, UC_HOOK_CODE, stop_after_tb_entry, &context,
                        code_address + 4, code_address + 4);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_add", error, 0);
        passed = false;
        goto cleanup;
    }

    for (iteration = 0; iteration < STOP_STRESS_ITERATIONS; iteration++) {
        if (!run_external_stop_iteration(&context, iteration)) {
            passed = false;
            goto cleanup;
        }
    }
    for (iteration = 0; iteration < STOP_STRESS_ITERATIONS; iteration++) {
        if (!run_timeout_stop_iteration(&context, iteration)) {
            passed = false;
            goto cleanup;
        }
    }

cleanup:
    error = uc_close(uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_close", error, 0);
        passed = false;
    }
    if (passed) {
        printf("exit gate stop stress passed: %u coalesced external, "
               "%u timeout\n",
               STOP_STRESS_ITERATIONS, STOP_STRESS_ITERATIONS);
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
