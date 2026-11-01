#!/usr/bin/env python3
"""How much of a rule set is inferable from its legal set alone?

The maintainer's complaint (llvm-dev, arsenm):

    If I have declared legal i32 operations, I shouldn't have to separately
    specify that smaller bitwidths need integer promotion and wider need
    truncation.

This measures exactly that.  For every slot-0 rule set and every feature
assignment:

  1. read the **declared legal set** off the chain (one query per type, no
     mutation): the types the ruleset answers `Legal` for;
  2. run a single, fixed **default policy** over the type universe `U`:

        scalar t:  bits(t) <  min(legal scalars)  -> min        (promotion)
                   bits(t) >  max(legal scalars)  -> max        (truncation)
                   size not a power of two        -> next pow2  (clamped to max)
                   otherwise                      -> no move
        vector t:  clamp the lane count to the legal range for that element
                   type; if there is no legal vector of that element type,
                   do nothing (scalarize is an explicit rule, not a default)
        pointer / ptrvector -> no move

  3. compare the policy's fixpoint against the chain's fixpoint, point by
     point.

The unit of the report is a **rule set**, not a type: "42 of 47 slot-0 rule
sets are exactly reproduced by the default" is the number the RFC needs, and
every exception is a type plus what the chain did there.

Usage:
  python3 tools/infer_defaults.py
  python3 tools/infer_defaults.py --op G_ADD -v
  python3 tools/infer_defaults.py --self-test
"""
import argparse
import itertools
import json
import re
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import check_decl as C
import decl_lib as D
import evaluate_core as E
import infer_diff as I
import policy_semantics as P

TERMINAL = ('Legal', 'Unsupported', 'Custom', 'Libcall', 'Lower')
dropped = [0]          # feature assignments the measurement domain excluded
merged = [0]           # ... and those an equivalent one already asked
mode_out = [0]         # ... and those outside the measured mode (Is64Bit=0)
raw = [0]              # the powerset, before any of the three filters
realized = [0]         # ... and those that survived both filters
cycles = [0]           # points where the chain itself never settles

# Feature implications, read from llvm/lib/Target/X86/X86.td by
# tools/gen_implies.py.  They used to be transcribed by hand; a stale copy
# moved the measurement denominator without failing anything (dropping
# `HasBWI -> HasAVX512` alone adds 216 unreachable envs and changes the
# headline count from 3058/3240 to 3267/3456, while the percentage moves by
# 0.1).  A missing table is an error, not a silent fallback.
IMPLIES_PATH = os.path.join(HERE, 'x86-implies.json')


def load_implies(path=IMPLIES_PATH):
    if not os.path.exists(path):
        raise SystemExit('%s is missing; run tools/gen_implies.py' % path)
    with open(path) as f:
        doc = json.load(f)
    table = doc.get('features') or {}
    if not table:
        raise SystemExit('%s has no feature table' % path)
    return table


IMPLIES = load_implies()


def closure(f, _memo={}):
    if f in _memo:
        return _memo[f]
    out = set()
    for g in IMPLIES.get(f, ()):
        out.add(g)
        out |= closure(g)
    _memo[f] = out
    return out


def realizable(env):
    """Can the target be in this feature assignment at all?"""
    for f, on in env.items():
        if not on:
            continue
        for g in closure(f):
            if env.get(g) is False:
                return False
    return True

# The policy is a fixed set of decisions, not a knob to tune until it agrees
# with the chain.  `--self-test` breaks each one and requires the score to
# drop: a default nobody can falsify is a guess with a table attached.
POLICY = {'promote': True, 'truncate': True, 'pow2': True, 'flag': True,
          'scalarize': True}

# The measurement domain, not the policy.  The env space is the cartesian
# product of the features a block mentions, which contains assignments the
# target cannot be in: `HasBWI=1, HasAVX2=0` is not a configuration, because
# FeatureBWI requires FeatureAVX512, which requires FeatureAVX2
# (X86.td:118-153).  In those assignments the chain's clamp names a vector
# that is illegal there and then scalarizes, while the default clamps to the
# legal hull -- so counting them would measure the env space, not the default.
MEASURE = {'realizable': True, 'optin': True}
# Points measured when the multi-slot path is off.  It moves with the
# environment axis, so it is one constant instead of a literal in a
# message: the control is that the fallback does not drift on its own.
MULTI_OFF = 1656
# Two toggles that exist so the self-test can turn the new machinery off and
# watch the number move.  Nothing else sets them.
MULTI = {'on': True}       # off = the old "arity > 1, not modelled here"
RELATION = {'on': True}    # off = move slots towards all of U, not towards
                           #       what the block's own legal relation allows


def legal_set(ctx, op):
    """The types this rule set accepts as-is, in this configuration."""
    out = set()
    for t in D.U:
        r = E.evaluate(ctx, {'opcode': op, 'types': [t], 'imms': [], 'mems': []})
        if r.get('result') == 'rule' and r.get('action') == 'Legal':
            out.add(t)
    return out


