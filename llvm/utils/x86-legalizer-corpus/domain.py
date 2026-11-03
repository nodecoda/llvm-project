"""Turn a parsed rule ledger into deterministic query points.

A point is a query shape with no expected answer.  The C++ driver fills the
answer from the frozen reference; this module never does.
"""

import expr as E
from parse import eval_int, eval_list
from target import BOUNDARY_PANEL, DEEP_PANEL, SKIP_WITNESS_SLOT1_AS


def sorted_llts(values):
    def key(t):
        if t[0] == "p":
            return (0, 0, 0, t)
        if t[0] == "s":
            return (1, t[1], 0, t)
        return (2, E.llt_scalar_bits(t), t[1], t)
    return sorted(set(values), key=key)


def point(rule_set, opcode, origin, types, mem=None, align=0, rule_index=None):
    if mem is None:
        align = 0
    return {
        "id": None,  # assigned after sorting
        "rule_set": rule_set,
        "opcode": opcode,
        "origin": origin,
        "rule_index": rule_index,
        "types": [E.llt_name(t) for t in types],
        "mem": E.llt_name(mem) if mem is not None else None,
        "align": int(align) if mem is not None else 0,
    }


def align_classes(aligns):
    # class 0 is "below every threshold"; class N is threshold N-1.
    thresholds = sorted(int(a) for a in aligns)
    return [0] + thresholds


def rep_class(classes):
    # The reviewed non-panel slot representative is s32.
    for t in classes:
        if t == E.scalar(32):
            return t
    for t in classes:
        if t[0] == "s":
            return t
    return classes[0]


# The reference applies the first matching rule's mutation.  A mutation that
# rebuilds a type element by element calls LLT::changeElementSize, which
# rejects pointer and pointer-vector types with an assertion.  Such a query is
# not a well-formed legalizer query: prune it instead of asking the reference
# to answer it.  This is a safety filter, not an oracle: it never produces an
# expected action.
# Only these can name a pointer as the mutated type.  clampScalar/minScalar/
# maxScalar/widenScalarToNextPow2 all predicate on isScalar(), so a pointer can
# never match them; widenScalarOrEltToNextPow2 needs a non-power-of-two scalar
# size and pointer sizes are powers of two.
ELEMENT_SIZE_SLOT0 = {
    "scalarSameSizeAs", "clampScalarOrElt", "minScalarOrElt", "maxScalarOrElt",
}


def is_pointer_like(t):
    return t[0] == "p" or (t[0] == "v" and t[2][0] == "p")


def element_size_slots(builder):
    """Slots an element-size mutation may target, conservatively."""
    slots = set()
    for rule in builder.rules:
        method = rule.method
        if method in ELEMENT_SIZE_SLOT0:
            idx = eval_int(rule.args[0]) if rule.args else None
            if idx is None:
                slots.update(range(builder.num_slots))
            else:
                slots.add(idx)
        elif method == "widenScalarIf":
            # The predicate is opaque; any slot it reads may be the target.
            slots.update(range(builder.num_slots))
    return slots


def pointer_unsafe(types, unsafe):
    return any(i in unsafe and is_pointer_like(t) for i, t in enumerate(types))


def class_pair(builder, classes, mem_classes, aligns):
    out = []
    pruned = 0
    rep = rep_class(classes)
    unsafe = element_size_slots(builder)
    if builder.has_mem:
        mems = sorted_llts(mem_classes) or [rep]
        for value in classes:
            for other in classes:
                if pointer_unsafe([value, other], unsafe):
                    pruned += len(mems) * len(align_classes(aligns))
                    continue
                for mem in mems:
                    for align in align_classes(aligns):
                        out.append(point(builder.name, builder.opcodes[0], "classpair",
                                         [value, other], mem, align))
    elif builder.num_slots == 1:
        for ty in classes:
            if pointer_unsafe([ty], unsafe):
                pruned += 1
                continue
            out.append(point(builder.name, builder.opcodes[0], "classpair", [ty]))
    else:
        for left in classes:
            for right in classes:
                if pointer_unsafe([left, right], unsafe):
                    pruned += 1
                    continue
                out.append(point(builder.name, builder.opcodes[0], "classpair",
                                 [left, right]))
    return out, pruned


