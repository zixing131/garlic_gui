#include "decompiler/expression_analyse.h"
#include "decompiler/expression.h"
#include "decompiler/stack.h"

static bool descriptor_is_object_ref(string type)
{
    return type != NULL && (type[0] == 'L' || type[0] == '[');
}

static bool val_is_zero_int(jd_val *val)
{
    return val != NULL && val->type == JD_VAR_INT_T &&
           val->data != NULL && val->data->primitive != NULL &&
           val->data->primitive->int_val == 0;
}

static bool exp_is_zero_int(jd_exp *exp)
{
    if (exp_is_const(exp) && exp->data != NULL) {
        jd_exp_const *c = exp->data;
        return val_is_zero_int(c->val);
    }
    if (exp_is_local_variable(exp))
        return val_is_zero_int(exp->data);
    return false;
}

static void coerce_zero_to_null(jd_exp *arg)
{
    if (exp_is_local_variable(arg) && arg->data != NULL) {
        jd_val *val = arg->data;
        if (val->stack_var != NULL && val->stack_var->use_count > 0)
            val->stack_var->use_count--;
    }
    jd_exp_const *c = make_obj(jd_exp_const);
    jd_val *val = stack_make_primitive_val(JD_VAR_NULL_T);
    val->data->cname = (string)g_str_null;
    c->val = val;
    arg->type = JD_EXPRESSION_CONST;
    arg->data = c;
}

static void refine_expression(jd_exp *exp);

static void refine_invoke_nulls(jd_exp_invoke *invoke)
{
    if (invoke == NULL || invoke->list == NULL || invoke->list->args == NULL)
        return;
    for (int i = 0; i < invoke->list->len; ++i)
        refine_expression(&invoke->list->args[i]);
    if (invoke->descriptor == NULL || invoke->descriptor->list == NULL)
        return;
    int nparams = (int)invoke->descriptor->list->size;
    for (int i = 0; i < nparams && i < invoke->list->len; ++i) {
        string type = lget_string(invoke->descriptor->list, i);
        if (!descriptor_is_object_ref(type))
            continue;
        jd_exp *arg = &invoke->list->args[i];
        if (exp_is_zero_int(arg))
            coerce_zero_to_null(arg);
    }
}

static void refine_expression(jd_exp *exp)
{
    if (exp == NULL || exp_is_nopped(exp) || exp->data == NULL)
        return;
    if (exp_is_invoke(exp)) {
        refine_invoke_nulls(exp->data);
        return;
    }
    if (exp_is_assignment(exp)) {
        jd_exp_assignment *assignment = exp->data;
        refine_expression(assignment->right);
        return;
    }
    switch (exp->type) {
        case JD_EXPRESSION_STORE:
        case JD_EXPRESSION_RETURN:
        case JD_EXPRESSION_ARRAY_STORE:
        case JD_EXPRESSION_PUT_STATIC:
        case JD_EXPRESSION_PUT_FIELD:
        case JD_EXPRESSION_TERNARY:
        case JD_EXPRESSION_INITIALIZE:
        case JD_EXPRESSION_NEW_OBJ:
        case JD_EXPRESSION_LAMBDA: {
            jd_exp_reader *reader = exp->data;
            if (reader->list == NULL || reader->list->args == NULL)
                break;
            for (int i = 0; i < reader->list->len; ++i)
                refine_expression(&reader->list->args[i]);
            break;
        }
        default:
            break;
    }
}

static void nop_unused_const_stores(jd_method *m)
{
    for (int i = 0; i < m->expressions->size; ++i) {
        jd_exp *exp = lget_obj(m->expressions, i);
        if (exp_is_nopped(exp) || !exp_is_store(exp) || exp->data == NULL)
            continue;
        jd_exp_store *store = exp->data;
        if (store->list == NULL || store->list->len < 2)
            continue;
        jd_exp *left = &store->list->args[0];
        jd_exp *right = &store->list->args[1];
        if (!exp_is_local_variable(left) || !exp_is_const(right))
            continue;
        jd_val *val = left->data;
        if (val != NULL && val->stack_var != NULL &&
            val->stack_var->use_count == 0)
            exp_mark_nopped(exp);
    }
}

void method_type_analyse(jd_method *m)
{
    if (m == NULL || m->expressions == NULL)
        return;
    for (int i = 0; i < m->expressions->size; ++i) {
        jd_exp *exp = lget_obj(m->expressions, i);
        if (exp_is_nopped(exp))
            continue;
        refine_expression(exp);
    }
    nop_unused_const_stores(m);
}
