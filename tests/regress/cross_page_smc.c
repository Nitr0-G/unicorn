#include <inttypes.h>
#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <unicorn/unicorn.h>

#define MAP_ADDRESS UINT64_C(0x1000)
#define TARGET_ADDRESS UINT64_C(0x1ffd)
#define CALLBACK_TARGET_ADDRESS UINT64_C(0x1800)
#define WRITER_ADDRESS UINT64_C(0x3000)

typedef struct WriteHookState {
    uint64_t address;
    int64_t value;
    int size;
    unsigned int count;
} WriteHookState;

typedef struct CallbackSmcState {
    uc_err write_error;
    unsigned int count;
} CallbackSmcState;

static void record_write(uc_engine *uc, uc_mem_type type, uint64_t address,
                         int size, int64_t value, void *user_data)
{
    WriteHookState *state = user_data;

    (void)uc;
    (void)type;

    state->address = address;
    state->size = size;
    state->value = value;
    state->count++;
}

static void modify_future_page(uc_engine *uc, uint64_t address, uint32_t size,
                               void *user_data)
{
    static const uint8_t replacement = 0x4b;
    CallbackSmcState *state = user_data;

    (void)address;
    (void)size;

    state->count++;
    if (state->count == 1) {
        state->write_error = uc_mem_write(uc, CALLBACK_TARGET_ADDRESS + 1,
                                          &replacement, sizeof(replacement));
    }
}

static void report_uc_error(const char *operation, uc_err error)
{
    fprintf(stderr, "%s failed with %u: %s\n", operation, (unsigned)error,
            uc_strerror(error));
}

static bool test_guest_cross_page_store(void)
{
    static const uint8_t original_code[] = {
        0xb8, 0x44, 0x33, 0x22, 0x11, /* mov eax, 0x11223344 */
    };
    static const uint8_t expected_code[] = {
        0xb8, 0x44, 0x88, 0x77, 0x11, /* mov eax, 0x11778844 */
    };
    static const uint8_t writer_code[] = {
        0x66, 0xc7, 0x05, 0xff, 0x1f, 0x00, 0x00, 0x88, 0x77,
        /* mov word ptr [0x1fff], 0x7788 */
    };
    WriteHookState state = {0};
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_err error;
    uint8_t code[sizeof(expected_code)];
    uint32_t eax = 0;
    bool failed = false;

    error = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_open(guest SMC)", error);
        return false;
    }
    error = uc_mem_map(uc, MAP_ADDRESS, 0x3000, UC_PROT_ALL);
    if (error == UC_ERR_OK) {
        error = uc_mem_write(uc, TARGET_ADDRESS, original_code,
                             sizeof(original_code));
    }
    if (error == UC_ERR_OK) {
        error =
            uc_mem_write(uc, WRITER_ADDRESS, writer_code, sizeof(writer_code));
    }
    if (error != UC_ERR_OK) {
        report_uc_error("guest SMC setup", error);
        failed = true;
        goto cleanup;
    }

    error = uc_emu_start(uc, TARGET_ADDRESS,
                         TARGET_ADDRESS + sizeof(original_code), 0, 0);
    if (error != UC_ERR_OK) {
        report_uc_error("initial target uc_emu_start", error);
        failed = true;
        goto cleanup;
    }
    error = uc_hook_add(uc, &hook, UC_HOOK_MEM_WRITE, record_write, &state,
                        TARGET_ADDRESS + 2, TARGET_ADDRESS + 2);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_hook_add(write)", error);
        failed = true;
        goto cleanup;
    }
    error = uc_emu_start(uc, WRITER_ADDRESS,
                         WRITER_ADDRESS + sizeof(writer_code), 0, 0);
    if (error != UC_ERR_OK) {
        report_uc_error("writer uc_emu_start", error);
        failed = true;
    }
    error = uc_mem_read(uc, TARGET_ADDRESS, code, sizeof(code));
    if (error != UC_ERR_OK) {
        report_uc_error("uc_mem_read(modified code)", error);
        failed = true;
    } else if (memcmp(code, expected_code, sizeof(code)) != 0) {
        fprintf(stderr, "cross-page store wrote unexpected bytes\n");
        failed = true;
    }
    if (state.count != 1 || state.address != TARGET_ADDRESS + 2 ||
        state.size != 2 || state.value != 0x7788) {
        fprintf(stderr,
                "unexpected cross-page hook: count=%u address=0x%" PRIx64
                " size=%d value=0x%" PRIx64 "\n",
                state.count, state.address, state.size, (uint64_t)state.value);
        failed = true;
    }

    eax = 0;
    error = uc_reg_write(uc, UC_X86_REG_EAX, &eax);
    if (error == UC_ERR_OK) {
        error = uc_emu_start(uc, TARGET_ADDRESS,
                             TARGET_ADDRESS + sizeof(expected_code), 0, 0);
    }
    if (error == UC_ERR_OK) {
        error = uc_reg_read(uc, UC_X86_REG_EAX, &eax);
    }
    if (error != UC_ERR_OK) {
        report_uc_error("modified target execution", error);
        failed = true;
    } else if (eax != 0x11778844) {
        fprintf(stderr, "stale target result: eax=0x%" PRIx32 "\n", eax);
        failed = true;
    }

