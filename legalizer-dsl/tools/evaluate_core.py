"""Independent evaluator for Core IR.

Given a Core IR document and a query, compute the observation

    Obs = (action, type_idx, new_type)

exactly as the charter defines it (LEGALIZER-SPEC.md 2.1), without looking at
the emitter or at any generated C++.  This is the judge side of edge T1/T3.

Rules are matched in document order (first match wins).  A rule set whose
config_guard is false for the current configuration is skipped.  A query whose
types/imms/mems are outside the declared domain yields OutOfDomain.

Deliberately no defaults:
  * an unknown predicate leaf, an unknown mutation value, or a feature with no
    value in the oracle raises instead of guessing;
  * an action that cannot carry a mutation, or a mutation whose target is not
    the rule's type_idx, raises.

Usage:
  python tools/evaluate_core.py --cases tests/core-tables/first-match.json
  python tools/evaluate_core.py --doc data/x86-core.json \
      --q0 --features tests/features/x86-generic.json
"""
import glob
import io
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
if HERE not in sys.path:
    sys.path.insert(0, HERE)

LLT_RE = re.compile(r"^(?:(v)(\d+))?(?:s(\d+)|p(\d+):(\d+))$")
DERIVED_RE = re.compile(r"^(nextPow2|nextPow2OrMin(\d+)|sameAsSlot(\d+)|elemOfSlot(\d+))$")

NO_MUTATION_ACTIONS = {"Legal", "Lower", "Libcall", "Custom", "Unsupported"}
MUTATION_KINDS = {"set_type", "set_elem_type", "set_elem_bits", "set_lanes",
                  "copy_type", "concat_bitcast"}


class EvalError(Exception):
    def __init__(self, code, detail):
        Exception.__init__(self, "%s: %s" % (code, detail))
        self.code = code
        self.detail = detail


# --------------------------------------------------------------------------
# LLT
# --------------------------------------------------------------------------

def parse_llt(text):
    m = LLT_RE.match(text)
    if not m:
        raise EvalError("bad_llt", "cannot parse LLT %r" % (text,))
    vec, lanes, scal, addr, width = m.groups()
    if scal is not None:
        elem = {"kind": "scalar", "bits": int(scal)}
    else:
        elem = {"kind": "pointer", "addr": int(addr), "bits": int(width)}
    if vec is None:
        return elem
    return {"kind": "vector", "lanes": int(lanes), "elem": elem}


def fmt_llt(ty):
    if ty["kind"] == "scalar":
        return "s%d" % ty["bits"]
    if ty["kind"] == "pointer":
        return "p%d:%d" % (ty["addr"], ty["bits"])
    return "v%d%s" % (ty["lanes"], fmt_llt(ty["elem"]))


def elem_of(ty):
    return ty["elem"] if ty["kind"] == "vector" else ty


def lanes_of(ty):
    return ty["lanes"] if ty["kind"] == "vector" else 1


def scalar_bits(ty):
    e = elem_of(ty)
    return e["bits"]


def size_bits(ty):
    """LLT::getSizeInBits: lanes * scalar size for vectors, else the width."""
    if ty["kind"] == "vector":
        return ty["lanes"] * ty["elem"]["bits"]
    return ty["bits"]


def with_elem_bits(ty, bits):
    e = elem_of(ty)
    if e["kind"] == "pointer":
        e = {"kind": "scalar", "bits": bits}
    else:
        e = {"kind": "scalar", "bits": bits}
    return make_vector(e, lanes_of(ty))


def make_vector(elem, lanes):
    if lanes == 1:
        return elem
    return {"kind": "vector", "lanes": lanes, "elem": elem}


def is_pow2(n):
    return n > 0 and (n & (n - 1)) == 0


def next_pow2(n):
    p = 1
    while p < n:
        p *= 2
    return p


# --------------------------------------------------------------------------
# Context
# --------------------------------------------------------------------------

