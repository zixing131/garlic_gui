#include "decompiler/method.h"
#include "dalvik/dex_method.h"
#include "dex_descriptor.h"
#include "dex_exception.h"
#include "parser/dex/metadata.h"
#include "dex_annotation.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// Specialize a dispatcher edge only when its state assignment is the unique
// predecessor. No instruction with side effects is skipped. Inspired by the
// constant-state edge specialization described by eShard's D810 (not copied).
static int java_string_hash(const char *text)
{
    // DEX strings use UTF-8 while String.hashCode() iterates UTF-16 code units.
    // Decode conservatively so malformed input can never make the optimizer read
    // past the string terminator.
    int32_t hash = 0;
    const unsigned char *p = (const unsigned char *)text;
    while (*p) {
        uint32_t cp = 0;
        size_t width = 1;
        if (*p < 0x80) {
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
            cp = '?';
        }
        hash = hash * 31 + (int32_t)(cp > 0xffff ? (0xd800 + ((cp - 0x10000) >> 10)) : cp);
        if (cp > 0xffff)
            hash = hash * 31 + (int32_t)(0xdc00 + ((cp - 0x10000) & 0x3ff));
        p += width;
    }
    return hash;
}

static encoded_method *find_encoded_method(jd_meta_dex *meta, unsigned method_index)
{
    if (method_index >= meta->header->method_ids_size)
        return NULL;
    const unsigned class_index = meta->method_ids[method_index].class_idx;
    if (class_index >= meta->header->type_ids_size)
        return NULL;
    for (u4 i = 0; i < meta->header->class_defs_size; ++i) {
        dex_class_def *klass = &meta->class_defs[i];
        if (klass->class_idx != class_index || !klass->class_data)
            continue;
        dex_class_data_item *data = klass->class_data;
        for (int kind = 0; kind < 2; ++kind) {
            encoded_method *methods = kind ? data->virtual_methods : data->direct_methods;
            unsigned count = kind ? data->virtual_methods_size : data->direct_methods_size;
            for (unsigned j = 0; j < count; ++j)
                if (methods[j].method_id == method_index)
                    return &methods[j];
        }
    }
    return NULL;
}

static bool is_hash_wrapper(jd_meta_dex *meta, unsigned method_index)
{
    encoded_method *wrapper = find_encoded_method(meta, method_index);
    if (!wrapper || !wrapper->code)
        return false;
    const u2 *insns = wrapper->code->insns;
    for (u4 i = 0; i < wrapper->code->insns_size;) {
        const u1 opcode = insns[i] & 0xff;
        const u4 length = dex_opcode_len(opcode);
        if (!length || length > wrapper->code->insns_size - i)
            break;
        if (opcode == DEX_INS_INVOKE_VIRTUAL || opcode == DEX_INS_INVOKE_VIRTUAL_RANGE) {
            const unsigned target_index = insns[i + 1];
            if (target_index < meta->header->method_ids_size &&
                strcmp(dex_str_of_method_id(meta, target_index), "hashCode") == 0)
                return true;
        }
        i += length;
    }
    return false;
}

/* A common anti-analysis trick is to hide an opaque predicate behind a tiny
 * static helper, for example `static int p() { return 0; }`.  Resolve only
 * methods whose complete bytecode is a single literal followed by return.  A
 * method with any other instruction is deliberately left alone: this keeps
 * the optimizer side-effect free and avoids executing application code. */
