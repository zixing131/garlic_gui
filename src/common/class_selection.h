#ifndef GARLIC_CLASS_SELECTION_H
#define GARLIC_CLASS_SELECTION_H

/* Optional CLI browsing mode. Configure before starting worker threads.
 * Names accepted here are DEX descriptors, JVM names or JAR entry paths.
 * Indexing returns false so callers never schedule decompilation work. */
#include "cJSON.h"
void class_selection_write(cJSON *entry);
int class_selection_open(const char *index_path, const char *class_name);
int class_selection_accept(const char *name);
int class_selection_close(void);
int class_selection_indexing(void);

#endif
