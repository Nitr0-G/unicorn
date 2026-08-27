#include <stdlib.h>
#include <string.h>

#include <unicorn/unicorn.h>

#define ADDRESS 0x1000
#define INVALID_SIZE 13000
#define VALID_SIZE 0x4000

int main(void)
{
    uint8_t *data;
    uc_engine *uc;

    data = malloc(VALID_SIZE);
    if (data == NULL) {
        return 1;
    }
    memset(data, 0xa5, VALID_SIZE);

    if (uc_open(UC_ARCH_X86, UC_MODE_64, &uc) != UC_ERR_OK) {
        free(data);
        return 1;
    }
    if (uc_mem_map(uc, ADDRESS, INVALID_SIZE, UC_PROT_ALL) != UC_ERR_ARG ||
        uc_mem_map(uc, ADDRESS, VALID_SIZE, UC_PROT_ALL) != UC_ERR_OK ||
        uc_mem_write(uc, ADDRESS, data, VALID_SIZE) != UC_ERR_OK) {
        uc_close(uc);
        free(data);
        return 1;
    }

    free(data);
    return uc_close(uc) == UC_ERR_OK ? 0 : 1;
}
