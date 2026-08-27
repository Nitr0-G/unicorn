#ifndef UNICORN_TESTS_FUZZ_EMU_H
#define UNICORN_TESTS_FUZZ_EMU_H

#include <stddef.h>
#include <stdint.h>
#include <stdlib.h>

#include <unicorn/unicorn.h>

#ifndef UC_FUZZ_ARCH
#error UC_FUZZ_ARCH must be defined
#endif

#ifndef UC_FUZZ_MODE
#error UC_FUZZ_MODE must be defined
#endif

#define FUZZ_CODE_ADDRESS UINT64_C(0x1000000)
#define FUZZ_MAP_SIZE (4 * 1024 * 1024)
#define FUZZ_MAX_INPUT_SIZE 4096
#define FUZZ_MAX_INSTRUCTIONS 4096

static void fuzz_require_ok(uc_err err)
{
    if (err != UC_ERR_OK) {
        abort();
    }
}

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size)
{
    uc_engine *uc = NULL;

    if (size > FUZZ_MAX_INPUT_SIZE) {
        return 0;
    }

    fuzz_require_ok(uc_open(UC_FUZZ_ARCH, UC_FUZZ_MODE, &uc));
    fuzz_require_ok(
        uc_mem_map(uc, FUZZ_CODE_ADDRESS, FUZZ_MAP_SIZE, UC_PROT_ALL));
    if (size != 0) {
        fuzz_require_ok(uc_mem_write(uc, FUZZ_CODE_ADDRESS, data, size));
    }

    (void)uc_emu_start(uc, FUZZ_CODE_ADDRESS, FUZZ_CODE_ADDRESS + size, 0,
                       FUZZ_MAX_INSTRUCTIONS);
    fuzz_require_ok(uc_close(uc));
    return 0;
}

#endif
