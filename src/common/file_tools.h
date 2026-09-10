#ifndef GARLIC_FILE_TOOLS_H
#define GARLIC_FILE_TOOLS_H

#include <unistd.h>
#include <stdbool.h>
#include <sys/stat.h>
#include <stdlib.h>
#include <string.h>

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
