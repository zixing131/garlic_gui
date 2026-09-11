#include "source_map.h"
#include "dalvik/dex_ins.h"
#include "dalvik/dex_meta_helper.h"
#include "dalvik/dex_structure.h"
#include "file_tools.h"
#include "parser/class/class_tools.h"

static __thread char *spans;
static __thread size_t spans_length, spans_capacity;
static __thread jd_method *location_method;
static __thread const char *location_scope;
static pthread_mutex_t pack_lock = PTHREAD_MUTEX_INITIALIZER;
typedef struct { FILE *file, *index; unsigned long long offset; char *path; } SourceArchive;
static SourceArchive map_archive, java_archive;
static int archive_cleanup_registered;
static void close_archives(void) {
    SourceArchive *archives[] = {&map_archive, &java_archive};
    int failed = 0;
    for (int i = 0; i < 2; ++i) {
        if (archives[i]->file && fclose(archives[i]->file)) failed = 1;
        if (archives[i]->index && fclose(archives[i]->index)) failed = 1;
    }
    if (failed) { fprintf(stderr, "Failed to flush source archive\n"); _Exit(EXIT_FAILURE); }
}
#ifdef _WIN32
#define archive_tell _ftelli64
#define archive_seek _fseeki64
#else
#define archive_tell ftello
#define archive_seek fseeko
#endif
static void write_pack(SourceArchive *archive, const char *directory, const char *filename,
                       const char *name, const char *data, size_t length) {
    pthread_mutex_lock(&pack_lock);
    if (!archive_cleanup_registered) { atexit(close_archives); archive_cleanup_registered = 1; }
    const size_t path_size = strlen(directory) + 32;
    char *path = malloc(path_size);
    snprintf(path, path_size, "%s/%s", directory, filename);
    if (!archive->path || strcmp(archive->path, path)) {
        if (archive->file) fclose(archive->file);
        if (archive->index) fclose(archive->index);
        free(archive->path); archive->path = path; path = NULL;
        archive->file = fopen(archive->path, "ab+");
        char *index_path = malloc(path_size + 8);
        snprintf(index_path, path_size + 8, "%s.index", archive->path);
        archive->index = fopen(index_path, "ab+"); free(index_path);
        if (archive->file) {
            setvbuf(archive->file, NULL, _IOFBF, 1024 * 1024);
            archive_seek(archive->file, 0, SEEK_END); archive->offset = (unsigned long long)archive_tell(archive->file);
            if (!archive->offset) { fwrite("GSMAP001", 1, 8, archive->file); archive->offset = 8; }
        }
        if (archive->index) {
            setvbuf(archive->index, NULL, _IOFBF, 256 * 1024);
            archive_seek(archive->index, 0, SEEK_END);
            if (archive_tell(archive->index) == 0) fwrite("GSMIDX01", 1, 8, archive->index);
        }
    }
    free(path);
    const size_t name_size = strlen(name);
    unsigned char header[8];
    for (int i = 0; i < 4; ++i) {
        header[i] = (unsigned char)(name_size >> (8 * i));
        header[i + 4] = (unsigned char)(length >> (8 * i));
    }
    if (!archive->file || fwrite(header, 1, 8, archive->file) != 8 ||
        fwrite(name, 1, name_size, archive->file) != name_size ||
        fwrite(data, 1, length, archive->file) != length) {
        fprintf(stderr, "Failed to write source map archive\n"); exit(EXIT_FAILURE);
    }
    unsigned char index_header[16];
    memcpy(index_header, header, 4);
    const unsigned long long position = archive->offset + 8 + name_size;
    for (int i = 0; i < 8; ++i) index_header[4 + i] = (unsigned char)(position >> (8 * i));
    memcpy(index_header + 12, header + 4, 4);
    if (!archive->index || fwrite(index_header, 1, 16, archive->index) != 16 ||
        fwrite(name, 1, name_size, archive->index) != name_size) {
        fprintf(stderr, "Failed to write source map directory\n"); exit(EXIT_FAILURE);
    }
    archive->offset += 8 + name_size + length;
    pthread_mutex_unlock(&pack_lock);
}
static void append_bytes(const char *text, size_t size) {
    if (spans_length + size + 1 > spans_capacity) {
        size_t capacity = spans_capacity ? spans_capacity : 4096;
        while (capacity < spans_length + size + 1) capacity *= 2;
        char *buffer = realloc(spans, capacity);
        if (!buffer) { fprintf(stderr, "Source map allocation failed\n"); exit(EXIT_FAILURE); }
        spans = buffer; spans_capacity = capacity;
    }
    memcpy(spans + spans_length, text, size);
    spans_length += size;
    spans[spans_length] = 0;
}
static void append_json_string(const char *text) {
    append_bytes("\"", 1);
    const unsigned char *at = (const unsigned char *)text, *begin = at;
    for (;; ++at) {
        if (*at && *at >= 32 && *at != '"' && *at != '\\') continue;
        append_bytes((const char *)begin, (size_t)(at - begin));
        if (!*at) break;
        char escaped[7];
        const char *short_escape = NULL;
        switch (*at) {
            case '"': short_escape = "\\\""; break;
            case '\\': short_escape = "\\\\"; break;
            case '\b': short_escape = "\\b"; break;
            case '\f': short_escape = "\\f"; break;
            case '\n': short_escape = "\\n"; break;
            case '\r': short_escape = "\\r"; break;
            case '\t': short_escape = "\\t"; break;
        }
        if (short_escape) append_bytes(short_escape, 2);
        else { snprintf(escaped, sizeof escaped, "\\u%04x", *at); append_bytes(escaped, 6); }
        begin = at + 1;
    }
    append_bytes("\"", 1);
}
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
    char header[128];
    const int size = snprintf(header, sizeof header, "%s{\"start\":%ld,\"end\":%ld,\"id\":", spans_length > 1 ? "," : "", start, end);
    append_bytes(header, (size_t)size);
    append_json_string(id);
    append_bytes(",\"token\":", 9);
    append_json_string(token);
    const char *tail = declaration ? ",\"declaration\":true}" : ",\"declaration\":false}";
    append_bytes(tail, strlen(tail));
}
int source_java_packed(void) {
    const char *enabled = getenv("GARLIC_JAVA_PACK");
    return enabled && !strcmp(enabled, "1") && getenv("GARLIC_SOURCE_MAP_DIR");
}
int source_java_pack(jsource_file *jf, const char *data, size_t length) {
    if (!source_java_packed()) return 0;
    char *name = strdup(jf->fname), *key = name;
    size_t size = strlen(name);
    if (size > 1 && name[0] == 'L' && name[size - 1] == ';') { key++; name[size - 1] = 0; }
    write_pack(&java_archive, getenv("GARLIC_SOURCE_MAP_DIR"), "java-sources.bin", key, data, length);
    free(name);
    return 1;
}
void source_map_begin(void) {
    if (!getenv("GARLIC_SOURCE_MAP_DIR"))
        return;
    location_method = NULL; location_scope = NULL;
    spans_length = 0;
    append_bytes("[", 1);
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
    const char *packed = getenv("GARLIC_MAP_PACK");
    if (packed && strcmp(packed, "1") == 0) {
        append_bytes("]", 1);
        write_pack(&map_archive, dir, "source-maps.bin", name, spans, spans_length);
        free(spans); spans = NULL;
        spans_capacity = spans_length = 0;
        return;
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
    append_bytes("]", 1);
    if (file) {
        int written = fwrite(spans, 1, spans_length, file) == spans_length;
        if (fclose(file) != 0)
            written = 0;
        // The GUI uses this file as the completion marker for a flushed source file.
        if (!written || rename(temporary, path) != 0)
            remove(temporary);
    }
    free(spans);
    spans = NULL;
    spans_capacity = spans_length = 0;
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
void source_map_location(FILE *stream, long start, jd_ins *ins) {
    if (!spans || start < 0 || !ins || !ins->method || ins->type != JD_TYPE_DALVIK) return;
    long end = ftell(stream);
    if (end <= start) return;
    jd_method *method = ins->method;
    jd_dex *dex = method->meta;
    encoded_method *em = method->meta_method;
    if (!dex || !em) return;
    dex_method_id *mid = &dex->meta->method_ids[em->method_id];
    if (location_method != method) {
        location_method = method;
        location_scope = str_create("%s->%s%s", dex_str_of_type_id(dex->meta, mid->class_idx),
            dex_str_of_idx(dex->meta, mid->name_idx), raw_proto(dex->meta, &dex->meta->proto_ids[mid->proto_idx]));
    }
    unsigned offset = (ins->state_flag & INS_STATE_DUPLICATE) ? ins->old_offset : ins->offset;
    char header[160];
    int size = snprintf(header, sizeof header, "%s{\"start\":%ld,\"end\":%ld,\"offset\":%u,\"scope\":",
        spans_length > 1 ? "," : "", start, end, offset);
    append_bytes(header, size); append_json_string(location_scope); append_bytes("}", 1);
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
