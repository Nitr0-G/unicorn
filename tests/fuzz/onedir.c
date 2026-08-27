#include <stdint.h>
#include <stdio.h>
#include <string.h>

#ifdef _WIN32
#include <windows.h>
#else
#include <dirent.h>
#include <sys/stat.h>
#endif

#define MAX_INPUT_SIZE 4096
#define MAX_PATH_SIZE 4096
#define RUN_FILE_SKIPPED 1

int LLVMFuzzerTestOneInput(const uint8_t *data, size_t size);

static int run_file(const char *path)
{
    uint8_t data[MAX_INPUT_SIZE];
    long file_size;
    FILE *file;

    file = fopen(path, "rb");
    if (file == NULL) {
        return 3;
    }
    if (fseek(file, 0, SEEK_END) != 0) {
        fclose(file);
        return 4;
    }
    file_size = ftell(file);
    if (file_size < 0) {
        fclose(file);
        return 5;
    }
    if (file_size > MAX_INPUT_SIZE) {
        fclose(file);
        return RUN_FILE_SKIPPED;
    }
    if (fseek(file, 0, SEEK_SET) != 0) {
        fclose(file);
        return 6;
    }
    if (file_size != 0 &&
        fread(data, 1, (size_t)file_size, file) != (size_t)file_size) {
        fclose(file);
        return 7;
    }
    fclose(file);
    LLVMFuzzerTestOneInput(data, (size_t)file_size);
    return 0;
}

#ifdef _WIN32
static int run_directory(const char *directory, unsigned int *file_count)
{
    WIN32_FIND_DATAA entry;
    char pattern[MAX_PATH_SIZE];
    char path[MAX_PATH_SIZE];
    HANDLE find;
    int result = 0;
    int length;

    length = snprintf(pattern, sizeof(pattern), "%s\\*", directory);
    if (length < 0 || (size_t)length >= sizeof(pattern)) {
        return 2;
    }
    find = FindFirstFileA(pattern, &entry);
    if (find == INVALID_HANDLE_VALUE) {
        return 2;
    }
    do {
        if ((entry.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        length =
            snprintf(path, sizeof(path), "%s\\%s", directory, entry.cFileName);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            result = 2;
            break;
        }
        result = run_file(path);
        if (result == RUN_FILE_SKIPPED) {
            result = 0;
            continue;
        }
        if (result != 0) {
            break;
        }
        (*file_count)++;
    } while (FindNextFileA(find, &entry));
    if (result == 0 && GetLastError() != ERROR_NO_MORE_FILES) {
        result = 2;
    }
    FindClose(find);
    return result;
}
#else
static int run_directory(const char *directory, unsigned int *file_count)
{
    char path[MAX_PATH_SIZE];
    struct dirent *entry;
    struct stat status;
    DIR *dir;
    int result = 0;
    int length;

    dir = opendir(directory);
    if (dir == NULL) {
        return 2;
    }
    while ((entry = readdir(dir)) != NULL) {
        if (strcmp(entry->d_name, ".") == 0 ||
            strcmp(entry->d_name, "..") == 0) {
            continue;
        }
        length =
            snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        if (length < 0 || (size_t)length >= sizeof(path)) {
            result = 2;
            break;
        }
        if (stat(path, &status) != 0 || !S_ISREG(status.st_mode)) {
            continue;
        }
        result = run_file(path);
        if (result == RUN_FILE_SKIPPED) {
            result = 0;
            continue;
        }
        if (result != 0) {
            break;
        }
        (*file_count)++;
    }
    closedir(dir);
    return result;
}
#endif

int main(int argc, char **argv)
{
    unsigned int file_count = 0;
    int result;

    if (argc != 2) {
        return 1;
    }
    result = run_directory(argv[1], &file_count);
    if (result != 0) {
        return result;
    }
    return file_count == 0 ? 8 : 0;
}
