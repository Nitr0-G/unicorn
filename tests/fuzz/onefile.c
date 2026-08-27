#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>

#define MAX_INPUT_SIZE 4096

int LLVMFuzzerTestOneInput(const uint8_t *Data, size_t Size);

int main(int argc, char **argv)
{
    uint8_t *data;
    long file_size;
    size_t size;
    FILE *file;

    if (argc != 2) {
        return 1;
    }
    file = fopen(argv[1], "rb");
    if (file == NULL) {
        return 2;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 2;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return 2;
    }
    if (file_size > MAX_INPUT_SIZE) {
        fclose(file);
        return 3;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 2;
    }
    size = (size_t)file_size;
    data = malloc(size == 0 ? 1 : size);
    if (data == NULL) {
        fclose(file);
        return 2;
    }
    if (size != 0 && fread(data, 1, size, file) != size) {
        free(data);
        fclose(file);
        return 2;
    }

    LLVMFuzzerTestOneInput(data, size);
    free(data);
    fclose(file);
    return 0;
}
