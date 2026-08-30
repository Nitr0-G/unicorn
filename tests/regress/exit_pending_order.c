#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <unistd.h>
#endif

#include <unicorn/unicorn.h>

#define EXIT_ORDER_STRESS_ITERATIONS 64
#define EXIT_ORDER_MAX_ATTEMPTS (EXIT_ORDER_STRESS_ITERATIONS * 4)
#define EXIT_ORDER_TIMEOUT_US 10000
#define EXIT_ORDER_WAIT_MS 10000

static const uint64_t code_address = 0x100000;
static const uint8_t code[] = { 0x40 }; /* inc eax */

typedef enum ExitOrder {
    EXIT_ORDER_COUNT_FIRST,
    EXIT_ORDER_TIMEOUT_FIRST,
} ExitOrder;

typedef struct ExitOrderContext {
    uc_err query_error;
    size_t timed_out;
    uint32_t calls;
    bool wait_for_timeout;
} ExitOrderContext;

static void sleep_one_millisecond(void)
{
#ifdef _WIN32
    Sleep(1);
#else
    usleep(1000);
#endif
}

static void exit_order_callback(uc_engine *uc, uint64_t address,
                                uint32_t size, void *user_data)
{
    ExitOrderContext *context = user_data;
    unsigned int elapsed;

    (void)address;
    (void)size;

    context->calls++;
    if (!context->wait_for_timeout) {
        return;
    }

    for (elapsed = 0; elapsed < EXIT_ORDER_WAIT_MS; elapsed++) {
        context->query_error =
            uc_query(uc, UC_QUERY_TIMEOUT, &context->timed_out);
        if (context->query_error != UC_ERR_OK || context->timed_out) {
            break;
        }
        sleep_one_millisecond();
    }
    if (context->query_error == UC_ERR_OK && context->timed_out) {
        /* timed_out is payload; let the timer publish its pending edge. */
        sleep_one_millisecond();
    }
}

static void report_uc_error(const char *operation, uc_err error,
                            ExitOrder order, unsigned int iteration)
{
    fprintf(stderr, "%s failed for order %u at iteration %u with %u: %s\n",
            operation, (unsigned int)order, iteration + 1,
            (unsigned int)error, uc_strerror(error));
}

static bool verify_registers(uc_engine *uc, ExitOrder order,
                             unsigned int iteration, uint32_t expected_eip,
                             uint32_t expected_eax)
{
    uint32_t eip = 0;
    uint32_t eax = 0;
    uc_err error;

    error = uc_reg_read(uc, UC_X86_REG_EIP, &eip);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_read(EIP)", error, order, iteration);
        return false;
    }
    error = uc_reg_read(uc, UC_X86_REG_EAX, &eax);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_read(EAX)", error, order, iteration);
        return false;
    }
    if (eip != expected_eip || eax != expected_eax) {
        fprintf(stderr,
                "unexpected state for order %u at iteration %u: "
                "eip=0x%08" PRIx32 " eax=%" PRIu32 "\n",
                (unsigned int)order, iteration + 1, eip, eax);
        return false;
    }
    return true;
}

