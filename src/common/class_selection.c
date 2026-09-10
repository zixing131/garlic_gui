#include "class_selection.h"
#include "cJSON.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static FILE *index_stream;
static const char *selected_class;
static int selection_error;
static int matched;

int class_selection_open(const char *index_path, const char *class_name)
{
    selected_class = class_name;
    selection_error = matched = 0;
    if (index_path) {
        index_stream = fopen(index_path, "wb");
        if (!index_stream) {
            fprintf(stderr, "[garlic] Cannot create class index: %s\n", index_path);
            return 0;
        }
    }
    return 1;
}

int class_selection_indexing(void) { return index_stream != NULL; }

int class_selection_accept(const char *name)
{
    if (!index_stream && !selected_class) return 1;
    size_t len = strlen(name);
    if (len > 2 && name[0] == 'L' && name[len - 1] == ';') {
        ++name;
        len -= 2;
    } else if (len > 6 && strcmp(name + len - 6, ".class") == 0) {
        len -= 6;
    }
    char *normalized = malloc(len + 1);
    if (!normalized) { selection_error = 1; return 0; }
    memcpy(normalized, name, len);
    normalized[len] = 0;
    int accept = !selected_class || strcmp(normalized, selected_class) == 0;
    if (accept) matched++;
    if (index_stream) {
        cJSON *entry = cJSON_CreateObject();
        cJSON_AddStringToObject(entry, "name", normalized);
        char *json = cJSON_PrintUnformatted(entry);
        if (!json || fprintf(index_stream, "%s\n", json) < 0) selection_error = 1;
        free(json);
        cJSON_Delete(entry);
        accept = 0;
    }
    free(normalized);
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
    return selection_error ? 1 : 0;
}
