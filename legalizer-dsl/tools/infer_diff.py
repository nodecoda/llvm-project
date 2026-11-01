#!/usr/bin/env python3
"""Differential: the chain's answer vs the declaration's inference.

The 209 rules that build_decl does not emit (`inferred` in its stderr) are
WidenScalar / NarrowScalar / MoreElements / FewerElements.  The claim is that
`D` + `prefer` + section 6's `step` reproduce them.  This measures it.

Judge side is tools/evaluate_core.py (independent of the emitter).  Model side
is tools/check_decl.py's step.  Query space is the type universe `U` for every
slot, under every feature assignment the block mentions.

Usage:
  python3 tools/infer_diff.py                     # every slot-0 ruleset
  python3 tools/infer_diff.py --op G_ADD -v
  python3 tools/infer_diff.py --all               # include 2-slot blocks
"""
import argparse
import itertools
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import check_decl as C
import decl_lib as D
import evaluate_core as E

# 64 was a runtime guard, not a semantic one: G_MUL mentions 9 features
# (2^9 assignments), so it was skipped outright and its rules were never
# measured.  The cap is now high enough to cover every block in the corpus.
MAX_COMBOS = 512
# Negative-control switch: see pinned_type and self_test.
PIN_CLASS = True


def hasfeature_strings(doc):
    """Every `hasFeature(<expr>)` string the corpus uses, composed or not."""
    out = set()

    def walk(p):
        if not isinstance(p, dict):
            return
        if p.get('kind') == 'leaf' and p.get('name') == 'hasFeature':
            out.add(p['args'][0])
        for a in (p.get('args') or []):
            walk(a)
        if p.get('arg'):
            walk(p['arg'])

    for rs in doc['rulesets']:
        for r in rs['rules']:
            walk(r.get('predicate') or {})
    return out


def features_for(env, composed=()):
    """short name (HasSSE2) -> the raw expression evaluate_core looks up.

    The corpus has feature strings that are not one of the atom names, e.g.
    `(!Subtarget.useSoftFloat() && Subtarget.hasX87() && !Subtarget.hasSSE1())`
    from X86_SITOFP's x87 path.  They are exactly the decl-level guards the
    generator writes (`UseX87 && !HasSSE1`), so evaluate that form against the
    same env instead of leaving the judge to raise unknown_feature.
    """
    if D.ENV['let']:
        # The environment now carries primitives (cmov, bwi, ...); the rules
        # still name the definitions (HasCMOV, HasBWI).  Resolve them once, in
        # the same place the judge looks values up, instead of at every use.
        env = dict(env)
        for name in D.ENV['let']:
            if name not in env:
                v = D.eval_bool(name, env)
                if v is not None:
                    env[name] = v
    feats = {}
    for raw, short in C_FEAT.items():
        feats[raw] = bool(env.get(short, False))
    for short, val in env.items():
        feats[short] = bool(val)
    for raw in composed:
        if raw in feats:
            continue
        v = C.eval_guard(C_FEAT_expr(raw), env, {}, {})
        if v is not None:
            feats[raw] = v
    return feats


def C_FEAT_import():
    import build_decl
    return build_decl.FEAT


C_FEAT = C_FEAT_import()


def C_FEAT_expr(raw):
    """The composed feature string in declaration vocabulary.

    build_decl.feat() emits Python `and`/`or` because it feeds the .decl
    printer; the evaluator here speaks the grammar's `&&`/`||`.
    """
    import build_decl
    return (build_decl.feat(raw)
            .replace(' and ', ' && ').replace(' or ', ' || '))


def mem_slots_of(block):
    """How many memdesc slots this block's queries need (max over clauses)."""
    n = 0
    for st in block['stmts']:
        if st.startswith('legal '):
            shape, _g = D.clause_of(st)
            for prod in D.products(shape):
                n = max(n, sum(1 for c in prod if c.strip().startswith('mem(')))
        for k in re.findall(r'mem\((\d+)\)', st):
            n = max(n, int(k) + 1)
    return n