class Context(object):
    def __init__(self, doc, features=None):
        self.doc = doc
        self.features = features or {}
        self.domain = doc.get("domain", {})
        self.domain_types = set(self.domain.get("types", []))
        self.domain_imms = set(self.domain.get("imms", []))
        self.domain_mems = set(
            (m["size_bits"], m["align_bits"]) for m in self.domain.get("mems", []))
        # A dump that can answer typeDiffersFromMemDesc carries the memory type
        # itself, not just its width: v2s32 and s64 are both 64 bits.
        self.domain_mem_types = set(
            (m.get("type"), m["size_bits"], m["align_bits"])
            for m in self.domain.get("mems", []) if "type" in m)


def check_domain(ctx, query):
    for ty in query["types"]:
        if ty not in ctx.domain_types:
            return "type %r is outside domain.types" % (ty,)
    for imm in query.get("imms", []):
        if imm not in ctx.domain_imms:
            return "imm %r is outside domain.imms" % (imm,)
    for mem in query.get("mems", []):
        if "type" in mem:
            key = (mem.get("type"), mem.get("size_bits"), mem.get("align_bits"))
            if key not in ctx.domain_mem_types:
                return "mem %r is outside domain.mems" % (key,)
        else:
            key = (mem.get("size_bits"), mem.get("align_bits"))
            if key not in ctx.domain_mems:
                return "mem %r is outside domain.mems" % (key,)
    return None


# --------------------------------------------------------------------------
# Predicates
# --------------------------------------------------------------------------

def arg(query, idx, what):
    types = query["types"]
    if not isinstance(idx, int) or idx < 0 or idx >= len(types):
        raise EvalError("bad_type_idx", "%s refers to type slot %r, query has %d"
                        % (what, idx, len(types)))
    return types[idx]


def _balanced(text):
    depth = 0
    for c in text:
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
            if depth < 0:
                return False
    return depth == 0


def eval_width(a, ctx, name):
    """A width is either a literal or a C++ expression over the same feature
    names the config records.

    `scalarNarrowerThan(1, Is64Bit ? 64 : 32)` cannot be folded when the rule is
    written: which number it is depends on the subtarget.  The Core IR keeps the
    expression, so the evaluator has to read it the same way it reads a feature
    leaf -- through the configuration -- rather than refuse the rule or guess a
    constant.
    """
    if isinstance(a, int):
        return a
    if not isinstance(a, str):
        raise EvalError("bad_width", "%s width must be an int or a string" % (name,))
    expr = a.strip()
    while len(expr) > 1 and expr[0] == "(" and expr[-1] == ")" and _balanced(expr[1:-1]):
        expr = expr[1:-1].strip()
    if expr.isdigit():
        return int(expr)
    # Both separators have to be read at depth zero.  A branch is itself a
    # width, and X86 writes "Is64Bit ? (HasAVX512 ? 8 : (HasAVX ? 4 : (2)))
    # : (HasAVX512 ? 16 : ...)", where the first ":" belongs to the then
    # branch.  Splitting on the first colon made the condition "(HasAVX512",
    # which is not a feature: the point was reported as an unknown feature
    # instead of being evaluated.
    parts = _split_top(expr, "?")
    if len(parts) > 1:
        cond = parts[0]
        rest = "?".join(parts[1:])
        arms = _split_top(rest, ":")
        if len(arms) < 2:
            raise EvalError("bad_width",
                            "%s width %r has '?' without ':'" % (name, expr))
        # A branch is itself a width: X86 writes
        # "HasBWI ? 64 : (HasAVX2 ? 32 : 16)", and reading the else branch as a
        # number left the whole clamp rule unevaluable -- which looked like an
        # unknown leaf further down instead of a parse failure here.
        chosen = (arms[0].strip() if eval_feature_expr(cond.strip(), ctx)
                  else ":".join(arms[1:]).strip())
        return eval_width(chosen, ctx, name)
    if expr in ctx.features:
        # A bare feature name is a bool, not a width; say so rather than
        # comparing against a string somewhere further down.
        raise EvalError("bad_width", "%s width %r is a feature, not a number"
                        % (name, expr))
    raise EvalError("bad_width", "%s width %r is not a number" % (name, expr))


