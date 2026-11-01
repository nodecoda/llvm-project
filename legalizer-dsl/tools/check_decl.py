#!/usr/bin/env python3
"""semantics section 10 gate, run on a .decl that bison already accepted.

  syntax  tools/lint_decl.py   (grammar: 2026-11-01-legalize-decl.v6.y)
  shapes  tools/decl_lib.py    (one copy of the shape/domain reading)
  meaning this file            (never re-implements the grammar)

Checks, per section 10:
  1 sort         `ty(0) < s32` is refused: the order is not on types
  2 unit         `legal _` is the empty set
  3 bounded      `-` / `never` need a finite domain, and v6 has no domain decl
  4 prefer guard a non-pow2 prefer without `when` is worth a second look
  5 domain       D_i != empty, per feature assignment (prefer may supply D_i)
  6 prefer target resolves to a type in the universe; a target outside the
                 legal set of that assignment is a *transient* (the next
                 iteration must legalize it), reported as a warning
  7 diagonal     a 2-slot block whose legal points are not the full product
                 needs `follow`
  8 mem position mem(...) only in the tail of a position tuple
  9 convergence  replay `step` (the same function tools/infer_diff.py
                 measures) from a uniform query of every type in U, per
                 assignment; a run that cycles or has not landed in 8 steps is
                 an error (this is the "the chain must be a fixed point" check)

Check 6 used to demand target in D_i as an *error*.  That is wrong: clamp
rules on X86 deliberately widen to a type that is not legal in feature-off
configurations (G_ADD's `clampMinNumElements(0, s8, 16)` runs with HasSSE2
off, G_SELECT's `clampScalar(0, HasCMOV ? s16 : s8, ...)` runs on s8 with
HasCMOV on).  Those targets are legalized by the next iteration, so they are
transients, and check 9 is what must hold them accountable.

Usage:
  python3 tools/check_decl.py 2026-11-01-x86.decl
  python3 tools/check_decl.py --domain 2026-11-01-x86.decl
  python3 tools/check_decl.py --self-test
"""
import itertools
import json
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import decl_lib as D

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAX_COMBOS = 4096
STEPS = 8


# ---------- numeric predicates on a bound query type ------------------------
def lanes(t):
    m = re.fullmatch(r'v(\d+)(?:s\d+|p\d+:\d+)', t)
    return int(m.group(1)) if m else 1


def is_scalar(t):
    return D.bits(t) >= 0 and not t.startswith('v')


def next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


def size_not_pow2(t):
    if not is_scalar(t):
        return False
    n = D.bits(t)
    return n > 0 and (n & (n - 1)) != 0


def num_or_type(text, env, macros, mems=None):
    """`32`, `s64`, `MaxScalar`, `bits(MaxScalar)` -> int.  None if it does not fold."""
    text = text.strip()
    while text.startswith('(') and text.endswith(')'):
        text = text[1:-1].strip()
    m = re.fullmatch(r'bits\((.*)\)', text)
    if m:
        return num_or_type(m.group(1), env, macros, mems)
    m = re.fullmatch(r'mem\((\d+)\)\.ty', text)
    if m:
        k = int(m.group(1))
        if mems is None or k >= len(mems):
            return None
        return D.bits(mems[k]['type'])
    m = re.fullmatch(r'lanes\((.*)\)', text)
    if m:
        t = D.eval_cond(m.group(1), env, macros)
        return lanes(t) if t else None
    if re.fullmatch(r'\d+', text):
        return int(text)
    # `cond(C, 64, 32)` folds to an *integer*, and D.eval_cond only returns
    # types (its last line is `return t if TY.fullmatch(t)`), so it gave None
    # for both arms.  X86_UITOFP's escape guard is
    # `bits(ty(1)) < cond(Is64Bit, 64, 32)` and was therefore never decidable.
    m = re.fullmatch(r'cond\((.*)\)', text)
    if m:
        parts = D.split_top(m.group(1), ',')
        if len(parts) == 3:
            v = D.eval_bool(parts[0], env)
            if v is None:
                return None
            return num_or_type(parts[1] if v else parts[2], env, macros)
        return None
    t = D.eval_cond(text, env, macros)
    return D.bits(t) if t else None


def eval_conj(c, env, tmap, macros, mems=None):
    """One conjunct -> True/False/None (None = cannot decide here).

    `mems` is the query's memdesc list, positionally `mem(0), mem(1), ...`.
    Without it every access-shape predicate is undecidable, which is how the
    six memdesc blocks stayed outside the differential for so long.
    """
    c = c.strip().strip('&').strip()
    if not c:
        return True
    if c.startswith('!'):
        v = eval_conj(c[1:], env, tmap, macros, mems)
        return None if v is None else (not v)
    if c == 'pow2':
        # In a prefer guard this is the .td predicate `sizeNotPow2` /
        # `scalarOrEltSizeNotPow2`, not a construction-time feature:
        # widenScalar{,OrElt}ToNextPow2 fires iff the (element) size is not a
        # power of two.
        if len(tmap) == 1:
            # `pow2` = the size IS a power of two, so `!pow2` is the .td
            # predicate sizeNotPow2.
            return not size_not_pow2(D.elem(next(iter(tmap.values()))))
        return None
    m = re.fullmatch(r'bits\((elem\()?ty\((\d+)\)\)?\)\s*(<=|>=|<|>|==|=)\s*(.+)', c)
    if m and int(m.group(2)) in tmap:
        n = num_or_type(m.group(4), env, macros, mems)
        if n is None:
            return None
        t = tmap[int(m.group(2))]
        a = D.bits(D.elem(t) if m.group(1) else t)
        op = m.group(3)
        return {'<': a < n, '>': a > n, '<=': a <= n, '>=': a >= n,
                '==': a == n, '=': a == n}[op]
    m = re.fullmatch(r'kind\(ty\((\d+)\)\)\s*=\s*(\w+)', c)
    if m and int(m.group(1)) in tmap:
        return D.kind(tmap[int(m.group(1))]) == m.group(2)
    m = re.fullmatch(r'elem\(ty\((\d+)\)\)\s*=\s*(.+)', c)
    if m and int(m.group(1)) in tmap:
        t = D.eval_cond(m.group(2), env, macros)
        return None if t is None else (D.elem(tmap[int(m.group(1))]) == t)
    m = re.fullmatch(r'lanes\(ty\((\d+)\)\)\s*(<=|>=|<|>|==|=)\s*(.+)', c)
    if m and int(m.group(1)) in tmap:
        n = num_or_type(m.group(3), env, macros)
        if n is None:
            n = None
            mt = re.fullmatch(r'lanes\((.+)\)', m.group(3))
            if mt:
                tt = D.eval_cond(mt.group(1), env, macros)
                n = lanes(tt) if tt else None
        if n is None:
            return None
        a = lanes(tmap[int(m.group(1))])
        return {'<': a < n, '>': a > n, '<=': a <= n, '>=': a >= n,
                '==': a == n, '=': a == n}[m.group(2)]
    m = re.fullmatch(r'bits\(mem\((\d+)\)\.ty\)\s*(<=|>=|<|>|==|=)\s*(.+)', c)
    if m:
        k = int(m.group(1))
        if mems is None or k >= len(mems):
            return None
        n = num_or_type(m.group(3), env, macros)
        if n is None:
            return None
        a = D.bits(mems[k]['type'])
        return {'<': a < n, '>': a > n, '<=': a <= n, '>=': a >= n,
                '==': a == n, '=': a == n}[m.group(2)]
    m = re.fullmatch(r'ty\((\d+)\)\s*(=|!=)\s*mem\((\d+)\)\.ty', c)
    if m:
        i, k = int(m.group(1)), int(m.group(3))
        if i not in tmap or mems is None or k >= len(mems):
            return None
        same = tmap[i] == mems[k]['type']
        return same if m.group(2) == '=' else not same
    m = re.fullmatch(r'ty\((\d+)\)\s*=\s*(.+)', c)
    if m and int(m.group(1)) in tmap:
        t = D.eval_cond(m.group(2), env, macros)
        return None if t is None else (tmap[int(m.group(1))] == t)
    m = re.fullmatch(r'ty\((\d+)\)\s+in\s+(.+)', c)
    if m and int(m.group(1)) in tmap:
        return tmap[int(m.group(1))] in D.types_of(m.group(2))
    if not D.is_query_conj(c) and 'pow2' not in c and re.search(r'&&|\|\|', c):
        # `HasSSE1 || UseX87` is a plain boolean over construction-time
        # features; without this, the conjunct evaluated to "unknown" and the
        # block looked as if it had no legal types at all.
        v = D.eval_bool(c, env)
        if v is not None:
            return v
    if c == 'true':
        return True
    if c == 'false':
        return False
    if re.fullmatch(r'[A-Za-z_][A-Za-z0-9_]*', c) and c in env:
        return env[c]
    return None