def mem_pool(block, doc):
    """The memdescs to query, deduplicated by what this block can observe.

    A rule that compares whole types (`ty(0) != mem(0).ty`, G_STORE) needs one
    entry per distinct (type, size, align); a rule that only reads the width
    (`bits(mem(0).ty)`, G_LOAD & friends) is blind to the memory type, so
    s64 and p0:64 collapse to one representative.  That is the same
    observability argument the cell partition uses, applied to memdescs.
    """
    mems = doc['domain'].get('mems', [])
    reads_type = any(re.search(r'ty\(\d+\)\s*[!=]=\s*mem\(\d+\)\.ty', st)
                     for st in block['stmts'])
    def key(m):
        return ((m['type'], m['size_bits'], m['align_bits']) if reads_type
                else (D.bits(m['type']), m['size_bits'], m['align_bits']))
    seen, out = set(), []
    for m in mems:
        k = key(m)
        if k not in seen:
            seen.add(k)
            out.append(m)
    return out


HERE = os.path.dirname(os.path.abspath(__file__))
IMPLIES_PATH = os.path.join(HERE, 'x86-implies.json')


def load_implies(path=IMPLIES_PATH):
    if not os.path.exists(path):
        raise SystemExit('%s is missing; run tools/gen_implies.py' % path)
    with open(path) as f:
        return json.load(f).get('features') or {}


IMPLIES = load_implies()
REACHABLE = True          # compare only configurations the target can be in


def _closure(f, _memo={}):
    if f in _memo:
        return _memo[f]
    out = set()
    for g in IMPLIES.get(f, ()):
        out.add(g)
        out |= _closure(g)
    _memo[f] = out
    return out


def realizable(env):
    for f, on in env.items():
        if not on:
            continue
        for g in _closure(f):
            if env.get(g) is False:
                return False
    return True


CHAIN_KINDS = True


def chain_query_kinds(doc, ops):
    """Type kinds the *chain* talks about, independent of what the .decl spells.

    The query pool used to be drawn from the kinds the declaration mentions.
    That makes a rule self-concealing: delete it and the kind disappears, so
    the queries that would have exposed the loss disappear with it.  The chain
    is the side that cannot be edited to hide a hole, so it seeds the pool.
    """
    names = set(o.strip() for o in ops.split(','))
    kinds = set()
    for rs in doc['rulesets']:
        if not names & set(rs.get('opcodes') or []):
            continue
        blob = json.dumps(rs.get('rules') or [])
        for m in re.finditer(r'"elementTypeIs", "args": \[\d+, "([^"]+)"', blob):
            kinds.add('ptrvector' if re.search(r'p\d+:', m.group(1)) else 'vector')
    return kinds