def eval_feature_expr(expr, ctx):
    """Evaluate a boolean expression over the recorded features."""
    text = expr.strip()
    for op in ("||", "&&"):
        if op in text:
            parts = _split_top(text, op)
            if len(parts) > 1:
                vals = [eval_feature_expr(p, ctx) for p in parts]
                return any(vals) if op == "||" else all(vals)
    if text.startswith("!") and "=" not in text:
        return not eval_feature_expr(text[1:], ctx)
    if text.startswith("(") and text.endswith(")"):
        return eval_feature_expr(text[1:-1], ctx)
    if text in ctx.features:
        return bool(ctx.features[text])
    raise EvalError("unknown_feature", "feature %r is not in the configuration"
                    % (text,))


def _split_top(text, op):
    """Split on an operator that is not inside parentheses."""
    parts, depth, cur, i = [], 0, [], 0
    while i < len(text):
        c = text[i]
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        if depth == 0 and text.startswith(op, i):
            parts.append("".join(cur))
            cur = []
            i += len(op)
            continue
        cur.append(c)
        i += 1
    parts.append("".join(cur))
    return parts


def eval_leaf(pred, query, ctx):
    name = pred["name"]
    a = pred.get("args", [])

    if name == "true":
        return True

    if name == "typeIs":
        if len(a) != 2:
            raise EvalError("bad_arity", "typeIs takes (type_idx, llt), got %d args" % len(a))
        return arg(query, a[0], "typeIs") == a[1]

    if name == "isScalar":
        if len(a) != 1:
            raise EvalError("bad_arity", "isScalar takes (type_idx)")
        return parse_llt(arg(query, a[0], name))["kind"] == "scalar"

    if name == "isVector":
        if len(a) != 1:
            raise EvalError("bad_arity", "isVector takes (type_idx)")
        return parse_llt(arg(query, a[0], name))["kind"] == "vector"

    if name == "isFixedVector":
        if len(a) != 1:
            raise EvalError("bad_arity", "isFixedVector takes (type_idx)")
        return parse_llt(arg(query, a[0], name))["kind"] == "vector"

    if name == "sizeNotPow2":
        if len(a) != 1:
            raise EvalError("bad_arity", "sizeNotPow2 takes (type_idx)")
        ty = parse_llt(arg(query, a[0], name))
        return ty["kind"] == "scalar" and not is_pow2(ty["bits"])

    if name == "scalarOrEltSizeNotPow2":
        if len(a) != 1:
            raise EvalError("bad_arity", "scalarOrEltSizeNotPow2 takes (type_idx)")
        return not is_pow2(scalar_bits(parse_llt(arg(query, a[0], name))))

    if name == "numElementsNotPow2":
        if len(a) != 1:
            raise EvalError("bad_arity", "numElementsNotPow2 takes (type_idx)")
        ty = parse_llt(arg(query, a[0], name))
        return ty["kind"] == "vector" and not is_pow2(ty["lanes"])

    if name == "elementTypeIs":
        if len(a) != 2:
            raise EvalError("bad_arity", "elementTypeIs takes (type_idx, llt)")
        ty = parse_llt(arg(query, a[0], name))
        return ty["kind"] == "vector" and fmt_llt(ty["elem"]) == a[1]

    if name == "typeIn":
        if len(a) != 2:
            raise EvalError("bad_arity", "typeIn takes (type_idx, [llt...])")
        return arg(query, a[0], name) in a[1]

    if name == "sizeIs":
        if len(a) != 2:
            raise EvalError("bad_arity", "sizeIs takes (type_idx, bits)")
        # LLT::getSizeInBits, not the scalar size: v4s32 and s128 both answer
        # 128.  LegalityPredicates::sizeIs is the C++ being modelled.
        return size_bits(parse_llt(arg(query, a[0], name))) == eval_width(
            a[1], ctx, name)

    if name == "scalarNarrowerThan":
        if len(a) != 2:
            raise EvalError("bad_arity", "scalarNarrowerThan takes (type_idx, bits)")
        ty = parse_llt(arg(query, a[0], name))
        return ty["kind"] == "scalar" and ty["bits"] < eval_width(a[1], ctx, name)

    if name == "scalarWiderThan":
        if len(a) != 2:
            raise EvalError("bad_arity", "scalarWiderThan takes (type_idx, bits)")
        ty = parse_llt(arg(query, a[0], name))
        return ty["kind"] == "scalar" and ty["bits"] > eval_width(a[1], ctx, name)

    if name == "scalarOrEltWiderThan":
        if len(a) != 2:
            raise EvalError("bad_arity",
                            "scalarOrEltWiderThan takes (type_idx, bits)")
        # LLT::getScalarSizeInBits: the element size for a vector, the pointer
        # width for a pointer.  Deliberately not size_bits -- a v4s32 is 128
        # bits wide and its element is 32, and this leaf asks the second.
        return scalar_bits(parse_llt(arg(query, a[0], name))) > eval_width(
            a[1], ctx, name)

    if name == "fewerElementsThan":
        if len(a) != 2:
            raise EvalError("bad_arity", "fewerElementsThan takes (type_idx, lanes)")
        ty = parse_llt(arg(query, a[0], name))
        return ty["kind"] == "vector" and ty["lanes"] < eval_width(a[1], ctx, name)

    if name == "moreElementsThan":
        if len(a) != 2:
            raise EvalError("bad_arity", "moreElementsThan takes (type_idx, lanes)")
        ty = parse_llt(arg(query, a[0], name))
        return ty["kind"] == "vector" and ty["lanes"] > eval_width(a[1], ctx, name)

    if name == "scalarSizeLargerThanSlot":
        if len(a) != 2:
            raise EvalError("bad_arity", "scalarSizeLargerThanSlot takes (other, type_idx)")
        return (scalar_bits(parse_llt(arg(query, a[0], name))) >
                size_bits(parse_llt(arg(query, a[1], name))))

    if name == "scalarSizeSmallerThanSlot":
        if len(a) != 2:
            raise EvalError("bad_arity", "scalarSizeSmallerThanSlot takes (other, type_idx)")
        return (scalar_bits(parse_llt(arg(query, a[0], name))) <
                size_bits(parse_llt(arg(query, a[1], name))))

    if name == "typeDiffersFromMemDesc":
        if len(a) != 2:
            raise EvalError(
                "bad_arity",
                "typeDiffersFromMemDesc takes (type_idx, mem_idx), got %d args" % len(a))
        mems = query.get("mems", [])
        if not isinstance(a[1], int) or a[1] < 0 or a[1] >= len(mems):
            raise EvalError("bad_mem_idx",
                            "typeDiffersFromMemDesc refers to mem slot %r, query has %d"
                            % (a[1], len(mems)))
        if "type" not in mems[a[1]]:
            # Refuse rather than compare against size_bits: the whole point of
            # the leaf is that two types of the same width can still differ.
            raise EvalError("bad_mem_type",
                            "mem slot %d has no type; the leaf compares the whole LLT"
                            % a[1])
        return arg(query, a[0], name) != mems[a[1]]["type"]

    if name == "memSize":
        if len(a) != 3:
            raise EvalError("bad_arity",
                            "memSize takes (mem_idx, size_bits, min_align_bits)")
        mems = query.get("mems", [])
        if not isinstance(a[0], int) or a[0] < 0 or a[0] >= len(mems):
            raise EvalError("bad_mem_idx", "memSize refers to mem slot %r, query has %d"
                            % (a[0], len(mems)))
        # TypePairAndMemDesc::isCompatible compares the memory type by size and
        # treats the rule's alignment as a lower bound (LegalizerInfo.h, and
        # LegalityPredicates::typePairAndMemDescInSet).  A stricter, equality
        # reading would report divergence on every access that is aligned more
        # than the rule requires.
        return (mems[a[0]]["size_bits"] == a[1] and
                mems[a[0]]["align_bits"] >= a[2])

    if name == "hasFeature":
        if len(a) != 1:
            raise EvalError("bad_arity", "hasFeature takes one expression")
        if a[0] in ctx.features:
            return bool(ctx.features[a[0]])
        # The oracle lists the guards the .td uses, but not every guard it can
        # produce: splitting a conditional into one rule per arm turns
        # `Cond ? A : B` into the guard `Cond`, and Cond may itself be a
        # compound ("!useSoftFloat() && hasX87() && !hasSSE1()") that nobody
        # wrote down.  Compose it from the atoms the oracle does record -- the
        # same reading eval_width already does -- rather than refusing a guard
        # whose parts are all known.  An atom the file lacks still refuses.
        return eval_feature_expr(a[0], ctx)

    if name == "opaque":
        raise EvalError("opaque_predicate",
                        "predicate %r is raw C++ and has no Core IR meaning" % (a[0] if a else name,))

    raise EvalError("unknown_leaf", "predicate leaf %r is not in the vocabulary" % (name,))


