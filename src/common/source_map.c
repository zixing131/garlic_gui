#include "source_map.h"
#include "cJSON.h"
#include "dalvik/dex_ins.h"
#include "dalvik/dex_meta_helper.h"
#include "dalvik/dex_structure.h"
#include "file_tools.h"
#include "parser/class/class_tools.h"

static __thread cJSON *spans;
static string class_id(const char *owner) {
    if (!owner)
        return "";
    if (owner[0] == 'L' && owner[strlen(owner) - 1] == ';')
        return str_dup(owner);
    return str_create("L%s;", owner);
}
static void record(FILE *stream, long start, const char *id, const char *token, int declaration) {
    if (!spans || start < 0 || !id || !token)
        return;
    long end = ftell(stream);
    if (end <= start)
        return;
    cJSON *r = cJSON_CreateObject();
    cJSON_AddNumberToObject(r, "start", start);
    cJSON_AddNumberToObject(r, "end", end);
    cJSON_AddStringToObject(r, "id", id);
    cJSON_AddStringToObject(r, "token", token);
    cJSON_AddBoolToObject(r, "declaration", declaration);
    cJSON_AddItemToArray(spans, r);
}
void source_map_begin(void) {
    if (!getenv("GARLIC_SOURCE_MAP_DIR"))
        return;
    if (spans)
        cJSON_Delete(spans);
    spans = cJSON_CreateArray();
}
void source_map_end(jsource_file *jf) {
    if (!spans)
        return;
    const char *dir = getenv("GARLIC_SOURCE_MAP_DIR");
    string name = str_dup(jf->fname);
    if (name[0] == 'L' && name[strlen(name) - 1] == ';') {
        name++;
        name[strlen(name) - 1] = 0;
    }
    char *stored = source_storage_name(name);
    string path = str_create("%s/%s.map.json", dir, stored);
    free(stored);
    string parent = str_dup(path);
    char *slash = strrchr(parent, '/');
    if (slash) {
        *slash = 0;
        mkdir_p(parent);
    }
    string temporary = str_create("%s.tmp", path);
    FILE *file = fopen(temporary, "wb");
    char *json = cJSON_PrintUnformatted(spans);
    if (file) {
        int written = json && fputs(json, file) >= 0;
        if (fclose(file) != 0)
            written = 0;
        // The GUI uses this file as the completion marker for a flushed source file.
        if (!written || rename(temporary, path) != 0)
            remove(temporary);
    }
    free(json);
    cJSON_Delete(spans);
    spans = NULL;
}
void source_map_definition(FILE *stream, long start, const char *owner, const char *name,
                           const char *desc, const char *token, int method) {
    if (!spans)
        return;
    const char *id = class_id(owner);
    if (name)
        id = str_create("%s->%s%s%s", id, name, method ? "" : ":", desc ? desc : "");
    record(stream, start, id, token, 1);
}
static string raw_proto(jd_meta_dex *meta, dex_proto_id *proto) {
    str_list *parts = str_list_init();
    str_concat(parts, "(");
    if (proto->type_list)
        for (u4 j = 0; j < proto->type_list->size; j++)
            str_concat(parts, dex_str_of_type_id(meta, proto->type_list->list[j].type_idx));
    str_concat(parts, ")");
    str_concat(parts, dex_str_of_type_id(meta, proto->return_type_idx));
    return str_join(parts);
}
void source_map_method(FILE *stream, long start, jsource_file *jf, jd_method *m) {
    if (!spans)
        return;
    string desc = NULL;
    if (jf->type == JD_TYPE_DALVIK) {
        jd_dex *dex = m->meta;
        encoded_method *em = m->meta_method;
        dex_method_id *mid = &dex->meta->method_ids[em->method_id];
        desc = raw_proto(dex->meta, &dex->meta->proto_ids[mid->proto_idx]);
    } else {
        jclass_file *jc = m->meta;
        jmethod *method = m->meta_method;
        desc = pool_str(jc, method->descriptor_index);
    }
    source_map_definition(stream, start, jf->fname, m->name, desc,
                          m->name[0] == '<' ? jf->sname : m->name, 1);
}
int source_map_tracks_expression(jd_exp *exp) {
    if (!spans || !exp->ins || !exp->ins->method) return 0;
    return exp->type == JD_EXPRESSION_INVOKE || exp->type == JD_EXPRESSION_PUT_FIELD ||
           exp->type == JD_EXPRESSION_GET_FIELD || exp->type == JD_EXPRESSION_GET_STATIC ||
           exp->type == JD_EXPRESSION_PUT_STATIC;
}
void source_map_expression(FILE *stream, long start, jd_exp *exp) {
    if (!spans || !exp->ins || !exp->ins->method)
        return;
    jd_ins *ins = exp->ins;
    string id = NULL, token = NULL;
    int method = exp->type == JD_EXPRESSION_INVOKE;
    int field = exp->type == JD_EXPRESSION_PUT_FIELD || exp->type == JD_EXPRESSION_GET_FIELD ||
                exp->type == JD_EXPRESSION_GET_STATIC || exp->type == JD_EXPRESSION_PUT_STATIC;
    if (!method && !field)
        return;
    if (ins->type == JD_TYPE_DALVIK) {
        jd_dex *dex = ins->method->meta;
        jd_meta_dex *meta = dex->meta;
        unsigned idx = dex_ins_parameter((jd_dex_ins *)ins, field && ins->code < 0x60 ? 2 : 1);
        if (method && idx < meta->header->method_ids_size &&
            ((ins->code >= 0x6e && ins->code <= 0x78) || ins->code == 0xfa || ins->code == 0xfb)) {
            dex_method_id *m = &meta->method_ids[idx];
            token = dex_str_of_idx(meta, m->name_idx);
            dex_proto_id *proto = &meta->proto_ids[m->proto_idx];
            str_list *parts = str_list_init();
            str_concat(parts, "(");
            if (proto->type_list)
                for (u4 j = 0; j < proto->type_list->size; j++)
                    str_concat(parts, dex_str_of_type_id(meta, proto->type_list->list[j].type_idx));
            str_concat(parts, ")");
            str_concat(parts, dex_str_of_type_id(meta, proto->return_type_idx));
            id = str_create("%s->%s%s", dex_str_of_type_id(meta, m->class_idx), token,
                            str_join(parts));
        } else if (field && idx < meta->header->field_ids_size && ins->code >= 0x52 &&
                   ins->code <= 0x6d) {
            dex_field_id *f = &meta->field_ids[idx];
            token = dex_str_of_idx(meta, f->name_idx);
            id = str_create("%s->%s:%s", dex_str_of_type_id(meta, f->class_idx), token,
                            dex_str_of_type_id(meta, f->type_idx));
        }
    } else if (ins->type == JD_TYPE_JVM && ins->param_length >= 2) {
        jclass_file *jc = ins->method->meta;
        u2 idx = be16toh((ins->param[0] << 8) | ins->param[1]);
        jcp_info *cp = pool_item(jc, idx);
        if (method && (cp->tag == CONST_METHODREF_TAG || cp->tag == CONST_INTERFACEMETHODREF_TAG)) {
            token = get_method_name(jc, cp);
            id = str_create("L%s;->%s%s", get_method_class(jc, cp), token,
                            get_method_descriptor(jc, cp));
        } else if (field && cp->tag == CONST_FIELDREF_TAG) {
            token = get_field_name(jc, cp);
            id = str_create("L%s;->%s:%s", get_field_class(jc, cp), token,
                            get_field_descriptor(jc, cp));
        }
    }
    if (id && token && token[0] != '<')
        record(stream, start, id, token, 0);
}
