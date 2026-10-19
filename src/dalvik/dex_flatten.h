#ifndef GARLIC_DEX_FLATTEN_H
#define GARLIC_DEX_FLATTEN_H

#include "decompiler/method.h"
#include "dalvik/dex_structure.h"

typedef void (*dex_graph_rebuild)(jd_method *m);

/* Opt-in via GARLIC_UNFLATTEN=1. No effect when the method has exception handlers. */
void dex_flatten_method(jd_method *m, encoded_method *em, dex_graph_rebuild rebuild);

#endif