cleanup:
    if (uc != NULL) {
        error = uc_close(uc);
        if (error != UC_ERR_OK) {
            report_uc_error("uc_close(guest SMC)", error);
            failed = true;
        }
    }
    return !failed;
}

static bool test_callback_active_tb_write(void)
{
    static const uint8_t code[] = {
        0x40, /* inc eax */
        0x43, /* inc ebx */
    };
    CallbackSmcState state = {UC_ERR_OK, 0};
    uc_engine *uc = NULL;
    uc_hook hook;
    uc_tb tb;
    uc_err error;
    uint8_t modified_byte = 0;
    uint32_t eax = 0;
    uint32_t ebx = 0;
    bool failed = false;

    error = uc_open(UC_ARCH_X86, UC_MODE_32, &uc);
    if (error != UC_ERR_OK) {
        report_uc_error("uc_open(callback SMC)", error);
        return false;
    }
    error = uc_mem_map(uc, MAP_ADDRESS, 0x2000, UC_PROT_ALL);
    if (error == UC_ERR_OK) {
        error = uc_mem_write(uc, CALLBACK_TARGET_ADDRESS, code, sizeof(code));
    }
    if (error == UC_ERR_OK) {
        error = uc_hook_add(uc, &hook, UC_HOOK_CODE, modify_future_page, &state,
                            CALLBACK_TARGET_ADDRESS, CALLBACK_TARGET_ADDRESS);
    }
    if (error == UC_ERR_OK) {
        error = uc_ctl_request_cache(uc, CALLBACK_TARGET_ADDRESS, &tb);
    }
    if (error != UC_ERR_OK) {
        report_uc_error("callback SMC setup", error);
        failed = true;
        goto cleanup;
    }
    if (tb.pc != CALLBACK_TARGET_ADDRESS || tb.size < sizeof(code)) {
        fprintf(stderr,
                "fixture did not create a two-page TB: pc=0x%" PRIx64
                " size=%u\n",
                tb.pc, (unsigned)tb.size);
        failed = true;
        goto cleanup;
    }

    error = uc_emu_start(uc, CALLBACK_TARGET_ADDRESS,
                         CALLBACK_TARGET_ADDRESS + sizeof(code), 0, 0);
    if (error != UC_ERR_OK) {
        report_uc_error("callback SMC uc_emu_start", error);
        failed = true;
    }
    error = uc_mem_read(uc, CALLBACK_TARGET_ADDRESS + 1, &modified_byte,
                        sizeof(modified_byte));
    if (error == UC_ERR_OK) {
        error = uc_reg_read(uc, UC_X86_REG_EAX, &eax);
    }
    if (error == UC_ERR_OK) {
        error = uc_reg_read(uc, UC_X86_REG_EBX, &ebx);
    }
    if (error != UC_ERR_OK) {
        report_uc_error("callback SMC verification", error);
        failed = true;
    } else if (state.write_error != UC_ERR_OK || state.count != 2 ||
               modified_byte != 0x4b || eax != 1 || ebx != UINT32_MAX) {
        fprintf(stderr,
                "stale future-page execution: write_error=%u count=%u "
                "byte=0x%02x eax=0x%" PRIx32 " ebx=0x%" PRIx32 "\n",
                (unsigned)state.write_error, state.count,
                (unsigned)modified_byte, eax, ebx);
        failed = true;
    }

cleanup:
    if (uc != NULL) {
        error = uc_close(uc);
        if (error != UC_ERR_OK) {
            report_uc_error("uc_close(callback SMC)", error);
            failed = true;
        }
    }
    return !failed;
}

int main(void)
{
    bool guest_store_ok = test_guest_cross_page_store();
    bool callback_write_ok = test_callback_active_tb_write();

    return guest_store_ok && callback_write_ok ? EXIT_SUCCESS : EXIT_FAILURE;
}