def pinned_type(block, slot):
    """The single type every legal clause pins `slot` to, else None.

    G_LOAD/G_STORE have no rules for slot 1 at all: every legal clause says
    `ty(1) = p0:64` and no prefer targets it.  Enumerating slot 1 over all of
    U would quadruple the run to test a slot the chain never reads.  A pinned
    slot is queried at its pinned type only, and the report says so.
    """
    pinned = set()
    for st in block['stmts']:
        if not st.startswith('legal '):
            continue
        shape, guard = D.clause_of(st)
        m = re.search(r'ty\((\d+)\)\s*=\s*([\w:]+)', guard or '')
        by_guard = m.group(2) if m and int(m.group(1)) == slot else None
        by_shape = None
        if shape.strip() != 'any':
            for prod in D.products(shape):
                ty_comps = [c.strip() for c in prod if not c.strip().startswith('mem(')
                            and c.strip() not in ('_', '')]
                if len(ty_comps) > slot:
                    c = ty_comps[slot]
                    if c != 'any':
                        ts = D.types_of(c)
                        if len(ts) == 1:
                            by_shape = next(iter(ts))
        cand = {t for t in (by_guard, by_shape) if t}
        if not cand:
            return None
        pinned |= cand
    if len(pinned) != 1:
        return None
    # A declaration without a slot is the default rule and rewrites every
    # slot; a group names the slots it covers.  Both unpin, so the check goes
    # through decl_slots rather than re-reading the text with a slot-0 guess.
    for pss, _kind, _tgt, _g in C.prefers_of(block):
        if pss is None or slot in pss:
            return None
    # v7 `policy` rewrites the slot exactly like a prefer does, so it unpins it
    # for the same reason: G_SELECT's slot 1 went 24 values -> 1 when the two
    # prefers became a policy, and the differential stopped asking.
    for idx, st in enumerate(block['stmts']):
        if not st.startswith(('policy ', 'policy:', 'shapes')):
            continue
        ss = C.decl_slots(block, idx)
        if ss is None or slot in ss:
            return None
    # A `class` default and `elemwise;` rewrite slot 0 even though neither is
    # a `prefer`.  Pinning slot 0 after the G_ICMP fold cut its query space
    # 256 -> 32 and the differential kept saying 0 because it only ever asked
    # about s8.  `elemwise` is the same shape: a block whose legal clauses pin
    # slot 0 and whose only slot-0 rule is the default would never be asked
    # about a vector at all.
    if slot == 0:
        if any(st == 'elemwise' for st in block['stmts']):
            return None
        if PIN_CLASS and any(st.startswith('class ') for st in block['stmts']):
            return None
    for lhs, rhs, _k in C.follows_of(block):
        if slot in (lhs, rhs):
            return None
    return next(iter(pinned))


def model_fixpoint(block, env, qtypes, macros, mems=None, steps=8):
    """What the composed answer is: run C.step until legal or stuck.

    There used to be a second `step` in this file and a third in check 9;
    both drifted from the one in tools/check_decl.py.  The differential is
    only worth its name if the model it measures is the one the checker
    enforces, so the query is replayed with that same function.
    """
    types = list(qtypes)
    for _ in range(steps):
        act, i, new = C.step(block, env, types, macros, mems)
        if act == 'Legal':
            return ('Legal', tuple(types))
        if new is None:
            # No rule moved the tuple.  `step` already ran the escape clauses
            # over the whole query (G_BUILD_VECTOR's customIf reads ty(0)),
            # so this only restates a decided escape and otherwise reports
            # that the chain has nothing to say.
            tmap = {i: t for i, t in enumerate(types)}
            return (C.except_action(block, env, tmap, macros, mems)
                    or 'Unsupported', None)
        if new == types[i]:
            # Same rule as the chain side: no progress is a failure, not a
            # cycle.
            return ('Unsupported', None)
        types[i] = new
    return ('cycle', None)


def chain_fixpoint(ctx, op, qtypes, mems=(), steps=8):
    types = list(qtypes)
    for _ in range(steps):
        r = E.evaluate(ctx, {'opcode': op, 'types': types, 'imms': [],
                             'mems': list(mems)})
        a = r.get('result')
        if a == 'out_of_domain':
            return ('out_of_domain', None)
        act = r.get('action')
        if act in ('Legal', 'Unsupported', 'Custom', 'Libcall', 'Lower'):
            return (act, tuple(types) if act == 'Legal' else None)
        nt, ti = r.get('new_type'), r.get('type_idx')
        if nt is None or ti is None:
            return (act, None)
        if nt == types[ti]:
            # A rule that rewrites a slot to the type it already has is not
            # progress.  The legalizer cannot legalize a query no rule moves,
            # so the answer is Unsupported, not "cycle" -- and looping eight
            # times to discover that only hid the fact.  The dump has one
            # such rule: X86_CTPOP's follow(0,1) extracted as
            # set_elem_bits(sameAsSlot1), a no-op on a vector (v16s16 stays
            # v16s16).
            return ('Unsupported', None)
        types[ti] = nt
    return ('cycle', None)