def split_op(s, op):
    """Split on a two-char operator at paren depth 0."""
    out, d, cur, i = [], 0, '', 0
    while i < len(s):
        ch = s[i]
        if ch in '([':
            d += 1
        elif ch in ')]':
            d -= 1
        if d == 0 and s.startswith(op, i):
            out.append(cur)
            cur = ''
            i += len(op)
            continue
        cur += ch
        i += 1
    out.append(cur)
    return out


def eval_guard(guard, env, tmap, macros, mems=None):
    """Three-valued evaluation of a whole guard.

    This used to split on `&&` and stop there, so a guard of the form
    `(A && ty(0) in (...)) || (B && ty(0) in (...))` -- which is what
    G_BUILD_VECTOR's customIf becomes -- came back unknown, and
    except_action() read unknown as "fires" and answered Custom where the
    chain says Unsupported.  The grammar has `||`, `!` and parentheses, so the
    evaluator has to as well.
    """
    if not guard or not guard.strip():
        return True
    s = guard.strip()
    parts = split_op(s, '||')
    if len(parts) > 1:
        vals = [eval_guard(p, env, tmap, macros, mems) for p in parts]
        if any(v is True for v in vals):
            return True
        if all(v is False for v in vals):
            return False
        return None
    parts = split_op(s, '&&')
    if len(parts) > 1:
        vals = [eval_guard(p, env, tmap, macros, mems) for p in parts]
        if any(v is False for v in vals):
            return False
        if all(v is True for v in vals):
            return True
        return None
    if D.balanced(s):
        return eval_guard(s[1:-1], env, tmap, macros, mems)
    if s.startswith('!'):
        v = eval_guard(s[1:], env, tmap, macros, mems)
        return None if v is None else (not v)
    return eval_conj(s, env, tmap, macros, mems)


# ---------- the .decl side --------------------------------------------------
def type_slots_of(block, arity=0):
    """The `ty` coordinates of this block.

    With a `query`, the declaration is the answer -- deleting a rule must not
    shrink the query space.  Without one, v6's inference stands: any slot a
    clause or a `ty(i)` mentions.
    """
    q = query_of(block)
    if q is not None:
        return sorted({i for k, i in q if k == 'ty'})
    return list(range(slot_count(block, arity)))


def scope_of(block, idx):
    sc = block.get('scope') or []
    return sc[idx] if idx < len(sc) else None


def decl_slots(block, idx):
    """The slots a slot-scoped statement names, or None for the default rule.

    Three ways to name slots, in decreasing order of specificity:

      `policy 1: ...`        the statement says it
      `with 0, 1 { ... }`    the enclosing group says it
      `policy: ...`          nobody does -- it is the *default*

    A default is a fallback, not a broadcast: it applies to the slots that no
    qualified declaration covers.
    """
    st = block['stmts'][idx]
    m = (re.match(r'policy\s+(\d+)\s*:', st) or
         re.match(r'shapes\s+on\s+(\d+)', st) or
         re.search(r'\son\s+(\d+)\b', st))
    if m:
        return (int(m.group(1)),)
    sc = scope_of(block, idx)
    return tuple(sc[1]) if sc else None


def prefers_of(block):
    """-> [(slots|None, kind, target_text, guard)] in declaration order."""
    out = []
    for idx, st in enumerate(block['stmts']):
        if not st.startswith('prefer '):
            continue
        body = st[len('prefer '):]
        guard = ''
        if ' when ' in body:
            body, guard = body.split(' when ', 1)
        b2 = body.strip()
        ss = decl_slots(block, idx)
        m = re.fullmatch(r'pow2(?:\s+min\s+(\d+))?(?:\s+on\s+\d+)?', b2)
        if m:
            out.append((ss, 'pow2', m.group(1) or '0', guard.strip()))
            continue
        m = re.fullmatch(r'elem(?:\s+on\s+\d+)?', b2)
        if m:
            out.append((ss, 'elem', '', guard.strip()))
            continue
        m = re.fullmatch(r'(.+?)(?:\s+on\s+\d+)?', b2)
        if not m:
            continue
        out.append((ss, 'target', m.group(1).strip(), guard.strip()))
    return out


def expand_prefers(block, slots):
    """Flatten defaults: specific declarations first, then uncovered slots.

    A default never lands on a slot that has a specific declaration of the
    same kind.  The kind matters: `prefer pow2;` and `prefer s32 on 1;` are
    different rewrites and both stay live on slot 1.
    """
    raw = prefers_of(block)
    covered = {(s, kind) for ss, kind, _t, _g in raw if ss for s in ss}
    out = []
    for ss, kind, tgt, guard in raw:
        if ss:
            out.extend((s, kind, tgt, guard) for s in ss)
        else:
            out.extend((s, kind, tgt, guard) for s in slots
                       if (s, kind) not in covered)
    return out


NOT_A_FEATURE = {
    'cond', 'true', 'false', 'pow2', 'any', 'mem', 'bits', 'lanes', 'elem',
    'kind', 'addrspace', 'opc', 'ty', 'legal', 'prefer', 'follow', 'from',
    'by', 'when', 'except', 'on', 'in', 'and', 'not', 'min', 'max',
    'delegate', 'custom', 'lower', 'libcall', 'true', 'false',
    'scalar', 'vector', 'pointer', 'ptrvector', 'size', 'class',
    # Statement keywords are not features.  `elemwise` got missed here on the
    # first cut: it became a phantom feature atom, doubling every block's env
    # space (G_MUL went from 8 atoms to 10).  A new statement kind has to be
    # added to this set in the same change.
    'elemwise', 'shapes',
    # v7: same trap, same place.
    'policy', 'query', 'variadic', 'imm', 'to', 'hull', 'promote', 'truncate',
}


def feature_atoms_of(block, macros):
    """Construction-time features mentioned anywhere in the block.

    Not just the `when` guards: G_UITOFP's `HasSSE2` shows up only inside an
    `except` condition and inside a `cond(...)` target.  Missing it makes
    eval_cond return None and turns a real answer into "undecided"."""
    text = ' '.join(block['stmts']) + ' ' + ' '.join(macros.values())
    atoms = set()
    # `p0` is a prefix of `p0:64`, not a feature.
    prefixes = {t[:i] for t in D.U for i in range(1, len(t))} - set(D.U)
    for w in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', text):
        if (w in NOT_A_FEATURE or w in D.CLASSES or w in macros
                or D.TY.fullmatch(w) or w in prefixes):
            continue
        atoms.add(w)
    return sorted(atoms)


def domain(block, env=None):
    dom, arity = {}, 0
    for st in block['stmts']:
        if not st.startswith('legal '):
            continue
        shape, guard = D.clause_of(st)
        if env is not None and not D.feature_holds(guard, env):
            continue
        d, a = D.dom_of(shape, guard)
        arity = max(arity, a)
        for i, ts in d.items():
            dom.setdefault(i, set()).update(ts)
    return dom, arity


EXCEPT_ACTION = {'delegate(lower)': 'Lower', 'delegate(libcall)': 'Libcall',
                 'delegate(custom)': 'Custom', 'unsupported': 'Unsupported'}


def except_of(block):
    out = []
    for st in block['stmts']:
        if st.startswith('except ') and ' -> ' in st:
            g, a = st[len('except '):].split(' -> ', 1)
            out.append((g.strip(), a.strip().replace(' ', '')))
    return out


UNDECIDED = 'Undecided'


