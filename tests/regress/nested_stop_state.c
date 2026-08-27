#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <unicorn/unicorn.h>

#define OUTER_ADDRESS 0x100000
#define INNER_ADDRESS 0x101000
#define RECOVERY_ADDRESS 0x102000

typedef struct NestedStopContext {
    uc_err stop_error;
    uc_err nested_error;
    unsigned int callback_count;
} NestedStopContext;

typedef struct InnerStopContext {
    uc_err nested_error;
    uc_err stop_error;
    unsigned int outer_callback_count;
    unsigned int inner_callback_count;
} InnerStopContext;

static void stop_and_run_nested(uc_engine *uc, uint64_t address, uint32_t size,
                                void *user_data)
{
    NestedStopContext *context = user_data;

    (void)address;
    (void)size;

    context->callback_count++;
    context->stop_error = uc_emu_stop(uc);
    if (context->stop_error == UC_ERR_OK) {
        context->nested_error =
            uc_emu_start(uc, INNER_ADDRESS, INNER_ADDRESS + 2, 0, 0);
    }
}

static void report_uc_error(const char *operation, uc_err error)
{
    fprintf(stderr, "%s failed with %u: %s\n", operation, (unsigned)error,
            uc_strerror(error));
}

static void stop_inner_run(uc_engine *uc, uint64_t address, uint32_t size,
                           void *user_data)
{
    InnerStopContext *context = user_data;

    (void)address;
    (void)size;

    context->inner_callback_count++;
    context->stop_error = uc_emu_stop(uc);
}

static void run_stopped_inner(uc_engine *uc, uint64_t address, uint32_t size,
                              void *user_data)
{
    InnerStopContext *context = user_data;

    (void)address;
    (void)size;

    context->outer_callback_count++;
    context->nested_error =
        uc_emu_start(uc, INNER_ADDRESS, INNER_ADDRESS + 2, 0, 0);
}

