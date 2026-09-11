#include "decompiler/method.h"
#include "dalvik/dex_method.h"
#include "dex_descriptor.h"
#include "dex_exception.h"
#include "parser/dex/metadata.h"
#include "dex_annotation.h"
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

// Constant-state edge specialization inspired by eShard's D810 (not copied).
// State is inferred from reachable CFG predecessors, never textual adjacency.
static int java_string_hash(const char *text)
{
    // DEX strings use UTF-8 while String.hashCode() iterates UTF-16 code units.
    // Decode conservatively so malformed input can never make the optimizer read
    // past the string terminator.
    uint32_t hash = 0;
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
    if (!wrapper || !wrapper->code || wrapper->code->tries_size ||
        wrapper->code->insns_size != 5 || !(wrapper->access_flags & ACC_DEX_STATIC)) return false;
    const u2 *w = wrapper->code->insns;
    if ((w[0] & 0xff) != DEX_INS_INVOKE_VIRTUAL || (w[0] >> 12) != 1 ||
        (w[3] & 0xff) != DEX_INS_MOVE_RESULT || (w[4] & 0xff) != DEX_INS_RETURN ||
        (w[3] >> 8) != (w[4] >> 8) || w[1] >= meta->header->method_ids_size ||
        wrapper->code->ins_size != 1 || (w[2] & 15) != wrapper->code->registers_size - 1)
        return false;
    dex_method_id *target = &meta->method_ids[w[1]];
    dex_proto_id *proto = &meta->proto_ids[target->proto_idx];
    const char *owner = dex_str_of_type_id(meta, target->class_idx);
    return (!strcmp(owner, "Ljava/lang/Object;") || !strcmp(owner, "Ljava/lang/String;")) &&
        !strcmp(dex_str_of_method_id(meta, w[1]), "hashCode") &&
        !strcmp(dex_str_of_type_id(meta, proto->return_type_idx), "I") &&
        (!proto->type_list || !proto->type_list->size);
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

/* Bounded backward path tracking, following D810's approach on Dalvik IR.
 * A join is constant only when all reachable predecessors agree; distinct
 * states and cycles remain unknown rather than choosing an arbitrary path. */
typedef struct {
    jd_method *method;
    int *pred;
    bool *reachable;
    unsigned budget;
    int *first_pred, *edge_source, *edge_next;
    unsigned char *invariant_kind;
    uint32_t *invariant_bits;
} state_graph;
typedef struct { uint32_t bits; bool string; } state_value;

static bool state_before(state_graph *g, jd_dex_ins *at, unsigned reg,
                         state_value *out, unsigned depth);

static bool hash_call(jd_meta_dex *meta, jd_dex_ins *ins)
{
    if ((!dex_ins_is_invokevirtual(ins) && !dex_ins_is_invokestatic(ins)) ||
        dex_ins_parameter(ins, 0) != 1) return false;
    unsigned index = dex_ins_parameter(ins, 1);
    if (index >= meta->header->method_ids_size) return false;
    dex_method_id *id = &meta->method_ids[index];
    dex_proto_id *proto = &meta->proto_ids[id->proto_idx];
    if (strcmp(dex_str_of_type_id(meta, proto->return_type_idx), "I")) return false;
    if (dex_ins_is_invokestatic(ins)) return is_hash_wrapper(meta, index);
    const char *owner = dex_str_of_type_id(meta, id->class_idx);
    return (!strcmp(owner, "Ljava/lang/String;") || !strcmp(owner, "Ljava/lang/Object;")) &&
        !strcmp(dex_str_of_method_id(meta, index), "hashCode") &&
        (!proto->type_list || !proto->type_list->size);
}

static bool state_writes_wide(unsigned op)
{
    return (op >= 0x04 && op <= 0x06) || op == 0x0b ||
        (op >= 0x16 && op <= 0x19) || op == 0x45 || op == 0x53 || op == 0x61 ||
        op == 0x7d || op == 0x7e || op == 0x80 || op == 0x81 || op == 0x83 ||
        op == 0x86 || op == 0x88 || op == 0x89 || op == 0x8b ||
        (op >= 0x9b && op <= 0xa5) || (op >= 0xab && op <= 0xaf) ||
        (op >= 0xbb && op <= 0xc5) || (op >= 0xcb && op <= 0xcf);
}

static bool state_after(state_graph *g, jd_dex_ins *p, unsigned reg,
                        state_value *out, unsigned depth);

static bool state_before(state_graph *g, jd_dex_ins *at, unsigned reg,
                         state_value *out, unsigned depth)
{
    if (!g->budget || !at || depth > 192 || reg >= g->method->max_locals) return false;
    if (g->invariant_kind[reg] == 1 || g->invariant_kind[reg] == 2) {
        out->bits = g->invariant_bits[reg]; out->string = g->invariant_kind[reg] == 2; return true;
    }
    if (at->idx == 0) return false;
    --g->budget;
    bool found = false;
    unsigned paths = 0;
    for (int e = g->first_pred[at->idx]; e >= 0; e = g->edge_next[e]) {
        if (++paths > 16) return false;
        state_value candidate;
        jd_dex_ins *p = lget_obj(g->method->instructions, g->edge_source[e]);
        if (!state_after(g, p, reg, &candidate, depth + 1)) return false;
        if (found && (candidate.bits != out->bits || candidate.string != out->string)) return false;
        *out = candidate; found = true;
    }
    return found;
}

static bool state_after(state_graph *g, jd_dex_ins *p, unsigned reg,
                        state_value *out, unsigned depth)
{
    if (!g->budget || depth > 192) return false;
    --g->budget;
    jd_meta_dex *meta = ((jd_dex *)g->method->meta)->meta;
    unsigned op = p->code;
    bool wide_def = state_writes_wide(op);
    if (wide_def && reg == dex_ins_parameter(p, 0) + 1) return false;
    if (!bitset_get(p->defs, reg)) return state_before(g, p, reg, out, depth + 1);
    out->string = false;
    switch (p->code) {
        case DEX_INS_CONST_4:
            out->bits = (uint32_t)((int32_t)(p->param[0] >> 12) - ((p->param[0] & 0x8000) ? 16 : 0)); return true;
        case DEX_INS_CONST_16:
            out->bits = (uint32_t)(int32_t)(int16_t)dex_ins_parameter(p, 1); return true;
        case DEX_INS_CONST:
            out->bits = dex_ins_parameter(p, 1); return true;
        case DEX_INS_CONST_HIGH16:
            out->bits = (uint32_t)dex_ins_parameter(p, 1) << 16; return true;
        case DEX_INS_CONST_STRING: case DEX_INS_CONST_STRING_JUMBO:
            out->bits = dex_ins_parameter(p, 1); out->string = true;
            return out->bits < meta->header->string_ids_size;
        case DEX_INS_MOVE: case DEX_INS_MOVE_FROM16: case DEX_INS_MOVE_16:
        case DEX_INS_MOVE_OBJECT: case DEX_INS_MOVE_OBJECT_FROM16: case DEX_INS_MOVE_OBJECT_16:
            return state_before(g, p, dex_ins_parameter(p, 1), out, depth + 1);
        case DEX_INS_MOVE_RESULT: {
            jd_dex_ins *invoke = p->prev;
            if (!invoke || g->pred[p->idx] != (int)invoke->idx) return false;
            if (dex_ins_is_invokestatic(invoke) &&
                eval_constant_static_method(meta, dex_ins_parameter(invoke, 1), &out->bits))
                return true;
            state_value text;
            if (!hash_call(meta, invoke) ||
                !state_before(g, invoke, dex_ins_parameter(invoke, 2), &text, depth + 1) ||
                !text.string) return false;
            out->bits = (uint32_t)java_string_hash(dex_str_of_idx(meta, text.bits)); return true;
        }
        default: break;
    }
    /* Integer arithmetic uses unsigned bits for Java's wrapping operations. */
    op = p->code;
    state_value a, b;
    if (op >= 0x90 && op <= 0x9a) {
        if (!state_before(g, p, dex_ins_parameter(p, 1), &a, depth + 1) ||
            !state_before(g, p, dex_ins_parameter(p, 2), &b, depth + 1)) return false;
        op -= 0x90;
    } else if (op >= 0xb0 && op <= 0xba) {
        if (!state_before(g, p, reg, &a, depth + 1) ||
            !state_before(g, p, dex_ins_parameter(p, 1), &b, depth + 1)) return false;
        op -= 0xb0;
    } else if (op >= 0xd0 && op <= 0xe2) {
        if (!state_before(g, p, dex_ins_parameter(p, 1), &a, depth + 1)) return false;
        b.string = false;
        b.bits = op < 0xd8 ? (uint32_t)(int32_t)(int16_t)dex_ins_parameter(p, 2) :
                           (uint32_t)(int32_t)(int8_t)dex_ins_parameter(p, 2);
        op -= op < 0xd8 ? 0xd0 : 0xd8;
        if (op == 1) { state_value tmp = a; a = b; b = tmp; }
    } else return false;
    if (a.string || b.string) return false;
    switch (op) {
        case 0: out->bits = a.bits + b.bits; break;
        case 1: out->bits = a.bits - b.bits; break;
        case 2: out->bits = a.bits * b.bits; break;
        case 3: case 4:
            if (!b.bits) return false;
            if (a.bits == 0x80000000u && b.bits == 0xffffffffu)
                out->bits = op == 3 ? a.bits : 0;
            else out->bits = op == 3 ? (uint32_t)((int32_t)a.bits / (int32_t)b.bits) :
                                      (uint32_t)((int32_t)a.bits % (int32_t)b.bits);
            break;
        case 5: out->bits = a.bits & b.bits; break;
        case 6: out->bits = a.bits | b.bits; break;
        case 7: out->bits = a.bits ^ b.bits; break;
        case 8: out->bits = a.bits << (b.bits & 31); break;
        case 9: out->bits = (uint32_t)((int32_t)a.bits >> (b.bits & 31)); break;
        case 10: out->bits = a.bits >> (b.bits & 31); break;
        default: return false;
    }
    return true;
}

static void state_make_goto(jd_dex_ins *ins, jd_dex_ins *target)
{
    ins->code = DEX_INS_GOTO;
    ins->name = "goto";
    ins->format = kFmt10t;
    ins->param_length = 1;
    ins->param = x_alloc(sizeof(u2) * 2);
    dex_setup_goto_offset(ins, target->offset);
    if (target == ins->next) {
        ins->code = 0; ins->name = "nop"; ins->format = kFmt10x; ins->param[0] = 0;
    }
    bitset_clear(ins->defs);
    bitset_clear(ins->uses);
}

/* Replacing a proven String hash is safe only when no user class initializer
 * can run. Other helper calls are retained, even if their return is constant. */
static bool hash_has_no_initializer(jd_meta_dex *meta, jd_dex_ins *invoke)
{
    if (dex_ins_is_invokevirtual(invoke)) return true;
    unsigned type = meta->method_ids[dex_ins_parameter(invoke, 1)].class_idx;
    for (u4 i = 0; i < meta->header->class_defs_size; ++i) {
        dex_class_def *c = &meta->class_defs[i];
        if (c->class_idx != type || !c->class_data) continue;
        if (strcmp(dex_str_of_type_id(meta, c->superclass_idx), "Ljava/lang/Object;")) return false;
        for (u4 j = 0; j < c->class_data->direct_methods_size; ++j)
            if (!strcmp(dex_str_of_method_id(meta, c->class_data->direct_methods[j].method_id), "<clinit>"))
                return false;
        return true;
    }
    return false;
}

/* Emulate compare-only dispatcher chains on the incoming edge. Stop before
 * every write or call: no state update or observable operation is skipped. */
static jd_dex_ins *trace_comparison_dispatcher(state_graph *g, jd_dex_ins *origin,
                                              jd_dex_ins *at)
{
    bool compared = false;
    jd_dex_ins *visited[64];
    for (int step = 0; at && step < 64; ++step) {
        for (int j = 0; j < step; ++j) if (visited[j] == at) return NULL;
        visited[step] = at;
        if (dex_ins_is_goto_jump(at)) {
            at = dex_ins_of_offset(g->method, dex_goto_offset(at)); continue;
        }
        if (!dex_ins_is_if(at)) return compared ? at : NULL;
        state_value a, b = {0, false};
        if (!state_before(g, origin, dex_ins_parameter(at, 0), &a, 0) || a.string)
            return compared ? at : NULL;
        if (at->code < DEX_INS_IF_EQZ &&
            (!state_before(g, origin, dex_ins_parameter(at, 1), &b, 0) || b.string))
            return compared ? at : NULL;
        unsigned op = at->code >= DEX_INS_IF_EQZ ? at->code - DEX_INS_IF_EQZ : at->code - DEX_INS_IF_EQ;
        bool taken;
        switch (op) {
            case 0: taken = a.bits == b.bits; break;
            case 1: taken = a.bits != b.bits; break;
            case 2: taken = (int32_t)a.bits < (int32_t)b.bits; break;
            case 3: taken = (int32_t)a.bits >= (int32_t)b.bits; break;
            case 4: taken = (int32_t)a.bits > (int32_t)b.bits; break;
            case 5: taken = (int32_t)a.bits <= (int32_t)b.bits; break;
            default: return NULL;
        }
        compared = true;
        at = taken ? dex_ins_of_offset(g->method, dex_ins_if_jump_offset(at)) : at->next;
    }
    return NULL;
}

static int specialize_state_edges(jd_method *m)
{
    int count = m->instructions->size, changed = 0;
    state_graph g = {m, calloc(count, sizeof(int)), calloc(count, sizeof(bool))};
    int *queue = calloc(count, sizeof(int));
    size_t edges = 0;
    for (int i = 0; i < count; ++i) edges += ((jd_dex_ins *)lget_obj(m->instructions, i))->targets->size;
    g.first_pred = malloc(count * sizeof(int));
    g.edge_source = malloc((edges + 1) * sizeof(int));
    g.edge_next = malloc((edges + 1) * sizeof(int));
    size_t edge = 0;
    for (int i = 0; i < count; ++i) g.first_pred[i] = g.pred[i] = -1;
    int head = 0, tail = 1; g.reachable[0] = true;
    while (head < tail) {
        jd_dex_ins *p = lget_obj(m->instructions, queue[head++]);
        for (int j = 0; j < p->targets->size; ++j) {
            jd_dex_ins *t = lget_obj(p->targets, j);
            if (!t) continue;
            g.edge_source[edge] = p->idx;
            g.edge_next[edge] = g.first_pred[t->idx];
            g.first_pred[t->idx] = edge++;
            if (!g.reachable[t->idx]) { g.reachable[t->idx] = true; queue[tail++] = t->idx; }
            if (g.pred[t->idx] == -1) g.pred[t->idx] = p->idx;
            else if (g.pred[t->idx] != (int)p->idx) g.pred[t->idx] = -2;
        }
    }
    /* Constants assigned identically at every definition are loop invariant.
     * Input registers are excluded because their initial value is unknown. */
    g.invariant_kind = calloc(m->max_locals, 1);
    g.invariant_bits = calloc(m->max_locals, sizeof(uint32_t));
    encoded_method *encoded = m->meta_method;
    for (int r = m->max_locals - encoded->code->ins_size; r < m->max_locals; ++r)
        g.invariant_kind[r] = 3;
    for (int i = 0; i < count; ++i) if (g.reachable[i]) {
        jd_dex_ins *ins = lget_obj(m->instructions, i);
        size_t reg = 0;
        while (bitset_next_set_bit(ins->defs, &reg)) {
            unsigned char kind = 3;
            uint32_t bits = 0;
            switch (ins->code) {
                case DEX_INS_CONST_4: case DEX_INS_CONST_16: case DEX_INS_CONST:
                    kind = 1; bits = (uint32_t)dex_ins_parameter(ins, 1);
                    if (ins->code == DEX_INS_CONST_16) bits = (uint32_t)(int32_t)(int16_t)bits;
                    break;
                case DEX_INS_CONST_HIGH16: kind = 1; bits = (uint32_t)dex_ins_parameter(ins, 1) << 16; break;
                case DEX_INS_CONST_STRING: case DEX_INS_CONST_STRING_JUMBO:
                    kind = 2; bits = dex_ins_parameter(ins, 1); break;
            }
            if (reg < m->max_locals) {
                unsigned char old = g.invariant_kind[reg];
                if (old && (old != kind || g.invariant_bits[reg] != bits)) kind = 3;
                g.invariant_kind[reg] = kind; g.invariant_bits[reg] = bits;
                /* Be conservative about overlapping wide register writes. */
                if (state_writes_wide(ins->code) && reg + 1 < m->max_locals) g.invariant_kind[reg + 1] = 3;
            }
            ++reg;
        }
    }
    g.pred[0] = -2; /* Method entry also has an external predecessor. */
    /* Plan against an immutable graph. Apply rewrites only after evaluation. */
    jd_dex_ins **targets = calloc(count, sizeof(*targets));
    jd_dex_ins **constants = calloc(count, sizeof(*constants));
    uint32_t *hashes = calloc(count, sizeof(*hashes));
    for (int i = 0; i < count; ++i) {
        jd_dex_ins *ins = lget_obj(m->instructions, i);
        if (!g.reachable[i]) continue;
        g.budget = 2048;
        state_value value;
        jd_dex_ins *target = NULL;
        if (dex_ins_is_switch(ins)) {
            if (state_before(&g, ins, dex_ins_parameter(ins, 0), &value, 0) && !value.string)
                target = switch_target_for_value(m, ins, value.bits, dex_ins_parameter(ins, 0));
        } else if (dex_ins_is_if(ins) && ins->code >= DEX_INS_IF_EQZ) {
            if (state_before(&g, ins, dex_ins_parameter(ins, 0), &value, 0) && !value.string)
                target = predicate_taken(ins, value.bits) ? dex_ins_of_offset(m, dex_ins_if_jump_offset(ins)) : ins->next;
        } else if (dex_ins_is_goto_jump(ins)) {
            jd_dex_ins *dispatch = dex_ins_of_offset(m, dex_goto_offset(ins));
            for (int step = 0; dispatch && dex_ins_is_goto_jump(dispatch) && step < 16; ++step)
                dispatch = dex_ins_of_offset(m, dex_goto_offset(dispatch));
            jd_meta_dex *meta = ((jd_dex *)m->meta)->meta;
            if (dispatch && hash_call(meta, dispatch) && dispatch->next &&
                dispatch->next->code == DEX_INS_MOVE_RESULT && dispatch->next->next) {
                jd_dex_ins *sw = dispatch->next->next;
                for (int step = 0; sw && dex_ins_is_goto_jump(sw) && step < 16; ++step)
                    sw = dex_ins_of_offset(m, dex_goto_offset(sw));
                unsigned dest = dex_ins_parameter(dispatch->next, 0);
                if (state_before(&g, ins, dex_ins_parameter(dispatch, 2), &value, 0) && value.string) {
                    target = switch_target_for_value(m, sw,
                        (uint32_t)java_string_hash(dex_str_of_idx(meta, value.bits)), dest);
                    jd_dex_ins *assignment = ins->prev;
                    jd_dex_ins *after = ins;
                    for (int step = 0; assignment && step < 32 &&
                         g.pred[after->idx] == (int)assignment->idx &&
                         !bitset_get(assignment->uses, dest) && !bitset_get(assignment->defs, dest) &&
                         !dex_ins_is_goto_jump(assignment) && !dex_ins_is_if(assignment); ++step) {
                        after = assignment; assignment = assignment->prev;
                    }
                    if (!assignment || g.pred[after->idx] != (int)assignment->idx ||
                        (assignment->code != DEX_INS_CONST_STRING && assignment->code != DEX_INS_CONST_STRING_JUMBO) ||
                        dex_ins_parameter(assignment, 0) != dest ||
                        dex_ins_parameter(dispatch, 2) != dest ||
                        !hash_has_no_initializer(meta, dispatch)) target = NULL;
                    if (target) {
                        constants[i] = assignment;
                        hashes[i] = (uint32_t)java_string_hash(dex_str_of_idx(meta, value.bits));
                    }
                }
            } else if (dispatch && dex_ins_is_if(dispatch)) {
                target = trace_comparison_dispatcher(&g, ins, dispatch);
            } else if (dispatch && dex_ins_is_switch(dispatch) &&
                       state_before(&g, ins, dex_ins_parameter(dispatch, 0), &value, 0) && !value.string) {
                target = switch_target_for_value(m, dispatch, value.bits, dex_ins_parameter(dispatch, 0));
            }
        }
        if (target && target != ins && (!dex_ins_is_goto_jump(ins) ||
            target->offset != dex_goto_offset(ins))) targets[i] = target;
    }
    for (int i = 0; i < count; ++i) if (targets[i]) {
        if (constants[i]) {
            jd_dex_ins *c = constants[i];
            unsigned reg = dex_ins_parameter(c, 0);
            c->code = DEX_INS_CONST; c->name = "const"; c->format = kFmt31i;
            c->param_length = 3; c->param = x_alloc(sizeof(u2) * 3);
            c->param[0] = (reg << 8) | DEX_INS_CONST;
            c->param[1] = hashes[i]; c->param[2] = hashes[i] >> 16;
        }
        state_make_goto(lget_obj(m->instructions, i), targets[i]);
        ++changed;
    }
    free(constants); free(hashes); free(targets); free(queue); free(g.pred); free(g.reachable);
    free(g.first_pred); free(g.edge_source); free(g.edge_next);
    free(g.invariant_kind); free(g.invariant_bits);
    return changed;
}

/* Drop dead executable instructions after rewriting, but keep DEX payloads.
 * Otherwise dead predecessors still become basic blocks and the structurer can
 * emit duplicate returns or weave discarded dispatcher arms into live code. */
static void prune_state_graph(jd_method *m)
{
    int count = m->instructions->size;
    bool *live = calloc(count, sizeof(bool));
    int *queue = calloc(count, sizeof(int));
    int head = 0, tail = 1; live[0] = true;
    while (head < tail) {
        jd_dex_ins *p = lget_obj(m->instructions, queue[head++]);
        for (int j = 0; j < p->targets->size; ++j) {
            jd_dex_ins *t = lget_obj(p->targets, j);
            if (t && !live[t->idx]) { live[t->idx] = true; queue[tail++] = t->idx; }
        }
    }
    list_object *kept = linit_object();
    jd_dex_ins *prev = NULL;
    m->offset2id_map = hashmap_init((hcmp_fn)i2i_cmp, 0);
    for (int i = 0; i < count; ++i) {
        jd_dex_ins *ins = lget_obj(m->instructions, i);
        bool payload = ins->code == 0 && ins->param[0] != 0;
        if (!live[i] && !payload) continue;
        ins->idx = kept->size; ladd_obj(kept, ins);
        hset_i2i(m->offset2id_map, ins->offset, ins->idx);
        ins->prev = prev; ins->next = NULL;
        if (prev) prev->next = ins;
        prev = ins;
    }
    m->instructions = kept;
    for (int i = 0; i < kept->size; ++i) {
        jd_dex_ins *ins = lget_obj(kept, i);
        if (dex_ins_is_goto_jump(ins) && ins->next && dex_goto_offset(ins) == ins->next->offset)
            state_make_goto(ins, ins->next);
    }
    free(queue); free(live);
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
    s4 packed_offset = (s4)((u4)ins->param[2] << 16 | ins->param[1]);
    hashmap *map = m->offset2id_map;
    int payload_idx = hget_i2i(map, ins->offset + packed_offset);
    jd_dex_ins *packed_ins = lget_obj(m->instructions, payload_idx);

    int size = packed_ins->param[1];
    int first_key = (s4)((u4)packed_ins->param[3] << 16 | packed_ins->param[2]);

    for (int i = 0; i < size; ++i) {
        int offset = (s4)((u4)packed_ins->param[5 + i * 2] << 16 |
                         packed_ins->param[4 + i * 2]);
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
    s4 offset = (s4)((u4)ins->param[2] << 16 | ins->param[1]);
    int payload_idx = hget_i2i(m->offset2id_map, ins->offset + offset);
    jd_dex_ins *packed_ins = lget_obj(m->instructions, payload_idx);
    int size = packed_ins->param[1];

    u2 *params = packed_ins->param;
    for (int i = 0; i < size; ++i) {
        int key = (s4)((u4)params[3+i*2] << 16 | params[2+i*2]);
        int val = (s4)((u4)params[3+size*2+i*2] << 16 | params[2+size*2+i*2]);
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
                u4 size = (u4)code->insns[i+2] | ((u4)code->insns[i+3] << 16);
                ins->param_length = ((uint64_t)size * element_size + 1) / 2 + 4;
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
        bool changed = false;
        for (int pass = 0; pass < 16; ++pass) {
            if (!specialize_state_edges(m)) break;
            changed = true;
            for (int i = 0; i < m->instructions->size; ++i) {
                jd_dex_ins *ins = lget_obj(m->instructions, i);
                lclear_object(ins->targets);
                lclear_object(ins->jumps);
                lclear_object(ins->comings);
            }
            init_dex_instruction_graph(m);
        }
        if (changed) {
            prune_state_graph(m);
            for (int i = 0; i < m->instructions->size; ++i) {
                jd_dex_ins *ins = lget_obj(m->instructions, i);
                lclear_object(ins->targets); lclear_object(ins->jumps); lclear_object(ins->comings);
            }
            init_dex_instruction_graph(m);
        }
    }

    dex_method_exception_init(m, em);
}
