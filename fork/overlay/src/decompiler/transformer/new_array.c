#include "decompiler/transformer/transformer.h"
#include "str.h"

/* Allocation dimensions and initializer elements are distinct. In particular,
 * a one-dimensional short[] allocation must never become new short[][]{}. */
string exp_new_array_to_s(jd_exp *expression)
{
    jd_exp_new_array *a = expression->data;
    str_list *out = str_list_init();
    bool initialized = a->values_only || a->fill_existing || a->list->len > 1;
    const char *suffix = strchr(a->class_name, '[');
    if (a->fill_existing) str_concat(out, "System.arraycopy(");
    str_concat(out, "new ");
    str_concat(out, initialized || !suffix ? a->class_name : strndup(a->class_name, suffix - a->class_name));
    str_concat(out, "[");
    if (!initialized && a->list->len) str_concat(out, exp_to_s(&a->list->args[0]));
    str_concat(out, "]");
    if (!initialized && suffix) str_concat(out, (char *)suffix);
    if (initialized) {
        str_concat(out, "{");
        int first = a->values_only ? 0 : 1;
        for (int i = first; i < a->list->len; ++i) {
            if (i > first) str_concat(out, ", ");
            str_concat(out, exp_to_s(&a->list->args[i]));
        }
        str_concat(out, "}");
    }
    if (a->fill_existing) {
        str_concat(out, ", 0, "); str_concat(out, exp_to_s(&a->list->args[0]));
        char count[32]; snprintf(count, sizeof(count), ", 0, %d)", a->list->len - 1);
        str_concat(out, strdup(count));
    }
    return str_join(out);
}

void exp_new_array_to_stream(FILE *stream, jd_node *node, jd_exp *expression)
{
    jd_exp_new_array *a = expression->data;
    bool initialized = a->values_only || a->fill_existing || a->list->len > 1;
    const char *suffix = strchr(a->class_name, '[');
    if (a->fill_existing) fprintf(stream, "System.arraycopy(");
    if (!initialized && suffix) fprintf(stream, "new %.*s[", (int)(suffix - a->class_name), a->class_name);
    else fprintf(stream, "new %s[", a->class_name);
    if (!initialized && a->list->len) expression_to_stream(stream, node, &a->list->args[0]);
    fprintf(stream, "]%s", !initialized && suffix ? suffix : "");
    if (initialized) {
        fprintf(stream, "{");
        int first = a->values_only ? 0 : 1;
        for (int i = first; i < a->list->len; ++i) {
            if (i > first) fprintf(stream, ", ");
            expression_to_stream(stream, node, &a->list->args[i]);
        }
        fprintf(stream, "}");
    }
    if (a->fill_existing) {
        fprintf(stream, ", 0, "); expression_to_stream(stream, node, &a->list->args[0]);
        fprintf(stream, ", 0, %d)", a->list->len - 1);
    }
}