def boundary(builder, classes, panel, mode):
    out = []
    pruned = 0
    rep = rep_class(classes)
    opcode = builder.opcodes[0]
    align = align_classes(builder.aligns)[1] if builder.aligns else 1
    if mode == "witness":
        for index, b in enumerate(panel):
            if builder.has_mem:
                if index % 2 == 0:
                    out.append(point(builder.name, opcode, "boundary", [b, E.ptr(0)],
                                     rep, align))
                else:
                    out.append(point(builder.name, opcode, "boundary", [rep, E.ptr(0)],
                                     b, align))
            elif builder.num_slots == 1:
                out.append(point(builder.name, opcode, "boundary", [b]))
            elif index % 2 == 0:
                out.append(point(builder.name, opcode, "boundary", [b, rep]))
            else:
                out.append(point(builder.name, opcode, "boundary", [rep, b]))
        return out, pruned
    # full/deep: both directions over the whole class axis.
    unsafe = element_size_slots(builder)
    if builder.has_mem:
        for c in classes:
            if pointer_unsafe([c, E.ptr(0)], unsafe):
                pruned += 2 * len(panel) * len(align_classes(builder.aligns))
                continue
            for b in panel:
                for al in align_classes(builder.aligns):
                    out.append(point(builder.name, opcode, "boundary",
                                     [c, E.ptr(0)], b, al))
                    out.append(point(builder.name, opcode, "boundary",
                                     [b, E.ptr(0)], c, al))
    elif builder.num_slots == 1:
        for b in panel:
            out.append(point(builder.name, opcode, "boundary", [b]))
    else:
        for c in classes:
            for b in panel:
                if pointer_unsafe([c, b], unsafe):
                    pruned += 1
                else:
                    out.append(point(builder.name, opcode, "boundary", [c, b]))
                if pointer_unsafe([b, c], unsafe):
                    pruned += 1
                else:
                    out.append(point(builder.name, opcode, "boundary", [b, c]))
    return out, pruned


def address_space(builder, classes):
    out = []
    opcode = builder.opcodes[0]
    p0, p1, p2 = E.ptr(0), E.ptr(1), E.ptr(2)
    align = align_classes(builder.aligns)[1] if builder.aligns else 1

    def emit(types, mem=None, al=0):
        name = builder.name
        if name in SKIP_WITNESS_SLOT1_AS:
            if len(types) == 2 and types[0] == p0 and types[1] in (p1, p2) \
                    and (mem is None or mem == p0):
                return
        out.append(point(name, opcode, "as", types, mem, al))

    if builder.has_mem:
        emit([p1, p0], p0, align)
        emit([p2, p0], p0, align)
        emit([p0, p1], p0, align)
        emit([p0, p2], p0, align)
        emit([p1, p0], p1, align)
        emit([p1, p0], p2, align)
        emit([p2, p0], p1, align)
        emit([p2, p0], p2, align)
        emit([p0, p0], p1, align)
        emit([p0, p0], p2, align)
    elif builder.num_slots == 1:
        emit([p1])
        emit([p2])
    else:
        emit([p1, p0])
        emit([p2, p0])
        emit([p0, p1])
        emit([p0, p2])
    return out


def opcode_probes(builder, classes, mem_classes):
    out = []
    if len(builder.opcodes) < 2:
        return out
    rep = rep_class(classes)
    mem = (sorted_llts(mem_classes) or [rep])[0]
    for opcode in builder.opcodes[1:]:
        if builder.has_mem:
            out.append(point(builder.name, opcode, "opcode",
                             [rep, E.ptr(0)], mem, 1))
        elif builder.num_slots == 1:
            out.append(point(builder.name, opcode, "opcode", [rep]))
        else:
            out.append(point(builder.name, opcode, "opcode", [rep, rep]))
    return out


