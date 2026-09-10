#include "decompiler/method.h"
#include "dalvik/dex_method.h"
#include "dex_descriptor.h"
#include "dex_exception.h"
#include "parser/dex/metadata.h"
#include "dex_annotation.h"
#include <stdlib.h>
#include <stdint.h>

// Specialize a dispatcher edge only when its state assignment is the unique
// predecessor. No instruction with side effects is skipped. Inspired by the
// constant-state edge specialization described by eShard's D810 (not copied).
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
        switch (assignment->code) {
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
        uint32_t payloadOffset = dispatcher->offset + (uint32_t)dispatcher->param[1] +
                                 ((uint32_t)dispatcher->param[2] << 16);
        jd_dex_ins *payload = dex_ins_of_offset(m, payloadOffset);
        if (!payload || payload->param_length < 2) continue;
        unsigned count = payload->param[1];
        bool packed = dex_ins_is_packed_switch(dispatcher);
        if (payload->param[0] != (packed ? 0x0100 : 0x0200) ||
            (unsigned)payload->param_length < (packed ? 4 + count * 2 : 2 + count * 4))
            continue;
        jd_dex_ins *target = dispatcher->next;
        for (unsigned k = 0; k < count; ++k) {
            unsigned keyIndex = packed ? 2 : 2 + k * 2;
            uint32_t key = (uint32_t)payload->param[keyIndex] |
                           ((uint32_t)payload->param[keyIndex + 1] << 16);
            if (packed) key += k;
            if (key != value) continue;
            unsigned targetIndex = packed ? 4 + k * 2 : 2 + count * 2 + k * 2;
            uint32_t relative = (uint32_t)payload->param[targetIndex] |
                                ((uint32_t)payload->param[targetIndex + 1] << 16);
            target = dex_ins_of_offset(m, dispatcher->offset + relative);
            break;
        }
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
    if (unflatten && strcmp(unflatten, "1") == 0 && em->code->tries_size == 0 &&
        unflatten_constant_dispatchers(m)) {
        for (int i = 0; i < m->instructions->size; ++i) {
            jd_dex_ins *ins = lget_obj(m->instructions, i);
            lclear_object(ins->targets);
            lclear_object(ins->jumps);
            lclear_object(ins->comings);
        }
        init_dex_instruction_graph(m);
    }

    dex_method_exception_init(m, em);
}
