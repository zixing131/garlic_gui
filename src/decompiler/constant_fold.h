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
        !v->data->cname ||
        (strcmp(v->data->cname, "int") && strcmp(v->data->cname, "short") &&
         strcmp(v->data->cname, "byte") && strcmp(v->data->cname, "char") &&
         strcmp(v->data->cname, "boolean"))) return false;
    *out = (uint32_t)v->data->primitive->int_val;
    return true;
}
/* Resolve the reaching definition of a Dalvik integer local.  Obfuscated
 * dispatchers deliberately assign the same register in several switch arms,
 * so the SSA variable can have redefinitions even though the value reaching
 * this call is still a literal.  The builder keeps that reaching instruction
 * in jd_val::ins; follow only STORE -> CONST/LOCAL edges and never evaluate
 * arbitrary code. */
static bool fold_reaching_int(jd_exp *e, uint32_t *out, int depth) {
    if (!e || depth > 16)
        return false;
    if (fold_int(e, out))
        return true;
    if (e->type != JD_EXPRESSION_LOCAL_VARIABLE || !e->data)
        return false;
    jd_val *v = e->data;
    if (!v->ins || !v->ins->expression || v->ins->expression == e)
        return false;
    jd_exp *definition = v->ins->expression;
    if (definition->type != JD_EXPRESSION_STORE || !definition->data)
        return false;
    jd_exp_store *store = definition->data;
    if (!store->list || store->list->len != 2)
        return false;
    jd_exp *right = &store->list->args[1];
    return fold_reaching_int(right, out, depth + 1);
}
static int collect_integer_definitions(jd_method *m, jd_var *target,
                                       uint32_t *values, int capacity) {
    if (!m || !m->expressions || !target || capacity <= 0)
        return 0;
    int count = 0;
    for (int i = 0; i < m->expressions->size && count < capacity; ++i) {
        jd_exp *expression = lget_obj(m->expressions, i);
        if (!expression || expression->type != JD_EXPRESSION_STORE || !expression->data)
            continue;
        jd_exp_store *store = expression->data;
        if (!store->list || store->list->len != 2 ||
            store->list->args[0].type != JD_EXPRESSION_LOCAL_VARIABLE ||
            !store->list->args[0].data)
            continue;
        jd_val *left = store->list->args[0].data;
        if (!left->stack_var || left->stack_var != target)
            continue;
        uint32_t value;
        if (!fold_reaching_int(&store->list->args[1], &value, 0))
            continue;
        bool duplicate = false;
        for (int j = 0; j < count; ++j)
            if (values[j] == value) { duplicate = true; break; }
        if (!duplicate)
            values[count++] = value;
    }
    return count;
}
static int collect_method_integer_literals(jd_method *m, uint32_t *values, int capacity) {
    if (!m || !m->expressions || capacity <= 0)
        return 0;
    int count = 0;
    for (int i = 0; i < m->expressions->size && count < capacity; ++i) {
        jd_exp *expression = lget_obj(m->expressions, i);
        if (!expression || expression->type != JD_EXPRESSION_STORE || !expression->data)
            continue;
        jd_exp_store *store = expression->data;
        if (!store->list || store->list->len != 2)
            continue;
        uint32_t value;
        if (!fold_reaching_int(&store->list->args[1], &value, 0))
            continue;
        bool duplicate = false;
        for (int j = 0; j < count; ++j)
            if (values[j] == value) { duplicate = true; break; }
        if (!duplicate)
            values[count++] = value;
    }
    return count;
}
static bool common_text_codepoint(uint32_t cp) {
    /* A small frequency filter prevents a wrong candidate key from turning
     * arbitrary UTF-16 into a convincing-looking run of rare CJK characters.
     * ASCII identifiers and punctuation do not need this filter. */
    static const char common[] =
        "的一是了我不人有在他这中大来上国个到说们为子和你地出道也时年得就那要下以生会自着去之过家学对可她里后小多天能好都然没日于起还发成事只作当想看文无开手用主行方又如前所本见经头面公同三已老从者意很最重相将外二无因心动方实全定深起去力问正明看位风现民电问通但并关打被给次并新内信使新高教理展务写入服务系统校验配置数据项目名称为空无法序列化对象一致成功失败返回调用参数结果读取保存集合列表字符串方法类字段变量常量控制流程";
    const unsigned char *p = (const unsigned char *)common;
    while (*p) {
        uint32_t value = 0;
        if (*p < 0x80) { p++; continue; }
        if ((*p & 0xe0) == 0xc0 && p[1]) {
            value = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f); p += 2;
        } else if ((*p & 0xf0) == 0xe0 && p[1] && p[2]) {
            value = ((uint32_t)(p[0] & 0x0f) << 12) |
                    ((uint32_t)(p[1] & 0x3f) << 6) | (p[2] & 0x3f); p += 3;
        } else if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3]) {
            value = ((uint32_t)(p[0] & 7) << 18) |
                    ((uint32_t)(p[1] & 0x3f) << 12) |
                    ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f); p += 4;
        } else { p++; continue; }
        if (value == cp) return true;
    }
    return false;
}
static char *decode_short_text(jd_exp_new_array *array, uint32_t start,
                               uint32_t length, uint32_t key, int *score) {
    unsigned first = array && array->values_only ? 0 : 1;
    if (!array || !array->list || array->list->len < first ||
        start > (uint32_t)array->list->len - first || length > 65536 ||
        length > (uint32_t)array->list->len - first - start)
        return NULL;
    size_t capacity = (size_t)length * 4 + 1;
    char *out = x_alloc(capacity);
    size_t used = 0;
    int quality = 0;
    int common = 0;
    bool has_ascii_word = false;
    for (uint32_t i = 0; i < length; ++i) {
        uint32_t encoded, cp;
        if (!fold_int(&array->list->args[first + start + i], &encoded))
            return NULL;
        cp = (encoded ^ key) & 0xffffu;
        if (cp == 0 || cp == 0xfffd || cp == 0xfffe || cp == 0xffff ||
            (cp >= 0xfdd0 && cp <= 0xfdef) ||
            (cp < 0x20 && cp != '\t' && cp != '\r' && cp != '\n') ||
            (cp >= 0xd800 && cp <= 0xdfff) || (cp >= 0xe000 && cp <= 0xf8ff))
            return NULL;
        if (cp < 0x80) {
            out[used++] = (char)cp;
            quality += (cp >= 0x20 && cp != 0x7f) ? 3 : 1;
            if ((cp >= 'A' && cp <= 'Z') || (cp >= 'a' && cp <= 'z') ||
                (cp >= '0' && cp <= '9'))
                has_ascii_word = true;
        } else if (cp < 0x800) {
            out[used++] = (char)(0xc0 | (cp >> 6));
            out[used++] = (char)(0x80 | (cp & 0x3f));
            quality += 2;
        } else {
            out[used++] = (char)(0xe0 | (cp >> 12));
            out[used++] = (char)(0x80 | ((cp >> 6) & 0x3f));
            out[used++] = (char)(0x80 | (cp & 0x3f));
            if (cp >= 0x4e00 && cp <= 0x9fff) {
                quality += 3;
                if (common_text_codepoint(cp)) ++common;
            } else {
                quality += 1;
            }
        }
        if (used + 4 >= capacity)
            return NULL;
    }
    out[used] = 0;
    if (!has_ascii_word && length >= 3 && common * 2 < (int)length)
        return NULL;
    if (score) *score = quality;
    return out;
}
static jd_exp_new_array *fold_short_array(jd_exp *e) {
    for (int depth = 0; e && depth < 16; ++depth) {
        if (e->type == JD_EXPRESSION_GET_STATIC && e->data)
            return ((jd_exp_get_static *)e->data)->constant_array;
        if (e->type == JD_EXPRESSION_NEW_ARRAY && e->data) {
            jd_exp_new_array *array = e->data;
            return array->class_name &&
                   (!strcmp(array->class_name, "short") ||
                    !strcmp(array->class_name, "short[]")) ? array : NULL;
        }
        if (e->type != JD_EXPRESSION_LOCAL_VARIABLE || !e->data)
            return NULL;
        jd_val *v = e->data;
        if (!v->ins || !v->ins->expression)
            return NULL;
        /* fill-array-data writes the same register as new-array.  Its
         * expression is already a literal array, even though the SSA variable
         * has a redefinition count greater than zero.  Prefer that latest
         * definition before applying the single-definition restriction to
         * ordinary assignments. */
        if (v->ins->expression->type == JD_EXPRESSION_NEW_ARRAY) {
            e = v->ins->expression;
            continue;
        }
        if (!v->stack_var || v->stack_var->redef_count != 0 ||
            v->ins->expression->type != JD_EXPRESSION_STORE)
            return NULL;
        jd_exp_store *store = v->ins->expression->data;
        if (!store || !store->list || store->list->len != 2 ||
            store->list->args[0].type != JD_EXPRESSION_LOCAL_VARIABLE ||
            store->list->args[0].data != v)
            return NULL;
        e = &store->list->args[1];
    }
    return NULL;
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
        /* Android protectors often keep an encoded short[] in a static field
         * and call a tiny [SIII -> String XOR decoder.  Decode only literal
         * arrays and integer arguments; no target bytecode is executed. */
        if (list->len == 4 && call && call->method_name) {
            jd_exp_new_array *array = fold_short_array(&list->args[0]);
            uint32_t start, length, key;
            if (array && array->list &&
                fold_reaching_int(&list->args[1], &start, 0) &&
                fold_reaching_int(&list->args[2], &length, 0) &&
                *budget >= (int)length) {
                uint32_t keys[256];
                int key_count = 0;
                if (fold_reaching_int(&list->args[3], &key, 0))
                    keys[key_count++] = key;
                if (list->args[3].type == JD_EXPRESSION_LOCAL_VARIABLE &&
                    list->args[3].data) {
                    jd_val *key_val = list->args[3].data;
                    key_count += collect_integer_definitions(e->ins ? e->ins->method : NULL,
                                                              key_val->stack_var,
                                                              keys + key_count,
                                                              (int)(sizeof(keys) / sizeof(keys[0])) - key_count);
                }
                /* A flattened method may carry the key through a phi-like
                 * register merge.  If the reaching definition is ambiguous,
                 * try the bounded set of integer literals present in this
                 * method and keep only printable decodings. */
                if (key_count < (int)(sizeof(keys) / sizeof(keys[0]))) {
                    int extra = collect_method_integer_literals(
                        e->ins ? e->ins->method : NULL, keys + key_count,
                        (int)(sizeof(keys) / sizeof(keys[0])) - key_count);
                    key_count += extra;
                }
                char *best = NULL;
                int best_score = -1;
                for (int i = 0; i < key_count; ++i) {
                    int score = 0;
                    char *decoded = decode_short_text(array, start, length, keys[i], &score);
                    if (decoded && score > best_score) {
                        best = decoded;
                        best_score = score;
                    }
                }
                if (best) {
                    *budget -= (int)length;
                    e->folded_string = str_dup((string)best);
                    return;
                }
            }
        }
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
    const char *enabled = getenv("GARLIC_DEOBFUSCATE_STRINGS");
    /* Preserve the legacy CLI switch; the GUI always supplies an explicit value. */
    if (!enabled) enabled = getenv("GARLIC_DEOBFUSCATE");
    if (!enabled || strcmp(enabled, "1")) return;
    int budget = 100000;
    for (int i = 0; i < m->expressions->size && budget > 0; ++i)
        fold_expression(lget_obj(m->expressions, i), 0, &budget, false);
}
#endif