def except_action(block, env, tmap, macros, mems=None):
    """The `except` that fires for this query, or None.

    An unknown guard is NOT a firing guard.  Reading `None` as "fires" turned
    every guard the evaluator could not decide into a confident Custom, which
    is the wrong way to be wrong: it hides the gap instead of reporting it.
    """
    unknown = False
    for g, a in except_of(block):
        v = eval_guard(g, env, tmap, macros, mems)
        if v is True:
            return EXCEPT_ACTION.get(a, 'Custom')
        if v is None:
            unknown = True
    return UNDECIDED if unknown else None


def trivially_total(block):
    return any(D.clause_of(s) == ('any', '') for s in block['stmts']
               if s.startswith('legal '))


POLICY_RE = re.compile(r'policy\s+(\d+)\s*:\s*(scalar|vector)\s+(.+)$', re.S)
POLICY_DEF_RE = re.compile(r'policy\s*:\s*(scalar|vector)\s+(.+)$', re.S)


def _policy_payload(k, rest, env, macros):
    """Resolve one policy body into (shape, kind, pow2, floor, guard, lo, hi)."""
    guard = True
    gm = re.search(r'\s+when\s+(.+)$', rest)
    if gm:
        if env is not None:
            guard = D.feature_holds(gm.group(1), env)
        rest = rest[:gm.start()].strip()
    pow2, floor = False, None
    pm = re.search(r'\s+pow2(?:\s+(\d+))?$', rest)
    if pm:
        pow2 = True
        floor = int(pm.group(1)) if pm.group(1) else None
        rest = rest[:pm.start()].strip()

    def bound(x):
        # num_or_type() deliberately returns a *width* (its callers want bits);
        # a policy bound is a type, so resolve with eval_cond.
        if env is None or macros is None:
            return x
        return D.eval_cond(x, env, macros)

    if rest == 'hull':
        pol = ('hull', None, None)
    elif rest.startswith('to '):
        v = bound(rest[3:].strip())
        pol = ('both', v, v)
    elif rest.startswith('promote '):
        pol = ('lo', bound(rest[8:].strip()), None)
    elif rest.startswith('truncate '):
        pol = ('hi', None, bound(rest[9:].strip()))
    elif rest.startswith('[') and rest.endswith(']'):
        parts = D.split_top(rest[1:-1], ',')
        if len(parts) != 2:
            pol = ('bad', None, None)
        else:
            # `hull` is a value, not a mode: G_SELECT needs the lower end from
            # the legal set (s16 when CMOV is available) and the upper end from
            # a constant (sMaxScalar).
            ends = [('hull' if x.strip() == 'hull' else bound(x.strip()))
                    for x in parts]
            pol = ('both', ends[0], ends[1])
    else:
        pol = ('bad', None, None)
    return (pol[0], k, pow2, floor, guard, pol[1], pol[2])


def policies_of(block, env=None, macros=None, slots=None):
    """`policy i: ...` / `policy: ...` -> {slot: Policy}.

    A policy without a slot is the **default rule**: it fills every `ty`
    coordinate that no slot-qualified policy (`policy i:` or one inside a
    `with` group) covers.  It is a fallback, not a broadcast -- with
    `policy: A` and `policy 1: B`, slot 1 is B.

    Bounds are resolved to concrete types here, per feature assignment: the
    policy is a property of the target configuration, so `MaxScalar` must
    become s64 or s32 before the step function sees it.  An unresolvable bound
    is kept verbatim; check() reports it.
    """
    if slots is None:
        slots = type_slots_of(block)
    spec, dflt = {}, {}
    for idx, st in enumerate(block['stmts']):
        m = POLICY_RE.fullmatch(st)
        if m:
            targets = (int(m.group(1)),)
            k, rest = m.group(2), m.group(3)
        else:
            md = POLICY_DEF_RE.fullmatch(st)
            if not md:
                continue
            targets = decl_slots(block, idx)
            k, rest = md.group(1), md.group(2)
        payload = _policy_payload(k, rest.strip(), env, macros)
        if targets is None:
            for s in slots:
                dflt[s] = payload
        else:
            for s in targets:
                spec.setdefault(s, payload)
    out = dict(dflt)
    out.update(spec)
    return out


QUERY_RE = re.compile(r'query\s+(.+)$')

def query_of(block):
    """`query ty 0, ty 1, mem 0;` -> [('ty',0), ('ty',1), ('mem',0)] | None.

    This is the v7 shape declaration.  Without it a block keeps v6's inferred
    arity (whichever slot someone mentioned), which is exactly the pool that
    shrinks silently when a rule is deleted.
    """
    for st in block['stmts']:
        m = QUERY_RE.fullmatch(st)
        if not m:
            continue
        out = []
        for part in D.split_top(m.group(1), ','):
            mm = re.fullmatch(r'(ty|mem|imm)\s+(\d+)', part.strip())
            out.append((mm.group(1), int(mm.group(2))) if mm else ('bad', -1))
        return out
    return None


def follows_of(block):
    """`follow L from R by elem;` -> [(L, R, 'elem')] in declaration order."""
    out = []
    for st in block['stmts']:
        m = re.fullmatch(r'follow\s+(\d+)\s+from\s+(\d+)\s+by\s+(\w+)', st)
        if m:
            out.append((int(m.group(1)), int(m.group(2)), m.group(3)))
    return out


def slot_count(block, arity=0):
    """How many type slots a query for this block needs.

    v7: a `query` statement is the answer, and it is the whole point of the
    statement -- deleting a rule must not shrink the query space.  Without a
    query we fall back to v6's inference (the highest slot anyone mentioned),
    which is exactly the pool that shrinks when a rule is deleted.
    """
    q = query_of(block)
    if q is not None:
        type_slots = [i for k, i in q if k == 'ty']
        return 1 + max(type_slots) if type_slots else 0

    # v6 fallback: `D` only knows slots that a legal clause binds.
    # G_LROUND/G_LLROUND have no legal clause at all -- one `prefer ... on 1`
    # plus a libcall -- so the domain says 0 slots and the differential
    # generated 1-slot queries against a rule that reads slot 1.  The rules
    # are the ground truth here.
    n = arity
    # Any `ty(i)` anywhere -- a shape, a prefer guard, an escape guard.  G_IS_FPCLASS has no legal clause and no prefer at all, just three
    # `except ... (ty(0) = s1 && ty(1) = ...)` escapes; counting only legal
    # slots made it a 1-slot rule set and the judge answered bad_type_idx.
    for st in block['stmts']:
        for i in re.findall(r'ty\((\d+)\)', st):
            n = max(n, int(i) + 1)
    return n


def mem_comp_holds(comp, mem):
    """`mem(size, min_align)` or `mem(T)` against one memdesc (semantics 3.5).

    The align argument is a *lower bound*, like TypePairAndMemDesc::isCompatible
    and LegalityPredicates::typePairAndMemDescInSet: an access that is more
    aligned than the rule asks for still matches.
    """
    inner = comp[len('mem('):-1].strip()
    args = [a.strip() for a in inner.split(',')] if inner else []
    if len(args) == 2 and all(a.isdigit() for a in args):
        return (mem['size_bits'] == int(args[0]) and
                mem['align_bits'] >= int(args[1]))
    if len(args) == 1 and 'type' in mem:
        # semantics 3.6: `mem(T)` is a size match, not type equality.  The
        # corpus writes this form only in the position tests, so today it is
        # unobservable in the differential -- but the code should not state a
        # different rule than the document.
        return D.bits(mem['type']) == D.bits(args[0])
    return False


