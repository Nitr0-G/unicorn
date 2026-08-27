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
#else
#include <pthread.h>
#include <sys/time.h>
#endif

#include <unicorn/unicorn.h>

#define EMULATION_START_TIMEOUT_MS 10000
#define EMULATION_RUN_TIMEOUT_US 20000000

static const uint64_t loop_address = 0x100000;
static const unsigned char loop_code[] = {
    0x02, 0x00, 0x04, 0x24, /* li $a0, 2 */
    0x00, 0x00, 0x00, 0x00, /* nop */
    0xfe, 0xff, 0x80, 0x14, /* bnez $a0, -2 */
    0x00, 0x00, 0x00, 0x00, /* nop */
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

typedef struct EmuThreadContext {
    uc_engine *uc;
    uc_err emu_error;
    uc_err hook_error;
    uc_hook start_hook;
    StartSignal start_signal;
#ifdef _WIN32
    DWORD signal_error;
#else
    int signal_error;
#endif
} EmuThreadContext;

#ifdef _WIN32
static DWORD start_signal_init(StartSignal *signal)
{
    signal->event = CreateEventW(NULL, TRUE, FALSE, NULL);
    return signal->event == NULL ? GetLastError() : ERROR_SUCCESS;
}

static DWORD start_signal_set(StartSignal *signal)
{
    if (InterlockedCompareExchange(&signal->started, 1, 0) == 0 &&
        !SetEvent(signal->event)) {
        return GetLastError();
    }
    return ERROR_SUCCESS;
}

static DWORD start_signal_wait(StartSignal *signal)
{
    DWORD result =
        WaitForSingleObject(signal->event, EMULATION_START_TIMEOUT_MS);

    if (result == WAIT_OBJECT_0) {
        return ERROR_SUCCESS;
    }
    if (result == WAIT_TIMEOUT) {
        return ERROR_TIMEOUT;
    }
    return GetLastError();
}

static DWORD start_signal_destroy(StartSignal *signal)
{
    return CloseHandle(signal->event) ? ERROR_SUCCESS : GetLastError();
}

static bool start_signal_was_set(StartSignal *signal)
{
    return InterlockedCompareExchange(&signal->started, 0, 0) != 0;
}
#else
static int start_signal_init(StartSignal *signal)
{
    int error = pthread_mutex_init(&signal->mutex, NULL);

    if (error != 0) {
        return error;
    }

    error = pthread_cond_init(&signal->condition, NULL);
    if (error != 0) {
        pthread_mutex_destroy(&signal->mutex);
    }
    return error;
}

static int start_signal_set(StartSignal *signal)
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

static int start_signal_wait(StartSignal *signal)
{
    struct timeval now;
    struct timespec deadline;
    int result = 0;
    int error;

    if (gettimeofday(&now, NULL) != 0) {
        return errno;
    }

    deadline.tv_sec = now.tv_sec + EMULATION_START_TIMEOUT_MS / 1000;
    deadline.tv_nsec =
        now.tv_usec * 1000 + (EMULATION_START_TIMEOUT_MS % 1000) * 1000000;
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

static int start_signal_destroy(StartSignal *signal)
{
    int condition_error = pthread_cond_destroy(&signal->condition);
    int mutex_error = pthread_mutex_destroy(&signal->mutex);

    return condition_error != 0 ? condition_error : mutex_error;
}

static bool start_signal_was_set(StartSignal *signal)
{
    return signal->started;
}
#endif

static void signal_emulation_started(uc_engine *uc, uint64_t address,
                                     uint32_t size, void *user_data)
{
    EmuThreadContext *context = user_data;

    (void)size;

    /* The first instruction has completed when this address is reached. */
    if (address == loop_address + 4 && context->signal_error == 0) {
        context->hook_error = uc_hook_del(uc, context->start_hook);
        if (context->hook_error == UC_ERR_OK) {
            context->signal_error = start_signal_set(&context->start_signal);
        }
    }
}

static void run_emulation(EmuThreadContext *context)
{
    context->emu_error = uc_emu_start(context->uc, loop_address,
                                      loop_address + sizeof(loop_code),
                                      EMULATION_RUN_TIMEOUT_US, 0);
}

#ifdef _WIN32
static unsigned int __stdcall emulation_thread(void *opaque)
{
    run_emulation(opaque);
    return 0;
}
#else
static void *emulation_thread(void *opaque)
{
    run_emulation(opaque);
    return NULL;
}
#endif

static void report_uc_error(const char *operation, uc_err error)
{
    fprintf(stderr, "%s failed with %u: %s\n", operation, (unsigned)error,
            uc_strerror(error));
}

int main(void)
{
    EmuThreadContext context;
    uc_engine *uc = NULL;
    uc_err error;
    uint32_t a0 = 0;
    uint32_t pc = 0;
    bool failed = false;
    bool signal_initialized = false;
#ifdef _WIN32
    uintptr_t thread_value;
    HANDLE thread;
    DWORD system_error;
    DWORD wait_result;
#else
    pthread_t thread;
    int system_error;
#endif

    memset(&context, 0, sizeof(context));

    error = uc_open(UC_ARCH_MIPS, UC_MODE_MIPS32, &uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_open", error);
        return EXIT_FAILURE;
    }
    context.uc = uc;

    error = uc_mem_map(uc, loop_address, 0x1000, UC_PROT_ALL);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_map", error);
        failed = true;
        goto cleanup;
    }

    error = uc_mem_write(uc, loop_address, loop_code, sizeof(loop_code));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_write", error);
        failed = true;
        goto cleanup;
    }

    error = uc_hook_add(uc, &context.start_hook, UC_HOOK_CODE,
                        signal_emulation_started, &context, 1, 0);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_add", error);
        failed = true;
        goto cleanup;
    }

    system_error = start_signal_init(&context.start_signal);
    if (system_error != 0) {
        fprintf(stderr, "start signal initialization failed with %lu\n",
                (unsigned long)system_error);
        failed = true;
        goto cleanup;
    }
    signal_initialized = true;

