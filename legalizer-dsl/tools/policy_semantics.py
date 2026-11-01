#!/usr/bin/env python3
"""Semantics of `policy i: ...` (v7.2), as a reference implementation.

The policy is a *destination*, not a rule: it answers "given this query, what
is the one type this slot should be", so that a fixpoint is reachable in one
step.  The chain may reach the same destination through several builders in a
particular order; the contract is the fixpoint, and `--self-test` pins the
cases the X86 corpus actually contains.

    policy 0: scalar to s8;                  -- clamp both ends to s8
    policy 1: scalar [s8, MaxScalar] pow2;   -- nominal interval, pow2 floor
    policy 0: scalar promote s32 pow2 16;    -- one-sided (chain: minScalar)
    policy 0: scalar truncate s64;           -- one-sided (chain: maxScalar)
    policy 0: scalar hull pow2 32;           -- v6 `class int pow2 32`
    policy 0: vector hull;                   -- v6 `shapes on 0`
"""
import sys


class PolicyError(Exception):
    pass


def bits(t):
    i = t.find('s', 1) if t.startswith('v') else 0
    return int(t[i + 1:]) if t.startswith(('s', 'v')) else None


def kind(t):
    return 'pointer' if t.startswith('p') else ('vector' if t.startswith('v') else 'scalar')


def elem(t):
    if not t.startswith('v'):
        return t
    return t[t.index('s'):] if 's' in t[1:] else t[t.index('p'):]


def lanes(t):
    return int(t[1:t.index('s')]) if t.startswith('v') and 's' in t[1:] else 1


def is_pow2(n):
    return n > 0 and (n & (n - 1)) == 0


def next_pow2(n):
    p = 1
    while p < n:
        p <<= 1
    return p


class Policy:
    """lo/hi are type strings or None (open end); `hull` reads them off legal."""

    def __init__(self, kind_, lo=None, hi=None, hull=False, pow2=False,
                 pow2_floor=None, guard=True):
        self.kind = kind_
        self.lo, self.hi = lo, hi
        self.hull = hull
        self.pow2 = pow2
        self.pow2_floor = pow2_floor
        self.guard = guard

    def __repr__(self):
        b = 'hull' if self.hull else '[%s, %s]' % (self.lo, self.hi)
        return 'policy(%s %s%s)' % (self.kind, b,
                                    ' pow2 %s' % (self.pow2_floor or '')
                                    if self.pow2 else '')


def hull_of(legal, k):
    """Narrowest and widest legal type of this kind.

    Gaps are allowed and deliberate: G_FCMP's legal slot 0 is {s8,s32,s64,s80}
    and `hull` is still well defined, because both endpoints are legal by
    construction.  An *empty* projection has no endpoint at all -- that is the
    case v6 answered with a silent "no move", and it is an error here.
    """
    # A vector hull is ordered by lane count, not by the element width: every
    # v4s32/v8s32 has bits == 32, and sorting by bits made the bounds depend on
    # set iteration order (the self-test caught it).
    same = sorted((t for t in legal if kind(t) == k),
                  key=lanes if k == 'vector' else bits)
    if not same:
        raise PolicyError('no legal %s in the projection: hull has no bounds' % k)
    return same[0], same[-1]


def step(t, legal, pol):
    """One policy step. -> ('Legal', None) | ('Mutate', new_t) | ('NoMove', None)"""
    if t in legal:
        return ('Legal', None)
    if not pol.guard:
        return ('NoMove', None)
    k = kind(t)
    if k != pol.kind:
        return ('NoMove', None)

    if pol.hull:
        lo, hi = hull_of(legal, k)
    else:
        lo, hi = pol.lo, pol.hi

    if k == 'scalar':
        b = bits(t)
        if lo is not None and b < bits(lo):
            return ('Mutate', lo)
        if hi is not None and b > bits(hi):
            return ('Mutate', hi)
        if pol.pow2 and not is_pow2(b):
            floor = max(bits(lo) if lo else 1, pol.pow2_floor or 1)
            n = max(next_pow2(b), floor)
            if hi is not None:
                n = min(n, bits(hi))
            return ('Mutate', 's%d' % n)
        return ('NoMove', None)

    if k == 'vector':
        if pol.pow2:
            raise PolicyError('pow2 is a scalar width; a vector is on the lane lattice')
        n = lanes(t)
        if lo is not None and n < lanes(lo):
            return ('Mutate', 'v%d%s' % (lanes(lo), elem(t)))
        if hi is not None and n > lanes(hi):
            return ('Mutate', 'v%d%s' % (lanes(hi), elem(t)))
        return ('NoMove', None)

    return ('NoMove', None)