def eval_pred(pred, query, ctx):
    kind = pred.get("kind")
    if kind == "leaf":
        return eval_leaf(pred, query, ctx)
    if kind == "nary":
        op = pred.get("op")
        vals = [eval_pred(p, query, ctx) for p in pred.get("args", [])]
        if op == "all_of":
            return all(vals)
        if op == "any_of":
            return any(vals)
        if op == "none_of":
            return not any(vals)
        raise EvalError("unknown_nary_op", "nary operator %r" % (op,))
    if kind == "not":
        return not eval_pred(pred["arg"], query, ctx)
    raise EvalError("unknown_predicate_kind", "predicate kind %r" % (kind,))


# --------------------------------------------------------------------------
# Mutations
# --------------------------------------------------------------------------

def derived_value(text, query, base):
    """`base` is the quantity the mutation changes: element bits for
    set_elem_bits, lane count for set_lanes.  nextPow2 rounds that, not
    whatever happens to be convenient."""
    m = DERIVED_RE.match(text)
    if not m:
        raise EvalError("unknown_mutation_value",
                        "%r is neither an LLT nor a registered derived value" % (text,))
    head = m.group(1)
    if head == "nextPow2":
        return next_pow2(base)
    if head.startswith("nextPow2OrMin"):
        return max(next_pow2(base), int(m.group(2)))
    if head.startswith("sameAsSlot"):
        return scalar_bits(parse_llt(query["types"][int(m.group(3))]))
    raise EvalError("unsupported_derived_value",
                    "%r is registered but not implemented for this mutation kind" % (text,))


