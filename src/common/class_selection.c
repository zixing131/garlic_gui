#include "class_selection.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *index_stream;
static const char *selected_class;
static int selection_error;
static int matched;
static char *origin;
static int excluded(const char *name, size_t len);
int class_selection_explicit(void) { return selected_class != NULL; }
void class_selection_origin(const char *name) { free(origin); origin = name ? strdup(name) : NULL; }
const char *class_selection_origin_text(void) { return origin; }
int class_selection_excluded_name(const char *name)
{
    return name != NULL && excluded(name, strlen(name));
}

int class_selection_open(const char *index_path, const char *class_name)
{
    class_selection_origin(NULL);
    selected_class = class_name;
    selection_error = matched = 0;
    if (index_path) {
        index_stream = fopen(index_path, "wb");
        if (!index_stream) {
            fprintf(stderr, "[garlic] Cannot create class index: %s\n", index_path);
            return 0;
        }
        setvbuf(index_stream, NULL, _IOFBF, 256 * 1024);
    }
    return 1;
}

int class_selection_indexing(void) { return index_stream != NULL; }

static const char *excluded_packages(void)
{
    static int loaded;
    static const char *list;
    if (!loaded) {
        list = getenv("GARLIC_EXCLUDED_PACKAGES");
        loaded = 1;
    }
    return list;
}
static void index_name_record(const char *name, size_t len);
static int excluded(const char *name, size_t len)
{
    const char *list = excluded_packages();
    while(list && *list) {
        const char *end=strchr(list,';'); size_t n=end?(size_t)(end-list):strlen(list);
        if(n && len>=n && !strncmp(name,list,n) && (len==n||name[n]=='/')) return 1;
        list=end?end+1:NULL;
    }
    return 0;
}
int class_selection_accept(const char *name)
{
    if (!index_stream && !selected_class && !excluded_packages()) return 1;
    size_t len = strlen(name);
    if (len > 2 && name[0] == 'L' && name[len - 1] == ';') {
        ++name;
        len -= 2;
    } else if (len > 6 && strcmp(name + len - 6, ".class") == 0) {
        len -= 6;
    }
    if (excluded(name,len)) return 0;
    int accept = 1;
    if (selected_class) {
        accept = strlen(selected_class) == len &&
                 memcmp(name, selected_class, len) == 0;
    }
    if (accept) matched++;
    if (index_stream) {
        index_name_record(name, len);
        accept = 0;
    }
    return accept;
}

int class_selection_close(void)
{
    if (index_stream && fclose(index_stream) != 0) selection_error = 1;
    index_stream = NULL;
    if (selected_class && !matched) {
        fprintf(stderr, "[garlic] Class not found: %s\n", selected_class);
        selection_error = 1;
    }
    class_selection_origin(NULL);
    return selection_error ? 1 : 0;
}

/* Formatting may run concurrently while the caller keeps origin stable. Only
 * the ordered producer writes the stream and updates its error state. */
char *class_selection_format(cJSON *entry)
{
    const cJSON *name = cJSON_GetObjectItem(entry, "name");
    if (name && name->valuestring && excluded(name->valuestring, strlen(name->valuestring))) return strdup("");
    if (origin) cJSON_AddStringToObject(entry, "origin", origin);
    return cJSON_PrintUnformatted(entry);
}
void class_selection_write_line(const char *line)
{
    if (!index_stream) return;
    if (!line) { selection_error = 1; return; }
    if (!*line) return;
    size_t n = strlen(line);
    if (fwrite(line, 1, n, index_stream) != n || fputc('\n', index_stream) == EOF)
        selection_error = 1;
}
static char *line_buf;
static size_t line_cap, line_len;

static void line_put(const char *text, size_t n)
{
    if (line_cap < line_len + n + 1) {
        size_t cap = line_cap ? line_cap : 256;
        while (cap < line_len + n + 1) cap *= 2;
        char *next = realloc(line_buf, cap);
        if (!next) { selection_error = 1; return; }
        line_buf = next;
        line_cap = cap;
    }
    memcpy(line_buf + line_len, text, n);
    line_len += n;
}

static void line_json(const char *text, size_t n)
{
    line_put("\"", 1);
    for (size_t i = 0; i < n; ++i) {
        unsigned char c = (unsigned char)text[i];
        if (c == '"' || c == '\\') {
            char escaped[2] = {'\\', (char)c};
            line_put(escaped, 2);
        } else if (c < 32) {
            char hex[7];
            int wrote = snprintf(hex, sizeof hex, "\\u%04x", c);
            if (wrote > 0) line_put(hex, (size_t)wrote);
        } else {
            char raw = (char)c;
            line_put(&raw, 1);
        }
    }
    line_put("\"", 1);
}

static void line_flush(void)
{
    line_put("\n", 1);
    if (!index_stream || !line_buf ||
        fwrite(line_buf, 1, line_len, index_stream) != line_len)
        selection_error = 1;
    line_len = 0;
}

static void index_name_record(const char *name, size_t len)
{
    if (!index_stream || !name) return;
    line_len = 0;
    line_put("{\"name\":", 8);
    line_json(name, len);
    line_put("}", 1);
    line_flush();
}

void class_selection_write_directory(const char *name, unsigned flags, int inner)
{
    if (!index_stream || !name) return;
    size_t len = strlen(name);
    if (excluded(name, len)) return;
    const char *kind = (flags & 0x2000) ? "annotation"
                      : (flags & 0x4000) ? "enum"
                      : (flags & 0x0200) ? "interface"
                      : (flags & 0x0400) ? "abstract"
                                         : "class";
    char tail[96];
    int wrote = snprintf(tail, sizeof tail,
                         ",\"flags\":%u,\"kind\":\"%s\",\"inner\":%s",
                         flags, kind, inner ? "true" : "false");
    if (wrote < 0) { selection_error = 1; return; }
    line_len = 0;
    line_put("{\"name\":", 8);
    line_json(name, len);
    line_put(tail, (size_t)wrote);
    if (origin) {
        line_put(",\"origin\":", 10);
        line_json(origin, strlen(origin));
    }
    line_put("}", 1);
    line_flush();
}

void class_selection_write(cJSON *entry)
{
    if (!index_stream) return;
    char *json = class_selection_format(entry);
    class_selection_write_line(json);
    free(json);
}
