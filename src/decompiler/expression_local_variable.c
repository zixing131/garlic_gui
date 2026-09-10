#include "common/str_tools.h"
#include "decompiler/expression_local_variable.h"
#include "decompiler/expression.h"
#include "decompiler/expression_node.h"
#include "expression_visitor.h"
#include <stdint.h>

void variables_rename(jd_method *m);

/* Debug local-variable tables are attacker-controlled input.  Obfuscators
 * commonly use combining Arabic marks, zero-width format characters, or
 * malformed UTF-8 as names.  Keep ordinary source names, but replace names
 * that cannot be read reliably with the same type-based names used for
 * compiler-generated locals. */
static bool readable_debug_name(string name)
{
    if (name == NULL || *name == '\0')
        return false;
    const unsigned char *p = (const unsigned char *)name;
    while (*p) {
        uint32_t cp = 0;
        size_t width = 1;
        if (*p < 0x80) {
            if (*p < 0x20 || *p == 0x7f)
                return false;
            cp = *p;
        } else if ((*p & 0xe0) == 0xc0 && p[1] && (p[1] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x1f) << 6) | (p[1] & 0x3f); width = 2;
        } else if ((*p & 0xf0) == 0xe0 && p[1] && p[2] &&
                   (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 0x0f) << 12) | ((uint32_t)(p[1] & 0x3f) << 6) |
                 (p[2] & 0x3f); width = 3;
        } else if ((*p & 0xf8) == 0xf0 && p[1] && p[2] && p[3] &&
                   (p[1] & 0xc0) == 0x80 && (p[2] & 0xc0) == 0x80 &&
                   (p[3] & 0xc0) == 0x80) {
            cp = ((uint32_t)(p[0] & 7) << 18) | ((uint32_t)(p[1] & 0x3f) << 12) |
                 ((uint32_t)(p[2] & 0x3f) << 6) | (p[3] & 0x3f); width = 4;
        } else {
            return false;
        }
        if ((cp >= 0x300 && cp <= 0x36f) || (cp >= 0x591 && cp <= 0x6ff) ||
            (cp >= 0x200b && cp <= 0x200f) || (cp >= 0x202a && cp <= 0x206f) ||
            (cp >= 0xe000 && cp <= 0xf8ff) || cp == 0xfffd)
            return false;
        p += width;
    }
    return true;
}

static void variables_declaration(jd_method *m);

static void variables_scope(jd_method *m);

static bool is_var_processed(jd_method *m, string name)
{
    for (int i = 0; i < m->var_scopes->size; ++i) {
        jd_variable_scope *r = lget_obj(m->var_scopes, i);
        if (STR_EQL(r->name, name))
            return true;
    }
    return false;
}

static bool is_from_parameter(jd_method *m, string name)
{
    jd_stack *enter_stack = m->enter;
    for (int i = 0; i < enter_stack->local_vars_count; ++i) {
        jd_val *val = enter_stack->local_vars[i];
        if (val != NULL && STR_EQL(val->name, name))
            return true;
    }
    return false;
}

static bool is_catch_param_v2(jd_method *m, jd_exp *exp)
{
    jd_bblock *b = exp->block;
    jd_node *node = b->node;
    jd_node *parent = node->parent;
    return parent != NULL &&
           node_is_catch(parent) &&
           parent->start_idx == exp->idx;
}

static bool first_store_can_be_declaraction(jd_method *m,
                                            jd_variable_scope *scope)
{
    int idx = bitset_minimum(scope->stores);
    jd_exp *first_store_exp = lget_obj(m->expressions, idx);

    jd_node *first_store_node = first_store_exp->block->node;

    jd_exp *last_exp = lget_obj(m->expressions, scope->end_idx);
    jd_node *last_node = last_exp->block->node;

    // 1. first store is smallest goto_offset
    //    first and last in same node
    // 2. first store is smallest goto_offset 
    //    first is anchor's slibing of others

    if (last_node->parent == first_store_node->parent) {
        return true;
    }
    else {
        return node_is_ancestor_sibling_of(first_store_node,
                                           last_node);
    }
}

static void insert_declaration_to(jd_method *m,
                                  jd_variable_scope *scope,
                                  jd_node *node)
{
    jd_exp *new_exp = make_obj(jd_exp);
    new_exp->idx = node->start_idx;
    new_exp->type = JD_EXPRESSION_DECLARATION;
    new_exp->data = scope;
    jd_node *new_block = create_expression_node(m, new_exp);
    new_block->parent = node;

    jd_node *prev = NULL;
    int insert_idx = 0;
    for (int i = 0; i < node->children->size-1; ++i) {
        jd_node *n = lget_obj(node->children, i);
        jd_node *next = lget_obj(node->children, i+1);
        // TODO: if/else if/else/catch/finally/case
        if (n->start_idx < scope->start_idx &&
                (node_is_else_if(next) ||
                node_is_else(next) ||
                node_is_catch(next) ||
                node_is_finally(next) ||
                node_is_case(next))) {
            insert_idx = i;
            continue;
        }
    }
    ladd_obj_at(node->children, new_block, insert_idx);
}

