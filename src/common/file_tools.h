#ifndef GARLIC_FILE_TOOLS_H
#define GARLIC_FILE_TOOLS_H

#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

/* GUI cache paths are UTF-8 hex, split into bounded components. This is
 * reversible and injective even on case-insensitive / Unicode-normalizing volumes. */
static inline char *hex_storage_name(const char *name)
{
    size_t len = strlen(name);
    char *result = malloc(16 + len * 2 + len / 32);
    if (!result) return NULL;
    memcpy(result, "_classes/", 9);
    size_t at = 9;
    const char *hex = "0123456789abcdef";
    for (size_t i = 0; i < len; ++i) {
        if (i && i % 32 == 0) result[at++] = '/';
        unsigned char c = (unsigned char)name[i];
        result[at++] = hex[c >> 4]; result[at++] = hex[c & 15];
    }
    result[at] = 0;
    return result;
}
static inline bool source_safe_paths_enabled(void)
{
    const char *enabled = getenv("GARLIC_SAFE_SOURCE_PATHS");
    if (enabled) return strcmp(enabled, "1") == 0;
#ifdef _WIN32
    return true;
#else
    return false;
#endif
}
static inline char *source_storage_name(const char *name)
{
    if (!source_safe_paths_enabled()) return strdup(name);
    size_t len = strlen(name);
    if (len > 2 && name[0] == 'L' && name[len - 1] == ';') {
        char *plain = strndup(name + 1, len - 2);
        char *result = hex_storage_name(plain);
        free(plain);
        return result;
    }
    return hex_storage_name(name);
}

static bool inline file_exist(const char *path)
{
    return access(path, F_OK) == 0;
}

static inline void make_dir(const char *dir)
{
    if (!dir || !*dir) return;
    size_t len = strlen(dir);
    char *tmp = malloc(len + 1);
    if (!tmp) return;
    memcpy(tmp, dir, len + 1);
    if (len > 1 && tmp[len - 1] == '/')
        tmp[len - 1] = 0;
    for (char *p = tmp + 1; *p; p++) {
        if (*p == '/'
#ifdef _WIN32
            || *p == '\\'
#endif
        ) {
            char separator = *p;
            *p = 0;
#ifdef _WIN32
            mkdir(tmp);
#else
            mkdir(tmp, S_IRWXU);
#endif
            *p = separator;
        }
    }
#ifdef _WIN32
    mkdir(tmp);
#else
    mkdir(tmp, S_IRWXU);
#endif
    free(tmp);
}

static inline void mkdir_p(string dir)
{
    struct stat sb;
    if (stat(dir, &sb) == -1)
        make_dir(dir);
}

#endif //GARLIC_FILE_TOOLS_H