def legal_clause_holds(shape, guard, env, qtypes, macros, mems=None):
    """One `legal` clause against one query (semantics 3.1)."""
    if guard and eval_guard(guard, env, {i: t for i, t in enumerate(qtypes)},
                            macros, mems) is not True:
        return False
    for prod in D.products(shape):
        # `legal any` (the unit shape) constrains nothing, so it matches every
        # arity.  Reading it as "exactly one slot" made every `legal any when
        # <guard>` clause dead -- which is exactly how the six memdesc blocks
        # are written, so nothing noticed while they were skipped.
        if len(prod) == 1 and prod[0].strip() == 'any':
            return True
        ty_comps = [c for c in prod
                    if not c.startswith('mem(') and c not in ('_', '')]
        mem_comps = [c for c in prod if c.startswith('mem(')]
        # A product that binds fewer slots than the query constrains only a
        # prefix.  `legal s8, s32 when <feature>` is how the .td's
        # LegalFor<[s8, s32]> is written: two 1-slot points, i.e.
        # `typeIs(0, s8) || typeIs(0, s32)`, with slot 1 untouched --
        # G_FCMP's chain answers Legal(s8, s1) for exactly that reason.
        # Demanding a slot count equal to the arity made every such clause
        # dead, and cost 1372 disagreements on G_FCMP alone.
        if len(ty_comps) > len(qtypes):
            continue
        if not all(qtypes[i] in D.types_of(c) for i, c in enumerate(ty_comps)):
            continue
        if mem_comps:
            if mems is None or len(mem_comps) > len(mems):
                continue
            if not all(mem_comp_holds(c, mems[k])
                       for k, c in enumerate(mem_comps)):
                continue
        return True
    return False


def rules_of(block):
    """The block as an ordered rule list: ('legal'|'prefer'|'except', ...).

    The chain is ordered and `legal` is one kind of rule, not a separate gate.
    G_MERGE_VALUES puts its legalIf after two widen/min rules, so the chain
    rewrites (s16, s8) to (s32, s8) even though (s16, s8) satisfies the
    legalIf; a model that checks every legal clause first never sees that.

    `prefer`, `policy` and `shapes` without a slot are defaults.  They expand
    in place, and only onto slots that no slot-qualified declaration of the
    same kind covers -- "specific wins" is a property of the declaration, not
    of where it happens to sit in the chain.
    """
    slots = type_slots_of(block)
    prefs = prefers_of(block)
    covered = {(s, kind) for ss, kind, _t, _g in prefs if ss for s in ss}
    shapes_slots = {s for idx, st in enumerate(block['stmts'])
                    if st.startswith('shapes')
                    for s in (decl_slots(block, idx) or ())}
    out, pi = [], 0
    for idx, st in enumerate(block['stmts']):
        if st.startswith('legal '):
            shape, guard = D.clause_of(st)
            out.append(('legal', shape, guard, None, None))
        elif st.startswith('prefer '):
            ss, kind, tgt, guard = prefs[pi]
            pi += 1
            if ss:
                out.extend(('prefer', s, kind, tgt, guard) for s in ss)
            else:
                out.extend(('prefer', s, kind, tgt, guard) for s in slots
                           if (s, kind) not in covered)
        elif st.startswith('except ') and ' -> ' in st:
            g, a = st[len('except '):].split(' -> ', 1)
            out.append(('except', g.strip(),
                        EXCEPT_ACTION.get(a.strip().replace(' ', ''), 'Custom'),
                        None, None))
        elif st.startswith('class '):
            # `class int;` / `class int pow2;` / `class int pow2 32;`
            m = re.fullmatch(r'class\s+(\w+)(?:\s+pow2(?:\s+(\d+))?)?', st)
            if m:
                out.append(('class', m.group(1), m.group(2), None, None))
        elif st == 'elemwise':
            out.append(('elemwise', 0, None, None, None))
        elif st.startswith('shapes'):
            ss = decl_slots(block, idx)
            if ss:
                out.extend(('shapes', s, None, None, None) for s in ss)
            else:
                out.extend(('shapes', s, None, None, None) for s in slots
                           if s not in shapes_slots)
    return out


CLASS_CACHE = {}


def class_legal_scalars(block, cname, env, qtypes, macros, mems):
    """The legal types of this class in slot 0, *given the rest of the query*.

    A class default is not a property of the legal set in the abstract: G_SELECT
    accepts s8 only without CMOV, and G_LOAD's legal value type depends on the
    memory size.  Both are read off the block's own `legal` clauses, with slot 0
    set to each member of the class and the other slots left as queried.
    """
    members = D.class_members(cname)
    if not members:
        return None
    msig = tuple((m.get('type'), m.get('size_bits'), m.get('align_bits'))
                 for m in (mems or ()))
    key = (id(block), cname, tuple(sorted(env.items())),
           tuple(qtypes[1:]), msig)
    hit = CLASS_CACHE.get(key)
    if hit is not None:
        return hit
    rules = [r for r in rules_of(block) if r[0] == 'legal']
    out = set()
    q = list(qtypes)
    for t in members:
        if D.kind(t) != 'scalar':
            continue
        q[0] = t
        if any(legal_clause_holds(a, b, env, q, macros, mems)
               for _k, a, b, _t, _g in rules):
            out.add(t)
    CLASS_CACHE[key] = out
    return out


SHAPES_CACHE = {}


def legal_shapes(block, slot, env, qtypes, macros, mems):
    """The legal vector types in `slot`, given the rest of the query.

    `shapes on i;` is the vector-side counterpart of `class int;`: the author
    declares that the slot is on a lane lattice, and the hull comes from the
    legal clauses instead of from 8-10 hand-written `prefer` lines.  Same
    caveat as the class: legality is not a property of the type alone
    (G_LOAD's value type depends on the memory size), so the other slots stay
    as queried.
    """
    msig = tuple((m.get('type'), m.get('size_bits'), m.get('align_bits'))
                 for m in (mems or ()))
    key = (id(block), slot, tuple(sorted(env.items())),
           tuple(qtypes[:slot] + qtypes[slot + 1:]), msig)
    hit = SHAPES_CACHE.get(key)
    if hit is not None:
        return hit
    legal = [r for r in rules_of(block) if r[0] == 'legal']
    custom = [r for r in rules_of(block) if r[0] == 'except' and r[2] == 'Custom']
    out = set()
    q = list(qtypes)
    for t in D.U:
        if D.kind(t) != 'vector':
            continue
        q[slot] = t
        # A shape the op answers `custom` for is admissible too: G_BUILD_VECTOR
        # has no `legal` clause at all, its admissible shapes are the ones its
        # custom lowering handles (`except <shapes> -> delegate(custom)`).
        if any(legal_clause_holds(a, b, env, q, macros, mems)
               for _k, a, b, _t, _g in legal):
            out.add(t)
        elif any(eval_guard(g, env, {i: x for i, x in enumerate(q)}, macros, mems) is True
                 for _k, g, _a, _t, _gg in custom):
            out.add(t)
    SHAPES_CACHE[key] = out
    return out