def apply_mutation(rule, query, ctx=None):
    mut = rule.get("mutation")
    if mut is None:
        # C++ determineMutation() returns (0, LLT{}) when the rule has no
        # mutation.  The charter (LEGALIZER-SPEC 2.1, D-20261028-02) writes
        # that as "the original type" so that Legal and mutation=null cannot
        # be spelled two ways.  A zero-type opcode has no original type, and
        # that is the one case whose answer is invalid rather than a type.
        idx = rule["type_idx"]
        if idx >= len(query["types"]):
            return None
        return fmt_llt(parse_llt(query["types"][idx]))

    idx = rule["type_idx"]
    if idx >= len(query["types"]):
        raise EvalError("bad_type_idx",
                        "mutation reads type slot %d, query has %d types"
                        % (idx, len(query["types"])))
    ty = parse_llt(query["types"][idx])

    if mut["target"] != rule["type_idx"]:
        raise EvalError("mutation_target_mismatch",
                        "mutation.target %r != type_idx %r"
                        % (mut["target"], rule["type_idx"]))

    kind = mut["kind"]
    value = mut["value"]

    if kind not in MUTATION_KINDS:
        raise EvalError("unknown_mutation_kind", "%r" % (kind,))
    if kind == "concat_bitcast":
        raise EvalError("unimplemented_mutation",
                        "concat_bitcast has no registered composed type")

    if kind == "set_type":
        if not isinstance(value, str):
            raise EvalError("bad_mutation_value", "set_type needs an LLT, got %r" % (value,))
        return fmt_llt(parse_llt(value))

    if kind == "set_elem_type":
        if not isinstance(value, str):
            raise EvalError("bad_mutation_value", "set_elem_type needs an LLT, got %r" % (value,))
        return fmt_llt(make_vector(parse_llt(value), lanes_of(ty)))

    if kind == "set_elem_bits":
        bits = value if isinstance(value, int) else derived_value(
            value, query, scalar_bits(ty))
        return fmt_llt(with_elem_bits(ty, bits))

    if kind == "set_lanes":
        if isinstance(value, int):
            n = value
        elif DERIVED_RE.match(value):
            n = derived_value(value, query, lanes_of(ty))
        else:
            # An explicit lane count, possibly a subtarget conditional.  The
            # number is exact: clampMaxNumElements says "at most 16 lanes", so
            # the mutation is 16, not the next power of two.
            n = eval_width(value, ctx, "set_lanes")
        return fmt_llt(make_vector(elem_of(ty), n))

    if kind == "copy_type":
        return query["types"][value]

    raise EvalError("unknown_mutation_kind", "%r" % (kind,))


