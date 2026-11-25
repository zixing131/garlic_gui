#include "browse_index.h"
#include "libs/threadpool/threadpool.h"
#include "decompiler/klass.h"
#include "cJSON.h"
#include "class_selection.h"
#include "dalvik/dex_descriptor.h"
#include "dalvik/dex_ins_helper.h"
#include "dalvik/dex_meta_helper.h"
#include "parser/class/class_tools.h"
#include "parser/dex/dex_tools.h"
#include "libs/hashmap/hashmap_tools.h"

#include <limits.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static __thread hashmap *reference_ids, *reference_groups;
static __thread cJSON *reference_dictionary;
static __thread int reference_count;

static cJSON *class_json(const char *name, unsigned flags, int inner) {
    cJSON *entry = cJSON_CreateObject();
    cJSON_AddStringToObject(entry, "name", name);
    cJSON_AddNumberToObject(entry, "flags", flags);
    cJSON_AddStringToObject(entry, "kind",
                            flags & 0x2000   ? "annotation"
                            : flags & 0x4000 ? "enum"
                            : flags & 0x0200 ? "interface"
                            : flags & 0x0400 ? "abstract"
                                             : "class");
    cJSON_AddNumberToObject(entry, "instruction_units", 0);
    cJSON_AddBoolToObject(entry, "inner", inner);
    cJSON_AddItemToObject(entry, "methods", cJSON_CreateArray());
    cJSON_AddItemToObject(entry, "fields", cJSON_CreateArray());
    const char *version = getenv("GARLIC_COMPACT_INDEX");
    if (!version) version = "2";
    const int compact = *version && strcmp(version, "0");
    cJSON_AddItemToObject(entry, "refs", compact ? cJSON_CreateObject() : cJSON_CreateArray());
    reference_groups = compact ? hashmap_init((hcmp_fn)s2o_cmp, 32) : NULL;
    reference_ids = compact && !strcmp(version, "2") ? hashmap_init((hcmp_fn)s2i_cmp, 128) : NULL;
    reference_dictionary = NULL;
    reference_count = 0;
    if (reference_ids) {
        reference_dictionary = cJSON_CreateArray();
        cJSON_AddItemToObject(entry, "ref_targets", reference_dictionary);
    }
    return entry;
}
static void member(cJSON *array, const char *owner, const char *name, const char *desc,
                   unsigned flags, int method) {
    cJSON *m = cJSON_CreateObject();
    cJSON_AddStringToObject(m, "id",
                            str_create("L%s;->%s%s%s", owner, name, method ? "" : ":", desc));
    cJSON_AddStringToObject(m, "name", name);
    cJSON_AddStringToObject(m, "descriptor", desc);
    cJSON_AddNumberToObject(m, "flags", flags);
    cJSON_AddItemToArray(array, m);
}
static void reference(cJSON *array, const char *from, const char *target, int offset,
                      const char *kind) {
    if (cJSON_IsObject(array)) {
        cJSON *group = hget_s2o(reference_groups, (string)from);
        if (!group) {
            group = cJSON_CreateArray(); cJSON_AddItemToObject(array, from, group);
            hset_s2o(reference_groups, (string)from, group);
        }
        cJSON *row = cJSON_CreateArray();
        if (reference_ids) {
            int id = hget_s2i(reference_ids, (string)target);
            if (id < 0) {
                id = reference_count++;
                hset_s2i(reference_ids, (string)target, id);
                cJSON_AddItemToArray(reference_dictionary, cJSON_CreateString(target));
            }
            cJSON_AddItemToArray(row, cJSON_CreateNumber(id));
        } else cJSON_AddItemToArray(row, cJSON_CreateString(target));
        cJSON_AddItemToArray(row, cJSON_CreateNumber(offset));
        if (strcmp(kind, "bytecode")) cJSON_AddItemToArray(row, cJSON_CreateString(kind));
        cJSON_AddItemToArray(group, row);
        return;
    }
    cJSON *r = cJSON_CreateObject();
    cJSON_AddStringToObject(r, "from", from);
    cJSON_AddStringToObject(r, "target", target);
    cJSON_AddNumberToObject(r, "offset", offset);
    cJSON_AddStringToObject(r, "kind", kind);
    cJSON_AddItemToArray(array, r);
}
static void descriptor_references(cJSON *refs, const char *from, const char *desc, const char *kind) {
    while (desc && (desc = strchr(desc, 'L'))) {
        const char *end = desc + 1;
        while (*end && *end != ';' && *end != '<') ++end;
        if (!*end) break;
        string target = x_alloc((size_t)(end - desc) + 2);
        memcpy(target, desc, (size_t)(end - desc));
        target[end - desc] = ';'; target[end - desc + 1] = 0;
        reference(refs, from, target, -1, kind);
        desc = end + 1;
    }
}
static void signature_references(cJSON *entry) {
    const char *kinds[] = {"methods", "fields"};
    for (int k = 0; k < 2; k++) {
        cJSON *m = NULL;
        cJSON_ArrayForEach(m, cJSON_GetObjectItem(entry, kinds[k]))
            descriptor_references(cJSON_GetObjectItem(entry, "refs"),
                cJSON_GetObjectItem(m, "id")->valuestring,
                cJSON_GetObjectItem(m, "descriptor")->valuestring, "signature");
    }
}
static jd_meta_dex *proto_meta;
static string *proto_desc, *method_desc, *field_desc;
static u4 proto_desc_n, method_desc_n, field_desc_n;
static int id_cache_ready;