static string get_array_variable_name(string name)
{
    char *p = strrchr(name, '[');
    if (p == NULL)
        return name;

    int len = p - name;
    string base_name = x_alloc(len + 1);
    memcpy(base_name, name, len);
    base_name[len] = '\0';

    int count = 0;
    for (int i = strlen(name)-1; i >=0 ; ++i) {
        char c = name[i];
        if (c != '[')
            break;
        count++;
    }

    string result = NULL;
    if (count == 1) {
        result = str_create("%sArr", base_name);
    }
    else {
        result = str_create("%sArr%d", base_name, count);
    }
    return result;
}

static string gen_variable_name(jd_method *m, jd_exp *exp, string cname)
{
    int count = hget_s2i(m->class_counter_map, cname);
    if (count == -1)
        count = 0;

    string var_name = NULL;
    if (STR_EQL(cname, "String"))
        var_name = str_create("str%d", count);
    else if (STR_EQL(cname, g_str_int))
        var_name = str_create("i%d", count);
    else if (STR_EQL(cname, g_str_long))
        var_name = str_create("l%d", count);
    else if (STR_EQL(cname, g_str_double))
        var_name = str_create("dbl%d", count);
    else if (STR_EQL(cname, g_str_float))
        var_name = str_create("flt%d", count);
    else if (STR_EQL(cname, g_str_byte))
        var_name = str_create("byte%d", count);
    else if (STR_EQL(cname, g_str_short))
        var_name = str_create("short%d", count);
    else if (STR_EQL(cname, g_str_boolean))
        var_name = str_create("bool%d", count);
    else if (STR_EQL(cname, g_str_char))
        var_name = str_create("char%d", count);

    string last_word = get_last_word_lower(cname);
    string lower = get_array_variable_name(last_word);
    if (var_name == NULL) {
        if (count == 0)
            var_name = str_create("%s", lower);
        else
            var_name = str_create("%sVar%d", lower, count);
    }
    return var_name;
}

void analyse_local_variables(jd_method *m)
{
    m->declarations = bitset_create();
    m->var_scopes = linit_object();

    variables_rename(m);

    variables_scope(m);

    variables_declaration(m);
}

static void variables_declaration(jd_method *m)
{
    jd_node *older;
    jd_node *younger;
    for (int i = 0; i < m->var_scopes->size; ++i) {
        jd_variable_scope *range = lget_obj(m->var_scopes, i);
        if (first_store_can_be_declaraction(m, range)) {
            range->declaration_idx = range->start_idx;
            bitset_set(m->declarations, range->start_idx);
            DEBUG_PRINT("[varibale first declaration]: %s %d\n",
                        range->name, range->declaration_idx);
        }
        else {
            // find declaration of variable
            jd_exp *first = get_exp(m, range->start_idx);
            jd_exp *last = get_exp(m, range->end_idx);
            jd_node *first_node = first->block->node;
            jd_node *last_node = last->block->node;

            int first_scope = node_scope(first_node);
            int last_scope = node_scope(last_node);

            older = first_scope > last_scope ? first_node : last_node;
            younger = first_scope > last_scope ? last_node : first_node;
            jd_node *older_parent = older->parent;
            while (older_parent != NULL) {
                if (node_is_ancestor_of(older_parent, younger))
                    break;
                older_parent = older_parent->parent;
            }
            if (older_parent == NULL)
                older_parent = lget_obj(m->nodes, 0);
            DEBUG_PRINT("[trie_insert declaration]: %s %d\n",
                        range->name, older_parent->start_idx);
            // trie_insert declaration to older_parent
            insert_declaration_to(m, range, older_parent);
        }
    }
}

