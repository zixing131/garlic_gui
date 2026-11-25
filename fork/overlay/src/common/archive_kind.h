#ifndef GARLIC_ARCHIVE_KIND_H
#define GARLIC_ARCHIVE_KIND_H

#include "common/str_tools.h"
#include "libs/zip/zip.h"

static inline int zip_name_has_slash(const char *name, size_t length)
{
    for (size_t i = 0; i < length; ++i)
        if (name[i] == '/')
            return 1;
    return 0;
}

static inline int zip_name_ends(const char *name, size_t length, const char *suffix)
{
    size_t n = strlen(suffix);
    if (length < n || name == NULL)
        return 0;
    for (size_t i = 0; i < n; ++i) {
        unsigned char a = (unsigned char)name[length - n + i];
        unsigned char b = (unsigned char)suffix[i];
        if (ascii_lower(a) != ascii_lower(b))
            return 0;
    }
    return 1;
}

/* Classify a ZIP by its entries. The outer filename is not consulted.
 * Root DEX and nested APK entries use the APK pipeline, which ignores
 * unrelated .dex files in subdirectories. A container whose only DEX
 * files live in subdirectories still has to be opened with nested DEX
 * enabled. Names are read from the central directory; a root DEX or a
 * nested APK decides the pipeline immediately. */
static inline void zip_classify(const char *path, int *has_class, int *has_root_dex,
                                int *has_nested_dex, int *has_apk)
{
    *has_class = 0;
    *has_root_dex = 0;
    *has_nested_dex = 0;
    *has_apk = 0;
    if (path == NULL)
        return;
    struct zip_t *zip = zip_open(path, 0, 'r');
    if (zip == NULL)
        return;
    const int total = (int)zip_entries_total(zip);
    for (int i = 0; i < total; ++i) {
        const char *name = NULL;
        size_t length = 0;
        if (zip_entry_filename(zip, (size_t)i, &name, &length) < 0 || length == 0)
            continue;
        if (name[length - 1] == '/')
            continue;
        if (zip_name_ends(name, length, ".class"))
            *has_class = 1;
        else if (zip_name_ends(name, length, ".apk"))
            *has_apk = 1;
        else if (zip_name_ends(name, length, ".dex")) {
            if (zip_name_has_slash(name, length))
                *has_nested_dex = 1;
            else
                *has_root_dex = 1;
        }
        if (*has_root_dex || *has_apk)
            break;
    }
    zip_close(zip);
}

static inline void zip_content_flags(const char *path, int *has_class, int *has_dex)
{
    int root = 0, nested = 0, apk = 0;
    zip_classify(path, has_class, &root, &nested, &apk);
    *has_dex = root || nested || apk;
}

#endif
