#include "java_string.h"
#include <math.h>
#include <stdint.h>
#include "common/str_tools.h"
#include "decompiler/transformer/transformer.h"
#include "decompiler/klass.h"
#include "decompiler/stack.h"

static inline bool const_exp_is_string(jd_exp_const *e)
{
    string full_class_name = e->val->data->cname;
    return STR_EQL(full_class_name, "String");
}

static inline bool const_exp_is_class(jd_exp_const *e)
{
    string full_class_name = e->val->data->cname;
    return STR_EQL(full_class_name, "Class");
}

static inline bool const_exp_is_boolean(jd_exp_const *e)
{
    if (e->val->data->cname == NULL)
        return false;

    return stack_val_is_boolean(e->val);
}

string const_integer_to_s(int64_t value, bool wide)
{
    const char *format = getenv("GARLIC_NUMBER_FORMAT");
    bool hex = format && !strcmp(format, "hex");
    if (!format || !strcmp(format, "auto")) {
        uint64_t n = (uint64_t)value;
        /* Large powers of two and bit masks benefit from hexadecimal notation. */
        hex = value >= 256 && (!(n & (n - 1)) || !(n & (n + 1)));
    }
    if (hex) return wide ? str_create("0x%llxL", (unsigned long long)(uint64_t)value)
                         : str_create("0x%x", (unsigned int)(uint32_t)value);
    return wide ? str_create("%lldL", (long long)value) : str_create("%d", (int)value);
}

static string get_const_value(jd_exp *expression)
{
    jd_exp_const *const_exp = expression->data;
    jd_val_data *data = const_exp->val->data;
    jd_primitive_union *primitive = data->primitive;

    switch (const_exp->val->type) {
        case JD_VAR_INT_T: {
            if (const_exp_is_boolean(const_exp) &&
                primitive->int_val == 0)
                return "false";
            else if (const_exp_is_boolean(const_exp) &&
                primitive->int_val == 1)
                return "true";
            else
                return const_integer_to_s(primitive->int_val, false);
        }
        case JD_VAR_LONG_T:
            return const_integer_to_s(primitive->long_val, true);
        case JD_VAR_FLOAT_T: {
            if (isfinite(primitive->float_val)) return str_create("%.9gF", primitive->float_val);
            uint32_t bits; memcpy(&bits, &primitive->float_val, sizeof(bits));
            return str_create("Float.intBitsToFloat(0x%08x)", bits);
        }
        case JD_VAR_DOUBLE_T: {
            if (isfinite(primitive->double_val)) return str_create("%.17gD", primitive->double_val);
            uint64_t bits; memcpy(&bits, &primitive->double_val, sizeof(bits));
            return str_create("Double.longBitsToDouble(0x%016llxL)", (unsigned long long)bits);
        }
        case JD_VAR_NULL_T:
            return str_dup("null");
        case JD_VAR_REFERENCE_T: {
            if (const_exp_is_string(const_exp)) {
                string new_str = java_escape_string(const_exp->val->data->val);
                return str_create("\"%s\"", new_str);
            }
            else if (const_exp_is_class(const_exp))
                return str_create("%s.class",
                                  class_simple_name(
                                          const_exp->val->data->val));
            else
                return str_create("%s", const_exp->val->data->val);
        }
        default:
            return (string)g_str_unknown;
    }
}

string exp_const_to_s(jd_exp *expression)
{
    return get_const_value(expression);
}

void exp_const_to_stream(FILE *stream, jd_node *node, jd_exp *expression)
{
    fprintf(stream, "%s", get_const_value(expression));
}
