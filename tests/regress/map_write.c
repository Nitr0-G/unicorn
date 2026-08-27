#include <stdio.h>
#include <stdlib.h>

#include <unicorn/unicorn.h>

#define MAP_ADDRESS UINT64_C(0x400000)
#define MAP_SIZE (64 * 1024)
#define OVERFLOW_SIZE (MAP_SIZE + 1)

int main(void)
{
    uint8_t *input = NULL;
    uint8_t *output = NULL;
    uc_engine *uc = NULL;
    uc_err err;
    size_t i;

    input = malloc(OVERFLOW_SIZE);
    output = malloc(MAP_SIZE);
    if (input == NULL || output == NULL) {
        fprintf(stderr, "allocation failed\n");
        goto fail;
    }
    for (i = 0; i < OVERFLOW_SIZE; i++) {
        input[i] = (uint8_t)i;
    }

    err = uc_open(UC_ARCH_X86, UC_MODE_64, &uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_open failed: %s\n", uc_strerror(err));
        goto fail;
    }
    err = uc_mem_map(uc, MAP_ADDRESS, MAP_SIZE, UC_PROT_ALL);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_map failed: %s\n", uc_strerror(err));
        goto fail;
    }

    err = uc_mem_write(uc, MAP_ADDRESS, input, OVERFLOW_SIZE);
    if (err != UC_ERR_WRITE_UNMAPPED) {
        fprintf(stderr, "expected UC_ERR_WRITE_UNMAPPED, got %s\n",
                uc_strerror(err));
        goto fail;
    }
    err = uc_mem_read(uc, MAP_ADDRESS, output, MAP_SIZE);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_mem_read failed: %s\n", uc_strerror(err));
        goto fail;
    }
    for (i = 0; i < MAP_SIZE; i++) {
        if (output[i] != 0) {
            fprintf(stderr, "failed write changed byte %zu\n", i);
            goto fail;
        }
    }

    err = uc_close(uc);
    if (err != UC_ERR_OK) {
        fprintf(stderr, "uc_close failed: %s\n", uc_strerror(err));
        uc = NULL;
        goto fail;
    }
    free(output);
    free(input);
    return 0;

fail:
    if (uc != NULL) {
        uc_close(uc);
    }
    free(output);
    free(input);
    return 1;
}