static bool eval_constant_static_method(jd_meta_dex *meta, unsigned method_index,
                                        uint32_t *value)
{
    encoded_method *helper = find_encoded_method(meta, method_index);
    if (!helper || !helper->code || helper->code->insns_size == 0)
        return false;
    bool have_const = false;
    unsigned const_reg = 0;
    uint32_t const_value = 0;
    bool have_return = false;
    for (u4 i = 0; i < helper->code->insns_size;) {
        const u1 opcode = helper->code->insns[i] & 0xff;
        const u4 length = dex_opcode_len(opcode);
        if (!length || length > helper->code->insns_size - i)
            return false;
        const u2 *words = helper->code->insns + i;
        switch (opcode) {
            case 0x00: /* nop */
                break;
            case 0x12: /* const/4 */
                if (have_const) return false;
                const_reg = (words[0] >> 8) & 15;
                const_value = (uint32_t)((int32_t)(words[0] >> 12) -
                                         ((words[0] & 0x8000) ? 16 : 0));
                have_const = true;
                break;
            case 0x13: /* const/16 */
                if (have_const) return false;
                const_reg = words[0] >> 8;
                const_value = (uint32_t)(int32_t)(int16_t)words[1];
                have_const = true;
                break;
            case 0x14: /* const */
                if (have_const) return false;
                const_reg = words[0] >> 8;
                const_value = (uint32_t)words[1] | ((uint32_t)words[2] << 16);
                have_const = true;
                break;
            case 0x15: /* const/high16 */
                if (have_const) return false;
                const_reg = words[0] >> 8;
                const_value = (uint32_t)words[1] << 16;
                have_const = true;
                break;
            case 0x0f: /* return */
                if (!have_const || (words[0] >> 8) != const_reg || have_return)
                    return false;
                have_return = true;
                break;
            default:
                return false;
        }
        i += length;
    }
    if (!have_const || !have_return)
        return false;
    *value = const_value;
    return true;
}

static bool encoded_integer_value(const encoded_value *value, uint32_t *out)
{
    if (!value)
        return false;
    if (value->value_type == kDexAnnotationBoolean) {
        *out = value->value_arg ? 1u : 0u;
        return true;
    }
    if (value->value_type != kDexAnnotationByte && value->value_type != kDexAnnotationShort &&
        value->value_type != kDexAnnotationChar && value->value_type != kDexAnnotationInt &&
        value->value_type != kDexAnnotationLong)
        return false;
    const unsigned width = value->value_length > 4 ? 4 : (unsigned)value->value_length;
    uint32_t bits = 0;
    for (unsigned i = 0; i < width; ++i)
        bits |= (uint32_t)value->value[i] << (i * 8);
    if (value->value_type != kDexAnnotationChar && width < 4 && (bits & (1u << (width * 8 - 1))))
        bits |= UINT32_MAX << (width * 8);
    *out = bits;
    return true;
}

static bool eval_static_field(jd_meta_dex *meta, unsigned field_index, uint32_t *value)
{
    if (field_index >= meta->header->field_ids_size)
        return false;
    const unsigned class_index = meta->field_ids[field_index].class_idx;
    for (u4 i = 0; i < meta->header->class_defs_size; ++i) {
        dex_class_def *klass = &meta->class_defs[i];
        dex_class_data_item *data = klass->class_data;
        encoded_array *initializers = klass->static_values;
        if (klass->class_idx != class_index || !data || !initializers)
            continue;
        for (u4 j = 0; j < data->static_fields_size && j < initializers->size; ++j) {
            if (data->static_fields[j].field_id == field_index)
                return encoded_integer_value(&initializers->values[j], value);
        }
    }
    return false;
}

static bool eval_constant_predicate(jd_method *m, jd_dex_ins *jump,
                                    uint32_t *value, unsigned *reg)
{
    jd_dex_ins *move = jump->prev;
    if (!move || !dex_ins_is_move_result(move))
        return false;
    jd_dex_ins *invoke = move->prev;
    if (!invoke || !dex_ins_is_invokestatic(invoke))
        return false;
    jd_meta_dex *meta = ((jd_dex *)m->meta)->meta;
    const unsigned method_index = dex_ins_parameter(invoke, 1);
    if (method_index >= meta->header->method_ids_size)
        return false;
    dex_method_id *method = &meta->method_ids[method_index];
    dex_proto_id *proto = &meta->proto_ids[method->proto_idx];
    const char *return_type = dex_str_of_type_id(meta, proto->return_type_idx);
    if (!return_type || (strcmp(return_type, "I") != 0 && strcmp(return_type, "Z") != 0))
        return false;
    if (!eval_constant_static_method(meta, method_index, value))
        return false;
    *reg = dex_ins_parameter(move, 0);
    return true;
}

