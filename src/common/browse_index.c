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
static string dex_mid(jd_meta_dex *meta, unsigned idx) {
    dex_method_id *m = &meta->method_ids[idx];
    return str_create("%s->%s%s", dex_str_of_type_id(meta, m->class_idx),
                      dex_str_of_idx(meta, m->name_idx),
                      dex_proto_to_descriptor(meta, &meta->proto_ids[m->proto_idx]));
}
static string dex_fid(jd_meta_dex *meta, unsigned idx) {
    dex_field_id *f = &meta->field_ids[idx];
    return str_create("%s->%s:%s", dex_str_of_type_id(meta, f->class_idx),
                      dex_str_of_idx(meta, f->name_idx), dex_str_of_type_id(meta, f->type_idx));
}
static void dex_annotation_refs(cJSON *, jd_meta_dex *, const char *, encoded_annotation *, int);
static void dex_annotation_value_refs(cJSON *refs, jd_meta_dex *meta, const char *from, encoded_value *value, int depth) {
    if (!value || depth > 64) return;
    unsigned idx = 0;
    if (value->value_length <= 4 && value->value)
        for (size_t i = 0; i < value->value_length; ++i) idx |= (unsigned)value->value[i] << (i * 8);
    switch (value->value_type) {
        case kDexAnnotationType:
            if (idx < meta->header->type_ids_size)
                descriptor_references(refs, from, dex_str_of_type_id(meta, idx), "annotation-value");
            break;
        case kDexAnnotationField: case kDexAnnotationEnum:
            if (idx < meta->header->field_ids_size) reference(refs, from, dex_fid(meta, idx), -1, "annotation-value");
            break;
        case kDexAnnotationMethod:
            if (idx < meta->header->method_ids_size) reference(refs, from, dex_mid(meta, idx), -1, "annotation-value");
            break;
        case kDexAnnotationArray: {
            encoded_array *array = (encoded_array *)value->value;
            if (array) for (u4 i = 0; i < array->size; ++i)
                dex_annotation_value_refs(refs, meta, from, &array->values[i], depth + 1);
            break;
        }
        case kDexAnnotationAnnotation:
            dex_annotation_refs(refs, meta, from, (encoded_annotation *)value->value, depth + 1);
            break;
    }
}
static void dex_annotation_refs(cJSON *refs, jd_meta_dex *meta, const char *from, encoded_annotation *annotation, int depth) {
    if (!annotation || depth > 64 || annotation->type_idx >= meta->header->type_ids_size) return;
    reference(refs, from, dex_str_of_type_id(meta, annotation->type_idx), -1, "annotation");
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
        descriptor_references(refs, from, str_join(signature), "signature");
    }
    for (u4 i = 0; i < annotation->size; ++i)
        dex_annotation_value_refs(refs, meta, from, annotation->elements[i].value, depth + 1);
}
static void dex_annotation_set_refs(cJSON *refs, jd_meta_dex *meta, const char *from, annotation_set_item *set) {
    if (set) for (u4 i = 0; i < set->size; ++i)
        if (set->entries[i].annotation_item)
            dex_annotation_refs(refs, meta, from, set->entries[i].annotation_item->encoded_annotation, 0);
}
static void dex_all_annotation_refs(cJSON *refs, jd_meta_dex *meta, dex_class_def *cf, const char *from) {
    dex_annotations_directory_item *annotations = cf->annotations;
    if (!annotations) return;
    dex_annotation_set_refs(refs, meta, from, annotations->class_annotation);
    for (u4 i = 0; i < annotations->fields_size; ++i) {
        field_annotation *field = &annotations->field_annotations[i];
        if (field->field_idx < meta->header->field_ids_size)
            dex_annotation_set_refs(refs, meta, dex_fid(meta, field->field_idx), field->annotation);
    }
    for (u4 i = 0; i < annotations->methods_size; ++i) {
        method_annotation *method = &annotations->method_annotations[i];
        if (method->method_idx < meta->header->method_ids_size)
            dex_annotation_set_refs(refs, meta, dex_mid(meta, method->method_idx), method->annotation);
    }
    for (u4 i = 0; i < annotations->parameters_size; ++i) {
        parameter_annotation *parameter = &annotations->parameter_annotations[i];
        if (!parameter->annotation || parameter->method_idx >= meta->header->method_ids_size) continue;
        const char *method = dex_mid(meta, parameter->method_idx);
        for (u4 j = 0; j < parameter->annotation->size; ++j)
            dex_annotation_set_refs(refs, meta, method, parameter->annotation->list[j].annotation);
    }
}
static void dex_methods(cJSON *entry, jd_meta_dex *meta, encoded_method *methods, unsigned size,
                        const char *owner) {
    for (unsigned n = 0; n < size; n++) {
        encoded_method *em = &methods[n];
        dex_method_id *m = &meta->method_ids[em->method_id];
        member(cJSON_GetObjectItem(entry, "methods"), owner, dex_str_of_idx(meta, m->name_idx),
               dex_proto_to_descriptor(meta, &meta->proto_ids[m->proto_idx]), em->access_flags, 1);
        dex_code_item *code = em->code;
        if (!code)
            continue;
        cJSON *units = cJSON_GetObjectItem(entry, "instruction_units");
        cJSON_SetNumberValue(units, units->valuedouble + code->insns_size);
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
                    reference(cJSON_GetObjectItem(entry, "refs"), from, target, i, "bytecode");
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
    name[strlen(name) - 1] = 0;
    cJSON *entry = class_json(name, cf->access_flags, cf->is_inner || cf->is_anonymous);
    const char *directory_mode = getenv("GARLIC_DIRECTORY_INDEX");
    if (directory_mode && !strcmp(directory_mode, "names")) {
        char *json = class_selection_format(entry);
        cJSON_Delete(entry);
        mem_pool_free(scratch);
        if (tls) tls->pool = previous_pool; else global_pool = previous_pool;
        return json;
    }
    if (cf->superclass_idx < meta->header->type_ids_size)
        reference(cJSON_GetObjectItem(entry, "refs"), desc,
                  dex_str_of_type_id(meta, cf->superclass_idx), -1, "extends");
    if (cf->interfaces)
        for (unsigned i = 0; i < cf->interfaces->size; i++)
            reference(cJSON_GetObjectItem(entry, "refs"), desc,
                      dex_str_of_type_id(meta, cf->interfaces->list[i].type_idx), -1, "implements");
    dex_class_data_item *data = cf->class_data;
    if (data) {
        for (int kind = 0; kind < 2; kind++) {
            encoded_field *fields = kind ? data->instance_fields : data->static_fields;
            unsigned count = kind ? data->instance_fields_size : data->static_fields_size;
            for (unsigned i = 0; i < count; i++) {
                dex_field_id *f = &meta->field_ids[fields[i].field_id];
                member(cJSON_GetObjectItem(entry, "fields"), name,
                       dex_str_of_idx(meta, f->name_idx), dex_str_of_type_id(meta, f->type_idx),
                       fields[i].access_flags, 0);
            }
        }
        dex_methods(entry, meta, data->direct_methods, data->direct_methods_size, name);
        dex_methods(entry, meta, data->virtual_methods, data->virtual_methods_size, name);
    }
    dex_all_annotation_refs(cJSON_GetObjectItem(entry, "refs"), meta, cf, desc);
    signature_references(entry);
    char *json = class_selection_format(entry);
    cJSON_Delete(entry);
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
        char *name = malloc(length + 1);
        if (!name) return -1;
        memcpy(name, data + offset + 1, length); name[length] = 0;
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", name);
        cJSON_AddNumberToObject(entry, "flags", flags);
        cJSON_AddStringToObject(entry, "kind", flags & 0x2000 ? "annotation" :
            flags & 0x4000 ? "enum" : flags & 0x0200 ? "interface" :
            flags & 0x0400 ? "abstract" : "class");
        char *simple = strrchr(name, '/');
        simple = simple ? simple + 1 : name;
        cJSON_AddBoolToObject(entry, "inner", is_inner_class(simple) || is_anonymous_class(simple));
        class_selection_write(entry);
        cJSON_Delete(entry); free(name);
    }
    return 1;
}