def action_of(old, new):
    if new is None or old == new:
        return 'None'
    if D.kind(old) != D.kind(new):
        return 'FewerElements' if D.kind(old) == 'vector' else 'MoreElements'
    if D.bits(new) > D.bits(old):
        return 'WidenScalar'
    if D.bits(new) < D.bits(old):
        return 'NarrowScalar'
    if D.lanes(new) > D.lanes(old):
        return 'MoreElements'
    if D.lanes(new) < D.lanes(old):
        return 'FewerElements'
    return 'Same'


def run(decl, only=None, verbose=False, max_arity=1, limit=0):
    D.set_universe(json.load(open(ROOT + '/data/x86-core.json'))['domain']['types'])
    macros, blocks = D.parse(decl)
    C.CLASS_CACHE.clear()
    doc = json.load(open(ROOT + '/data/x86-core.json'))
    HFEATS = hasfeature_strings(doc)
    rows, skipped = [], []
    for b in blocks:
        ops = b['ops']
        op0 = b['ops'].split(',')[0].strip()
        if only and only not in ops:
            continue
        composed = HFEATS - set(C_FEAT)
        atoms = C.feature_atoms_of(b, macros)
        if 2 ** len(atoms) > MAX_COMBOS:
            skipped.append((ops, '%d feature atoms' % len(atoms)))
            continue
        _dom, arity = C.domain(b, None)
        if arity > max_arity:
            skipped.append((ops, 'arity %d' % arity))
            continue
        # `domain` only counts slots a legal clause binds; G_LROUND binds none
        # (one prefer on slot 1 + a libcall) and every query used to be
        # 1-slot, so the judge answered bad_type_idx for the whole block.
        arity = max(C.slot_count(b, arity), 1)
        envs = [dict(zip(atoms, v))
                for v in itertools.product([False, True], repeat=len(atoms))]
        # data/x86-core.json bakes `sMaxScalar` to the x86-64 value (see the
        # note printed below), and its pointers are all p0:64, so a query set
        # is only meaningful at Is64Bit=1 until the dump is fixed.
        envs = [e for e in envs if e.get('Is64Bit', True)]
        if REACHABLE:
            envs = [e for e in envs if realizable(e)]
        kinds = {D.kind(u) for u in D.U if u in set(D.U)}
        dom0, _a0 = C.domain(b, None)
        kinds = {D.kind(u) for ts in dom0.values() for u in ts}
        for _ps, kind, tgt, guard in C.prefers_of(b):
            if kind == 'target':
                for t in D.types_of(tgt):
                    kinds.add(D.kind(t))
            kinds.update(re.findall(r'kind\(ty\(\d+\)\)\s*=\s*(\w+)', guard or ''))
        if CHAIN_KINDS:
            kinds |= chain_query_kinds(doc, ops)
        pool = [u for u in D.U if D.kind(u) in kinds]
        if not pool and C.trivially_total(b):
            # `legal any;` has no domain, so the kind filter has nothing to
            # work with -- and the block used to produce zero comparisons.
            pool = list(D.U)
        if not pool:
            # A rule set that mentions no type at all (G_MEMCPY's libcall,
            # G_SEXT_INREG's escapes) has nothing to filter by.  Querying all
            # of U is still a real check: it says the answer is the same
            # escape for every type, which is what the chain says.
            pool = list(D.U)
        # memdesc dimension: the block's rules only see memdescs through the
        # predicates they write, so the pool is deduplicated by exactly those
        # observables (mem_pool), and a slot the chain never reads is pinned
        # to its one legal type (pinned_type).
        mem_n = mem_slots_of(b)
        mp = mem_pool(b, doc) if mem_n else []
        if mem_n and not mp:
            skipped.append((ops, 'memdesc block, domain.mems is empty'))
            continue
        pools = []
        for i in range(arity):
            p = pinned_type(b, i)
            pools.append([p] if p else pool)
        n = 0
        for env in envs:
            ctx = E.Context(doc, features_for(env, composed))
            for q in itertools.product(*pools):
                for ms in itertools.product(mp, repeat=mem_n):
                    n += 1
                    if limit and n > limit:
                        break
                    try:
                        ref = chain_fixpoint(ctx, op0, list(q), ms)
                    except E.EvalError as exc:
                        skipped.append((ops, 'judge: %s' % exc.code))
                        break
                    if ref[0] == 'out_of_domain':
                        continue
                    got = model_fixpoint(b, env, list(q), macros, ms)
                    ok = ref[0] == got[0] and (ref[1] is None or ref[1] == got[1])
                    qtext = 'x'.join(q)
                    if ms:
                        qtext += '|mem ' + ','.join(
                            '%s/%d' % (m['type'], m['size_bits']) for m in ms)
                    rows.append((ops, C.fmt(env), qtext, ref[0], ref[1],
                                 got[0], got[1], ok, ''))
                if limit and n > limit:
                    break
            if limit and n > limit:
                break
    return rows, skipped