static bool eval_predicate_value(jd_method *m, jd_dex_ins *jump,
                                 uint32_t *value, unsigned *reg)
{
    if (eval_constant_predicate(m, jump, value, reg))
        return true;
    jd_dex_ins *source = jump->prev;
    if (!source || (source->code != DEX_INS_SGET && source->code != DEX_INS_SGET_BOOLEAN &&
                    source->code != DEX_INS_SGET_BYTE && source->code != DEX_INS_SGET_CHAR &&
                    source->code != DEX_INS_SGET_SHORT))
        return false;
    *reg = dex_ins_parameter(source, 0);
    if (dex_ins_parameter(jump, 0) != *reg)
        return false;
    return eval_static_field(((jd_dex *)m->meta)->meta, dex_ins_parameter(source, 1), value);
}

static bool predicate_taken(jd_dex_ins *ins, uint32_t value)
{
    const int32_t v = (int32_t)value;
    switch (ins->code) {
        case DEX_INS_IF_EQZ: return v == 0;
        case DEX_INS_IF_NEZ: return v != 0;
        case DEX_INS_IF_LTZ: return v < 0;
        case DEX_INS_IF_GEZ: return v >= 0;
        case DEX_INS_IF_GTZ: return v > 0;
        case DEX_INS_IF_LEZ: return v <= 0;
        default: return false;
    }
}

static int simplify_constant_predicates(jd_method *m)
{
    int changed = 0;
    for (int i = 0; i < m->instructions->size; ++i) {
        jd_dex_ins *jump = lget_obj(m->instructions, i);
        if (!dex_ins_is_if(jump) || jump->code < DEX_INS_IF_EQZ)
            continue;
        uint32_t value;
        unsigned reg;
        if (!eval_predicate_value(m, jump, &value, &reg) ||
            dex_ins_parameter(jump, 0) != reg)
            continue;
        jd_dex_ins *taken = dex_ins_of_offset(m, dex_ins_if_jump_offset(jump));
        jd_dex_ins *fallthrough = jump->next;
        jd_dex_ins *target = predicate_taken(jump, value) ? taken : fallthrough;
        if (!target)
            continue;
        if (target == fallthrough) {
            jump->code = 0;
            jump->name = "nop";
            jump->param_length = 1;
            jump->param[0] = 0;
        } else {
            jump->code = DEX_INS_GOTO;
            jump->name = "goto";
            jump->param_length = 1;
            dex_setup_goto_offset(jump, target->offset);
        }
        ++changed;
    }
    return changed;
}

static bool eval_hash_dispatcher_state(jd_method *m, jd_dex_ins *jump,
                                       uint32_t *value, unsigned *reg)
{
    jd_dex_ins *move = jump->prev;
    unsigned moved = 0;
    while (move && dex_ins_is_move_to(move) && moved++ < 8)
        move = move->prev;
    if (!move || !dex_ins_is_move_result(move))
        return false;
    jd_dex_ins *invoke = move->prev;
    if (!invoke || dex_ins_parameter(invoke, 0) != 1)
        return false;
    jd_dex *dex = m->meta;
    jd_meta_dex *meta = dex->meta;
    const unsigned method_index = dex_ins_parameter(invoke, 1);
    if (method_index >= meta->header->method_ids_size)
        return false;
    dex_method_id *method = &meta->method_ids[method_index];
    const char *method_name = dex_str_of_idx(meta, method->name_idx);
    dex_proto_id *proto = &meta->proto_ids[method->proto_idx];
    const char *return_type = dex_str_of_type_id(meta, proto->return_type_idx);
    const bool direct_hash = dex_ins_is_invokevirtual(invoke) && method_name &&
                             strcmp(method_name, "hashCode") == 0;
    const bool wrapped_hash = dex_ins_is_invokestatic(invoke) && is_hash_wrapper(meta, method_index);
    const bool no_params = !proto->type_list || proto->type_list->size == 0;
    const bool one_object_param = proto->type_list && proto->type_list->size == 1;
    if ((!direct_hash && !wrapped_hash) || !return_type || strcmp(return_type, "I") != 0 ||
        (direct_hash && !no_params) || (wrapped_hash && !one_object_param))
        return false;
    const unsigned source_reg = dex_ins_parameter(invoke, 2);
    jd_dex_ins *constant = invoke->prev;
    if (!constant || (constant->code != DEX_INS_CONST_STRING &&
                      constant->code != DEX_INS_CONST_STRING_JUMBO) ||
        dex_ins_parameter(constant, 0) != source_reg)
        return false;
    uint32_t string_index = constant->param[1];
    if (constant->code == DEX_INS_CONST_STRING_JUMBO)
        string_index |= (uint32_t)constant->param[2] << 16;
    if (string_index >= meta->header->string_ids_size)
        return false;
    const char *text = dex_str_of_idx(meta, string_index);
    if (!text)
        return false;
    *value = (uint32_t)java_string_hash(text);
    *reg = dex_ins_parameter(move, 0);
    return true;
}

