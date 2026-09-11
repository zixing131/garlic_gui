#ifndef GARLIC_SOURCE_MAP_H
#define GARLIC_SOURCE_MAP_H
#include "decompiler/structure.h"
int source_java_packed(void);
int source_java_pack(jsource_file *jf, const char *data, size_t length);
void source_map_begin(void);
void source_map_end(jsource_file *jf);
void source_map_definition(FILE *stream, long start, const char *owner, const char *name,
                           const char *desc, const char *token, int method);
void source_map_method(FILE *stream, long start, jsource_file *jf, jd_method *method);
int source_map_tracks_expression(jd_exp *exp);
void source_map_expression(FILE *stream, long start, jd_exp *exp);
#endif