MUTANTS = [
    # (opcode, old, new, why)
    ('G_FCMP',
     '  prefer s8 on 0 when kind(ty(0)) = scalar && bits(ty(0)) < 8;\n',
     '',
     'drop the lower half of clampScalar<0, s8, s8>'),
    ('G_BUILD_VECTOR',
     '  except (HasSSE1 && ty(0) in (v4s32))',
     '  except true || (HasSSE1 && ty(0) in (v4s32))',
     'let the customIf escape fire unconditionally'),
    # NB: the `min N` of `prefer pow2 min N` is NOT observable on this
    # universe: domain.types has no non-power-of-two scalar below 128, so
    # max(nextPow2(x), N) is the same for every N <= 128.  G_ADD's `min 32`
    # and G_ANYEXT's `min 8` therefore cannot be mutation-tested from here.
    # That is a property of the query universe, not of the model; widening
    # data/x86-core.json's `domain.types` is the only way to close it.
    # The memdesc dimension.  Nothing above touches it: these two are why the
    # six access rule sets are no longer "uncovered".
    ('G_LOAD',
     'mem(1, 1)',
     'mem(4, 1)',
     'make the s8/1-bit load pattern ask for a 4-bit memory'),
    ('G_STORE',
     '  except ty(0) != mem(0).ty -> delegate(custom);\n',
     '',
     'drop the store value-differs-from-memory escape'),
    # G_ANYEXT is the block where pow2 is load-bearing: s80 -> s128 (legal),
    # so dropping pow2 lands on s64 instead.  Final legal types are compared,
    # not just the action.
    ('G_ANYEXT',
     '  class int pow2 8;\n',
     '  class int;\n',
     'drop pow2 from G_ANYEXT: s80 must reach the legal s128'),
    # The class default is a real rule now, not a comment: dropping it must
    # break G_ADD, and dropping s64 from the class must change where s128
    # lands (class -> s32, chain -> s64).
    ('G_ADD', '  class int pow2 32;\n', '',
     'drop the integer default from G_ADD'),
]

GLOBAL_MUTANTS = [
    ('class int = s8, s16, s32, s64;', 'class int = s8, s16, s32;',
     'drop s64 from the integer width class'),
]


def _mutate_op(text, op, old, new):
    """Replace `old` inside the one `op <op> {...}` block; None if absent."""
    out, cur, hit = [], None, False
    for line in text.splitlines(True):
        if line.startswith('op '):
            cur = line[3:line.index('{')].strip() if '{' in line else None
        if cur and op in [x.strip() for x in cur.split(',')] and old in line:
            line = line.replace(old, new)
            hit = True
        out.append(line)
    return ''.join(out) if hit else None