static jd_dex_ins *switch_target_for_value(jd_method *m, jd_dex_ins *dispatcher,
                                           uint32_t value, unsigned reg)
{
    if (!dispatcher || !dex_ins_is_switch(dispatcher) ||
        (dispatcher->param[0] >> 8) != reg)
        return NULL;
    uint32_t payload_offset = dispatcher->offset + (uint32_t)dispatcher->param[1] +
                              ((uint32_t)dispatcher->param[2] << 16);
    jd_dex_ins *payload = dex_ins_of_offset(m, payload_offset);
    if (!payload || payload->param_length < 2)
        return NULL;
    unsigned count = payload->param[1];
    bool packed = dex_ins_is_packed_switch(dispatcher);
    if (payload->param[0] != (packed ? 0x0100 : 0x0200) ||
        (unsigned)payload->param_length < (packed ? 4 + count * 2 : 2 + count * 4))
        return NULL;
    for (unsigned k = 0; k < count; ++k) {
        unsigned key_index = packed ? 2 : 2 + k * 2;
        uint32_t key = (uint32_t)payload->param[key_index] |
                       ((uint32_t)payload->param[key_index + 1] << 16);
        if (packed)
            key += k;
        if (key != value)
            continue;
        unsigned target_index = packed ? 4 + k * 2 : 2 + count * 2 + k * 2;
        uint32_t relative = (uint32_t)payload->param[target_index] |
                            ((uint32_t)payload->param[target_index + 1] << 16);
        return dex_ins_of_offset(m, dispatcher->offset + relative);
    }
    return dispatcher->next;
}

static int specialize_hash_dispatchers(jd_method *m)
{
    int changed = 0;
    for (int i = 0; i < m->instructions->size; ++i) {
        jd_dex_ins *dispatcher = lget_obj(m->instructions, i);
        if (!dex_ins_is_switch(dispatcher))
            continue;
        uint32_t value;
        unsigned reg;
        if (!eval_hash_dispatcher_state(m, dispatcher, &value, &reg)) {
            continue;
        }
        jd_dex_ins *target = switch_target_for_value(m, dispatcher, value, reg);
        if (!target || target == dispatcher || dex_ins_is_switch(target))
            continue;
        dispatcher->code = DEX_INS_GOTO;
        dispatcher->name = "goto";
        dispatcher->param_length = 1;
        dex_setup_goto_offset(dispatcher, target->offset);
        ++changed;
    }
    return changed;
}