# --------------------------------------------------------------------------
# Evaluation
# --------------------------------------------------------------------------

def guard_holds(ruleset, query, ctx):
    guard = ruleset.get("config_guard")
    if guard is None:
        return True
    return eval_pred(guard, query, ctx)


def evaluate(ctx, query):
    """Return a result dict; raise EvalError on anything the spec forbids."""
    outside = check_domain(ctx, query)
    if outside:
        return {"result": "out_of_domain", "reason": outside}

    matching = [rs for rs in ctx.doc["rulesets"] if query["opcode"] in rs["opcodes"]]
    if not matching:
        return {"result": "no_ruleset", "action": "Unsupported", "type_idx": 0,
                "new_type": None}

    for ruleset in matching:
        if not guard_holds(ruleset, query, ctx):
            continue
        for index, rule in enumerate(ruleset["rules"]):
            # C++ LegalizeRuleSet::apply() calls Rule.match() first and only
            # then determineMutation(), so a rule whose predicate is false
            # never touches its type slot.  Deciding the slot is in range
            # before matching would reject a query the chain answers; a
            # zero-type opcode (G_INVOKE_REGION_START) is the witness.
            if not eval_pred(rule["predicate"], query, ctx):
                continue
            action = rule["action"]
            if action in NO_MUTATION_ACTIONS and rule.get("mutation") is not None:
                raise EvalError("action_carries_mutation",
                                "%s rule %d: %s must not carry a mutation"
                                % (ruleset.get("def"), index, action))
            # rule_index is the position in the source Rules list, which is
            # what escapes[] also records; a rule set where some rules escaped
            # has gaps, so the array position is not that number.  Hand-written
            # tables omit the field and fall back to the position.
            return {"result": "rule",
                    "ruleset": ruleset.get("def"),
                    "rule_index": rule.get("rule_index", index),
                    "action": action,
                    "type_idx": rule["type_idx"],
                    "new_type": apply_mutation(rule, query, ctx)}

    # Every ruleset for this opcode was guarded off, or nothing matched.  LLVM
    # returns (Unsupported, 0, LLT{}) for both; new_type stays null so this can
    # never be confused with a rule that says Unsupported on purpose.
    return {"result": "no_match", "action": "Unsupported", "type_idx": 0,
            "new_type": None}