def step(block, env, qtypes, macros, mems=None):
    """One step (semantics 6.2): the first rule in file order that matches.

    There is deliberately no "nearest legal type" fallback when no rule
    fires: the chain has no generic move either.  X86_FADD has no mutation
    rules at all, so v16s32 is Unsupported there; a bit-distance argmin
    invented `v16s32 -> s32 -> Legal`, the largest class of disagreement the
    differential ever had.  Scalarize is a rule (`prefer elem on i`), not an
    inference.  The old per-slot `nearest` helper is gone for this reason: it
    was a third implementation of this function and it drifted.

    -> ('Legal'|'Custom'|'Lower'|'Libcall'|'Unsupported'|'Undecided', None, None)
       ('Mutate', slot, new_type)
       ('NoMove', None, None)      no rule applies; no `follow` either

    The one implementation.  tools/infer_diff.py used to carry a second copy,
    and check 9 used a third (per-slot `nearest`); they drifted twice.
    """
    tmap = {i: t for i, t in enumerate(qtypes)}
    for kind, a, b, tgt, guard in rules_of(block):
        if kind == 'legal':
            if legal_clause_holds(a, b, env, qtypes, macros, mems):
                return ('Legal', None, None)
            continue
        if kind == 'except':
            g = eval_guard(a, env, tmap, macros, mems)
            if g is True:
                return (b, None, None)
            if g is None:
                return ('Undecided', None, None)
            continue
        if kind == 'class':
            # One step of the class default: pow2, then clamp to the hull.  The
            # chain has these as two rules, but a fixpoint over one move or two
            # is the same set of answers; doing the clamp here keeps the
            # number of iterations down.
            if not qtypes or D.kind(qtypes[0]) != 'scalar':
                continue
            cname, pow2spec = a, b
            scal = class_legal_scalars(block, cname, env, qtypes, macros, mems)
            if not scal:
                continue
            floor = min(D.bits(t) for t in scal)
            ceil = max(D.bits(t) for t in scal)
            cur = qtypes[0]
            bits = D.bits(cur)
            if pow2spec is not None and size_not_pow2(cur):
                # One rule per step, like the chain: do NOT clamp here.  The
                # next iteration checks `legal` first, and the pow2 result can
                # be legal above the clamp ceiling (G_ANYEXT: s80 -> s128,
                # which is legal; capping to s64 in the same step was wrong).
                n = max(next_pow2(bits), floor)
                if n != bits:
                    return ('Mutate', 0, 's%d' % n)
            if bits < floor:
                return ('Mutate', 0, 's%d' % floor)
            if bits > ceil:
                return ('Mutate', 0, 's%d' % ceil)
            continue
        if kind == 'shapes':
            # `shapes on i;` clamps a vector's lane count into the legal hull
            # for that element type.  Only the lane count moves; the element
            # stays.  When the hull is empty for that element, nothing fires --
            # the chain clamps to a shape that is not legal there and then
            # fails, which is the same fixpoint.
            slot = a
            if slot >= len(qtypes) or D.kind(qtypes[slot]) != 'vector':
                continue
            cur = qtypes[slot]
            e = D.elem(cur)
            same = [x for x in legal_shapes(block, slot, env, qtypes, macros, mems)
                    if D.elem(x) == e]
            if not same:
                continue
            ln = sorted(lanes(x) for x in same)
            n = lanes(cur)
            if n < ln[0]:
                return ('Mutate', slot, 'v%d%s' % (ln[0], e))
            if n > ln[-1]:
                return ('Mutate', slot, 'v%d%s' % (ln[-1], e))
            continue
        if kind == 'elemwise':
            # `elemwise;` is a property of the op ("applied per element"), and
            # the one action it licenses is collapsing a vector the rules did
            # not legalize to its element.  It is not `prefer elem on 0` under
            # another name: that is a slot rule the author writes per slot,
            # this is the statement inference needs to tell G_ADD (chain:
            # Scalarize) from G_CONSTANT (chain: Unsupported) -- both have a
            # legal set with no vector in it.  Scalars are untouched.
            if qtypes and D.kind(qtypes[0]) == 'vector':
                e = D.elem(qtypes[0])
                if e != qtypes[0]:
                    return ('Mutate', 0, e)
            continue
        slot, pkind = a, b
        if slot >= len(qtypes):
            continue
        g = eval_guard(guard, env, tmap, macros)
        if g is None:
            return ('Undecided', slot, None)
        if not g:
            continue
        if pkind == 'target':
            tt = D.eval_cond(tgt, env, macros)
            if tt is None:
                return ('Undecided', slot, None)
            if tt != qtypes[slot]:
                return ('Mutate', slot, tt)
            continue
        if pkind == 'elem':
            e = D.elem(qtypes[slot])
            if e != qtypes[slot]:
                return ('Mutate', slot, e)
            continue
        # pkind == 'pow2': WidenScalar{,OrElt}ToNextPow2 fires iff the (element)
        # size is not a power of two; the mutation is max(nextPow2, MinSize).
        e = D.elem(qtypes[slot])
        if D.kind(e) != 'scalar' or not size_not_pow2(e):
            continue
        n = max(next_pow2(D.bits(e)), int(tgt or 0), 1)
        nt = ('v' + re.fullmatch(r'v(\d+)\w+', qtypes[slot]).group(1) + 's%d' % n
              if qtypes[slot].startswith('v') else 's%d' % n)
        if nt != qtypes[slot]:
            return ('Mutate', slot, nt)

    # `follow` is not positional in the chain dump and repairs the tuple, not
    # one type: it runs after every single-slot rule has declined.
    for lhs, rhs, _kind in follows_of(block):
        if rhs >= len(qtypes) or lhs >= len(qtypes):
            continue
        t = D.elem(qtypes[rhs])
        if t != qtypes[lhs]:
            return ('Mutate', lhs, t)
    return ('NoMove', None, None)