#ifdef _WIN32
    thread_value = _beginthreadex(NULL, 0, emulation_thread, &context, 0, NULL);
    if (thread_value == 0) {
        fprintf(stderr, "_beginthreadex failed with %d: %s\n", errno,
                strerror(errno));
        failed = true;
        goto cleanup;
    }
    thread = (HANDLE)thread_value;
#else
    system_error = pthread_create(&thread, NULL, emulation_thread, &context);
    if (system_error != 0) {
        fprintf(stderr, "pthread_create failed with %d: %s\n", system_error,
                strerror(system_error));
        failed = true;
        goto cleanup;
    }
#endif

    system_error = start_signal_wait(&context.start_signal);
    if (system_error != 0) {
        if (system_error ==
#ifdef _WIN32
            ERROR_TIMEOUT
#else
            ETIMEDOUT
#endif
        ) {
            fprintf(stderr, "worker did not enter the loop within %d ms\n",
                    EMULATION_START_TIMEOUT_MS);
        } else {
            fprintf(stderr, "waiting for the worker failed with %lu\n",
                    (unsigned long)system_error);
        }
        failed = true;
    }

    error = uc_emu_stop(uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_emu_stop", error);
        failed = true;
    }

#ifdef _WIN32
    wait_result = WaitForSingleObject(thread, EMULATION_START_TIMEOUT_MS * 3);
    if (wait_result != WAIT_OBJECT_0) {
        if (wait_result == WAIT_TIMEOUT) {
            fprintf(stderr, "worker did not exit within %d ms\n",
                    EMULATION_START_TIMEOUT_MS * 3);
        } else {
            fprintf(stderr, "waiting for worker exit failed with %lu\n",
                    GetLastError());
        }
        return EXIT_FAILURE;
    }
    if (!CloseHandle(thread)) {
        fprintf(stderr, "CloseHandle(worker) failed with %lu\n",
                GetLastError());
        failed = true;
    }
#else
    system_error = pthread_join(thread, NULL);
    if (system_error != 0) {
        fprintf(stderr, "pthread_join failed with %d: %s\n", system_error,
                strerror(system_error));
        return EXIT_FAILURE;
    }
#endif

    if (context.emu_error != UC_ERR_OK) {
        report_uc_error("worker uc_emu_start", context.emu_error);
        failed = true;
    }
    if (context.signal_error != 0) {
        fprintf(stderr, "worker signaling failed with %lu\n",
                (unsigned long)context.signal_error);
        failed = true;
    }
    if (context.hook_error != UC_ERR_OK) {
        report_uc_error("worker uc_hook_del", context.hook_error);
        failed = true;
    }
    if (!start_signal_was_set(&context.start_signal)) {
        fprintf(stderr, "worker never reached the emulation loop\n");
        failed = true;
    }

    error = uc_reg_read(uc, UC_MIPS_REG_PC, &pc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_read(PC)", error);
        failed = true;
    }

    error = uc_reg_read(uc, UC_MIPS_REG_A0, &a0);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_read(A0)", error);
        failed = true;
    }

    if (a0 != 2) {
        fprintf(stderr, "unexpected A0 value: 0x%" PRIx32 "\n", a0);
        failed = true;
    }
    if (pc < loop_address + 4 || pc >= loop_address + sizeof(loop_code) ||
        (pc - loop_address) % 4 != 0) {
        fprintf(stderr, "unexpected PC value: 0x%" PRIx32 "\n", pc);
        failed = true;
    }

cleanup:
    if (signal_initialized) {
        system_error = start_signal_destroy(&context.start_signal);
        if (system_error != 0) {
            fprintf(stderr, "start signal cleanup failed with %lu\n",
                    (unsigned long)system_error);
            failed = true;
        }
    }

    if (uc != NULL) {
        error = uc_close(uc);
        if (error != UC_ERR_OK) {
            report_uc_error("uc_close", error);
            failed = true;
        }
    }

    if (!failed) {
        printf("threaded emulation stopped at PC=0x%" PRIx32
               " with A0=0x%" PRIx32 "\n",
               pc, a0);
    }

    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