# --------------------------------------------------------------------------
# CLI
# --------------------------------------------------------------------------

def load(path):
    with io.open(path, encoding="utf-8") as fh:
        return json.load(fh)


def load_features(path):
    """An oracle file is either a flat map or {"features": {...}}.

    The values are a claim about a configuration, not something this tool can
    derive: nothing here knows what Subtarget.hasAVX2() returns.
    """
    data = load(path)
    if "features" in data:
        return data["features"]
    return data


def validate_doc(doc):
    import validate_core
    with io.open(validate_core.SCHEMA_PATH, encoding="utf-8") as fh:
        schema = json.load(fh)
    errs = validate_core.Errors()
    validate_core.validate(doc, schema, schema, "", errs)
    validate_core.semantic(doc, errs, lenient=False)
    return errs.items


def run_cases(path, quiet=False):
    table = load(path)
    doc = table["doc"]
    if table.get("validate", True):
        problems = validate_doc(doc)
        if problems:
            print("FAIL %s: the embedded document violates the schema" % path)
            for where, message in problems[:5]:
                print("      %s: %s" % (where, message))
            return 1
    ctx = Context(doc, table.get("features"))
    failed = 0
    for case in table["cases"]:
        name = case["name"]
        try:
            got = evaluate(ctx, case["query"])
            problem = None
        except EvalError as exc:
            got = {"error": exc.code, "detail": exc.detail}
            problem = None
        expect = case["expect"]
        if "error" in expect:
            ok = got.get("error") == expect["error"]
        else:
            ok = got == expect
        if not ok:
            failed += 1
        print("%s %-42s %s" % ("PASS" if ok else "FAIL", name,
                               "" if ok else "\n      got    %s\n      expect %s"
                               % (json.dumps(got, sort_keys=True),
                                  json.dumps(expect, sort_keys=True))))
        if problem:
            print("      " + problem)
    total = len(table["cases"])
    if not quiet:
        print("%d/%d passed" % (total - failed, total))
    return 1 if failed else 0


def run_all(pattern):
    paths = sorted(glob.glob(pattern))
    if not paths:
        print("no tables matched %s" % pattern)
        return 1
    failed = 0
    for path in paths:
        name = os.path.basename(path)
        print("== %s" % name)
        if run_cases(path):
            failed += 1
    print("%d/%d tables passed" % (len(paths) - failed, len(paths)))
    return 1 if failed else 0


def collect_features(pred, out):
    kind = pred.get("kind")
    if kind == "leaf":
        if pred["name"] == "hasFeature" and pred.get("args"):
            out.add(pred["args"][0])
    elif kind == "nary":
        for p in pred.get("args", []):
            collect_features(p, out)
    elif kind == "not":
        collect_features(pred["arg"], out)


def collect_mem_leaves(pred, out):
    kind = pred.get("kind")
    if kind == "leaf":
        args = pred.get("args", [])
        if pred["name"] == "memSize" and args:
            out.add(args[0])
        elif pred["name"] == "typeDiffersFromMemDesc" and len(args) == 2:
            out.add(args[1])
    elif kind == "nary":
        for p in pred.get("args", []):
            collect_mem_leaves(p, out)
    elif kind == "not":
        collect_mem_leaves(pred["arg"], out)


def collect_typeis(pred, out):
    kind = pred.get("kind")
    if kind == "leaf":
        if pred["name"] == "typeIs" and len(pred.get("args", [])) == 2:
            out[pred["args"][0]] = pred["args"][1]
    elif kind == "nary":
        for p in pred.get("args", []):
            collect_typeis(p, out)
    elif kind == "not":
        collect_typeis(pred["arg"], out)