def default_step(t, legal, elemwise=False, has_class=True, forced=False):
    """One application of the default policy.  -> ('Legal'|'Mutate'|'NoMove', ...)

    `elemwise` is declared by the op (`elemwise;`), not read off the legal set.
    G_ADD and G_CONSTANT both have a legal set with no vector in it, and the
    chain scalarizes one and rejects the other; nothing in the legal set says
    which.  The property that licenses scalarizing is "this op is applied per
    element", and that is what the declaration states.
    """
    if t in legal:
        return ('Legal', None)
    kind = D.kind(t)
    if MEASURE['optin'] and kind == 'scalar' and not has_class:
        # `class int;` is how a block opts into the width lattice; without it
        # the language says nothing, and neither may the model.
        #
        return ('NoMove', None)
    if MEASURE['optin'] and kind == 'vector' and not (has_class or elemwise):
        return ('NoMove', None)
    if kind == 'scalar':
        scal = sorted((x for x in legal if D.kind(x) == 'scalar'), key=D.bits)
        if not scal:
            return ('NoMove', None)
        lo, hi = scal[0], scal[-1]
        b = D.bits(t)
        # A rule set whose only legal scalar is s1 is not on a width lattice:
        # `G_BRCOND` takes a flag, and promoting/narrowing a value into a flag
        # is a different program.  There is nothing to interpolate between.
        if POLICY['flag'] and len(scal) == 1 and D.bits(lo) == 1:
            return ('NoMove', None)
        if POLICY['promote'] and b < D.bits(lo):
            return ('Mutate', lo)
        if POLICY['truncate'] and b > D.bits(hi):
            return ('Mutate', hi)
        if not POLICY['pow2'] or not C.size_not_pow2(t):
            return ('NoMove', None)
        n = C.next_pow2(b)
        return ('Mutate', 's%d' % n if n <= D.bits(hi) else hi)
    if kind == 'vector':
        e = D.elem(t)
        same = [x for x in legal if D.kind(x) == 'vector' and D.elem(x) == e]
        if same:
            lanes_ = sorted(C.lanes(x) for x in same)
            n = C.lanes(t)
            if n < lanes_[0]:
                return ('Mutate', 'v%d%s' % (lanes_[0], e))
            if n > lanes_[-1]:
                return ('Mutate', 'v%d%s' % (lanes_[-1], e))
            if elemwise and POLICY['scalarize']:
                # Inside the legal lane interval but not a legal shape: the
                # chain's Scalarize<0> fires here too (it is the last rule).
                return ('Mutate', e)
            return ('NoMove', None)
        # No legal vector of that element type.  Scalarize only when the op
        # says it is element-wise: G_CONSTANT has no Scalarize rule and
        # answers Unsupported for every vector, so a blanket default would
        # turn a reject into a different instruction.
        if elemwise and POLICY['scalarize']:
            return ('Mutate', e)
        return ('NoMove', None)
    return ('NoMove', None)


def default_fixpoint(t, legal, elemwise=False, has_class=True, steps=8):
    cur = t
    for _ in range(steps):
        act, new = default_step(cur, legal, elemwise, has_class)
        if act == 'Legal':
            return ('Legal', (cur,))
        if new is None:
            return ('Unsupported', None)
        if new == cur:
            return ('cycle', None)
        cur = new
    return ('cycle', None)


CHOICE = {}           # ops -> (declared choice, ok as declared, ok without)


def score_block(b, op0, envs, doc, composed, macros, kind, has_class):
    """Score one rule set under one choice of declaration."""
    elemwise = any(st == 'elemwise' for st in b['stmts'])
    total = ok = cyc = 0
    bad = []
    for env in envs:
        ctx = E.Context(doc, I.features_for(env, composed))
        legal = legal_set(ctx, op0)
        for t in D.U:
            if kind and D.kind(t) != kind:
                continue
            total += 1
            try:
                ref = I.chain_fixpoint(ctx, op0, [t])
            except E.EvalError:
                continue
            got = default_fixpoint(t, legal, elemwise, has_class)
            # The policy models *mutations*.  When it has no move, the
            # rule set's escape decides -- and that escape is written
            # explicitly in the .decl, so it is not something the default
            # has to infer.  Any terminal non-Legal answer therefore
            # agrees with "no move", whatever action it is.
            if ref[0] == 'cycle':
                cyc += 1
            if got[0] == 'Unsupported' and ref[0] in ('Unsupported', 'Lower',
                                                      'Libcall', 'Custom'):
                ok += 1
                continue
            if ref[0] == 'cycle' and got[0] == 'Unsupported':
                continue
            if ref[0] == got[0] and (ref[1] is None or tuple(ref[1]) == got[1]):
                ok += 1
            else:
                bad.append((C.fmt(env), t, ref, got, set()))
    return total, ok, bad, cyc