# --- per-rule candidate probes ---------------------------------------------

def rule_probes(builder, classes, mem_classes):
    out = []
    rep = rep_class(classes)
    mem = (sorted_llts(mem_classes) or [rep])[0]
    for rule in builder.rules:
        q = probe_for_rule(builder, rule, classes, rep, mem)
        if q is None:
            continue
        types, memdesc, align = q
        while len(types) < builder.num_slots:
            types.append(rep)
        if builder.has_mem:
            types = types[:2]
            if len(types) < 2:
                types.append(E.ptr(0))
            out.append(point(builder.name, builder.opcodes[0], "rule", types,
                             memdesc if memdesc is not None else mem,
                             align if memdesc is not None else 1,
                             rule.index))
        else:
            out.append(point(builder.name, builder.opcodes[0], "rule", types,
                             None, 0, rule.index))
    return out


def probe_for_rule(builder, rule, classes, rep, mem):
    method = rule.method
    args = rule.args

    def list_arg(arg):
        return eval_list(arg, probe_env(builder, classes))

    # Type-list rules: the first listed tuple is the natural witness.
    if method in ("legalFor", "customFor", "lowerFor", "libcallFor",
                  "alwaysLegal"):
        if method == "alwaysLegal" or not args:
            return ([rep], None, 0)
        for arg in reversed(args):
            lst = list_arg(arg)
            if lst:
                return tuple_from_item(lst[0], builder, mem)
        return None

    if method in ("legalForCartesianProduct", "customForCartesianProduct"):
        picked = []
        for arg in args:
            lst = list_arg(arg)
            if lst:
                picked.append(lst[0])
        if picked:
            return (picked, None, 0)
        return None

    if method == "legalForTypesWithMemDesc":
        for arg in args:
            lst = list_arg(arg)
            if lst:
                for item in lst:
                    if isinstance(item, list) and len(item) == 4:
                        return ([item[0]], item[2], item[3])
        return None

    if method in ("clampNumElements", "clampMinNumElements", "clampMaxNumElements"):
        if len(args) < 3:
            return None
        slot = eval_int(args[0], probe_env(builder, classes))
        elem = safe_eval(args[1], probe_env(builder, classes))
        count = eval_int(args[2], probe_env(builder, classes))
        if slot is None or not E.is_llt(elem) or count is None:
            return None
        is_min = method in ("clampNumElements", "clampMinNumElements")
        best = None
        for t in classes:
            if t[0] != "v" or E.llt_scalar_bits(t) != E.llt_scalar_bits(elem):
                continue
            lanes = t[1]
            if (is_min and lanes < count) or (not is_min and lanes > count):
                if best is None or abs(lanes - count) < abs(best[1] - count):
                    best = t
        if best is None:
            lanes = max(2, count - 1) if is_min else max(2, count + 1)
            best = E.vec(lanes, E.scalar(E.llt_scalar_bits(elem)))
        return ([best], None, 0)

    if method in ("clampScalar", "minScalar", "maxScalar", "clampScalarOrElt"):
        if len(args) < 2:
            return None
        slot = eval_int(args[0], probe_env(builder, classes))
        bound = safe_eval(args[1], probe_env(builder, classes))
        if not E.is_llt(bound):
            return None
        want_lower = method in ("clampScalar", "minScalar", "clampScalarOrElt")
        cands = [t for t in classes if t[0] == bound[0]]
        if want_lower:
            cands = [t for t in cands if E.llt_bits(t) < E.llt_bits(bound)]
            cands.sort(key=E.llt_bits, reverse=True)
        else:
            cands = [t for t in cands if E.llt_bits(t) > E.llt_bits(bound)]
            cands.sort(key=E.llt_bits)
        return ([cands[0] if cands else bound], None, 0)

    if method in ("widenScalarToNextPow2", "widenScalarOrEltToNextPow2"):
        if len(args) < 2:
            return None
        minimum = eval_int(args[1], probe_env(builder, classes))
        if minimum is None:
            return None
        return ([E.scalar(minimum + 1)], None, 0)

    if method == "moreElementsToNextPow2":
        return ([E.vec(3, E.scalar(32))], None, 0)

    if method == "scalarize":
        return ([E.vec(2, E.scalar(32))], None, 0)

    if method == "scalarizeIf":
        # scalarizeIf is used with scalarOrEltWiderThan(0, 64) in the X86
        # chain; a 2-lane 128-bit vector is a valid trigger.
        return ([E.vec(2, E.scalar(128))], None, 0)

    if method == "scalarSameSizeAs":
        return ([E.scalar(8), E.scalar(8)], None, 0)

    if method in ("lower", "libcall"):
        return ([rep], None, 0)

    if method in ("customIf", "legalIf", "lowerIf", "widenScalarIf"):
        return probe_from_predicate(args, classes, rep)

    return None