static void variables_scope(jd_method *m)
{
    list_object *var_scopes = m->var_scopes;
    for (int i = 0; i < m->expressions->size; ++i) {
        jd_exp *exp = lget_obj(m->expressions, i);

        if (exp_is_nopped(exp))
            continue;

        if (!exp_is_store(exp) && !exp_is_define_stack_var(exp))
            continue;

        string name = NULL;
        string cname = NULL;
        jd_val *val = NULL;
        jd_var *var = NULL;
        if (exp_is_store(exp)) {
            jd_exp_store *store = exp->data;
            jd_exp *left = &store->list->args[0];
            jd_val *_val = left->data;
            val = _val;
            name = _val->name;
            cname = _val->data->cname;
        }
        else if (exp_is_define_stack_var(exp)) {
            jd_exp_def_var *def = exp->data;
            jd_exp *left = &def->list->args[0];
            jd_var *_var = left->data;
            name = _var->name;
            var = _var;
            cname = _var->cname;
        }

        if (is_var_processed(m, name))
            continue;
        if (is_from_parameter(m, name))
            continue;
        if (is_catch_param_v2(m, exp))
            continue;

        jd_variable_scope *range = make_obj(jd_variable_scope);
        range->start_idx = exp->idx;
        range->stores = bitset_create();
        range->cname = cname;
        range->name = name;
        range->var = var;
        range->val = val;
        bitset_set(range->stores, exp->idx);
        range->declaration_idx = -1;
        ladd_obj(var_scopes, range);

        int end_idx = 0;
        if (exp_is_store(exp)) {
            for (int j = i+1; j < m->expressions->size; ++j) {
                jd_exp *e = lget_obj(m->expressions, j);
                if (exp_is_nopped(e))
                    continue;
                if (!exp_is_assignment(e) && !exp_is_store(e))
                    continue;

                if (exp_is_store(e)) {
                    jd_exp_store *s = e->data;
                    jd_exp *l = &s->list->args[0];
                    jd_val *v = l->data;
                    if (STR_EQL(val->name, v->name)) {
                        end_idx = e->idx;
                    }
                }
                else {
                    jd_exp_assignment *assign = e->data;
                    jd_exp *r = assign->right;
                    if (!exp_is_local_variable(r))
                        continue;
                    jd_val *v = r->data;
                    if (STR_EQL(val->name, v->name)) {
                        end_idx = e->idx;
                    }
                }
            }
            range->end_idx = end_idx == 0 ? exp->idx : end_idx;

            DEBUG_PRINT("[local variable]: %s range: %d -> %d %s\n",
                        val->name,
                        range->start_idx,
                        range->end_idx,
                        val->data->cname);
        }
        else if (exp_is_define_stack_var(exp)) {
            for (int j = i+1; j < m->expressions->size; ++j) {
                jd_exp *e = lget_obj(m->expressions, j);
                if (exp_is_nopped(e))
                    continue;

                if (has_stack_var(e, var))
                    end_idx = MAX(end_idx, e->idx);
            }
            range->end_idx = end_idx == 0 ? exp->idx : end_idx;
        }
    }
}

static int get_class_counter(jd_method *m, string cname)
{
    int count = hget_s2i(m->class_counter_map, cname);
    if (count == -1)
        count = 0;
    return count;
}

static void increase_class_counter(jd_method *m, string cname)
{
    int count = hget_s2i(m->class_counter_map, cname);
    if (count == -1)
        count = 0;
    hset_s2i(m->class_counter_map, cname, count + 1);
}

void variables_rename(jd_method *m)
{
    if (!DEBUG_RENAME_VARIABLES) return;

    jd_stack *enter_stack = m->enter;
    for (int i = 0; i < enter_stack->local_vars_count; ++i) {
        jd_val *val = enter_stack->local_vars[i];
        if (val == NULL)
            continue;
        if (val->name_type == JD_VAR_NAME_DEBUG && readable_debug_name(val->name))
            continue;
        if (STR_EQL(val->name, g_str_this))
            continue;
        string var_name = gen_variable_name(m, NULL, val->data->cname);
        hset_s2s(m->var_name_map, val->name, var_name);
        increase_class_counter(m, val->data->cname);

        val->name = var_name;
    }


    for (int i = 0; i < m->expressions->size; ++i) {
        jd_exp *exp = lget_obj(m->expressions, i);
        if (exp_is_nopped(exp))
            continue;

        if (exp_is_define_stack_var(exp)) {
            jd_exp_def_var *def = exp->data;
            jd_exp *left = &def->list->args[0];
            jd_var *var = left->data;
            string var_name = hget_s2s(m->var_name_map, var->name);
            if (var_name != NULL) {
                var->name = var_name;
                continue;
            }
            var_name = gen_variable_name(m, exp, var->cname);
            hset_s2s(m->var_name_map, var->name, var_name);
            increase_class_counter(m, var->cname);

            var->name = var_name;
            DEBUG_PRINT("[variable name]: %s -> %s -> %s\n",
                        var->name,
                        var_name,
                        var->cname);
        }
        else if (exp_is_store(exp)) {
            jd_exp_store *store = exp->data;
            jd_exp *left = &store->list->args[0];
            jd_val *val = left->data;

            if (is_from_parameter(m, val->name))
                continue;
            if (is_catch_param_v2(m, exp))
                continue;

            if (val->name_type == JD_VAR_NAME_DEBUG && readable_debug_name(val->name))
                continue;

            string var_name = hget_s2s(m->var_name_map, val->name);
            if (var_name != NULL) {
                val->name = var_name;
                continue;
            }

            var_name = gen_variable_name(m, exp, val->data->cname);
            hset_s2s(m->var_name_map, val->name, var_name);
            increase_class_counter(m, val->data->cname);

            val->name = var_name;

            DEBUG_PRINT("[variable name]: %s -> %s -> %s\n",
                        val->name,
                        var_name,
                        val->data->cname);
        }
    }
}