static string dex_proto_to_descriptor(jd_meta_dex *meta, dex_proto_id *proto) {
    str_list *list = str_list_init();
    str_concat(list, "(");
    if (proto->type_list)
        for (u4 i = 0; i < proto->type_list->size; i++)
            str_concat(list, dex_str_of_type_id(meta, proto->type_list->list[i].type_idx));
    str_concat(list, ")");
    str_concat(list, dex_str_of_type_id(meta, proto->return_type_idx));
    return str_join(list);
}
static string dex_mid(jd_meta_dex *meta, unsigned idx);
static string dex_fid(jd_meta_dex *meta, unsigned idx);
static string proto_of(jd_meta_dex *meta, u4 idx)
{
    if (proto_meta == meta && idx < proto_desc_n && proto_desc[idx])
        return proto_desc[idx];
    return dex_proto_to_descriptor(meta, &meta->proto_ids[idx]);
}
void browse_index_dex_end(void)
{
    proto_meta = NULL;
    proto_desc = method_desc = field_desc = NULL;
    proto_desc_n = method_desc_n = field_desc_n = 0;
    id_cache_ready = 0;
}
void browse_index_dex_begin(jd_meta_dex *meta)
{
    browse_index_dex_end();
    if (!meta || !meta->header || !meta->pool)
        return;
    thread_local_data *tls = get_thread_local_data();
    mem_pool *previous = tls ? tls->pool : global_pool;
    if (tls) tls->pool = meta->pool; else global_pool = meta->pool;
    proto_desc_n = meta->header->proto_ids_size;
    proto_desc = x_alloc_in(meta->pool, sizeof(string) * (proto_desc_n + 1));
    for (u4 i = 0; i < proto_desc_n; ++i)
        proto_desc[i] = dex_proto_to_descriptor(meta, &meta->proto_ids[i]);
    method_desc_n = meta->header->method_ids_size;
    field_desc_n = meta->header->field_ids_size;
    method_desc = x_alloc_in(meta->pool, sizeof(string) * (method_desc_n + 1));
    field_desc = x_alloc_in(meta->pool, sizeof(string) * (field_desc_n + 1));
    proto_meta = meta;
    for (u4 i = 0; i < method_desc_n; ++i)
        method_desc[i] = dex_mid(meta, i);
    for (u4 i = 0; i < field_desc_n; ++i)
        field_desc[i] = dex_fid(meta, i);
    id_cache_ready = 1;
    if (tls) tls->pool = previous; else global_pool = previous;
}
static string dex_mid(jd_meta_dex *meta, unsigned idx) {
    if (id_cache_ready && proto_meta == meta && idx < method_desc_n)
        return method_desc[idx];
    dex_method_id *m = &meta->method_ids[idx];
    return str_create("%s->%s%s", dex_str_of_type_id(meta, m->class_idx),
                      dex_str_of_idx(meta, m->name_idx),
                      proto_of(meta, m->proto_idx));
}
static string dex_fid(jd_meta_dex *meta, unsigned idx) {
    if (id_cache_ready && proto_meta == meta && idx < field_desc_n)
        return field_desc[idx];
    dex_field_id *f = &meta->field_ids[idx];
    return str_create("%s->%s:%s", dex_str_of_type_id(meta, f->class_idx),
                      dex_str_of_idx(meta, f->name_idx), dex_str_of_type_id(meta, f->type_idx));
}
typedef struct {
    char *data;
    size_t len, cap;
    int err;
} idx_buf;
typedef struct idx_group {
    const char *from;
    idx_buf body;
    struct idx_group *next;
} idx_group;
typedef struct {
    const char *id, *desc;
    int method;
} idx_sig;
typedef struct idx {
    int compact;
    uint64_t units;
    idx_buf methods, fields, targets, flat;
    hashmap *groups, *ids;
    idx_group *head, *tail;
    int target_count;
    idx_sig *sigs;
    size_t sig_n, sig_cap;
} idx;

