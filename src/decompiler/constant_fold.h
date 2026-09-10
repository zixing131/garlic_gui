#ifndef GARLIC_CONSTANT_FOLD_H
#define GARLIC_CONSTANT_FOLD_H
#include "decompiler/expression.h"
#include "decompiler/stack.h"
#include "parser/class/class_tools.h"
#include "dalvik/dex_ins.h"
#include "dalvik/dex_meta_helper.h"
#include <stdint.h>
#include <limits.h>

// Bounded, pure AST evaluation. Never load classes or execute target code.
static bool fold_string_owner(jd_ins *ins) {
    if (!ins) return false;
    string raw = NULL;
    if (ins->type == JD_TYPE_JVM) {
        if (ins->code < 0xb6 || ins->code > 0xb9) return false;
        jcp_info *ref = jvm_invoke_methodref_info(ins);
        if (ref) raw = get_class_name(ins->method->meta, ref);
    } else if (ins->type == JD_TYPE_DALVIK) {
        if (!((ins->code >= 0x6e && ins->code <= 0x72) ||
              (ins->code >= 0x74 && ins->code <= 0x78))) return false;
        jd_meta_dex *meta = dex_ins_meta(((jd_dex_ins *)ins));
        u2 idx = dex_ins_parameter((jd_dex_ins *)ins, 1);
        raw = dex_str_of_type_id(meta, meta->method_ids[idx].class_idx);
    }
    return raw && (!strcmp(raw, "java/lang/String") || !strcmp(raw, "Ljava/lang/String;"));
}
static jd_val *fold_literal(jd_exp *e) {
    if (e && e->type == JD_EXPRESSION_LOCAL_VARIABLE && e->data) {
        jd_val *v = e->data;
        if (v->ins && v->stack_var && v->stack_var->def_count == 1 &&
            v->stack_var->redef_count == 0 && v->ins->expression &&
            v->ins->expression->type == JD_EXPRESSION_STORE) {
            jd_exp_store *store = v->ins->expression->data;
            if (store && store->list && store->list->len == 2 &&
                store->list->args[0].type == JD_EXPRESSION_LOCAL_VARIABLE &&
                store->list->args[0].data == v)
                e = &store->list->args[1];
        }
    }
    if (!e || e->type != JD_EXPRESSION_CONST || !e->data) return NULL;
    jd_exp_const *c = e->data;
    return c->val && c->val->data ? c->val : NULL;
}
static bool fold_int(jd_exp *e, uint32_t *out) {
    jd_val *v = fold_literal(e);
    if (!v || v->type != JD_VAR_INT_T || !v->data->primitive ||
        !v->data->cname || strcmp(v->data->cname, "int")) return false;
    *out = (uint32_t)v->data->primitive->int_val;
    return true;
}
static const char *fold_string(jd_exp *e) {
    if (e && e->folded_string) return e->folded_string;
    jd_val *v = fold_literal(e);
    if (!v || v->type != JD_VAR_REFERENCE_T || !v->data->cname ||
        strcmp(v->data->cname, "String") || !v->data->val) return NULL;
    const unsigned char *s = (const unsigned char *)v->data->val;
    // Byte indexing equals Java UTF-16 indexing only for ASCII. Leave others intact.
    size_t n = 0;
    while (*s) { if (*s++ > 127 || ++n > 65536) return NULL; }
    return v->data->val;
}
static void fold_result(jd_exp *e, uint32_t bits, const char *text) {
    if (text) { e->folded_string = str_dup((string)text); return; }
    jd_exp_const *c = make_obj(jd_exp_const);
    c->val = stack_create_empty_val();
    c->val->type = text ? JD_VAR_REFERENCE_T : JD_VAR_INT_T;
    c->val->data->cname = text ? "String" : "int";
    if (text) c->val->data->val = str_dup((string)text);
    else {
        c->val->data->primitive = make_obj(jd_primitive_union);
        int32_t signed_value;
        memcpy(&signed_value, &bits, sizeof(bits));
        c->val->data->primitive->int_val = signed_value;
    }
    e->type = JD_EXPRESSION_CONST;
    e->data = c;
}
static void fold_expression(jd_exp *e, int depth, int *budget, bool value) {
    if (!e || !e->data || depth > 64 || --*budget <= 0) return;
    if (e->type == JD_EXPRESSION_ASSIGNMENT) {
        jd_exp_assignment *a = e->data;
        fold_expression(a->right, depth + 1, budget, true);
        return;
    }
    jd_exp_list *list;
    switch (e->type) {
        case JD_EXPRESSION_OPERATOR: case JD_EXPRESSION_SINGLE_OPERATOR:
        case JD_EXPRESSION_INVOKE: case JD_EXPRESSION_RETURN:
        case JD_EXPRESSION_STORE: case JD_EXPRESSION_ARRAY_STORE:
        case JD_EXPRESSION_NEW_ARRAY: case JD_EXPRESSION_INITIALIZE:
        case JD_EXPRESSION_PUT_STATIC: case JD_EXPRESSION_PUT_FIELD:
        case JD_EXPRESSION_CAST: case JD_EXPRESSION_SWITCH:
            list = ((jd_exp_reader *)e->data)->list; break;
        default: return;
    }
    if (!list || list->len < 0 || list->len > 1024) return;
    for (int i = 0; i < list->len; ++i)
        fold_expression(&list->args[i], depth + 1, budget, true);
    if (!value) return;
    if (e->type == JD_EXPRESSION_INITIALIZE && list->len == 1) {
        jd_exp_initialize *init = e->data;
        if (!fold_string_owner(init->constructor) || list->args[0].type != JD_EXPRESSION_NEW_ARRAY) return;
        jd_exp_new_array *array = list->args[0].data;
        if (!array || !array->class_name || strcmp(array->class_name, "char") || !array->list) return;
        uint32_t count;
        if (array->list->len < 1 || !fold_int(&array->list->args[0], &count) ||
            count > 1024 || count + 1 != array->list->len) return;
        if (*budget < (int)count) return;
        *budget -= (int)count;
        char *out = x_alloc(count + 1);
        for (unsigned i = 0; i < count; ++i) {
            uint32_t ch;
            if (!fold_int(&array->list->args[i + 1], &ch) || ch == 0 || ch > 127) return;
            out[i] = (char)ch;
        }
        out[count] = 0;
        fold_result(e, 0, out);
    } else if (e->type == JD_EXPRESSION_OPERATOR && list->len == 2) {
        uint32_t a, b, r;
        jd_exp *left = &list->args[0], *right = &list->args[1];
        jd_operator op = ((jd_exp_operator *)e->data)->operator;
        // Algebraic identities are restricted to local integer reads: never discard calls or fields.
        if (left->type == JD_EXPRESSION_LOCAL_VARIABLE && right->type == JD_EXPRESSION_LOCAL_VARIABLE) {
            jd_val *lv = left->data, *rv = right->data;
            if (lv && rv && lv->type == JD_VAR_INT_T && rv->type == JD_VAR_INT_T &&
                lv->data && lv->data->cname && !strcmp(lv->data->cname, "int") &&
                rv->data && rv->data->cname && !strcmp(rv->data->cname, "int") &&
                lv->stack_var && lv->stack_var == rv->stack_var && (op == JD_OP_XOR || op == JD_OP_SUB)) {
                fold_result(e, 0, NULL); return;
            }
        }
        if (!fold_int(&list->args[0], &a) || !fold_int(&list->args[1], &b)) return;
        int64_t sa = a <= INT32_MAX ? a : (int64_t)a - 4294967296LL;
        int64_t sb = b <= INT32_MAX ? b : (int64_t)b - 4294967296LL;
        switch (op) {
            case JD_OP_ADD: r = a + b; break;
            case JD_OP_SUB: r = a - b; break;
            case JD_OP_MUL: r = a * b; break;
            case JD_OP_DIV: if (!b) return; r = (uint32_t)(sa / sb); break;
            case JD_OP_REM: if (!b) return; r = (uint32_t)(sa % sb); break;
            case JD_OP_AND: r = a & b; break;
            case JD_OP_OR: r = a | b; break;
            case JD_OP_XOR: r = a ^ b; break;
            case JD_OP_SHL: r = a << (b & 31); break;
            case JD_OP_USHR: r = a >> (b & 31); break;
            case JD_OP_SHR:
                r = a >> (b & 31);
                if ((a & 0x80000000U) && (b & 31)) r |= UINT32_MAX << (32 - (b & 31));
                break;
            default: return;
        }
        fold_result(e, r, NULL);
    } else if (e->type == JD_EXPRESSION_INVOKE && list->len >= 1) {
        jd_exp_invoke *call = e->data;
        if (!fold_string_owner(e->ins) || !call->method_name) return;
        const char *s = fold_string(&list->args[list->len - 1]); // receiver is last
        if (!s) return;
        size_t len = strlen(s);
        if (*budget < (int)len) return;
        *budget -= (int)len;
        if (!strcmp(call->method_name, "concat") && list->len == 2) {
            const char *other = fold_string(&list->args[0]);
            if (!other || len + strlen(other) > 65536) return;
            if (*budget < (int)(len + strlen(other))) return;
            *budget -= (int)(len + strlen(other));
            char *out = x_alloc(len + strlen(other) + 1);
            strcpy(out, s); strcat(out, other);
            fold_result(e, 0, out);
        } else if (!strcmp(call->method_name, "substring") && (list->len == 2 || list->len == 3)) {
            uint32_t start, end = (uint32_t)len;
            if (!fold_int(&list->args[0], &start) ||
                (list->len == 3 && !fold_int(&list->args[1], &end)) || start > end || end > len) return;
            if (*budget < (int)(end - start)) return;
            *budget -= (int)(end - start);
            char *out = x_alloc(end - start + 1);
            memcpy(out, s + start, end - start); out[end - start] = 0;
            fold_result(e, 0, out);
        } else if (!strcmp(call->method_name, "replace") && list->len == 3) {
            const char *needle = fold_string(&list->args[0]);
            const char *replacement = fold_string(&list->args[1]);
            if (needle && replacement && *needle) {
                size_t n = strlen(needle), r = strlen(replacement), used = 0;
                size_t hits = 0;
                for (const char *p = s; (p = strstr(p, needle)); p += n) ++hits;
                size_t capacity = len - hits * n + hits * r;
                if (capacity > 65536 || *budget < (int)capacity) return;
                *budget -= (int)capacity;
                char *out = x_alloc(capacity + 1);
                for (size_t i = 0; i < len;) {
                    bool match = i + n <= len && !memcmp(s + i, needle, n);
                    size_t take = match ? r : 1;
                    if (used + take > 65536) return;
                    memcpy(out + used, match ? replacement : s + i, take);
                    used += take; i += match ? n : 1;
                }
                out[used] = 0;
                fold_result(e, 0, out); return;
            }
            uint32_t from, to;
            if (!fold_int(&list->args[0], &from) || !fold_int(&list->args[1], &to) ||
                from > 127 || to == 0 || to > 127) return;
            char *out = str_dup((string)s);
            for (size_t i = 0; i < len; ++i) if ((unsigned char)out[i] == from) out[i] = (char)to;
            fold_result(e, 0, out);
        }
    }
}
static void fold_method_constants(jd_method *m) {
    const char *enabled = getenv("GARLIC_DEOBFUSCATE");
    if (!enabled || strcmp(enabled, "1")) return;
    int budget = 100000;
    for (int i = 0; i < m->expressions->size && budget > 0; ++i)
        fold_expression(lget_obj(m->expressions, i), 0, &budget, false);
}
#endif
