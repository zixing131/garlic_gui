#ifndef GARLIC_BROWSE_INDEX_H
#define GARLIC_BROWSE_INDEX_H
#include "parser/class/metadata.h"
#include "parser/dex/metadata.h"
int browse_index_dex_directory(const unsigned char *data, size_t size);
void browse_index_dex(jd_meta_dex *meta, dex_class_def *cf);
void browse_index_jvm(jclass_file *jc, int inner);
#endif