def run_smoke(doc_path):
    """Feed every rule a query built from its own typeIs leaves.

    This is not evidence about the target.  It is a coverage check: it drives
    the evaluator over every rule in a real dump and reports which rules it
    cannot interpret, grouped by reason.  Features default to true, which is a
    stand-in, not a claim about any configuration.
    """
    doc = load(doc_path)
    features = set()
    for rs in doc["rulesets"]:
        if rs.get("config_guard"):
            collect_features(rs["config_guard"], features)
        for rule in rs["rules"]:
            collect_features(rule["predicate"], features)
    ctx = Context(doc, dict((f, True) for f in features))
    fallback = doc["domain"]["types"][0] if doc["domain"]["types"] else "s32"

    ok = 0
    errors = {}
    unreachable = 0
    for rs in doc["rulesets"]:
        for rule in rs["rules"]:
            slots = {}
            collect_typeis(rule["predicate"], slots)
            if not slots:
                unreachable += 1
                continue
            width = max(slots) + 1
            types = [slots.get(i, fallback) for i in range(width)]
            query = {"opcode": rs["opcodes"][0], "types": types}
            mem_slots = set()
            collect_mem_leaves(rule["predicate"], mem_slots)
            if mem_slots:
                # The smoke query has to carry the axis the rule reads, or the
                # leaf is unanswerable.  A declared domain point is enough: this
                # is a coverage check, not a claim about any instruction.
                mems = doc["domain"].get("mems", [])
                if not mems:
                    errors.setdefault("bad_mem_idx", []).append(
                        (rs.get("def"), "rule reads a memdesc, domain has no mems"))
                    continue
                query["mems"] = [dict(mems[0])]
            try:
                evaluate(ctx, query)
                ok += 1
            except EvalError as exc:
                errors.setdefault(exc.code, []).append((rs.get("def"), exc.detail))

    rules = sum(len(rs["rules"]) for rs in doc["rulesets"])
    print("rules in dump:      %d" % rules)
    print("evaluated:          %d" % ok)
    print("no typeIs in pred:  %d" % unreachable)
    print("errors:             %d" % sum(len(v) for v in errors.values()))
    for code, where in sorted(errors.items()):
        print("  %-28s %d" % (code, len(where)))
        for name, detail in where[:3]:
            print("      %s: %s" % (name, detail))
    return 1 if errors else 0


def run_q0(doc_path, features_path):
    doc = load(doc_path)
    features = load_features(features_path) if features_path else {}
    ctx = Context(doc, features)
    points = doc.get("domain", {}).get("q0", [])
    if not points:
        print("no q0 points in %s" % doc_path)
        return 1
    for query in points:
        try:
            print(json.dumps(evaluate(ctx, query), sort_keys=True))
        except EvalError as exc:
            print(json.dumps({"error": exc.code, "detail": exc.detail}, sort_keys=True))
    return 0


def main(argv):
    args = argv[1:]
    if not args or args[0] in ("-h", "--help"):
        print(__doc__)
        return 2
    if args[0] == "--cases":
        return run_cases(args[1])
    if args[0] == "--smoke":
        return run_smoke(args[1])
    if args[0] == "--all":
        pattern = args[1] if len(args) > 1 else os.path.join(
            os.path.dirname(HERE), "tests", "core-tables", "*.json")
        return run_all(pattern)
    if args[0] == "--doc":
        rest = args[2:]
        features = None
        if "--features" in rest:
            features = rest[rest.index("--features") + 1]
        if "--q0" in rest:
            return run_q0(args[1], features)
        if "--query" in rest:
            query = json.loads(rest[rest.index("--query") + 1])
            doc = load(args[1])
            ctx = Context(doc, load_features(features) if features else {})
            try:
                print(json.dumps(evaluate(ctx, query), sort_keys=True))
            except EvalError as exc:
                print(json.dumps({"error": exc.code, "detail": exc.detail}, sort_keys=True))
            return 0
        print("nothing to do: pass --q0 or --query")
        return 2
    print(__doc__)
    return 2


if __name__ == "__main__":
    sys.exit(main(sys.argv))