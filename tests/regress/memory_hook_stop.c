#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

#include <unicorn/unicorn.h>

#define CODE_ADDRESS 0x100000
#define WRITE_ADDRESS 0x100100
#define DATA_ADDRESS 0x101000

typedef struct StopHookContext {
    uc_err stop_error;
    uc_mem_type type;
    uint64_t address;
    int64_t value;
    uint32_t size;
    unsigned int count;
} StopHookContext;

static void stop_memory_access(uc_engine *uc, uc_mem_type type,
                               uint64_t address, int size, int64_t value,
                               void *user_data)
{
    StopHookContext *context = user_data;

    context->type = type;
    context->address = address;
    context->size = (uint32_t)size;
    context->value = value;
    context->count++;
    context->stop_error = uc_emu_stop(uc);
}

static void report_uc_error(const char *operation, uc_err error)
{
    fprintf(stderr, "%s failed with %u: %s\n", operation, (unsigned)error,
            uc_strerror(error));
}

static bool check_hook(const StopHookContext *context, uc_mem_type type,
                       int64_t value)
{
    if (context->stop_error != UC_ERR_OK || context->count != 1 ||
        context->type != type || context->address != DATA_ADDRESS ||
        context->size != 4 ||
        (type == UC_MEM_WRITE && context->value != value)) {
        fprintf(stderr,
                "unexpected hook state: error=%u count=%u type=%u "
                "address=0x%" PRIx64 " size=%" PRIu32 " value=0x%" PRIx64 "\n",
                (unsigned)context->stop_error, context->count,
                (unsigned)context->type, context->address, context->size,
                (uint64_t)context->value);
        return false;
    }
    return true;
}

int main(void)
{
    static const uint8_t read_code[] = {
        0xa1, 0x00, 0x10, 0x10, 0x00, /* mov eax, [0x101000] */
        0x43,                         /* inc ebx */
    };
    static const uint8_t write_code[] = {
        0xa3, 0x00, 0x10, 0x10, 0x00, /* mov [0x101000], eax */
        0x43,                         /* inc ebx */
    };
    StopHookContext context = {0};
    const uint32_t original_data = 0x11223344;
    const uint32_t original_eax = 0xdeadbeef;
    const uint32_t write_eax = 0x87654321;
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_err error;
    uint32_t eax;
    uint32_t ebx = 0;
    uint32_t data;
    bool failed = false;

    error = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_open", error);
        return EXIT_FAILURE;
    }
    error = uc_mem_map(uc, CODE_ADDRESS, 0x2000, UC_PROT_ALL);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_map", error);
        failed = true;
        goto cleanup;
    }
    error = uc_mem_write(uc, CODE_ADDRESS, read_code, sizeof(read_code));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_write(read code)", error);
        failed = true;
        goto cleanup;
    }
    error = uc_mem_write(uc, WRITE_ADDRESS, write_code, sizeof(write_code));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_write(write code)", error);
        failed = true;
        goto cleanup;
    }
    error =
        uc_mem_write(uc, DATA_ADDRESS, &original_data, sizeof(original_data));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_write(data)", error);
        failed = true;
        goto cleanup;
    }

    error = uc_reg_write(uc, UC_X86_REG_EAX, &original_eax);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_write(EAX)", error);
        failed = true;
        goto cleanup;
    }
    error = uc_hook_add(uc, &hook, UC_HOOK_MEM_READ, stop_memory_access,
                        &context, DATA_ADDRESS, DATA_ADDRESS);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_add(read)", error);
        failed = true;
        goto cleanup;
    }
    error =
        uc_emu_start(uc, CODE_ADDRESS, CODE_ADDRESS + sizeof(read_code), 0, 0);
    if (error != UC_ERR_OK) {
        report_uc_error("read uc_emu_start", error);
        failed = true;
    }
    eax = 0;
    error = uc_reg_read(uc, UC_X86_REG_EAX, &eax);
    if (error != UC_ERR_OK || eax != original_eax ||
        !check_hook(&context, UC_MEM_READ, 0)) {
        fprintf(stderr, "read continued after stop: eax=0x%" PRIx32 "\n", eax);
        failed = true;
    }
    error = uc_hook_del(uc, hook);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_del(read)", error);
        failed = true;
    }

    context = (StopHookContext){0};
    error = uc_reg_write(uc, UC_X86_REG_EAX, &write_eax);
    error = error == UC_ERR_OK ? uc_reg_write(uc, UC_X86_REG_EBX, &ebx) : error;
    if (error != UC_ERR_OK) {
        report_uc_error("uc_reg_write(write registers)", error);
        failed = true;
        goto cleanup;
    }
    error = uc_hook_add(uc, &hook, UC_HOOK_MEM_WRITE, stop_memory_access,
                        &context, DATA_ADDRESS, DATA_ADDRESS);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_add(write)", error);
        failed = true;
        goto cleanup;
    }
    error = uc_emu_start(uc, WRITE_ADDRESS, WRITE_ADDRESS + sizeof(write_code),
                         0, 0);
    if (error != UC_ERR_OK) {
        report_uc_error("write uc_emu_start", error);
        failed = true;
    }
    data = 0;
    ebx = 0;
    error = uc_mem_read(uc, DATA_ADDRESS, &data, sizeof(data));
    error = error == UC_ERR_OK ? uc_reg_read(uc, UC_X86_REG_EBX, &ebx) : error;
    if (error != UC_ERR_OK || data != original_data || ebx != 0 ||
        !check_hook(&context, UC_MEM_WRITE, write_eax)) {
        fprintf(stderr,
                "write continued after stop: data=0x%" PRIx32 " ebx=0x%" PRIx32
                "\n",
                data, ebx);
        failed = true;
    }
    error = uc_hook_del(uc, hook);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_del(write)", error);
        failed = true;
    }

    error = uc_emu_start(uc, WRITE_ADDRESS, WRITE_ADDRESS + sizeof(write_code),
                         0, 0);
    if (error != UC_ERR_OK) {
        report_uc_error("recovery uc_emu_start", error);
        failed = true;
    }
    data = 0;
    ebx = 0;
    error = uc_mem_read(uc, DATA_ADDRESS, &data, sizeof(data));
    error = error == UC_ERR_OK ? uc_reg_read(uc, UC_X86_REG_EBX, &ebx) : error;
    if (error != UC_ERR_OK || data != write_eax || ebx != 1) {
        fprintf(stderr,
                "recovery write failed: data=0x%" PRIx32 " ebx=0x%" PRIx32 "\n",
                data, ebx);
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