static void idx_add(idx_buf *b, const char *s, size_t n)
{
    if (!b || b->err) return;
    if (b->cap < b->len + n + 1) {
        size_t cap = b->cap ? b->cap : 256;
        while (cap < b->len + n + 1) cap *= 2;
        char *next = realloc(b->data, cap);
        if (!next) { b->err = 1; return; }
        b->data = next;
        b->cap = cap;
    }
    if (n) memcpy(b->data + b->len, s, n);
    b->len += n;
    b->data[b->len] = 0;
}
static void idx_puts(idx_buf *b, const char *s) { idx_add(b, s, strlen(s)); }
static void idx_json(idx_buf *b, const char *text)
{
    idx_add(b, "\"", 1);
    if (!text) { idx_add(b, "\"", 1); return; }
    for (const unsigned char *p = (const unsigned char *)text; *p; ++p) {
        if (*p == '"' || *p == '\\') {
            char escaped[2] = {'\\', (char)*p};
            idx_add(b, escaped, 2);
        } else if (*p == '\b') idx_puts(b, "\\b");
        else if (*p == '\f') idx_puts(b, "\\f");
        else if (*p == '\n') idx_puts(b, "\\n");
        else if (*p == '\r') idx_puts(b, "\\r");
        else if (*p == '\t') idx_puts(b, "\\t");
        else if (*p < 32) {
            char hex[8];
            int n = snprintf(hex, sizeof hex, "\\u%04x", *p);
            if (n > 0) idx_add(b, hex, (size_t)n);
        } else {
            char raw = (char)*p;
            idx_add(b, &raw, 1);
        }
    }
    idx_add(b, "\"", 1);
}
static void idx_int(idx_buf *b, int value)
{
    char tmp[16];
    int n = snprintf(tmp, sizeof tmp, "%d", value);
    if (n > 0) idx_add(b, tmp, (size_t)n);
}
static int idx_compact_mode(void)
{
    const char *version = getenv("GARLIC_COMPACT_INDEX");
    if (!version) return 2;
    if (!*version || !strcmp(version, "0")) return 0;
    return !strcmp(version, "2") ? 2 : 1;
}
static void idx_init(idx *x)
{
    memset(x, 0, sizeof *x);
    x->compact = idx_compact_mode();
    if (x->compact)
        x->groups = hashmap_init((hcmp_fn)s2o_cmp, 32);
    if (x->compact == 2)
        x->ids = hashmap_init((hcmp_fn)s2i_cmp, 128);
}
static void idx_reference(idx *x, const char *from, const char *target, int offset, const char *kind)
{
    int bytecode = kind && !strcmp(kind, "bytecode");
    if (x->compact) {
        idx_group *group = hget_s2o(x->groups, (string)from);
        if (!group) {
            group = x_alloc(sizeof *group);
            memset(group, 0, sizeof *group);
            group->from = from;
            if (x->tail) x->tail->next = group; else x->head = group;
            x->tail = group;
            hset_s2o(x->groups, (string)from, group);
        }
        if (group->body.len) idx_add(&group->body, ",", 1);
        idx_add(&group->body, "[", 1);
        if (x->compact == 2) {
            int id = hget_s2i(x->ids, (string)target);
            if (id < 0) {
                id = x->target_count++;
                hset_s2i(x->ids, (string)target, id);
                if (x->targets.len) idx_add(&x->targets, ",", 1);
                idx_json(&x->targets, target);
            }
            idx_int(&group->body, id);
        } else idx_json(&group->body, target);
        idx_add(&group->body, ",", 1);
        idx_int(&group->body, offset);
        if (!bytecode) {
            idx_add(&group->body, ",", 1);
            idx_json(&group->body, kind);
        }
        idx_add(&group->body, "]", 1);
        return;
    }
    if (x->flat.len) idx_add(&x->flat, ",", 1);
    idx_puts(&x->flat, "{\"from\":");
    idx_json(&x->flat, from);
    idx_puts(&x->flat, ",\"target\":");
    idx_json(&x->flat, target);
    idx_puts(&x->flat, ",\"offset\":");
    idx_int(&x->flat, offset);
    idx_puts(&x->flat, ",\"kind\":");
    idx_json(&x->flat, kind);
    idx_add(&x->flat, "}", 1);
}
static void idx_descriptor_references(idx *x, const char *from, const char *desc, const char *kind)
{
    while (desc && (desc = strchr(desc, 'L'))) {
        const char *end = desc + 1;
        while (*end && *end != ';' && *end != '<') ++end;
        if (!*end) break;
        string target = x_alloc((size_t)(end - desc) + 2);
        memcpy(target, desc, (size_t)(end - desc));
        target[end - desc] = ';';
        target[end - desc + 1] = 0;
        idx_reference(x, from, target, -1, kind);
        desc = end + 1;
    }
}
static void idx_member(idx *x, int method, const char *owner, const char *name,
                       const char *desc, unsigned flags)
{
    idx_buf *body = method ? &x->methods : &x->fields;
    const char *id = method ? str_create("L%s;->%s%s", owner, name, desc)
                            : str_create("L%s;->%s:%s", owner, name, desc);
    if (body->len) idx_add(body, ",", 1);
    idx_puts(body, "{\"id\":");
    idx_json(body, id);
    idx_puts(body, ",\"name\":");
    idx_json(body, name);
    idx_puts(body, ",\"descriptor\":");
    idx_json(body, desc);
    idx_puts(body, ",\"flags\":");
    idx_int(body, (int)flags);
    idx_add(body, "}", 1);
    if (x->sig_n == x->sig_cap) {
        size_t cap = x->sig_cap ? x->sig_cap * 2 : 16;
        idx_sig *next = realloc(x->sigs, cap * sizeof *next);
        if (!next) return;
        x->sigs = next;
        x->sig_cap = cap;
    }
    x->sigs[x->sig_n].id = id;
    x->sigs[x->sig_n].desc = desc;
    x->sigs[x->sig_n].method = method;
    x->sig_n++;
}
static const char *idx_kind(unsigned flags)
{
    return flags & 0x2000 ? "annotation"
         : flags & 0x4000 ? "enum"
         : flags & 0x0200 ? "interface"
         : flags & 0x0400 ? "abstract"
                          : "class";
}
static char *idx_finish(idx *x, const char *name, unsigned flags, int inner)
{
    idx_buf out = {0};
    char units[32];
    int unit_len = x->units <= (uint64_t)INT_MAX
        ? snprintf(units, sizeof units, "%d", (int)x->units)
        : snprintf(units, sizeof units, "%.15g", (double)x->units);
    idx_puts(&out, "{\"name\":");
    idx_json(&out, name);
    idx_puts(&out, ",\"flags\":");
    idx_int(&out, (int)flags);
    idx_puts(&out, ",\"kind\":");
    idx_json(&out, idx_kind(flags));
    idx_puts(&out, ",\"instruction_units\":");
    if (unit_len > 0) idx_add(&out, units, (size_t)unit_len);
    idx_puts(&out, ",\"inner\":");
    idx_puts(&out, inner ? "true" : "false");
    idx_puts(&out, ",\"methods\":[");
    idx_add(&out, x->methods.data, x->methods.len);
    idx_puts(&out, "],\"fields\":[");
    idx_add(&out, x->fields.data, x->fields.len);
    idx_puts(&out, "],\"refs\":");
    if (x->compact) {
        idx_add(&out, "{", 1);
        for (idx_group *group = x->head; group; group = group->next) {
            if (group != x->head) idx_add(&out, ",", 1);
            idx_json(&out, group->from);
            idx_puts(&out, ":[");
            idx_add(&out, group->body.data, group->body.len);
            idx_add(&out, "]", 1);
        }
        idx_add(&out, "}", 1);
    } else {
        idx_add(&out, "[", 1);
        idx_add(&out, x->flat.data, x->flat.len);
        idx_add(&out, "]", 1);
    }
    if (x->compact == 2) {
        idx_puts(&out, ",\"ref_targets\":[");
        idx_add(&out, x->targets.data, x->targets.len);
        idx_add(&out, "]", 1);
    }
    const char *origin = class_selection_origin_text();
    if (origin) {
        idx_puts(&out, ",\"origin\":");
        idx_json(&out, origin);
    }
    idx_add(&out, "}", 1);
    if (out.err) { free(out.data); return strdup(""); }
    return out.data ? out.data : strdup("");
}
static void idx_dispose(idx *x)
{
    free(x->methods.data);
    free(x->fields.data);
    free(x->targets.data);
    free(x->flat.data);
    free(x->sigs);
    for (idx_group *group = x->head; group; group = group->next)
        free(group->body.data);
}

