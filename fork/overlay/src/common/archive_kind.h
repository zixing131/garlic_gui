#ifndef GARLIC_ARCHIVE_KIND_H
#define GARLIC_ARCHIVE_KIND_H

#include "common/str_tools.h"
#include "libs/zip/zip.h"

/* Classify a ZIP by its entries. The filename suffix is not consulted. */
static inline void zip_content_flags(const char *path, int *has_class, int *has_dex)
{
    *has_class = 0;
    *has_dex = 0;
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
        if (name != NULL) {
            if (str_end_with_lower(name, ".class"))
                *has_class = 1;
            if (str_end_with_lower(name, ".dex") || str_end_with_lower(name, ".apk"))
                *has_dex = 1;
        }
        zip_entry_close(zip);
        if (*has_class && *has_dex)
            break;
    }
    zip_close(zip);
}

#endif