# ---------- checks ----------------------------------------------------------
def check(path, self_test_errors=None):
    D.set_universe(json.load(open(ROOT + '/data/x86-core.json'))['domain']['types'])
    macros, blocks = D.parse(path)
    CLASS_CACHE.clear()
    errs, warns, skipped = [], [], []      # (kind, ops, detail)

    # Class definitions are what LLT cannot give us, so an undeclared class or
    # a non-scalar member is a declaration error, not a warning: the default
    # would otherwise silently do nothing.
    for name, body in D.CLASSES.items():
        mem = D.class_members(name)
        if not mem:
            errs.append(('12-class-empty', '-', name))
        for t in sorted(mem):
            if D.kind(t) != 'scalar':
                errs.append(('12-class-nonscalar', name, t))

    for b in blocks:
        ops = b['ops']
        legal = [s for s in b['stmts'] if s.startswith('legal ')]
        for st in b['stmts']:
            if st.startswith('class '):
                m = re.fullmatch(r'class\s+(\w+)(?:\s+pow2(?:\s+(\d+))?)?', st)
                if not m:
                    errs.append(('12-class-syntax', ops, st))
                elif m.group(1) not in D.CLASSES:
                    errs.append(('12-class-undefined', ops, m.group(1)))
                elif any('mem(' in x for x in legal):
                    # The default's bounds are the hull of the class members
                    # that are legal *here*.  For an access rule set that hull
                    # moves with the memory size, while the authored clamp is a
                    # fixed width -- the differential found the model
                    # legalizing a v16s16 load from 32 bits that the chain
                    # rejects.  Fail closed instead of guessing.
                    errs.append(('12-class-on-mem', ops, st))
        # v7: the format is declared, then constrained.  A query shape must
        # be well-formed, and a slot reference outside it is an error -- that
        # is what "several types have to be named in the format" means.
        q = query_of(b)
        for msg in b.get('errors', []):
            kind, _, detail = msg.partition(': ')
            errs.append((kind, ops, detail or msg))
        if q is not None:
            if any(k == 'bad' for k, _ in q):
                errs.append(('13-query-syntax', ops, 'query'))
            for axis in ('ty', 'mem', 'imm'):
                got = [i for k, i in q if k == axis]
                if len(got) != len(set(got)):
                    errs.append(('13-query-duplicate', ops, axis))
                elif got and sorted(set(got)) != list(range(len(set(got)))):
                    errs.append(('13-query-hole', ops,
                                 '%s %s' % (axis, sorted(set(got)))))
            rules_at = [i for i, st in enumerate(b['stmts'])
                        if not (st.startswith('query') or
                                st.startswith('variadic') or
                                st.startswith('group '))]
            if rules_at and any(st.startswith(('query', 'variadic'))
                                for st in b['stmts'][rules_at[0]:]):
                errs.append(('13-query-order', ops,
                             'declare the format before the rules'))
            type_slots = {i for k, i in q if k == 'ty'}
            arity = 1 + max(type_slots, default=-1)
            axes = {k: {i for kk, i in q if kk == k}
                    for k in ('ty', 'mem', 'imm')}
        else:
            arity = slot_count(b, domain(b, None)[1])
            type_slots = set(range(arity))
            axes = None

        if axes is not None:
            for idx, st in enumerate(b['stmts']):
                if st.startswith(('prefer ', 'policy', 'shapes')):
                    for s in (decl_slots(b, idx) or ()):
                        if s not in type_slots:
                            errs.append(('13-ty-undeclared', ops,
                                         'slot %d is not in the query' % s))
                m = re.match(r'follow\s+(\d+)\s+from\s+(\d+)', st)
                if m:
                    for s in m.groups():
                        if int(s) not in type_slots:
                            errs.append(('13-ty-undeclared', ops,
                                         'slot %s is not in the query' % s))
                for axis, i in re.findall(r'\b(ty|mem|imm)\((\d+)\)', st):
                    if int(i) not in axes[axis]:
                        errs.append(('13-ty-undeclared', ops,
                                     '%s(%s) is not in the query'
                                     % (axis, i)))

            # A declared slot that nothing mentions is dead weight: the
            # declaration promises a coordinate the rules never look at.
            dom_all, _ = domain(b, None)
            mentioned = set(dom_all)
            for idx, st in enumerate(b['stmts']):
                mentioned |= set(decl_slots(b, idx) or ())
                mentioned |= {int(i) for i in re.findall(r'ty\((\d+)\)', st)}
                m = re.match(r'follow\s+(\d+)\s+from\s+(\d+)', st)
                if m:
                    mentioned |= {int(m.group(1)), int(m.group(2))}
            for s in sorted(type_slots - mentioned):
                warns.append(('13-slot-unconstrained', ops,
                              'ty %d is declared but no rule mentions it' % s))

        # A `policy` without a slot is the default rule; two declarations
        # competing for the same slot are a conflict, not "last one wins"
        # (`%nonassoc` in the yacc analogy).
        pols = policies_of(b, slots=sorted(type_slots))
        seen_pol, seen_def = {}, 0
        for idx, st in enumerate(b['stmts']):
            if not st.startswith('policy'):
                continue
            ss = decl_slots(b, idx)
            if ss is None:
                seen_def += 1
                if seen_def > 1:
                    errs.append(('13-policy-duplicate', ops, 'default'))
                continue
            for s in ss:
                if s in seen_pol:
                    errs.append(('13-policy-duplicate', ops, 'slot %d' % s))
                seen_pol[s] = idx
        if not type_slots:
            for idx, st in enumerate(b['stmts']):
                if st.startswith(('policy', 'prefer ', 'shapes')) and \
                   decl_slots(b, idx) is None:
                    errs.append(('13-no-slot-no-target', ops, st))
        for slot, (shape, k, pow2, _floor, _guard, _lo, _hi) in pols.items():
            if shape == 'bad':
                errs.append(('13-policy-syntax', ops, 'slot %d' % slot))
            if slot not in type_slots:
                errs.append(('13-policy-not-type', ops,
                             'slot %d is not a ty coordinate' % slot))
            elif slot >= arity:
                errs.append(('13-policy-slot', ops,
                             'slot %d, arity %d' % (slot, arity)))
            if k == 'vector' and pow2:
                errs.append(('13-policy-pow2-vector', ops, 'slot %d' % slot))

        ew = [i for i, st in enumerate(b['stmts']) if st == 'elemwise']
        if len(ew) > 1:
            errs.append(('12-elemwise-duplicate', ops, '%d occurrences' % len(ew)))
        if ew and any(st.startswith('legal ') for st in b['stmts'][ew[0] + 1:]):
            # Rules are an ordered list and `legal` after `elemwise` is
            # unreachable: the default would scalarize a type the author just
            # declared legal.  Order is semantics (semantics §4).
            errs.append(('12-elemwise-order', ops,
                         'a legal clause follows `elemwise;`'))
        shp = [i for i, st in enumerate(b['stmts']) if st.startswith('shapes')]
        if len(shp) > 1:
            errs.append(('12-shapes-duplicate', ops, '%d occurrences' % len(shp)))
        if shp and any(st.startswith('legal ') for st in b['stmts'][shp[0] + 1:]):
            # Same ordering rule as `elemwise`: a legal clause after the lane
            # lattice is unreachable, because `shapes` moves the type first.
            errs.append(('12-shapes-order', ops,
                         'a legal clause follows `shapes on`'))
        prefs = expand_prefers(b, type_slots_of(b, domain(b, None)[1]))
        follows = [s for s in b['stmts'] if s.startswith('follow ')]
        total = trivially_total(b)

        # 1 sort / 2 unit / 3 bounded
        for st in b['stmts']:
            if re.search(r'ty\(\d+\)\s*(<=|>=|<|>)', st):
                errs.append(('1-sort', ops, st))
        for st in legal:
            shape, _g = D.clause_of(st)
            if shape == '_':
                errs.append(('2-unit', ops, st))
            if ' - ' in ' %s ' % shape or re.search(r'\bnever\b', shape):
                errs.append(('3-bounded', ops, st))

        # 4 prefer without guard (pow2 carries its predicate implicitly)
        for pslot, kind, tgt, guard in prefs:
            if kind == 'target' and not guard:
                warns.append(('4-prefer-no-guard', ops, stmt_of(b, pslot, tgt)))

        # 8 mem position
        for st in legal:
            shape, _g = D.clause_of(st)
            for prod in D.products(shape):
                seen_mem = False
                for c in prod:
                    if c.startswith('mem('):
                        seen_mem = True
                    elif seen_mem:
                        errs.append(('8-mem-position', ops, st))
                        break

        if total:
            continue

        atoms = feature_atoms_of(b, macros)
        if 2 ** len(atoms) > MAX_COMBOS:
            skipped.append(('5/6/9', ops, '%d feature atoms' % len(atoms)))
            continue
        envs = [dict(zip(atoms, v))
                for v in itertools.product([False, True], repeat=len(atoms))]
        pref_slots = {p[0] for p in prefs}

        # 5 D_i != empty, per assignment
        for env in envs:
            dom, arity = domain(b, env)
            for i in range(arity):
                if not dom.get(i) and i not in pref_slots:
                    errs.append(('5-empty-domain', ops,
                                 'ty(%d) has no candidate set when %s'
                                 % (i, fmt(env))))
                    pref_slots.add(i)       # report once
        dom_all, arity = domain(b, None)

        # 6 prefer target: universe (error) / legality (warning)
        seen = set()
        trans = {}
        for pslot, kind, tgt, guard in prefs:
            if kind != 'target':      # pow2 / elem carry no fixed target type
                continue
            for env in envs:
                if not D.feature_holds(guard, env):
                    continue
                t = D.eval_cond(tgt, env, macros)
                key = (tgt, pslot)
                if t is None:
                    if ('unresolved',) + key not in seen:
                        seen.add(('unresolved',) + key)
                        errs.append(('6-prefer-unresolved', ops,
                                     '%s under %s' % (tgt, fmt(env))))
                    continue
                if t not in D.U:
                    if ('universe',) + key not in seen:
                        seen.add(('universe',) + key)
                        errs.append(('6-prefer-universe', ops,
                                     '%s is not a type in U' % t))
                    continue
                dom, _a = domain(b, env)
                if dom.get(pslot) and t not in dom[pslot]:
                    slot = trans.setdefault((tgt, pslot, t), [0, env, dom[pslot]])
                    slot[0] += 1

        for (tgt, pslot, t), (n, env0, d) in sorted(trans.items()):
            warns.append(('6-prefer-transient', ops,
                          '%s is not legal in %d configuration(s), e.g. %s '
                          '(D_%d=%s); the next iteration must legalize it'
                          % (t, n, fmt(env0), pslot, ','.join(sorted(d)))))

        # 7 diagonal needs follow
        if arity == 2 and not follows:
            pts, ok = set(), True
            for st in legal:
                sh, g = D.clause_of(st)
                if g:
                    continue
                for prod in D.products(sh):
                    if len(prod) != 2 or any(c.startswith('mem(') for c in prod):
                        ok = False
                        break
                    vs = [D.types_of(c) for c in prod]
                    if any(len(v) != 1 for v in vs):
                        ok = False
                        break
                    pts.add((next(iter(vs[0])), next(iter(vs[1]))))
                if not ok:
                    break
            if ok and pts and len(pts) < len(dom_all.get(0, ())) * len(dom_all.get(1, ())):
                warns.append(('7-diagonal-needs-follow', ops,
                              '%d points vs %dx%d domain'
                              % (len(pts), len(dom_all.get(0, ())), len(dom_all.get(1, ())))))

        # 9 convergence: replay the block's own rules -- the same `step` the
        # differential uses -- from a uniform query of every type whose kind
        # the block talks about.  Only a cycle (or a run that never terminates)
        # is an error; landing on NoMove / an escape is a defined outcome.
        slots = sorted(set(dom_all) | pref_slots)
        nslots = max(slot_count(b, arity), 1)
        bad = set()
        for env in envs:
            dom, _a = domain(b, env)
            supplied = {}
            for pslot, kind, tgt, guard in prefs:
                if kind == 'target':
                    t = D.eval_cond(tgt, env, macros)
                    if t:
                        supplied.setdefault(pslot, set()).add(t)
            kinds = set()
            for i in slots:
                goal = dom.get(i) or supplied.get(i, set())
                kinds |= {D.kind(u) for u in goal}
            for _ps, kind, tgt, guard in prefs:
                if kind == 'target':
                    t = D.eval_cond(tgt, env, macros)
                    if t:
                        kinds.add(D.kind(t))
                kinds.update(re.findall(r'kind\(ty\(\d+\)\)\s*=\s*(\w+)',
                                        guard or ''))
            kinds.discard('?')
            if not kinds:
                continue
            for t0 in sorted(u for u in D.U if D.kind(u) in kinds):
                q = [t0] * nslots
                seen, why = set(), None
                for _ in range(STEPS):
                    key = tuple(q)
                    if key in seen:
                        why = 'cycle through %s' % (key,)
                        break
                    seen.add(key)
                    act, si, new = step(b, env, q, macros)
                    if new is None:
                        break           # Legal / escape / NoMove / Undecided
                    q[si] = new
                else:
                    why = 'still moving after %d steps' % STEPS
                if why:
                    bad.add((t0, fmt(env) + ': ' + why))
        for t0, why in sorted(bad)[:8]:
            errs.append(('9-nonconvergent', ops,
                         'starting from %s: %s' % (t0, why)))

    return errs, warns, skipped, len(blocks), len(macros)