static void dex_annotation_refs(idx *, jd_meta_dex *, const char *, encoded_annotation *, int);
static void dex_annotation_value_refs(idx *x, jd_meta_dex *meta, const char *from, encoded_value *value, int depth) {
    if (!value || depth > 64) return;
    unsigned idx = 0;
    if (value->value_length <= 4 && value->value)
        for (size_t i = 0; i < value->value_length; ++i) idx |= (unsigned)value->value[i] << (i * 8);
    switch (value->value_type) {
        case kDexAnnotationType:
            if (idx < meta->header->type_ids_size)
                idx_descriptor_references(x, from, dex_str_of_type_id(meta, idx), "annotation-value");
            break;
        case kDexAnnotationField: case kDexAnnotationEnum:
            if (idx < meta->header->field_ids_size) idx_reference(x, from, dex_fid(meta, idx), -1, "annotation-value");
            break;
        case kDexAnnotationMethod:
            if (idx < meta->header->method_ids_size) idx_reference(x, from, dex_mid(meta, idx), -1, "annotation-value");
            break;
        case kDexAnnotationArray: {
            encoded_array *array = (encoded_array *)value->value;
            if (array) for (u4 i = 0; i < array->size; ++i)
                dex_annotation_value_refs(x, meta, from, &array->values[i], depth + 1);
            break;
        }
        case kDexAnnotationAnnotation:
            dex_annotation_refs(x, meta, from, (encoded_annotation *)value->value, depth + 1);
            break;
    }
}
static void dex_annotation_refs(idx *x, jd_meta_dex *meta, const char *from, encoded_annotation *annotation, int depth) {
    if (!annotation || depth > 64 || annotation->type_idx >= meta->header->type_ids_size) return;
    idx_reference(x, from, dex_str_of_type_id(meta, annotation->type_idx), -1, "annotation");
    if (!strcmp(dex_str_of_type_id(meta, annotation->type_idx), "Ldalvik/annotation/Signature;")) {
        str_list *signature = str_list_init();
        for (u4 i = 0; i < annotation->size; ++i) {
            encoded_value *value = annotation->elements[i].value;
            if (!value || value->value_type != kDexAnnotationArray || !value->value) continue;
            encoded_array *array = (encoded_array *)value->value;
            for (u4 j = 0; j < array->size; ++j) {
                encoded_value *part = &array->values[j]; unsigned idx = 0;
                if (part->value_type != kDexAnnotationString || !part->value || part->value_length > 4) continue;
                for (size_t k = 0; k < part->value_length; ++k) idx |= (unsigned)part->value[k] << (8 * k);
                if (idx < meta->header->string_ids_size) str_concat(signature, dex_str_of_idx(meta, idx));
            }
        }
        idx_descriptor_references(x, from, str_join(signature), "signature");
    }
    for (u4 i = 0; i < annotation->size; ++i)
        dex_annotation_value_refs(x, meta, from, annotation->elements[i].value, depth + 1);
}
static void dex_annotation_set_refs(idx *x, jd_meta_dex *meta, const char *from, annotation_set_item *set) {
    if (set) for (u4 i = 0; i < set->size; ++i)
        if (set->entries[i].annotation_item)
            dex_annotation_refs(x, meta, from, set->entries[i].annotation_item->encoded_annotation, 0);
}
static void dex_all_annotation_refs(idx *x, jd_meta_dex *meta, dex_class_def *cf, const char *from) {
    dex_annotations_directory_item *annotations = cf->annotations;
    if (!annotations) return;
    dex_annotation_set_refs(x, meta, from, annotations->class_annotation);
    for (u4 i = 0; i < annotations->fields_size; ++i) {
        field_annotation *field = &annotations->field_annotations[i];
        if (field->field_idx < meta->header->field_ids_size)
            dex_annotation_set_refs(x, meta, dex_fid(meta, field->field_idx), field->annotation);
    }
    for (u4 i = 0; i < annotations->methods_size; ++i) {
        method_annotation *method = &annotations->method_annotations[i];
        if (method->method_idx < meta->header->method_ids_size)
            dex_annotation_set_refs(x, meta, dex_mid(meta, method->method_idx), method->annotation);
    }
    for (u4 i = 0; i < annotations->parameters_size; ++i) {
        parameter_annotation *parameter = &annotations->parameter_annotations[i];
        if (!parameter->annotation || parameter->method_idx >= meta->header->method_ids_size) continue;
        const char *method = dex_mid(meta, parameter->method_idx);
        for (u4 j = 0; j < parameter->annotation->size; ++j)
            dex_annotation_set_refs(x, meta, method, parameter->annotation->list[j].annotation);
    }
}
static void dex_methods(idx *x, jd_meta_dex *meta, encoded_method *methods, unsigned size,
                        const char *owner) {
    for (unsigned n = 0; n < size; n++) {
        encoded_method *em = &methods[n];
        dex_method_id *m = &meta->method_ids[em->method_id];
        idx_member(x, 1, owner, dex_str_of_idx(meta, m->name_idx),
                   proto_of(meta, m->proto_idx), em->access_flags);
        dex_code_item *code = em->code;
        if (!code)
            continue;
        x->units += code->insns_size;
        string from = dex_mid(meta, em->method_id);
        for (u4 i = 0; i < code->insns_size;) {
            u2 word = code->insns[i];
            unsigned op = word & 255;
            uint64_t length = dex_opcode_len(op);
            if (!length)
                length = 1;
            if (op == 0 && word != 0) {
                if (i + 1 >= code->insns_size)
                    break;
                if (word == 0x0100)
                    length = 4 + 2 * (uint64_t)code->insns[i + 1];
                else if (word == 0x0200)
                    length = 2 + 4 * (uint64_t)code->insns[i + 1];
                else if (word == 0x0300) {
                    if (i + 3 >= code->insns_size)
                        break;
                    uint64_t count = code->insns[i + 2] | ((uint64_t)code->insns[i + 3] << 16);
                    length = 4 + (count * code->insns[i + 1] + 1) / 2;
                } else
                    break;
            }
            if (length > code->insns_size - i)
                break;
            if (length >= 2) {
                unsigned idx = code->insns[i + 1];
                string target = NULL;
                if (((op >= 0x6e && op <= 0x72) || (op >= 0x74 && op <= 0x78) || op == 0xfa ||
                     op == 0xfb) &&
                    idx < meta->header->method_ids_size)
                    target = dex_mid(meta, idx);
                else if (op >= 0x52 && op <= 0x6d && idx < meta->header->field_ids_size)
                    target = dex_fid(meta, idx);
                else if ((op == 0x1c || op == 0x1f || op == 0x20 || op == 0x22 || op == 0x23 ||
                          op == 0x24 || op == 0x25) &&
                         idx < meta->header->type_ids_size)
                    target = dex_str_of_type_id(meta, idx);
                if (target)
                    idx_reference(x, from, target, (int)i, "bytecode");
            }
            i += (u4)length;
        }
    }
}
char *browse_index_dex_json(jd_meta_dex *meta, dex_class_def *cf) {
    thread_local_data *tls = get_thread_local_data();
    mem_pool *previous_pool = tls ? tls->pool : global_pool;
    mem_pool *scratch = mem_create_pool();
    if (tls) tls->pool = scratch; else global_pool = scratch;
    string desc = dex_str_of_type_id(meta, cf->class_idx);
    string name = str_dup(desc + 1);
    if (name[0]) name[strlen(name) - 1] = 0;
    if (class_selection_excluded_name(name)) {
        mem_pool_free(scratch);
        if (tls) tls->pool = previous_pool; else global_pool = previous_pool;
        return strdup("");
    }
    const char *directory_mode = getenv("GARLIC_DIRECTORY_INDEX");
    if (directory_mode && !strcmp(directory_mode, "names")) {
        cJSON *entry = class_json(name, cf->access_flags, cf->is_inner || cf->is_anonymous);
        char *json = class_selection_format(entry);
        cJSON_Delete(entry);
        mem_pool_free(scratch);
        if (tls) tls->pool = previous_pool; else global_pool = previous_pool;
        return json;
    }
    idx built;
    idx_init(&built);
    if (cf->superclass_idx < meta->header->type_ids_size)
        idx_reference(&built, desc, dex_str_of_type_id(meta, cf->superclass_idx), -1, "extends");
    if (cf->interfaces)
        for (unsigned i = 0; i < cf->interfaces->size; i++)
            idx_reference(&built, desc,
                          dex_str_of_type_id(meta, cf->interfaces->list[i].type_idx), -1,
                          "implements");
    dex_class_data_item *data = cf->class_data;
    if (data) {
        for (int kind = 0; kind < 2; kind++) {
            encoded_field *fields = kind ? data->instance_fields : data->static_fields;
            unsigned count = kind ? data->instance_fields_size : data->static_fields_size;
            for (unsigned i = 0; i < count; i++) {
                dex_field_id *f = &meta->field_ids[fields[i].field_id];
                idx_member(&built, 0, name, dex_str_of_idx(meta, f->name_idx),
                           dex_str_of_type_id(meta, f->type_idx), fields[i].access_flags);
            }
        }
        dex_methods(&built, meta, data->direct_methods, data->direct_methods_size, name);
        dex_methods(&built, meta, data->virtual_methods, data->virtual_methods_size, name);
    }
    dex_all_annotation_refs(&built, meta, cf, desc);
    for (int pass = 1; pass >= 0; --pass)
        for (size_t i = 0; i < built.sig_n; ++i)
            if (built.sigs[i].method == pass)
                idx_descriptor_references(&built, built.sigs[i].id, built.sigs[i].desc, "signature");
    char *json = idx_finish(&built, name, cf->access_flags, cf->is_inner || cf->is_anonymous);
    idx_dispose(&built);
    mem_pool_free(scratch);
    if (tls) tls->pool = previous_pool; else global_pool = previous_pool;
    return json;
}
void browse_index_dex(jd_meta_dex *meta, dex_class_def *cf) {
    char *json = browse_index_dex_json(meta, cf);
    class_selection_write_line(json);
    free(json);
}
void browse_index_jvm(jclass_file *jc, int inner) {
    mem_pool *previous_pool = global_pool;
    global_pool = mem_create_pool();
    string name = get_class_name(jc, pool_item(jc, jc->this_class));
    cJSON *entry = class_json(name, be16toh(jc->access_flags), inner);
    if (jc->super_class)
        reference(cJSON_GetObjectItem(entry, "refs"), str_create("L%s;", name),
                  str_create("L%s;", get_class_name(jc, pool_item(jc, jc->super_class))), -1, "extends");
    for (unsigned i = 0; i < be16toh(jc->interfaces_count); i++)
        reference(cJSON_GetObjectItem(entry, "refs"), str_create("L%s;", name),
                  str_create("L%s;", get_class_name(jc, pool_item(jc, jc->interfaces[i]))), -1,
                  "implements");
    for (unsigned i = 0; i < be16toh(jc->methods_count); i++) {
        jmethod *m = &jc->methods[i];
        if (m->code_attribute) {
            cJSON *units = cJSON_GetObjectItem(entry, "instruction_units");
            cJSON_SetNumberValue(units, units->valuedouble + be32toh(m->code_attribute->code_length));
        }
        member(cJSON_GetObjectItem(entry, "methods"), name, pool_str(jc, m->name_index),
               pool_str(jc, m->descriptor_index), be16toh(m->access_flags), 1);
    }
    for (unsigned i = 0; i < be16toh(jc->fields_count); i++) {
        jfield *f = &jc->fields[i];
        member(cJSON_GetObjectItem(entry, "fields"), name, pool_str(jc, f->name_index),
               pool_str(jc, f->descriptor_index), be16toh(f->access_flags), 0);
    }
    for (unsigned i = 0; i < be16toh(jc->constant_pool_count) - 1; i++) {
        jcp_info *cp = &jc->constant_pool[i];
        string target = NULL;
        if (cp->tag == CONST_METHODREF_TAG || cp->tag == CONST_INTERFACEMETHODREF_TAG)
            target = str_create("L%s;->%s%s", get_method_class(jc, cp), get_method_name(jc, cp),
                                get_method_descriptor(jc, cp));
        else if (cp->tag == CONST_FIELDREF_TAG)
            target = str_create("L%s;->%s:%s", get_field_class(jc, cp), get_field_name(jc, cp),
                                get_field_descriptor(jc, cp));
        else if (cp->tag == CONST_CLASS_TAG)
            target = str_create("L%s;", get_class_name(jc, cp));
        if (target)
            reference(cJSON_GetObjectItem(entry, "refs"), str_create("L%s;", name), target, -1,
                      "constant-pool");
    }
    signature_references(entry);
    class_selection_write(entry);
    cJSON_Delete(entry);
    mem_free_pool();
    global_pool = previous_pool;
}