def fixpoint(t, legal, pol, steps=8):
    cur = t
    for _ in range(steps):
        act, new = step(cur, legal, pol)
        if act != 'Mutate':
            return act, cur
        cur = new
    raise PolicyError('policy did not settle in %d steps from %s' % (steps, t))


# ---------------------------------------------------------------- self-test
CASES = []


def case(name, t, legal, pol, want_act, want_ty):
    CASES.append((name, t, legal, pol, want_act, want_ty))


# G_FCMP: legal slot 0 = {s8,s32,s64,s80}; the chain clamps to a point.
FCMP = {'s8', 's32', 's64', 's80'}
case('fcmp wide', 's128', FCMP, Policy('scalar', lo='s8', hi='s8'), 'Legal', 's8')
case('fcmp legal stays', 's32', FCMP, Policy('scalar', lo='s8', hi='s8'), 'Legal', 's32')

# G_ANYEXT slot 1: no legal projection at all; only a nominal bound can answer.
case('anyext src s1', 's1', set(), Policy('scalar', lo='s8', hi='s64'), 'NoMove', 's8')
case('anyext src s128', 's128', set(), Policy('scalar', lo='s8', hi='s64'), 'NoMove', 's64')
case('anyext src s16', 's16', set(), Policy('scalar', lo='s8', hi='s64'), 'NoMove', 's16')

# G_MERGE_VALUES slot 0: pow2 floor 16, nominal floor 32 (the two differ!).
MERGE = Policy('scalar', lo='s32', pow2=True, pow2_floor=16)
case('merge s4', 's4', set(), MERGE, 'NoMove', 's32')
case('merge s16', 's16', set(), MERGE, 'NoMove', 's32')
case('merge s24', 's24', set(), MERGE, 'NoMove', 's32')

# One-sided truncate (G_SELECT: chain says sMaxScalar, not the hull's s80).
case('select wide', 's128', {'s8', 's32', 's64', 's80'},
     Policy('scalar', hi='s64'), 'Legal', 's64')
case('select s32', 's32', {'s8', 's32', 's64', 's80'},
     Policy('scalar', hi='s64'), 'Legal', 's32')

# hull: endpoints come from the legal set, gaps allowed.
case('hull narrow', 's1', {'s8', 's16', 's32', 's64'},
     Policy('scalar', hull=True, pow2=True), 'Legal', 's8')
case('hull wide', 's128', {'s8', 's16', 's32', 's64'},
     Policy('scalar', hull=True), 'Legal', 's64')

# guard off: the policy is inert in this feature assignment.
case('guard off', 's128', FCMP, Policy('scalar', lo='s8', hi='s8', guard=False),
     'NoMove', 's128')

# vector: lane clamp, no pow2.
case('vec lanes low', 'v2s32', {'v4s32', 'v8s32'}, Policy('vector', hull=True),
     'Legal', 'v4s32')
case('vec lanes high', 'v16s32', {'v4s32', 'v8s32'}, Policy('vector', hull=True),
     'Legal', 'v8s32')


def self_test():
    bad = 0
    for name, t, legal, pol, want_act, want_ty in CASES:
        try:
            act, ty = fixpoint(t, legal, pol)
        except PolicyError as E:
            print('FAIL %-18s raised %s' % (name, E))
            bad += 1
            continue
        if (act, ty) != (want_act, want_ty):
            print('FAIL %-18s got (%s, %s) want (%s, %s)' %
                  (name, act, ty, want_act, want_ty))
            bad += 1
    # Negative control: an empty hull must raise, not silently decline.
    try:
        hull_of(set(), 'scalar')
        print('FAIL empty-hull      did not raise')
        bad += 1
    except PolicyError:
        pass
    # Negative control: vector pow2 is a category error.
    try:
        step('v4s32', set(), Policy('vector', pow2=True))
        print('FAIL vector-pow2     did not raise')
        bad += 1
    except PolicyError:
        pass
    print('%d checks, %d failed' % (len(CASES) + 2, bad))
    return 1 if bad else 0


if __name__ == '__main__':
    if len(sys.argv) == 2 and sys.argv[1] == '--self-test':
        sys.exit(self_test())
    print(__doc__)