def stmt_of(block, slot, tgt):
    for st in block['stmts']:
        if st.startswith('prefer ') and tgt in st and ' on %d' % slot in st:
            return st
    return 'prefer %s on %d' % (tgt, slot)


def fmt(env):
    if not env:
        return 'no features'
    return ','.join('%s=%d' % (k, v) for k, v in sorted(env.items()))


# ---------- negative controls ----------------------------------------------
SELF_TEST = [
    ('legal s8; prefer s3 on 0;', '6-prefer-universe'),
    ('legal s8; prefer s32 on 0;', '6-prefer-transient'),
    ('legal s8; prefer pow2 on 0;', None),
    ('prefer s32 on 1; except true -> delegate(libcall);', None),
    ('legal _;', '2-unit'),
    ('legal s32 when ty(0) < s64;', '1-sort'),
    ('legal (s8, any);', '5-empty-domain'),
    ('legal (s8, s16), (s16, s8);', '7-diagonal-needs-follow'),
    ('legal (mem(s8), s8, p0:64);', '8-mem-position'),
    ('legal s8; prefer s16 on 0 when bits(ty(0)) < 16;'
     ' prefer s4 on 0 when bits(ty(0)) > 8;', '9-nonconvergent'),
    ('legal s8, s16, s32; prefer s8 on 0 when bits(ty(0)) < 8;', None),
]


# Guards that broke the evaluator, one case each.  `want is None` means
# "undecided", which is a real answer: the point is that it is not True.
GUARD_SELF_TEST = [
    # The shape G_BUILD_VECTOR's customIf compiles to.  Before the structural
    # evaluator this came back None, and `None is not False` meant "fires".
    ('(HasSSE1 && ty(0) in (v4s32)) || (HasSSE2 && ty(0) in (v2s64 + v8s16))',
     {0: 'v8s16'}, {'HasSSE1': False, 'HasSSE2': True}, True),
    ('(HasSSE1 && ty(0) in (v4s32)) || (HasSSE2 && ty(0) in (v2s64 + v8s16))',
     {0: 'v4s8'}, {'HasSSE1': False, 'HasSSE2': True}, False),
    # unknown AND false is false, not unknown -- and the other way round.
    ('(HasSSE1 && ty(0) in (v4s32)) || (HasSSE2 && ty(0) in (v2s64 + v8s16))',
     {0: 'v4s8'}, {'HasSSE1': False}, False),
    ('(HasSSE1 && ty(0) in (v4s32)) || (HasSSE2 && ty(0) in (v2s64 + v8s16))',
     {0: 'v8s16'}, {'HasSSE1': False}, None),
    ('(HasSSE1 && ty(0) in (v4s32)) || (HasSSE2 && ty(0) in (v2s64 + v8s16))',
     {0: 'v8s16'}, {'HasSSE1': True}, None),
    ('!(HasSSE2 && ty(0) = v8s16)', {0: 'v8s16'}, {'HasSSE2': True}, False),
    ('bits(ty(0)) < cond(Is64Bit, 64, 32)', {0: 's16'}, {'Is64Bit': True}, True),
    ('bits(ty(0)) < cond(Is64Bit, 64, 32)', {0: 's16'}, {'Is64Bit': False}, True),
    ('bits(ty(0)) < cond(Is64Bit, 64, 32)', {0: 's16'}, {}, None),
]


def guard_self_test():
    D.set_universe(['s8', 's16', 's32', 's64', 'v4s8', 'v4s32', 'v8s16'])
    fails = 0
    for guard, tmap, env, want in GUARD_SELF_TEST:
        got = eval_guard(guard, env, tmap, {})
        if got is not want and got != want:
            print('  FAIL eval_guard(%r, %r) = %r, want %r'
                  % (guard[:50], tmap, got, want))
            fails += 1
    # An undecidable escape is undecided, not Custom.
    block = {'ops': 'G_T', 'stmts': [
        'except (HasSSE2 && ty(0) in (v8s16)) -> delegate(custom);']}
    if except_action(block, {'HasSSE2': True}, {0: 'v4s8'}, {}) is not None:
        print('  FAIL a false escape guard still fires'); fails += 1
    if except_action(block, {'HasSSE2': True}, {0: 'v8s16'}, {}) != 'Custom':
        print('  FAIL a true escape guard does not fire'); fails += 1
    if except_action(block, {}, {0: 'v8s16'}, {}) != UNDECIDED:
        print('  FAIL an unknown escape guard is not reported as undecided'); fails += 1
    print('  guard self-test: %d cases, %d failed' % (len(GUARD_SELF_TEST) + 3, fails))
    return fails


CLASS_SELF_TEST = [
    ('class int = s8, s16, s32, s64;\n'
     'op G_TEST {\n  legal s8, s16, s32;\n  class int;\n}\n', None),
    ('op G_TEST {\n  legal s8;\n  class foo;\n}\n', '12-class-undefined'),
    ('class int = s8, v4s8;\n'
     'op G_TEST {\n  legal s8;\n  class int;\n}\n', '12-class-nonscalar'),
    ('class int = s8, s16;\n'
     'op G_TEST {\n  legal any when ty(1) = p0:64 && bits(ty(0)) = bits(mem(0).ty);\n'
     '  class int;\n}\n', '12-class-on-mem'),
    ('op G_TEST {\n  legal s8, v4s8;\n  elemwise;\n}\n', None),
    ('op G_TEST {\n  legal s8;\n  elemwise;\n  legal v4s8;\n}\n',
     '12-elemwise-order'),
    ('op G_TEST {\n  legal s8;\n  elemwise;\n  elemwise;\n}\n',
     '12-elemwise-duplicate'),
    ('op G_TEST {\n  legal v4s8;\n  shapes on 0;\n}\n', None),
    ('op G_TEST {\n  legal v4s8;\n  shapes on 0;\n  legal v8s8;\n}\n',
     '12-shapes-order'),
    ('op G_TEST {\n  legal v4s8;\n  shapes on 0;\n  shapes on 0;\n}\n',
     '12-shapes-duplicate'),
]


def class_self_test():
    ok = 0
    for src, want in CLASS_SELF_TEST:
        with tempfile.NamedTemporaryFile('w', suffix='.decl', delete=False) as f:
            f.write(src)
            path = f.name
        errs, warns, _sk, _nb, _nm = check(path)
        got = {e[0] for e in errs} | {w[0] for w in warns}
        os.unlink(path)
        hit = (want in got) if want else not errs
        if not hit:
            print('  FAIL class case %-40r want %-22s got %s'
                  % (src[:40], want or 'no error', sorted(got)))
        ok += 1 if hit else 0
    print('  class self-test: %d cases, %d failed'
          % (len(CLASS_SELF_TEST), len(CLASS_SELF_TEST) - ok))
    return len(CLASS_SELF_TEST) - ok