def probe_from_predicate(args, classes, rep):
    for arg in args:
        if isinstance(arg, E.Call):
            if arg.name in ("typeIs", "typeIsNot", "typeInSet") and len(arg.args) >= 2:
                ty = safe_eval(arg.args[1], {})
                if E.is_llt(ty):
                    return ([ty], None, 0)
            if arg.name in ("scalarOrEltWiderThan", "scalarNarrowerThan") and len(arg.args) >= 2:
                bits = eval_int(arg.args[1])
                if bits is not None:
                    return ([E.scalar(bits + 1)], None, 0)
            if arg.name in ("typePairInSet", "typePairIs") and len(arg.args) >= 3:
                lst = eval_list(arg.args[2], {})
                if lst and isinstance(lst[0], list) and len(lst[0]) >= 2:
                    return (list(lst[0][:2]), None, 0)
        if isinstance(arg, E.Lambda):
            return None
    return None


def tuple_from_item(item, builder, mem):
    if isinstance(item, list):
        types = [x for x in item if E.is_llt(x)]
        if len(item) == 4 and E.is_llt(item[2]):
            return ([types[0]], item[2], item[3])
        return (types, None, 0)
    if E.is_llt(item):
        return ([item], None, 0)
    return None


def probe_env(builder, classes):
    env = {}
    for t in classes:
        env[E.llt_name(t)] = t
    return env


def safe_eval(node, env):
    try:
        return E.eval_expr(node, env)
    except E.ExprError:
        return None


# --- top level -------------------------------------------------------------

def build_points(result, tier):
    classes = sorted_llts(result.classes)
    mem_classes = sorted_llts(result.mem_classes)
    if tier == "witness":
        panel = BOUNDARY_PANEL
        mode = "witness"
    elif tier == "deep":
        panel = DEEP_PANEL
        mode = "full"
    elif tier == "full":
        panel = BOUNDARY_PANEL
        mode = "full"
    else:
        raise ValueError(f"unknown tier {tier}")

    points = []
    pruned = 0
    for builder in result.builders:
        if tier in ("full", "deep"):
            built, skipped = class_pair(builder, classes, mem_classes,
                                        result.aligns)
            points.extend(built)
            pruned += skipped
        built, skipped = boundary(builder, classes, panel, mode)
        points.extend(built)
        pruned += skipped
        points.extend(address_space(builder, classes))
        points.extend(opcode_probes(builder, classes, mem_classes))
        points.extend(rule_probes(builder, classes, mem_classes))

    points.sort(key=point_sort_key)
    counters = {}
    for p in points:
        key = (p["origin"], p["rule_set"])
        counters[key] = counters.get(key, 0) + 1
        p["id"] = (f"cpp/{p['rule_set']}/{p['opcode']}/{p['origin']}/"
                   f"{counters[key] - 1}")
    return points, classes, mem_classes, panel, pruned


def point_sort_key(p):
    return (
        p["origin"],
        p["rule_set"],
        p["opcode"],
        -1 if p["rule_index"] is None else p["rule_index"],
        tuple(p["types"]),
        p["mem"] or "",
        p["align"],
    )