# --- multi-slot ------------------------------------------------------------
#
# `infer_defaults` used to stop at arity > 1: 34 of the 63 rule sets were
# "not measured".  That is the half of the question the maintainer actually
# asked ("I declared legal i32 ops, why do I have to spell out promotion and
# truncation"), because a two-operand op is where the answer stops being
# obvious.
#
# The legal set stops being a set of types and becomes a relation over the
# slots.  Everything else stays: the same policy, applied to one slot at a
# time, with the admissible set for that slot read as the *projection*
#
#     L_i = { t in pool(i) | some assignment of the other slots is legal }
#
# of the relation.  A per-tuple projection ("given the other slots as they
# are now") was tried first and is wrong: on G_SHL, the query (s1, s1) has an
# empty projection at both slots, while the chain promotes each slot on its
# own to s8.  The chain's per-slot rules never look at the other slots, so
# neither may the default.  At arity 1 the projection is exactly the legal
# set, so this degrades to the old model.

def slot_tuple(q, i, t):
    out = list(q)
    out[i] = t
    return tuple(out)


def projections(pools, member, mems):
    """L_i for every slot, from the relation restricted to the pools."""
    if not RELATION['on']:
        return [set(pool) for pool in pools]
    out = []
    for i, pool in enumerate(pools):
        keep = set()
        for t in pool:
            for other in itertools.product(*pools):
                if member(slot_tuple(other, i, t), mems):
                    keep.add(t)
                    break
        out.append(keep)
    return out


