#ifndef GARLIC_ARCHIVE_KIND_H
#define GARLIC_ARCHIVE_KIND_H

#include "common/str_tools.h"
#include "libs/zip/zip.h"

/* Classify a ZIP by its entries. The outer filename is not consulted.
 * Root DEX and nested APK entries use the APK pipeline, which ignores
 * unrelated .dex files in subdirectories. A container whose only DEX
 * files live in subdirectories still has to be opened with nested DEX
 * enabled. */
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
        if (zip_entry_openbyindex(zip, i) < 0)
            continue;
        const char *name = zip_entry_name(zip);
        if (name != NULL && name[0] != '\0' && name[strlen(name) - 1] != '/') {
            if (str_end_with_lower(name, ".class"))
                *has_class = 1;
            else if (str_end_with_lower(name, ".apk"))
                *has_apk = 1;
            else if (str_end_with_lower(name, ".dex")) {
                if (strchr(name, '/') == NULL)
                    *has_root_dex = 1;
                else
                    *has_nested_dex = 1;
            }
        }
        zip_entry_close(zip);
        if ((*has_root_dex || *has_apk) && *has_class)
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