static int unflatten_constant_dispatchers(jd_method *m)
{
    int changed = 0;
    for (int i = 1; i < m->instructions->size; ++i) {
        jd_dex_ins *jump = lget_obj(m->instructions, i);
        if (!dex_ins_is_goto_jump(jump) || jump->comings->size != 0)
            continue;
        jd_dex_ins *assignment = jump->prev;
        if (!assignment || assignment->next != jump)
            continue;
        uint32_t value;
        unsigned reg;
        bool evaluated = eval_hash_dispatcher_state(m, jump, &value, &reg);
        if (!evaluated) switch (assignment->code) {
            case 0x12: // const/4
                reg = (assignment->param[0] >> 8) & 15;
                value = (uint32_t)((int32_t)(assignment->param[0] >> 12) -
                                  ((assignment->param[0] & 0x8000) ? 16 : 0));
                break;
            case 0x13: // const/16
                reg = assignment->param[0] >> 8;
                value = (uint32_t)(int32_t)(int16_t)assignment->param[1];
                break;
            case 0x14: // const
                reg = assignment->param[0] >> 8;
                value = (uint32_t)assignment->param[1] | ((uint32_t)assignment->param[2] << 16);
                break;
            case 0x15: // const/high16
                reg = assignment->param[0] >> 8;
                value = (uint32_t)assignment->param[1] << 16;
                break;
            default: continue;
        }
        jd_dex_ins *dispatcher = dex_ins_of_offset(m, dex_goto_offset(jump));
        if (!dispatcher || !dex_ins_is_switch(dispatcher) ||
            (dispatcher->param[0] >> 8) != reg)
            continue;
        jd_dex_ins *target = switch_target_for_value(m, dispatcher, value, reg);
        if (!target || target == dispatcher || target->param[0] == 0x0100 ||
            target->param[0] == 0x0200 || target->param[0] == 0x0300)
            continue;
        dex_setup_goto_offset(jump, target->offset);
        if (target == jump->next) {
            jump->code = 0;
            jump->name = "nop";
            jump->param[0] = 0;
        }
        ++changed;
    }
    return changed;
}

void dex_method_access_flag_with_flags(u4 flags, str_list *list)
{
    CONCAT_ACCESS_FLAG(flags, ACC_DEX_PUBLIC, list, "public ")
    CONCAT_ACCESS_FLAG(flags, ACC_DEX_PRIVATE, list, "private ")
    CONCAT_ACCESS_FLAG(flags, ACC_DEX_PROTECTED, list, "protected ")
    CONCAT_ACCESS_FLAG(flags, ACC_DEX_STATIC, list, "static ")
    CONCAT_ACCESS_FLAG(flags, ACC_DEX_FINAL, list, "final ")
    CONCAT_ACCESS_FLAG(flags, ACC_DEX_NATIVE, list, "native ")
    CONCAT_ACCESS_FLAG(flags, ACC_DEX_ABSTRACT, list, "abstract ")
    // CONCAT_ACCESS_FLAG(flags, ACC_DEX_SYNTHETIC, list, "/* synthetic */")
}

void dex_method_access_flags(jd_method *m, str_list *list)
{
    dex_method_access_flag_with_flags(m->access_flags, list);
}

jd_val* dex_method_parameter_val(jd_method *m, int index)
{
    if (m->enter == NULL)
        return NULL;
    int max = m->max_locals;
    int desc_size = m->desc->list->size;
    int start = max - desc_size;
    // int i = method_is_member(m) ? (max-1)-index+1 : (max-1)-index;
    // int i = method_is_member(m) ? start + index : start + index;
    int i = start + index;
    // TODO: enum constructor
    //      i += method_is_enum_constructor(m) ? 2 : 0;
    return m->enter->local_vars[i];
}

static void init_dex_ins_unconditional_jump(jd_dex_ins *ins)
{
    u4 jump_offset = dex_goto_offset(ins);
    jd_dex_ins *target_ins = ins_of_offset(ins->method, jump_offset);
    ladd_obj(ins->targets, target_ins);
    ladd_obj(ins->jumps, target_ins);
}