int main(void)
{
    static const uint8_t outer_code[] = {
        0xff, 0xc0, /* inc eax */
        0xff, 0xc3, /* inc ebx */
        0xff, 0xc1, /* inc ecx */
    };
    static const uint8_t inner_code[] = {
        0xff, 0xc2, /* inc edx */
    };
    static const uint8_t recovery_code[] = {
        0xff, 0xc6, /* inc esi */
    };
    NestedStopContext context = {UC_ERR_OK, UC_ERR_OK, 0};
    InnerStopContext inner_stop = {UC_ERR_OK, UC_ERR_OK, 0, 0};
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_hook inner_hook;
    uc_err error;
    uint32_t eax = 0;
    uint32_t ebx = 0;
    uint32_t ecx = 0;
    uint32_t edx = 0;
    uint32_t esi = 0;
    bool failed = false;

    error = uc_open(UC_ARCH_X86, UC_MODE_64, &uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_open", error);
        return EXIT_FAILURE;
    }

    error = uc_mem_map(uc, OUTER_ADDRESS, 0x3000, UC_PROT_ALL);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_map", error);
        failed = true;
        goto cleanup;
    }
    error = uc_mem_write(uc, OUTER_ADDRESS, outer_code, sizeof(outer_code));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_write(outer)", error);
        failed = true;
        goto cleanup;
    }
    error = uc_mem_write(uc, INNER_ADDRESS, inner_code, sizeof(inner_code));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_write(inner)", error);
        failed = true;
        goto cleanup;
    }
    error = uc_mem_write(uc, RECOVERY_ADDRESS, recovery_code,
                         sizeof(recovery_code));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_write(recovery)", error);
        failed = true;
        goto cleanup;
    }

    error = uc_hook_add(uc, &hook, UC_HOOK_CODE, stop_and_run_nested, &context,
                        OUTER_ADDRESS, OUTER_ADDRESS);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_add", error);
        failed = true;
        goto cleanup;
    }

    error = uc_emu_start(uc, OUTER_ADDRESS, OUTER_ADDRESS + sizeof(outer_code),
                         0, 0);
    if (error != UC_ERR_OK) {
        report_uc_error("outer uc_emu_start", error);
        failed = true;
    }
    if (context.stop_error != UC_ERR_OK) {
        report_uc_error("nested callback uc_emu_stop", context.stop_error);
        failed = true;
    }
    if (context.nested_error != UC_ERR_OK) {
        report_uc_error("nested uc_emu_start", context.nested_error);
        failed = true;
    }
    if (context.callback_count != 1) {
        fprintf(stderr, "unexpected callback count: %u\n",
                context.callback_count);
        failed = true;
    }

    error = uc_reg_read(uc, UC_X86_REG_EAX, &eax);
    error = error == UC_ERR_OK ? uc_reg_read(uc, UC_X86_REG_EBX, &ebx) : error;
    error = error == UC_ERR_OK ? uc_reg_read(uc, UC_X86_REG_ECX, &ecx) : error;
    error = error == UC_ERR_OK ? uc_reg_read(uc, UC_X86_REG_EDX, &edx) : error;
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_read", error);
        failed = true;
    } else if (eax != 0 || ebx != 0 || ecx != 0 || edx != 1) {
        fprintf(stderr,
                "unexpected registers after nested stop: eax=%" PRIu32
                " ebx=%" PRIu32 " ecx=%" PRIu32 " edx=%" PRIu32 "\n",
                eax, ebx, ecx, edx);
        failed = true;
    }

    error = uc_emu_start(uc, RECOVERY_ADDRESS,
                         RECOVERY_ADDRESS + sizeof(recovery_code), 0, 0);
    if (error != UC_ERR_OK) {
        report_uc_error("recovery uc_emu_start", error);
        failed = true;
    }
    error = uc_reg_read(uc, UC_X86_REG_ESI, &esi);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_read(ESI)", error);
        failed = true;
    } else if (esi != 1) {
        fprintf(stderr, "unexpected recovery ESI value: %" PRIu32 "\n", esi);
        failed = true;
    }

    error = uc_hook_del(uc, hook);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_del(outer stop)", error);
        failed = true;
    }
    eax = 0;
    ebx = 0;
    ecx = 0;
    edx = 0;
    error = uc_reg_write(uc, UC_X86_REG_EAX, &eax);
    error = error == UC_ERR_OK ? uc_reg_write(uc, UC_X86_REG_EBX, &ebx) : error;
    error = error == UC_ERR_OK ? uc_reg_write(uc, UC_X86_REG_ECX, &ecx) : error;
    error = error == UC_ERR_OK ? uc_reg_write(uc, UC_X86_REG_EDX, &edx) : error;
    if (error == UC_ERR_OK) {
        error = uc_hook_add(uc, &inner_hook, UC_HOOK_CODE, stop_inner_run,
                            &inner_stop, INNER_ADDRESS, INNER_ADDRESS);
    }
    if (error == UC_ERR_OK) {
        error = uc_hook_add(uc, &hook, UC_HOOK_CODE, run_stopped_inner,
                            &inner_stop, OUTER_ADDRESS, OUTER_ADDRESS);
    }
    if (error != UC_ERR_OK) {
        report_uc_error("inner stop setup", error);
        failed = true;
        goto cleanup;
    }

    error = uc_emu_start(uc, OUTER_ADDRESS, OUTER_ADDRESS + sizeof(outer_code),
                         0, 0);
    if (error != UC_ERR_OK || inner_stop.nested_error != UC_ERR_OK ||
        inner_stop.stop_error != UC_ERR_OK) {
        report_uc_error("inner stop emulation", error);
        failed = true;
    }
    error = uc_reg_read(uc, UC_X86_REG_EAX, &eax);
    error = error == UC_ERR_OK ? uc_reg_read(uc, UC_X86_REG_EBX, &ebx) : error;
    error = error == UC_ERR_OK ? uc_reg_read(uc, UC_X86_REG_ECX, &ecx) : error;
    error = error == UC_ERR_OK ? uc_reg_read(uc, UC_X86_REG_EDX, &edx) : error;
    if (error != UC_ERR_OK || inner_stop.outer_callback_count != 1 ||
        inner_stop.inner_callback_count != 1 || eax != 1 || ebx != 1 ||
        ecx != 1 || edx != 0) {
        fprintf(stderr,
                "inner stop escaped its frame: outer=%u inner=%u eax=%" PRIu32
                " ebx=%" PRIu32 " ecx=%" PRIu32 " edx=%" PRIu32 "\n",
                inner_stop.outer_callback_count,
                inner_stop.inner_callback_count, eax, ebx, ecx, edx);
        failed = true;
    }

cleanup:
    if (uc != NULL) {
        error = uc_close(uc);
        if (error != UC_ERR_OK) {
            report_uc_error("uc_close", error);
            failed = true;
        }
    }

    return failed ? EXIT_FAILURE : EXIT_SUCCESS;
}