/* Directory-only reads need just three DEX tables. Avoid constructing strings,
 * methods, annotation graphs and hash maps for every item in a large APK. */
static uint32_t directory_u32(const unsigned char *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}
int browse_index_dex_directory(const unsigned char *data, size_t size) {
    const char *mode = getenv("GARLIC_DIRECTORY_INDEX");
    if (!class_selection_indexing() || !mode || strcmp(mode, "names")) return 0;
    if (size < 112 || memcmp(data, "dex\n", 4)) return -1;
    uint32_t strings = directory_u32(data + 56), string_off = directory_u32(data + 60);
    uint32_t types = directory_u32(data + 64), type_off = directory_u32(data + 68);
    uint32_t count = directory_u32(data + 96), class_off = directory_u32(data + 100);
    if ((uint64_t)string_off + (uint64_t)strings * 4 > size ||
        (uint64_t)type_off + (uint64_t)types * 4 > size ||
        (uint64_t)class_off + (uint64_t)count * 32 > size) return -1;
    for (uint32_t i = 0; i < count; ++i) {
        const unsigned char *definition = data + class_off + (size_t)i * 32;
        uint32_t type = directory_u32(definition), flags = directory_u32(definition + 4);
        if (type >= types) return -1;
        uint32_t string = directory_u32(data + type_off + (size_t)type * 4);
        if (string >= strings) return -1;
        size_t offset = directory_u32(data + string_off + (size_t)string * 4);
        unsigned n = 0;
        do { if (offset >= size || ++n > 5) return -1; } while (data[offset++] & 128);
        const unsigned char *end = memchr(data + offset, 0, size - offset);
        if (!end || end - (data + offset) < 3 || data[offset] != 'L' || end[-1] != ';') return -1;
        size_t length = end - (data + offset) - 2;
        char stack[1024];
        char *name = stack;
        if (length >= sizeof stack) {
            name = malloc(length + 1);
            if (!name) return -1;
        }
        memcpy(name, data + offset + 1, length); name[length] = 0;
        char *simple = strrchr(name, '/');
        simple = simple ? simple + 1 : name;
        class_selection_write_directory(name, flags,
            is_inner_class(simple) || is_anonymous_class(simple));
        if (name != stack) free(name);
    }
    return 1;
}