static void init_dex_ins_conditional_jump(jd_dex_ins *ins)
{
    u4 jump_offset = (s2)(ins->param[1]) + ins->offset;
    jd_dex_ins *next = ins->next;
    if (next != NULL)
        ladd_obj_no_dup(ins->targets, next);

    jd_dex_ins *jump_to_ins = ins_of_offset(ins->method, jump_offset);
    ladd_obj(ins->targets, jump_to_ins);
    ladd_obj(ins->jumps, jump_to_ins);
}

static void init_dex_ins_default_jump(jd_dex_ins *ins)
{
    jd_dex_ins *next = ins->next;
    if (next == NULL)
        return;
    ladd_obj(ins->targets, next);
}

static void init_dex_packed_switch_jump(jd_dex_ins *ins)
{
    jd_method *m = ins->method;
    s4 packed_offset = (s4)ins->param[2] << 16 | ins->param[1];
    hashmap *map = m->offset2id_map;
    int payload_idx = hget_i2i(map, ins->offset + packed_offset);
    jd_dex_ins *packed_ins = lget_obj(m->instructions, payload_idx);

    int size = packed_ins->param[1];
    int first_key = packed_ins->param[3] << 16 | packed_ins->param[2];

    for (int i = 0; i < size; ++i) {
        int offset = packed_ins->param[5 + i * 2] << 16 |
                     packed_ins->param[4 + i * 2];
        jd_dex_ins *target_ins = ins_of_offset(m, ins->offset+offset);
        ladd_obj(ins->targets, target_ins);
        ladd_obj(ins->jumps, target_ins);
        DEBUG_PRINT("key: %d, goto_offset: %d\n",
                first_key+i, offset+ins->offset);
    }
    if (ins->next != NULL)
        ladd_obj_no_dup(ins->targets, ins->next);
}

static void init_dex_sparse_switch_jump(jd_dex_ins *ins)
{
    jd_method *m = ins->method;
    s4 offset = ins->param[2] << 16 | ins->param[1];
    int payload_idx = hget_i2i(m->offset2id_map, ins->offset + offset);
    jd_dex_ins *packed_ins = lget_obj(m->instructions, payload_idx);
    int size = packed_ins->param[1];

    u2 *params = packed_ins->param;
    for (int i = 0; i < size; ++i) {
        int key = params[3+i*2] << 16 | params[2+i*2];
        int val = params[3+size*2+i*2] << 16 | params[2+size*2+i*2];
        int target_id = hget_i2i(m->offset2id_map, ins->offset + val);
        jd_dex_ins *target_ins = get_dex_ins(m, target_id);
        ladd_obj(ins->targets, target_ins);
        ladd_obj(ins->jumps, target_ins);
        DEBUG_PRINT("key is: %d -> val: %d\n", key, val);
    }
    if (ins->next != NULL)
        ladd_obj_no_dup(ins->targets, ins->next);
}

static void init_dex_instruction_graph(jd_method *m)
{
    list_object *instructions = m->instructions;
    for (int i = 0; i < instructions->size; ++i) {
        jd_dex_ins *ins = lget_obj(instructions, i);
        if (dex_ins_is_unconditional_jump(ins)) {
            init_dex_ins_unconditional_jump(ins);
        }
        else if (dex_ins_is_conditional_jump(ins)) {
            init_dex_ins_conditional_jump(ins);
        }
        else if (dex_ins_is_packed_switch(ins)) {
            init_dex_packed_switch_jump(ins);
        }
        else if (dex_ins_is_sparse_switch(ins)) {
            init_dex_sparse_switch_jump(ins);
        }
        else if (dex_ins_is_return_op(ins) || dex_ins_is_throw(ins)) {

        }
        else {
            init_dex_ins_default_jump(ins);
        }

        for (int j = 0; j < ins->jumps->size; ++j) {
            jd_dex_ins *jump_to_ins = lget_obj(ins->jumps, j);
            ladd_obj(jump_to_ins->comings, ins);
        }
    }
}