def self_test(decl):
    """Every mutant must produce disagreements, and the pristine file none.

    Without this, "0 disagreements" is just a number: the model could have
    been bent until it agreed with the judge on the one input we look at.
    """
    import tempfile
    fails = 0
    rows, _sk = run(decl, None, False, 2, 0)
    base = len([r for r in rows if not r[7]])
    print('  pristine   %5d comparisons, %d disagreements' % (len(rows), base))
    if base:
        fails += 1
    # The pool is seeded from the chain so that deleting a rule cannot delete
    # the queries that would catch it.  That seeding has to be load-bearing or
    # it is decoration: turn it off and the pointer-vector queries must vanish.
    global CHAIN_KINDS
    on, _ = run(decl, 'G_IMPLICIT_DEF', False, 1, 0)
    CHAIN_KINDS = False
    try:
        off, _ = run(decl, 'G_IMPLICIT_DEF', False, 1, 0)
    finally:
        CHAIN_KINDS = True
    print('  coverage   chain-seeded pool %d comparisons, decl-only %d'
          % (len(on), len(off)))
    if len(on) <= len(off):
        print('  FAIL the chain contributes no queries; a missing rule would '
              'take its own test with it')
        fails += 1
    # A rule whose mutation returns the type it already has is not progress.
    # LLVM's legalizer bails out; before this check the fixpoint spun eight
    # times and reported a cycle.  X86_CTPOP's follow(0,1) is extracted as
    # set_elem_bits(sameAsSlot1), which is exactly that on a vector.
    doc = json.load(open(os.path.join(ROOT, 'data/x86-core.json')))
    ctx = E.Context(doc, features_for({'HasPOPCNT': True, 'Is64Bit': True}))
    res = chain_fixpoint(ctx, 'G_CTPOP', ['v16s16', 's16'])
    print('  progress   no-op rule -> %s (want Unsupported)' % res[0])
    if res[0] != 'Unsupported':
        print('  FAIL a no-op mutation reported %r; the fixpoint is still '
              'spinning instead of failing like the legalizer' % res[0])
        fails += 1
    text = open(decl).read()
    for op, old, new, why in MUTANTS:
        mut = _mutate_op(text, op, old, new)
        if mut is None:
            print('  FAIL mutation did not apply: %s' % why)
            fails += 1
            continue
        with tempfile.NamedTemporaryFile('w', suffix='.decl', delete=False) as f:
            f.write(mut)
            path = f.name
        # max_arity=2: a mutant of a 2-slot block (every memdesc block is one)
        # would otherwise produce no comparisons at all and pass vacuously.
        rows, _sk = run(path, op, False, 2, 0)
        os.unlink(path)
        bad = len([r for r in rows if not r[7]])
        print('  mutant     %-52s %4d comparisons, %d disagreements'
              % (why[:52], len(rows), bad))
        if not rows:
            print('  FAIL mutant produced no comparisons'); fails += 1
        elif not bad:
            print('  FAIL mutant not detected'); fails += 1
    for old, new, why in GLOBAL_MUTANTS:
        if old not in text:
            print('  FAIL global mutation did not apply: %s' % why)
            fails += 1
            continue
        mut = text.replace(old, new, 1)
        with tempfile.NamedTemporaryFile('w', suffix='.decl', delete=False) as f:
            f.write(mut)
            path = f.name
        rows, _sk = run(path, None, False, 2, 0)
        os.unlink(path)
        bad = len([r for r in rows if not r[7]])
        print('  mutant     %-52s %4d comparisons, %d disagreements'
              % (why[:52], len(rows), bad))
        if not rows or not bad:
            print('  FAIL global mutant not detected'); fails += 1

    # Coverage control: if the class default is ignored when deciding that a
    # slot is never rewritten, G_ICMP's slot 0 is pinned to s8 and the query
    # space collapses.  "0 disagreements" over a shrunken space is exactly the
    # failure this catches.
    global PIN_CLASS
    PIN_CLASS = False
    rows, _sk = run(decl, 'G_ICMP', False, 2, 0)
    bug = len(rows)
    PIN_CLASS = True
    rows, _sk = run(decl, 'G_ICMP', False, 2, 0)
    fixed = len(rows)
    print('  coverage   class-blind pin %4d comparisons, class-aware %4d'
          % (bug, fixed))
    if not bug < fixed:
        print('  FAIL the class default does not widen the query space'); fails += 1
    # Same lesson for `elemwise;`, as a unit case: the corpus has no block
    # whose legal clauses pin slot 0 while the default is its only slot-0 rule,
    # so a whole-decl control cannot see this.  Removing the branch above makes
    # this return s8 and the check fails.
    if pinned_type({'ops': 'G_T', 'stmts': ['legal s8', 'elemwise']}, 0) is not None:
        print('  FAIL a pinned slot ignores elemwise; its query space would collapse')
        fails += 1

    # The memdesc align is a *lower bound* (TypePairAndMemDesc::isCompatible,
    # memSize).  No .decl mutation can change that comparison, so mutate the
    # model itself: a strict `>` must break G_LOAD.  Without this, "align is a
    # lower bound" would be an assertion nobody checks.
    orig_comp = C.mem_comp_holds

    def strict_align(comp, mem):
        if comp.startswith('mem('):
            inner = comp[len('mem('):-1]
            args = [a.strip() for a in inner.split(',')]
            if len(args) == 2 and all(a.isdigit() for a in args):
                return (mem['size_bits'] == int(args[0]) and
                        mem['align_bits'] > int(args[1]))
        return orig_comp(comp, mem)

    C.mem_comp_holds = strict_align
    try:
        rows, _sk = run(decl, 'G_LOAD', False, 2, 0)
    finally:
        C.mem_comp_holds = orig_comp
    bad = len([r for r in rows if not r[7]])
    print('  model mutant read `align >= N` as `align > N`%s %4d comparisons, '
          '%d disagreements' % (' ' * 8, len(rows), bad))
    if not bad:
        print('  FAIL the align lower bound is not observable'); fails += 1
    print('infer_diff self-test: %d checks, %d failed'
          % (len(MUTANTS) + len(GLOBAL_MUTANTS) + 4, fails))
    return 0 if not fails else 1


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument('decl', nargs='?', default='2026-11-01-x86.decl')
    ap.add_argument('--op', default=None)
    ap.add_argument('--all', action='store_true')
    ap.add_argument('--all-envs', action='store_true',
                    help='also compare feature assignments the target cannot '
                         'be in (counterfactual: `shapes` is verified on the '
                         'reachable domain)')
    ap.add_argument('-v', '--verbose', action='store_true')
    ap.add_argument('--limit', type=int, default=0)
    ap.add_argument('--self-test', action='store_true')
    a = ap.parse_args(argv)
    if a.self_test:
        return self_test(a.decl)
    global REACHABLE
    if a.all_envs:
        REACHABLE = False
        _closure.__defaults__[0].clear()
    rows, skipped = run(a.decl, a.op, a.verbose, 2 if a.all else 1, a.limit)
    bad = [r for r in rows if not r[7]]
    by_op = {}
    for r in rows:
        d = by_op.setdefault(r[0], [0, 0])
        d[0 if r[7] else 1] += 1
    print('%s: %d comparisons, %d disagreements' % (a.decl, len(rows), len(bad)))
    for ops, (ok, no) in sorted(by_op.items(), key=lambda kv: -kv[1][1])[:15]:
        print('  %-42s %5d ok  %4d bad' % (ops[:42], ok, no))
    if a.verbose or a.op:
        for r in bad[:20]:
            print('  BAD %s %s %s: chain %s %s | model %s %s'
                  % (r[0][:28], r[1][:44], r[2], r[3], r[4], r[5], r[6]))
    for ops, why in skipped[:10]:
        print('  SKIP %s: %s' % (ops[:40], why))
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