def narrow_once(t):
    """One narrowing step: vector and pointer-vector take the element, a
    scalar halves.  This is what `narrowScalar` does one iteration at a time;
    the fixpoint supplies the rest."""
    e = D.elem(t)
    if e != t:
        return e
    if D.kind(t) == 'scalar' and D.bits(t) % 2 == 0:
        n = 's%d' % (D.bits(t) // 2)
        return n if n in D.U else None
    return None


def default_step_multi(q, member, projs, elemwise, has_class, follows=(),
                       forced=False, mem_bits=None, policies=None):
    """One step of the default, one slot at a time. -> (act, new tuple)

    `follow i from j by elem` is a declaration, not an inference: it says the
    two slots are coupled, and the default may use it the same way it uses
    `class int` or `elemwise`.  Without it G_CTPOP's (s32, s16) has no default
    move at all, while the chain answers Legal (s16, s16)."""
    if member(q):
        return ('Legal', None)
    # Memory blocks: the value is narrowed to the access size.  Never widened
    # -- reading 64 bits into an s32 is a different instruction, and the chain
    # answers Unsupported for it (G_LOAD: s128 -> s64, but s32 stays rejected).
    if forced and mem_bits is not None and D.bits(q[0]) > mem_bits:
        nt = narrow_once(q[0])
        if nt is not None and member(slot_tuple(q, 0, nt)):
            return ('Mutate', slot_tuple(q, 0, nt))
    for i in range(len(q)):
        pol = policies.get(i) if policies else None
        # Only the kind the policy declares is taken over: a `vector hull`
        # policy must not swallow the scalar-elementwise path, and a
        # `scalar` policy must not stop `elemwise` from scalarizing a vector.
        if pol is not None and P.kind(q[i]) == pol.kind:
            # v7 `policy`: the destination is declared, not read off the hull.
            # A per-slot 'Legal' means "this type is in the projection", not
            # "the query is legal", so it falls through like default_step's.
            act, new = P.step(q[i], projs[i], pol)
            if act == 'Mutate' and new != q[i]:
                return ('Mutate', slot_tuple(q, i, new))
            continue
        if not projs[i]:
            # Nothing can appear in this slot; the other slots must move.
            continue
        act, new = default_step(q[i], projs[i],
                                elemwise if i == 0 else False,
                                has_class if i == 0 else True)
        if act == 'Mutate' and new != q[i]:
            return ('Mutate', slot_tuple(q, i, new))
    for lhs, rhs, _kind in follows:
        if rhs >= len(q) or lhs >= len(q):
            continue
        t = D.elem(q[rhs])
        if t != q[lhs]:
            return ('Mutate', slot_tuple(q, lhs, t))
    return ('NoMove', None)


def default_fixpoint_multi(q, member, projs, elemwise, has_class, follows=(),
                           forced=False, mem_bits=None, steps=8, policies=None):
    cur = tuple(q)
    for _ in range(steps):
        act, new = default_step_multi(cur, member, projs, elemwise, has_class,
                                      follows, forced, mem_bits, policies)
        if act == 'Legal':
            return ('Legal', cur)
        if new is None:
            return ('Unsupported', None)
        if new == cur:
            return ('cycle', None)
        cur = new
    return ('cycle', None)


def chain_fixpoint_slots(ctx, op, qtypes, mems=(), steps=8):
    """I.chain_fixpoint, plus which slots the chain rewrote.

    The report has to say *where* a rule set needs a rule; a count of
    unexplained points per block cannot distinguish "slot 0 needs a rule"
    from "slot 1 needs a rule", and `G_ICMP` needs the second.
    """
    types = list(qtypes)
    moved = set()
    for _ in range(steps):
        r = E.evaluate(ctx, {'opcode': op, 'types': types, 'imms': [],
                             'mems': list(mems)})
        a = r.get('result')
        if a == 'out_of_domain':
            return ('out_of_domain', None, moved)
        act = r.get('action')
        if act in TERMINAL:
            return (act, tuple(types) if act == 'Legal' else None, moved)
        nt, ti = r.get('new_type'), r.get('type_idx')
        if nt is None or ti is None:
            return (act, None, moved)
        if nt == types[ti]:
            # No progress: the legalizer cannot legalize this query.  Same
            # rule as infer_diff.chain_fixpoint (see the note there).
            return ('Unsupported', None, moved)
        types[ti] = nt
        moved.add(ti)
    return ('cycle', None, moved)


def score_block_multi(b, op0, envs, doc, composed, macros, kind, has_class):
    """Score a rule set whose queries have more than one type slot."""
    elemwise = any(st == 'elemwise' for st in b['stmts'])
    follows = C.follows_of(b)
    arity = C.slot_count(b, C.domain(b, None)[1])
    mem_n = I.mem_slots_of(b)
    mp = I.mem_pool(b, doc)
    if mem_n and not mp:
        return 0, 0, [], 'memdesc block, data domain has no mems', 0
    # A slot the chain pins to one type is queried at that type only; the
    # same rule infer_diff uses, so both tools ask the same questions.
    pools = []
    for i in range(arity):
        p = I.pinned_type(b, i)
        pools.append([p] if p else list(D.U))
    total = ok = cyc = 0
    bad = []
    for env in envs:
        ctx = E.Context(doc, I.features_for(env, composed))
        # The policy guard is a property of this feature assignment, so the
        # bound is resolved here, once per env, not per query.
        pols = {}
        for slot, (shape, k, pow2, floor, guard, spec_lo, spec_hi) in \
                C.policies_of(b, env, macros, slots=range(arity)).items():
            if shape == 'bad':
                continue
            lo = hi = None
            hull = shape == 'hull'
            if not hull:
                lo, hi = spec_lo, spec_hi
                if (shape in ('both', 'lo') and lo is None) or \
                   (shape in ('both', 'hi') and hi is None):
                    continue      # unresolvable bound: check() reports it
            pols[slot] = P.Policy(k, lo=lo, hi=hi, hull=hull,
                                  pow2=pow2, pow2_floor=floor, guard=guard)
        cache = {}

        def member(q, ms=(), _ctx=ctx, _cache=cache):
            key = (tuple(q), tuple((m['type'], m['size_bits']) for m in ms))
            try:
                return _cache[key]
            except KeyError:
                try:
                    r = E.evaluate(_ctx, {'opcode': op0, 'types': list(q),
                                          'imms': [], 'mems': list(ms)})
                except E.EvalError:
                    return None
                hit = (r.get('result') == 'rule'
                       and r.get('action') == 'Legal')
                _cache[key] = hit
                return hit

        for ms in itertools.product(mp, repeat=mem_n):
            projs = projections(pools, member, ms)
            # Is the access size itself an admissible value size?  On G_LOAD
            # it is (a 64-bit access can be read into s64), so narrowing a
            # wider value to it is a move the memdesc licenses.  On
            # G_SEXTLOAD nothing at the access size is legal -- the value must
            # be *wider* than the access -- so the same move would invent an
            # answer the chain does not give.
            size_ok = False
            if mem_n:
                base = [p[0] for p in pools]
                size_ok = any(
                    D.bits(t) == ms[0]['size_bits']
                    and member(slot_tuple(base, 0, t), ms)
                    for t in D.U)
            for q in itertools.product(*pools):
                if kind and D.kind(q[0]) != kind:
                    continue
                total += 1
                try:
                    ref = chain_fixpoint_slots(ctx, op0, list(q), ms)
                except E.EvalError:
                    continue
                if ref[0] == 'out_of_domain':
                    continue
                if ref[0] == 'cycle':
                    cyc += 1
                got = default_fixpoint_multi(
                    q, lambda t, _m=ms: member(t, _m), projs, elemwise,
                    has_class, follows, forced=size_ok,
                    mem_bits=ms[0]['size_bits'] if ms else None,
                    policies=pols)
                # Same escape rule as the single-slot model: no move by the
                # default agrees with any explicit non-Legal terminal.
                if got[0] == 'Unsupported' and ref[0] in ('Unsupported',
                                                          'Lower', 'Libcall',
                                                          'Custom'):
                    ok += 1
                    continue
                if ref[0] == 'cycle' and got[0] == 'Unsupported':
                    # The chain never settles, so there is no answer to
                    # reproduce -- not evidence for either side.  Counted in
                    # `no answer`, kept out of the denominator.
                    continue
                if ref[0] == got[0] and (ref[1] is None or ref[1] == got[1]):
                    ok += 1
                else:
                    label = 'x'.join(q) if not ms else '%s|mem %s' % (
                        'x'.join(q), ','.join('%s/%d' % (m['type'],
                                                         m['size_bits'])
                                              for m in ms))
                    bad.append((C.fmt(env), label, ref[:2], got, ref[2]))
    return total, ok, bad, '', cyc


def chain_arity(doc, ops):
    """How many type slots the *chain* reads, independent of the .decl.

    Arity used to come from the `.decl` alone (`C.slot_count` counts `ty(i)`
    mentions), so deleting a slot-1 rule shrank the query space and hid the
    loss -- the same self-concealing pool infer_diff fixed by seeding kinds
    from the chain.  Here it is the slot count: G_SHL has no `ty(1)` left
    after its two slot-1 rules are deleted, but the chain still reads slot 1.
    """
    names = set(o.strip() for o in ops.split(','))
    n = 1
    leaves = ('typeIs', 'typeInSet', 'typeIsNot', 'scalarNarrowerThan',
              'scalarWiderThan', 'elementTypeIs')
    pat = r'"(%s)", "args": \[\s*(\d+)' % '|'.join(leaves)
    for rs in doc['rulesets']:
        if not names & set(rs.get('opcodes') or []):
            continue
        blob = json.dumps(rs.get('rules') or [])
        for m in re.finditer(pat, blob):
            n = max(n, int(m.group(2)) + 1)
        for m in re.finditer(r'"target":\s*(\d+)', blob):
            n = max(n, int(m.group(1)) + 1)
        for r in rs.get('rules') or []:
            if r.get('type_idx') is not None:
                n = max(n, int(r['type_idx']) + 1)
    return n


def run(decl, only=None, verbose=False, kind=None, only_class=False):
    """kind: None (all of U), 'scalar', or 'vector'.

    Restricting the universe is what makes the number readable: "the default
    explains 100% of the scalar points in an integer rule set" and "40% of the
    vector points" are two different design statements, and averaging them
    hides the argument the RFC has to make.
    """
    D.set_universe(json.load(open(ROOT + '/data/x86-core.json'))['domain']['types'])
    macros, blocks = D.parse(decl)
    doc = json.load(open(ROOT + '/data/x86-core.json'))
    HFEATS = I.hasfeature_strings(doc)
    composed = HFEATS - set(I.C_FEAT)
    _dom, ar = C.domain(blocks[0], None) if blocks else ({}, 0)
    rows = []
    dropped[0] = 0
    merged[0] = 0
    mode_out[0] = 0
    raw[0] = 0
    realized[0] = 0
    cycles[0] = 0
    for b in blocks:
        ops = b['ops']
        op0 = ops.split(',')[0].strip()
        if only and only not in ops:
            continue
        if only_class and not any(st.startswith('class ') for st in b['stmts']):
            # The language's scalar default is opt-in (`class int;`) because
            # LLT cannot tell s32 from float.  Measuring the policy on a block
            # that does not declare a class measures an error the language
            # already forbids: on G_FADD, promote turns half into s32 while the
            # chain lowers.  The self-test scopes itself to where it applies.
            continue
        arity = max(C.slot_count(b, C.domain(b, None)[1]),
                    chain_arity(doc, b['ops']))
        atoms = C.feature_atoms_of(b, macros)
        # With the reachability filter the product is built only to be pruned:
        # G_MUL mentions 10 features (1024 assignments) and 13 of them can
        # occur.  The cap is on the product, so it has to be raised together
        # with the filter, or the filter would *reduce* coverage -- it did:
        # G_MUL disappeared from the report the first time this ran.
        cap = 65536 if MEASURE['realizable'] else I.MAX_COMBOS
        if 2 ** len(atoms) > cap:
            rows.append((ops, None, None, None, '%d feature atoms' % len(atoms)))
            continue
        envs = [dict(zip(atoms, v))
                for v in itertools.product([False, True], repeat=len(atoms))]
        raw[0] += len(envs)
        before = len(envs)
        envs = [e for e in envs if e.get('Is64Bit', True)]
        mode_out[0] += before - len(envs)
        n_all = len(envs)
        if MEASURE['realizable']:
            envs = [e for e in envs if realizable(e)]
        n_real = len(envs)
        realized[0] += n_real
        # Environments the block cannot tell apart are the same question, so
        # they are asked once.  This is the environment half of the same
        # quotient `rule_atoms.py` does on the type axis.
        envs = C.env_classes(b, envs, macros, range(arity))
        merged[0] += n_real - len(envs)
        dropped[0] += n_all - n_real
        elemwise = any(st == 'elemwise' for st in b['stmts'])
        has_class = any(st.startswith('class ') for st in b['stmts'])
        if arity > 1 and not MULTI['on']:
            rows.append((ops, None, None, None,
                         'multi-slot, not modelled here'))
            continue
        if arity > 1:
            total, ok, bad, why, cyc = score_block_multi(
                b, op0, envs, doc, composed, macros, kind, has_class)
            if why:
                rows.append((ops, None, None, None, why))
                continue
            declared = 'class' if has_class else 'no class'
            choice, alt_ok = declared, ok
            if MEASURE['optin'] and bad:
                # The same block-level question as slot 0: would *declaring*
                # a class reproduce it?  G_FCMP is the clean case -- its only
                # rule is `s8 on 0 when bits < 8`, which is the class floor.
                t2, o2, b2, _w2, cyc2 = score_block_multi(
                    b, op0, envs, doc, composed, macros, kind, not has_class)
                if o2 > ok:
                    total, ok, bad, cyc = t2, o2, b2, cyc2
                    choice, alt_ok = ('no class' if has_class else 'class'), ok
            cycles[0] += cyc
            CHOICE[ops] = (declared, choice, ok, alt_ok)
            rows.append((ops, total, ok, bad, ''))
            continue
        total, ok, bad, cyc = score_block(b, op0, envs, doc, composed, macros,
                                          kind, has_class)
        declared = 'class' if has_class else 'no class'
        choice, alt_ok = declared, ok
        if MEASURE['optin']:
            # The language makes the width lattice opt-in (`class int;`): a
            # block that declares nothing is not on a lattice, and the model
            # must be allowed to say so.  Asking only "what happens if the
            # policy always fires" measures a design the language forbids --
            # it is what puts 182 wrong points on the five float rule sets.
            # So the unit of the claim is the block: does *some* declaration
            # reproduce it?  Report which one.
            t2, o2, b2, cyc2 = score_block(b, op0, envs, doc, composed, macros,
                                          kind, not has_class)
            alt_ok = o2
            if o2 > ok:
                total, ok, bad, cyc = t2, o2, b2, cyc2
                choice = 'no class' if has_class else 'class'
            if has_class and o2 > ok:
                print('  WARN %s declares a class but the default reproduces it '
                      'only without one (%d vs %d)' % (ops[:40], o2, ok))
        CHOICE[ops] = (declared, choice, ok, alt_ok)
        cycles[0] += cyc
        rows.append((ops, total, ok, bad, None))
    return rows


def score(rows):
    return sum(r[2] or 0 for r in rows if r[1])


def frac(rows):
    tot = sum(r[1] for r in rows if r[1])
    ok = sum(r[2] or 0 for r in rows if r[1])
    return ok, tot


def self_test(decl):
    """Every policy decision must be load-bearing somewhere.

    Not on the total: `promote` is net-negative over the whole corpus, because
    on a float rule set it turns `half` into s32 while the chain lowers, and
    that is exactly why the language makes the default opt-in (`class int;`).
    The question "is this decision doing work" is per rule set, so it is
    answered per rule set: turning it off must lower at least one block.
    """
    # The policy loop is the slot-0 corpus: that is where these decisions
    # act, and running the 34 multi-slot rule sets twenty times would make
    # the self-test unusable.  The multi-slot model gets its own cases below.
    MULTI['on'] = False
    base = run(decl)
    base_ok = {r[0]: (r[2] or 0) for r in base if r[1]}
    fails = 0
    b_ok, b_tot = frac(base)
    print('  baseline   %d / %d points explained' % (b_ok, b_tot))
    for key in sorted(POLICY):
        hit = None
        for mode in (True, False):
            MEASURE['optin'] = mode
            b2 = run(decl)
            ok2 = {r[0]: (r[2] or 0) for r in b2 if r[1]}
            POLICY[key] = False
            try:
                off = run(decl)
            finally:
                POLICY[key] = True
            off_ok = {r[0]: (r[2] or 0) for r in off if r[1]}
            hurts = [k for k in ok2 if off_ok.get(k, 0) < ok2[k]]
            helps = [k for k in ok2 if off_ok.get(k, 0) > ok2[k]]
            print('  policy %-9s off -> %d / %d (%s); hurts %d block(s), helps %d'
                  % (key, sum(off_ok.values()), b_tot,
                     'opt-in' if mode else 'unconditional', len(hurts), len(helps)))
            if hurts and hit is None:
                hit = mode
        MEASURE['optin'] = True
        if hit is None:
            print('  FAIL turning %r off lowered no block in either mode; the '
                  'decision is dead' % key)
            fails += 1
    # Different domain, so compare fractions: the filter removes points from
    # the denominator as well, and raw counts across domains say nothing.
    MEASURE['realizable'] = False
    try:
        n_ok, n_tot = frac(run(decl))
    finally:
        MEASURE['realizable'] = True
    print('  domain   all-envs -> %d / %d (%.1f%%), reachable %d / %d (%.1f%%)'
          % (n_ok, n_tot, 100.0 * n_ok / max(n_tot, 1),
             b_ok, b_tot, 100.0 * b_ok / max(b_tot, 1)))
    if n_ok * b_tot >= b_ok * n_tot:
        print('  FAIL turning reachability off changed nothing; the filter is '
              'not what makes the difference')
        fails += 1
    # The implication table is generated (tools/gen_implies.py); these two
    # cases say it is present, and that it is what the reachability filter
    # stands on.  Empty it and the impossible env below becomes "real".
    closure.__defaults__[0].clear()
    if not (realizable({'HasBWI': True, 'HasAVX512': True})
            and not realizable({'HasBWI': True, 'HasAVX512': False})):
        print('  FAIL the implication table does not say BWI needs AVX512')
        fails += 1
    saved = dict(IMPLIES)
    IMPLIES.clear()
    closure.__defaults__[0].clear()
    try:
        leaked = realizable({'HasBWI': True, 'HasAVX512': False})
    finally:
        IMPLIES.update(saved)
        closure.__defaults__[0].clear()
    if not leaked:
        print('  FAIL without the table an impossible env is still rejected; '
              'the filter is not what rejects it')
        fails += 1
    print('  implies  %d features, 2 cases' % len(IMPLIES))
    # --- multi-slot -----------------------------------------------------
    # Control 1: the old behaviour is the control group.  Turn the new path
    # off and the corpus must fall back to 34 "not measured" and MULTI_OFF
    # points.  If it does not, the new path is not what makes the difference.
    # MULTI_OFF moves whenever the environment axis changes (it was 3240
    # before the env quotient and 1656 after); the control is that it stays
    # put, not that it equals some era's number.
    old = run(decl)
    MULTI['on'] = True
    old_pts = frac(old)[1]
    not_measured = len([r for r in old if not r[1]])
    print('  multi    off -> %d not measured, %d points' % (not_measured, old_pts))
    if not_measured != 34 or old_pts != MULTI_OFF:
        print('  FAIL with the multi-slot path off the corpus is not %d '
              'points (got %d / %d)' % (MULTI_OFF, not_measured, old_pts))
        fails += 1
    # Control 2: the legal relation is what the projections are made of.
    # Replace them with "any type" and G_SHL must lose points: (s32, s32) has
    # no default move without the relation, and the chain answers (s32, s8).
    shl = [r for r in run(decl, only='G_SHL') if r[1]][0]
    RELATION['on'] = False
    try:
        shl_off = [r for r in run(decl, only='G_SHL') if r[1]][0]
    finally:
        RELATION['on'] = True
    print('  relation G_SHL   off -> %d / %d (was %d / %d)'
          % (shl_off[2], shl_off[1], shl[2], shl[1]))
    if not shl_off[2] < shl[2]:
        print('  FAIL without the legal relation G_SHL still scores the same; '
              'the projections are not doing the work')
        fails += 1
    # Control 3: the block-level declaration choice.  G_FCMP's only slot-0
    # rule is `s8 on 0 when bits < 8`, which is the class floor: scored as
    # declared (no class) it cannot move s1, scored with a class it can.
    doc = json.load(open(ROOT + '/data/x86-core.json'))
    macros, blocks = D.parse(decl)
    composed = I.hasfeature_strings(doc) - set(I.C_FEAT)
    b = [x for x in blocks if x['ops'].startswith('G_FCMP')][0]
    atoms = C.feature_atoms_of(b, macros)
    envs = [dict(zip(atoms, v))
            for v in itertools.product([False, True], repeat=len(atoms))]
    envs = [e for e in envs if e.get('Is64Bit', True) and realizable(e)]
    _t, declared_ok, _b, _w, _c = score_block_multi(
        b, 'G_FCMP', envs, doc, composed, macros, None, False)
    _t, class_ok, _b, _w, _c = score_block_multi(
        b, 'G_FCMP', envs, doc, composed, macros, None, True)
    print('  class    G_FCMP   as declared %d, with a class %d'
          % (declared_ok, class_ok))
    if not class_ok > declared_ok:
        print('  FAIL declaring a class does not improve G_FCMP; the '
              'declaration choice is not doing the work')
        fails += 1
    # The environment quotient is only sound if a representative answers for
    # its whole class.  Checked against the chain judge, not against the same
    # signature that built the class: asking the signature would be the
    # self-check comparing itself.
    doc = json.load(open(ROOT + '/data/x86-core.json'))
    macros, blocks = D.parse(decl)
    b = [x for x in blocks if x['ops'].startswith('G_FPEXT')][0]
    atoms = C.feature_atoms_of(b, macros)
    envs = [dict(zip(atoms, v))
            for v in itertools.product([False, True], repeat=len(atoms))]
    envs = [e for e in envs if e.get('Is64Bit', True) and realizable(e)]
    slots = list(range(C.slot_count(b, C.domain(b, None)[1])))
    reps = {C.env_signature(b, e, macros, slots): e
            for e in C.env_classes(b, envs, macros, slots)}
    bad = 0
    for e in envs:
        r = reps[C.env_signature(b, e, macros, slots)]
        for q in (['s16', 's32'], ['s80', 's64'], ['v4s32', 'v2s64']):
            a = chain_fixpoint_slots(E.Context(doc, I.features_for(e)),
                                     'G_FPEXT', q)[0]
            c = chain_fixpoint_slots(E.Context(doc, I.features_for(r)),
                                     'G_FPEXT', q)[0]
            bad += (a != c)
    print('  quotient G_FPEXT %d envs -> %d classes, %d representative '
          'disagreements' % (len(envs), len(reps), bad))
    if bad:
        print('  FAIL a representative does not answer for its class')
        fails += 1
    # The four counters must partition the powerset.  This is the check the
    # report could not make before: 1688 = 844 + 543 + 301 was three numbers
    # printed in two different places, and nothing compared them.
    run(decl)
    if raw[0] != mode_out[0] + dropped[0] + realized[0]:
        print('  FAIL domain counters do not partition the powerset: '
              '%d != %d + %d + %d' % (raw[0], mode_out[0], dropped[0],
                                      realized[0]))
        fails += 1
    if not (mode_out[0] and dropped[0] and realized[0]):
        print('  FAIL a domain counter is zero: raw %d, mode %d, implies %d, '
              'measured %d' % (raw[0], mode_out[0], dropped[0], realized[0]))
        fails += 1
    print('  domain   partition %d = %d + %d + %d'
          % (raw[0], mode_out[0], dropped[0], realized[0]))
    # The chain-side fixpoint in this file has the same no-progress rule as
    # infer_diff's; check the copy, not just the original.
    doc = json.load(open(ROOT + '/data/x86-core.json'))
    ctx = E.Context(doc, I.features_for({'HasPOPCNT': True, 'Is64Bit': True}))
    res = chain_fixpoint_slots(ctx, 'G_CTPOP', ['v16s16', 's16'])
    print('  progress G_CTPOP  no-op rule -> %s (want Unsupported)' % res[0])
    if res[0] != 'Unsupported':
        print('  FAIL the local fixpoint still reports %r for a no-op rule'
              % res[0])
        fails += 1
    print('infer_defaults self-test: %d policies + 1 domain + 2 implies + '
          '3 multi-slot + 1 progress, %d failed' % (len(POLICY), fails))
    return 1 if fails else 0


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('decl', nargs='?', default='2026-11-01-x86.decl')
    ap.add_argument('--op', default=None)
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--kind', choices=('scalar', 'vector'), default=None)
    ap.add_argument('--self-test', action='store_true')
    ap.add_argument('--unconditional', action='store_true',
                    help='apply the policy to every block, as if `class int;` '
                         'were not opt-in (the counterfactual)')
    ap.add_argument('--all-envs', action='store_true',
                    help='measure every feature assignment, including the ones '
                         'the target cannot be in')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test(a.decl)
    if a.all_envs:
        MEASURE['realizable'] = False
    if a.unconditional:
        MEASURE['optin'] = False
    rows = run(a.decl, a.op, a.verbose, a.kind)
    single = [r for r in rows if r[1]]
    full = [r for r in single if not r[3]]
    bad = [r for r in single if r[3]]
    tot = sum(r[1] for r in single)
    ok = sum(r[2] for r in single)
    tot -= cycles[0]
    print('%s: %d rule sets, %d fully explained by the default, '
          '%d need rules' % (a.decl, len(single), len(full), len(bad)))
    print('  points: %d / %d explained (%.1f%%)' % (ok, tot, 100.0 * ok / max(tot, 1)))
    if cycles[0]:
        print('  no answer: %d points where the chain itself cycles; they are '
              'left out of the denominator' % cycles[0])
    if MEASURE['realizable']:
        print('  domain: %d feature assignments dropped as unreachable '
              '(tools/x86-implies.json); use --all-envs to count them' % dropped[0])
        print('  quotient: %d more merged into a representative the block '
              'cannot tell apart' % merged[0])
        print('  arithmetic: %d raw = %d outside the measured mode + %d pruned '
              'by implies + %d measured'
              % (raw[0], mode_out[0], dropped[0], raw[0] - mode_out[0] - dropped[0]))
    if CHOICE and MEASURE['optin']:
        add = [k for k, (d, c, _on, _off) in CHOICE.items()
               if d == 'no class' and c == 'class']
        drop = [k for k, (d, c, _on, _off) in CHOICE.items()
                if d == 'class' and c == 'no class']
        for k in drop:
            print('  WARN %s declares a class but is reproduced without it'
                  % k.split(',')[0])
        print('  declaration: %d blocks reproduced by declining, %d by a class; '
              '%d would be explained by declaring one%s'
              % (sum(1 for d, c, _a, _b in CHOICE.values() if c == 'no class'),
                 sum(1 for d, c, _a, _b in CHOICE.values() if c == 'class'),
                 len(add),
                 (': ' + ', '.join(k.split(',')[0] for k in add)) if add else ''))
    rest = [r for r in rows if not r[1]]
    if rest:
        print('  not measured: %d rule sets (%s)' % (
            len(rest), ', '.join('%s: %s' % (r[0].split(',')[0], r[4]) for r in rest[:6])))
    for ops, total, n, b, _why in sorted(bad, key=lambda r: -len(r[3])):
        print('  %-38s %4d/%4d  %d unexplained' % (ops[:38], n, total, len(b)))
        if a.verbose or a.op:
            kinds = {}
            for env, t, ref, got, _mv in b:
                kinds.setdefault('%s: chain %s, default %s'
                                 % (D.kind(t), ref[0], got[0]), 0)
                kinds['%s: chain %s, default %s' % (D.kind(t), ref[0], got[0])] += 1
            for k, v in sorted(kinds.items(), key=lambda kv: -kv[1])[:4]:
                print('      %-42s %d' % (k, v))
            slots = {}
            for env, t, ref, got, mv in b:
                key = ('slot %d' % sorted(mv)[0] if len(mv) == 1 else
                       'slots %s' % ','.join(map(str, sorted(mv))) if mv
                       else 'chain did not move')
                slots[key] = slots.get(key, 0) + 1
            for k, v in sorted(slots.items(), key=lambda kv: -kv[1]):
                print('      %-42s %d' % (k, v))
            for env, t, ref, got, _mv in b[:6]:
                print('      e.g. %s %s: chain %s %s | default %s %s'
                      % (env, t, ref[0], ref[1], got[0], got[1]))
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