static void dex_code_item_instruction(jd_method *m, dex_code_item *code)
{
    m->instructions = linit_object();
    m->offset2id_map = hashmap_init((hcmp_fn) i2i_cmp, 0);
    jd_dex_ins *prev = NULL;
    uint32_t offset = 0;
    for (int i = 0; i < code->insns_size; ++i) {
        u2 item = code->insns[i];
        u1 opcode = item & 0xFF;

        jd_dex_ins *ins = make_obj(jd_dex_ins);
        ins->code = opcode;
        ins->name = dex_opcode_name(opcode);
        ins->format = dex_opcode_fmt(opcode);
        ins->idx = m->instructions->size;
        ins->offset = offset;
        ins->type = m->type;
        ins->targets = linit_object_with_capacity(1);
        ins->jumps = linit_object_with_capacity(2);
        ins->comings = linit_object_with_capacity(1);
        ins->extra = NULL;

        ins->method = m;
        ins->param = &code->insns[i];
        ins->uses = bitset_create_with_capacity(m->max_locals);
        ins->defs = bitset_create_with_capacity(m->max_locals);
        ins->fn = ((jd_dex*)(m->meta))->ins_fn;
        ladd_obj(m->instructions, ins);
        hset_i2i(m->offset2id_map, offset, ins->idx);
        dex_ins_use_def_init(ins);

        if (dex_ins_is_goto_jump(ins)) {
            uint32_t target = dex_original_goto_offset(ins);
            ins->param = x_alloc(sizeof(u2) * 2);
            ins->param[0] = target >> 16;
            ins->param[1] = target;
        }

        if (dex_ins_is_if(ins)) {
            u2 *new_param = x_alloc(sizeof(u2) * 2);
            memcpy(new_param, ins->param, sizeof(u2) * 2);
            ins->param = new_param;
        }

        if (opcode == 0x00) {
            if (item == 0x0100) {
                u2 size = code->insns[i+1];
                ins->param_length = size * 2 + 4;
            }
            else if (item == 0x0200) {
                u2 size = code->insns[i+1];
                ins->param_length = size * 4 + 2;
            }
            else if (item == 0x0300) {
                u2 element_size = code->insns[i+1];
                u2 size = code->insns[i+2];
                ins->param_length = (size * element_size + 1) / 2 + 4;
            }
            else {
                ins->param_length = 1;
            }
        }
        else {
            ins->param_length = dex_opcode_len(opcode);
        }
        i += (ins->param_length - 1);
        offset += ins->param_length;

        if (prev == NULL)
            ins->prev = NULL;
        else {
            ins->prev = prev;
            prev->next = ins;
        }
        prev = ins;
    }
}

void dex_method_init(jsource_file *jf, jd_method *m, encoded_method *em)
{
    jd_dex *dex = jf->meta;
    jd_meta_dex *meta = dex->meta;
    dex_method_id *method_id = &meta->method_ids[em->method_id];
    m->name = dex_str_of_idx(meta, method_id->name_idx);
    m->access_flags = em->access_flags;
    m->meta_method = em;
    m->meta = dex;
    m->jfile = jf;
    m->type = JD_TYPE_DALVIK;
    m->fn = dex->method_fn;

    dex_method_descriptor(m);

    if (em->code == NULL)
        return;

    m->max_locals = em->code->registers_size;

    dex_code_item_instruction(m, em->code);

    init_dex_instruction_graph(m);

    const char *unflatten = getenv("GARLIC_UNFLATTEN");
    // Exception handlers introduce implicit predecessors: leave such methods intact.
    if (unflatten && strcmp(unflatten, "1") == 0 && em->code->tries_size == 0) {
        bool changed = specialize_hash_dispatchers(m);
        changed |= unflatten_constant_dispatchers(m);
        changed |= simplify_constant_predicates(m);
        if (!changed)
            goto dex_method_graph_done;
        for (int i = 0; i < m->instructions->size; ++i) {
            jd_dex_ins *ins = lget_obj(m->instructions, i);
            lclear_object(ins->targets);
            lclear_object(ins->jumps);
            lclear_object(ins->comings);
        }
        init_dex_instruction_graph(m);
    }
dex_method_graph_done:

    dex_method_exception_init(m, em);
}