SHAPE_SELF_TEST = [
    # A default rule is legal on its own, and on a two-slot block.
    ('op G_T {\n  query ty 0;\n  legal s8;\n  policy: scalar hull;\n}\n', None),
    ('op G_T {\n  query ty 0, ty 1;\n  legal s8, s16;\n'
     '  policy: scalar hull;\n}\n', None),
    # Format first.
    ('op G_T {\n  legal s8;\n  query ty 0;\n}\n', '13-query-order'),
    ('op G_T {\n  query ty 0, ty 0;\n  legal s8;\n}\n', '13-query-duplicate'),
    ('op G_T {\n  query ty 0, ty 2;\n  legal s8;\n}\n', '13-query-hole'),
    # A mention outside the declared shape is an error.
    ('op G_T {\n  query ty 0;\n  legal any when ty(1) = p0:64;\n}\n',
     '13-ty-undeclared'),
    ('op G_T {\n  query ty 0;\n  legal s8;\n  policy 1: scalar hull;\n}\n',
     '13-policy-not-type'),
    ('op G_T {\n  query ty 0, ty 1;\n  legal s8;\n}\n',
     '13-slot-unconstrained'),
    # Two declarations compete for one slot: %nonassoc, not "last wins".
    ('op G_T {\n  query ty 0, ty 1;\n  legal s8, s16;\n'
     '  policy 0: scalar hull;\n  policy 0: scalar to s8;\n}\n',
     '13-policy-duplicate'),
    ('op G_T {\n  query ty 0, ty 1;\n  legal s8, s16;\n'
     '  policy: scalar hull;\n  policy: scalar to s8;\n}\n',
     '13-policy-duplicate'),
    ('op G_T {\n  query mem 0;\n  legal s8;\n  policy: scalar hull;\n}\n',
     '13-no-slot-no-target'),
    # Groups: named and inline.
    ('op G_T {\n  query ty 0, ty 1;\n  group pair = 0, 1;\n'
     '  with pair {\n    policy: scalar hull;\n  }\n  legal s8, s16;\n}\n',
     None),
    ('op G_T {\n  query ty 0;\n  with 0, 1 { policy: scalar hull; }\n'
     '  legal s8;\n}\n', '13-ty-undeclared'),
    ('op G_T {\n  query ty 0, ty 1;\n'
     '  with 0 { with 1 { policy: scalar hull; } }\n  legal s8, s16;\n}\n',
     '13-with-nested'),
    ('op G_T {\n  query ty 0, ty 1;\n  with 0 { legal s8; }\n'
     '  legal s8, s16;\n}\n', '13-with-non-slot'),
    ('op G_T {\n  query ty 0, ty 1;\n  with nosuch { policy: scalar hull; }\n'
     '  legal s8, s16;\n}\n', '13-with-unknown-group'),
]


def shape_self_test():
    ok = 0
    for src, want in SHAPE_SELF_TEST:
        with tempfile.NamedTemporaryFile('w', suffix='.decl', delete=False) as f:
            f.write(src)
            path = f.name
        errs, warns, _sk, _nb, _nm = check(path)
        got = {e[0] for e in errs} | {w[0] for w in warns}
        os.unlink(path)
        hit = (want in got) if want else not errs
        if not hit:
            print('  FAIL shape case %-46r want %-24s got %s'
                  % (src[:46], want or 'no error', sorted(got)))
        ok += 1 if hit else 0
    print('  shape self-test: %d cases, %d failed'
          % (len(SHAPE_SELF_TEST), len(SHAPE_SELF_TEST) - ok))
    return len(SHAPE_SELF_TEST) - ok


def slot_self_test():
    """The precedence rule, asserted: default fills, specific wins, order free."""
    fails = 0

    def block(src):
        with tempfile.NamedTemporaryFile('w', suffix='.decl', delete=False) as f:
            f.write(src)
            p = f.name
        _m, blocks = D.parse(p)
        os.unlink(p)
        return blocks[0]

    def want(cond, msg):
        nonlocal fails
        if not cond:
            print('  FAIL ' + msg)
            fails += 1

    b = block('op G_T {\n query ty 0, ty 1;\n'
              ' policy: scalar to s8;\n policy 1: scalar to s32;\n'
              ' legal s8, s16;\n}\n')
    pol = policies_of(b)
    want(pol[0][5] == 's8', 'the default fills slot 0')
    want(pol[1][5] == 's32', 'the specific declaration wins slot 1')

    b = block('op G_T {\n query ty 0, ty 1;\n'
              ' policy 1: scalar to s32;\n policy: scalar to s8;\n'
              ' legal s8, s16;\n}\n')
    pol = policies_of(b)
    want(pol[1][5] == 's32', 'specific wins regardless of textual order')

    b = block('op G_T {\n query ty 0, ty 1;\n'
              ' with 0 { policy: scalar to s8; }\n legal s8, s16;\n}\n')
    pol = policies_of(b)
    want(0 in pol and 1 not in pol, 'a group covers exactly its slots')

    b = block('op G_T {\n query ty 0, ty 1;\n'
              ' prefer s8;\n prefer s32 on 1;\n legal s8, s16;\n}\n')
    ep = expand_prefers(b, [0, 1])
    want(any(s == 0 and t == 's8' for s, _k, t, _g in ep),
         'the default prefer lands on the uncovered slot')
    want(not any(s == 1 and t == 's8' for s, _k, t, _g in ep),
         'the specific prefer suppresses the default of the same kind')
    want(any(s == 1 and t == 's32' for s, _k, t, _g in ep),
         'the specific prefer survives')

    b = block('op G_T {\n query ty 0, ty 1;\n'
              ' prefer pow2 min 8;\n prefer s32 on 1;\n legal s8, s16;\n}\n')
    ep = expand_prefers(b, [0, 1])
    want(any(s == 1 and k == 'pow2' for s, k, _t, _g in ep),
         'a different kind is not suppressed by a specific prefer')

    print('  slot self-test: 7 cases, %d failed' % fails)
    return fails


def self_test():
    ok = 0
    for src, want in SELF_TEST:
        with tempfile.NamedTemporaryFile('w', suffix='.decl', delete=False) as f:
            f.write('op G_TEST {\n')
            for st in src.split(';'):
                if st.strip():
                    f.write('  %s;\n' % st.strip())
            f.write('}\n')
            path = f.name
        errs, warns, _sk, _nb, _nm = check(path)
        got = {e[0] for e in errs} | {w[0] for w in warns}
        os.unlink(path)
        hit = (want in got) if want else not errs
        if not hit:
            print('  FAIL %-58r want %-22s got %s' % (src[:58], want or 'no error', sorted(got)))
        ok += 1 if hit else 0
    print('check_decl self-test: %d cases, %d failed' % (len(SELF_TEST), len(SELF_TEST) - ok))
    sfails = shape_self_test()
    gfails = guard_self_test()
    cfails = class_self_test()
    lfails = slot_self_test()
    return 0 if (ok == len(SELF_TEST) and not sfails and not gfails
                 and not cfails and not lfails) else 1


def main(argv):
    if '--self-test' in argv:
        return self_test()
    show_domain = '--domain' in argv
    files = [a for a in argv if not a.startswith('--')]
    if not files:
        print(__doc__.strip())
        return 2
    rc = 0
    for f in files:
        errs, warns, skipped, nb, nm = check(f)
        print('%s: %d blocks, %d macros' % (f, nb, nm))
        for kind, ops, detail in errs:
            print('  ERROR  [%s] %s: %s' % (kind, ops, detail[:160]))
        for kind, ops, detail in warns:
            print('  warn   [%s] %s: %s' % (kind, ops, detail[:160]))
        for kind, ops, detail in skipped:
            print('  SKIP   [%s] %s: %s' % (kind, ops, detail))
        print('  %d errors, %d warnings' % (len(errs), len(warns)))
        rc |= 1 if errs else 0
    return rc


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