static bool run_order_stress(ExitOrder order, unsigned int *attempts_out)
{
    ExitOrderContext context = {0};
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_err error;
    uint32_t eax;
    unsigned int attempts = 0;
    unsigned int qualified_iterations = 0;
    bool passed = true;

    error = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_open", error, order, 0);
        return false;
    }
    error = uc_mem_map(uc, code_address, 0x1000, UC_PROT_ALL);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_map", error, order, 0);
        passed = false;
        goto cleanup;
    }
    error = uc_mem_write(uc, code_address, code, sizeof(code));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_write", error, order, 0);
        passed = false;
        goto cleanup;
    }
    error = uc_hook_add(uc, &hook,
                        order == EXIT_ORDER_COUNT_FIRST ? UC_HOOK_CODE :
                                                          UC_HOOK_BLOCK,
                        exit_order_callback, &context, code_address,
                        code_address);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_add", error, order, 0);
        passed = false;
        goto cleanup;
    }

    /* Pretranslate the counted TB before starting the timing stress. */
    eax = 0;
    error = uc_reg_write(uc, UC_X86_REG_EAX, &eax);
    if (error == UC_ERR_OK) {
        error = uc_emu_start(uc, code_address, code_address + sizeof(code),
                             0, 1);
    }
    if (error != UC_ERR_OK || context.calls != 1 ||
        !verify_registers(uc, order, 0, code_address + sizeof(code), 1)) {
        if (error != UC_ERR_OK) {
            report_uc_error("pretranslation", error, order, 0);
        }
        passed = false;
        goto cleanup;
    }

    while (qualified_iterations < EXIT_ORDER_STRESS_ITERATIONS &&
           attempts < EXIT_ORDER_MAX_ATTEMPTS) {
        size_t timed_out = 0;
        unsigned int iteration = qualified_iterations;

        attempts++;
        eax = 0;
        context.query_error = UC_ERR_OK;
        context.timed_out = 0;
        context.calls = 0;
        context.wait_for_timeout = true;
        error = uc_reg_write(uc, UC_X86_REG_EAX, &eax);
        if (error == UC_ERR_OK) {
            error = uc_emu_start(uc, code_address,
                                 code_address + sizeof(code),
                                 EXIT_ORDER_TIMEOUT_US, 1);
        }
        if (error != UC_ERR_OK) {
            report_uc_error("timed uc_emu_start", error, order, iteration);
            passed = false;
            break;
        }
        if (context.query_error != UC_ERR_OK) {
            report_uc_error("callback uc_query", context.query_error, order,
                            iteration);
            passed = false;
            break;
        }
        error = uc_query(uc, UC_QUERY_TIMEOUT, &timed_out);
        if (error != UC_ERR_OK) {
            report_uc_error("uc_query", error, order, iteration);
            passed = false;
            break;
        }
        if (context.calls == 0 && timed_out == 1) {
            /* The timeout fired before either ordering edge was exercised. */
            if (!verify_registers(uc, order, iteration, code_address, 0)) {
                passed = false;
                break;
            }
            continue;
        }
        if (context.calls != 1 || context.timed_out != 1 || timed_out != 1 ||
            !verify_registers(uc, order, iteration, code_address, 0)) {
            fprintf(stderr,
                    "timed order %u failed at iteration %u: calls=%u "
                    "callback_timeout=%zu timeout=%zu\n",
                    (unsigned int)order, iteration + 1, context.calls,
                    context.timed_out, timed_out);
            passed = false;
            break;
        }

        context.calls = 0;
        context.wait_for_timeout = false;
        error = uc_emu_start(uc, code_address, code_address + sizeof(code),
                             0, 1);
        if (error != UC_ERR_OK) {
            report_uc_error("reuse uc_emu_start", error, order, iteration);
            passed = false;
            break;
        }
        error = uc_query(uc, UC_QUERY_TIMEOUT, &timed_out);
        if (error != UC_ERR_OK || timed_out != 0 || context.calls != 1 ||
            !verify_registers(uc, order, iteration,
                              code_address + sizeof(code), 1)) {
            if (error != UC_ERR_OK) {
                report_uc_error("reuse uc_query", error, order, iteration);
            }
            fprintf(stderr,
                    "reuse order %u failed at iteration %u: calls=%u "
                    "timeout=%zu\n",
                    (unsigned int)order, iteration + 1, context.calls,
                    timed_out);
            passed = false;
            break;
        }
        qualified_iterations++;
    }
    if (passed && qualified_iterations != EXIT_ORDER_STRESS_ITERATIONS) {
        fprintf(stderr,
                "timed order %u exhausted %u attempts after %u qualified "
                "iterations\n",
                (unsigned int)order, attempts, qualified_iterations);
        passed = false;
    }

cleanup:
    error = uc_close(uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_close", error, order, 0);
        passed = false;
    }
    *attempts_out = attempts;
    return passed;
}

int main(void)
{
    unsigned int count_first_attempts = 0;
    unsigned int timeout_first_attempts = 0;
    bool passed = run_order_stress(EXIT_ORDER_COUNT_FIRST,
                                   &count_first_attempts);

    passed = run_order_stress(EXIT_ORDER_TIMEOUT_FIRST,
                              &timeout_first_attempts) && passed;
    if (passed) {
        printf("exit pending order stress passed: %u count-first in %u "
               "attempts, %u timeout-first in %u attempts\n",
               EXIT_ORDER_STRESS_ITERATIONS, count_first_attempts,
               EXIT_ORDER_STRESS_ITERATIONS, timeout_first_attempts);
    }
    return passed ? EXIT_SUCCESS : EXIT_FAILURE;
}
