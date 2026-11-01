#!/usr/bin/env python3
"""Reading a .decl file: shapes, candidate domains, feature predicates.

One copy, not two.  check_domain.py and check_decl.py used to carry separate
copies of `products`, and they disagreed with the comment above the code:

    legal p0:64, s1, s8, s16, s32, s64;      # one slot, six candidate types

`products` returned one *product of six components* = six slots, so the six
types landed in six different slots and the union was lost.  check_domain.py
did not notice (every slot was non-empty either way); check_decl.py did, as a
pile of false `6-prefer-target` errors.  The shape rule is semantics 3.6:

    top-level `,`   union into one slot
    `,` in `(...)`  product: one component per slot
    `*`             product
    `mem(...)`      binds a memdesc, not a ty slot

The module keeps one piece of state, the type universe U, filled by
`set_universe` before anything that needs it.
"""
import re
import sys

TY = re.compile(r's\d+|p\d+:\d+|v\d+s\d+|v\d+p\d+:\d+')
QUERY = ('ty(', 'mem(', 'bits(', 'lanes(', 'elem(', 'kind(', 'addrspace(', 'opc')

U = []


def set_universe(types):
    global U
    U = list(types)


def bits(t):
    m = re.fullmatch(r'p\d+:(\d+)', t)
    if m:
        return int(m.group(1))
    m = re.fullmatch(r'v(\d+)s(\d+)', t)
    if m:
        return int(m.group(1)) * int(m.group(2))
    m = re.fullmatch(r'v(\d+)p\d+:(\d+)', t)
    if m:
        return int(m.group(1)) * int(m.group(2))
    m = re.fullmatch(r's(\d+)', t)
    return int(m.group(1)) if m else -1


def elem(t):
    """Element type of a vector, or the type itself."""
    m = re.fullmatch(r'v\d+(s\d+|p\d+:\d+)', t)
    return m.group(1) if m else t


def kind(t):
    if re.fullmatch(r's\d+', t):
        return 'scalar'
    if re.fullmatch(r'p\d+:\d+', t):
        return 'pointer'
    if re.fullmatch(r'v\d+s\d+', t):
        return 'vector'
    if re.fullmatch(r'v\d+p\d+:\d+', t):
        return 'ptrvector'
    return '?'


def split_top(s, sep):
    out, d, cur = [], 0, ''
    for ch in s:
        if ch in '([':
            d += 1
        elif ch in ')]':
            d -= 1
        if ch == sep and d == 0:
            out.append(cur)
            cur = ''
        else:
            cur += ch
    out.append(cur)
    return [x.strip() for x in out]


def balanced(s):
    s = s.strip()
    if not (s.startswith('(') and s.endswith(')')):
        return False
    d = 0
    for i, ch in enumerate(s):
        if ch == '(':
            d += 1
        elif ch == ')':
            d -= 1
            if d == 0 and i != len(s) - 1:
                return False
    return d == 0


def types_of(expr):
    out = set()
    for n in re.findall(r'size\((\d+)\)', expr):
        out.update(t for t in U if bits(t) == int(n))
    out.update(TY.findall(re.sub(r'size\(\d+\)', '', expr)))
    return out


def products(shape, top=True):
    """shape -> [product]; a product is a list with one string per ty slot."""
    s = shape.strip()
    terms = split_top(s, ',')
    if len(terms) > 1:
        paren = [balanced(t) for t in terms]
        if all(paren):
            out = []
            for t in terms:
                out.extend(products(t[1:-1], top=False))
            return out
        if not any(paren):
            if not top:
                comps = []
                for t in terms:
                    comps.extend(products(t, top=False)[0])
                return [comps]
            return [[s]]                      # top-level atoms: ONE slot, union
        raise ValueError('mixed tuple/atom list: %r' % shape)

    factors = split_top(s, '*')
    if len(factors) > 1:
        comps = []
        for f in factors:
            comps.extend(products(f, top=False)[0])
        return [comps]

    t = s.strip()
    if balanced(t):
        return products(t[1:-1], top=False)
    return [[t]]


def clause_of(stmt):
    body = stmt[len('legal '):]
    if ' when ' in body:
        shape, guard = body.split(' when ', 1)
    else:
        shape, guard = body, ''
    return shape.strip(), guard.strip()


def dom_of(shape, guard):
    """semantics 3.4: shape first, then enum literals in the guard.

    The guard also fixes the arity.  `legal any when ty(1) = p0:64 && ...` is
    how every memdesc rule set is written: the shape is the unit, so the shape
    alone says "one slot", while the guard names slot 1.  Counting only the
    shape made G_STORE a 1-slot rule set, the differential generated 1-slot
    queries, and the judge answered bad_type_idx on every one of them.
    """
    d, arity = {}, 0
    for prod in products(shape):
        i = 0
        for c in prod:
            c = c.strip()
            if c.startswith('mem('):
                continue
            if c in ('_', ''):
                continue
            if c == 'any':
                i += 1
                continue
            d.setdefault(i, set()).update(types_of(c))
            i += 1
        arity = max(arity, i)
    for c in conj_split(guard):
        if not is_query_conj(c):
            continue
        m = re.fullmatch(r'ty\((\d+)\)\s*=\s*([\w:]+)', c)
        if m:
            i = int(m.group(1))
            d.setdefault(i, set()).add(m.group(2))
            arity = max(arity, i + 1)
            continue
        m = re.fullmatch(r'ty\((\d+)\)\s+in\s+(.+)', c)
        if m:
            i = int(m.group(1))
            d.setdefault(i, set()).update(types_of(m.group(2)))
            arity = max(arity, i + 1)
    return d, arity


CLASSES = {}

# The declared environment alphabet.  Empty for a v7 file, which keeps the old
# behaviour (atoms are guessed from the text).  A v8 file declares it, and then
# every tool uses the declaration instead of guessing -- see `let_env`.
ENV = {'mode': [], 'feature': [], 'let': {}, 'table_over': []}


def env_declared():
    """True when the file declares an environment alphabet (v8)."""
    return bool(ENV['mode'] or ENV['feature'] or ENV['let'])


def env_primitives():
    """The atoms the environment is *sampled* on: modes and features.

    A `let` is a definition, not a dimension: it is substituted away before
    enumeration, so it must not appear here.
    """
    return set(ENV['mode']) | set(ENV['feature'])


def expand_lets(text, _seen=None):
    """Substitute every `let` name by its body, transitively.

    Elimination, not constraint: after this, a defined name is gone from the
    text, so no later stage has to remember to filter it.  A cycle leaves the
    name in place (check_decl reports `13-let-cycle`); silently looping would
    turn a declaration error into a hang.
    """
    if not ENV['let']:
        return text
    seen = set(_seen or ())
    out, changed = text, True
    while changed:
        changed = False
        for name, body in ENV['let'].items():
            if name in seen:
                continue
            pat = r'(?<![A-Za-z0-9_])' + re.escape(name) + r'(?![A-Za-z0-9_])'
            if re.search(pat, out):
                out = re.sub(pat, '(' + body + ')', out)
                seen.add(name)
                changed = True
    return out


def expand_macros_used(text, macros):
    """Expand only the macros the text mentions, transitively.

    v7 concatenated *every* macro body into *every* block, so a block that
    mentions nothing inherited `{Is64Bit, HasAVX, HasAVX512}` from macros it
    never used -- 37 of 63 X86 blocks had exactly that alphabet.  Those are
    dimensions nothing in the block reads.
    """
    macros = macros or {}
    out, changed = text, True
    while changed:
        changed = False
        for name, body in macros.items():
            pat = r'(?<![A-Za-z0-9_])' + re.escape(name) + r'(?![A-Za-z0-9_])'
            if re.search(pat, out):
                out = re.sub(pat, '(' + body + ')', out)
                changed = True
    return out


def class_members(name):
    """`class int = s8, s16;` -> {s8, s16}; None when the class is undeclared."""
    body = CLASSES.get(name)
    if body is None:
        return None
    out = set()
    for term in split_top(body, ','):
        out |= types_of(term)
    return out


def strip_list(s):
    """`[0, 1]` -> `0, 1`; anything else unchanged.

    v8 puts lists in brackets, but the older comma form is still accepted:
    both spellings reach the tools as the same text.  One helper, because
    three call sites (query, group, with) must agree on what a list is.
    """
    s = s.strip()
    if len(s) >= 2 and s.startswith('[') and s.endswith(']'):
        return s[1:-1].strip()
    return s


def parse(path):
    """Read a .decl file into (macros, blocks).

    The real grammar is legalize-decl.v7.y; this reader only has to be good
    enough for the tools that need a *block* structure, and it must agree with
    the grammar on one thing the tools depend on: scopes.  `with 0, 1 { ... }`
    is flattened here -- a statement without a slot inside a group becomes one
    slot-qualified statement per slot in the group -- because every consumer
    downstream (check_decl, infer_defaults) works on flat statements.

    `scope[i]` records where statement i came from: None for a top-level
    statement, ('with', slots) for one expanded out of a group.  The checker
    needs it to tell a declaration the author wrote for a slot from one a group
    spelled out.
    """
    global CLASSES, ENV
    CLASSES = {}
    ENV = {'mode': [], 'feature': [], 'let': {}, 'table_over': []}
    src = re.sub(r'/\*.*?\*/', '', open(path).read(), flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    macros, blocks = {}, []
    stack = []          # {'kind': 'op'|'with', ...}
    buf = ''

    def fail(msg):
        for e in reversed(stack):
            if e['kind'] == 'op':
                e['blk'].setdefault('errors', []).append(msg)
                return

    def add(blk, text, scope):
        blk['stmts'].append(text)
        blk['scope'].append(scope)

    def has_slot(text):
        return bool(re.match(r'policy\s+\d+\s*:', text) or
                    re.match(r'shapes\s+on\s+\d+', text) or
                    re.search(r'\son\s+\d+\b', text))

    def emit(text):
        nonlocal buf
        text = ' '.join(text.split())
        if not text:
            return
        if not stack:
            m = re.match(r'table\s+over\s+(.+)$', text)
            if m:
                ENV['table_over'] = [x.strip() for x in
                                     strip_list(m.group(1)).split(',')
                                     if x.strip()]
                return
            m = re.match(r'macro\s+(\w+)\s*=\s*(.+)$', text)
            if m:
                macros[m.group(1)] = m.group(2)
                return
            m = re.match(r'class\s+(\w+)\s*=\s*(.+)$', text)
            if m:
                CLASSES[m.group(1)] = m.group(2).strip()
            return
        top = stack[-1]
        if top['kind'] == 'env':
            m = re.match(r'(mode|feature)\s+([\w,\s]+?)(?:\s+from\s+(\w+))?$',
                         text)
            if m:
                kind = m.group(1)
                for name in m.group(2).split(','):
                    name = name.strip()
                    if name:
                        ENV[kind].append(name)
                return
            m = re.match(r'let\s+(\w+)\s*=\s*(.+)$', text)
            if m:
                ENV['let'][m.group(1)] = ' '.join(m.group(2).split())
                return
            fail('13-env-syntax: %s' % text[:60])
            return
        if top['kind'] == 'op':
            blk = top['blk']
            m = re.match(r'group\s+(\w+)\s*=\s*(.+)$', text)
            if m:
                try:
                    blk['groups'][m.group(1)] = tuple(
                        int(x) for x in strip_list(m.group(2)).split(','))
                except ValueError:
                    fail('13-group-syntax: %s' % text[:60])
                return
            add(blk, text, None)
            return

        blk, slots = top['blk'], top['slots']
        if text.startswith('legal ') or text.startswith('except ') or \
           text.startswith('follow ') or text.startswith('query ') or \
           text.startswith('variadic ') or text.startswith('class ') or \
           text == 'elemwise':
            # These are positional or whole-query statements: they have no
            # slot for a group to name, so wrapping them would look like it
            # did something.  `policy`/`prefer`/`shapes` are the three that do.
            fail('13-with-non-slot: %s' % text[:60])
            return
        # Keep the statement as written and record the scope.  Expanding here
        # would make `with 0, 1 { policy: A; policy 1: B; }` indistinguishable
        # from two explicit declarations, and the default/override rule lives
        # in check_decl, not in the reader.
        add(blk, text, ('with', slots))

    for ch in src:
        if ch == '{':
            head = buf.strip()
            buf = ''
            if head.startswith('op '):
                blk = {'ops': head[3:].strip(), 'stmts': [], 'scope': [],
                       'groups': {}}
                blocks.append(blk)
                stack.append({'kind': 'op', 'blk': blk})
            elif head == 'env':
                stack.append({'kind': 'env'})
            elif head.startswith('with '):
                opent = [e for e in stack if e['kind'] == 'op']
                if not opent:
                    fail('13-with-outside-op: %s' % head[:60])
                    continue
                blk = opent[-1]['blk']
                spec = strip_list(head[len('with '):].strip())
                if spec.isdigit():
                    slots = (int(spec),)
                elif spec in blk['groups']:
                    slots = blk['groups'][spec]
                elif re.fullmatch(r'\d+(\s*,\s*\d+)*', spec):
                    slots = tuple(int(x) for x in spec.split(','))
                else:
                    fail('13-with-unknown-group: %s' % spec[:60])
                    slots = ()
                outer = [e for e in stack if e['kind'] == 'with']
                if outer and slots and not set(slots) <= set(outer[-1]['slots']):
                    fail('13-with-nested: %s not inside %s'
                         % (sorted(slots), sorted(outer[-1]['slots'])))
                stack.append({'kind': 'with', 'blk': blk, 'slots': slots})
            else:
                fail('13-block-syntax: %s {' % head[:60])
        elif ch == '}':
            emit(buf)
            buf = ''
            if not stack:
                fail('13-brace: unbalanced }')
            else:
                stack.pop()
        elif ch == ';':
            emit(buf)
            buf = ''
        else:
            buf += ch
    emit(buf)
    return macros, blocks


# ---------- three-valued feature evaluation ---------------------------------
def eval_bool(expr, env):
    """Only && || ! and identifiers; a query predicate evaluates to None.

    `let` names are expanded first: a defined name is not an atom, so an env
    only ever carries primitives."""
    if ENV['let']:
        expr = expand_lets(expr)
    toks = re.findall(r'&&|\|\||!|\(|\)|[A-Za-z_][A-Za-z0-9_]*|\S', expr)
    pos = [0]

    def peek():
        return toks[pos[0]] if pos[0] < len(toks) else None

    def take():
        t = peek()
        pos[0] += 1
        return t

    def atom():
        t = take()
        if t is None:
            return None
        if t == '(':
            v = orx()
            if peek() == ')':
                take()
            return v
        if t == '!':
            v = atom()
            return None if v is None else (not v)
        if t in env:
            return env[t]
        return None

    def andx():
        v = atom()
        while peek() == '&&':
            take()
            w = atom()
            v = None if (v is None or w is None) else (v and w)
        return v

    def orx():
        v = andx()
        while peek() == '||':
            take()
            w = andx()
            v = None if (v is None or w is None) else (v or w)
        return v

    try:
        return orx()
    except Exception:
        return None


def conj_split(g):
    return [c for c in split_top(g, '&') if c.strip(' &')]


def is_query_conj(c):
    return any(q in c for q in QUERY)


def feature_atoms(g):
    """Feature names a guard mentions.  `pow2` is not one: it is the prefer
    modifier, so expanding it as a free boolean produced `pow2=0` envs and
    made check 6 demand types under configurations that cannot exist."""
    out = set()
    for c in conj_split(g):
        if is_query_conj(c):
            continue
        for w in re.findall(r'[A-Za-z_][A-Za-z0-9_]*', c):
            if w not in ('true', 'false', 'pow2'):
                out.add(w)
    return out


def feature_part(g):
    return [c for c in conj_split(g) if not is_query_conj(c)]


def feature_holds(g, env):
    for c in feature_part(g):
        if eval_bool(c, env) is False:
            return False
    return True


# ---------- macros ----------------------------------------------------------
def expand(text, macros, depth=0):
    if depth > 12:
        return text
    changed = False
    for name, body in macros.items():
        if re.search(r'\b%s\b' % re.escape(name), text):
            text = re.sub(r'\b%s\b' % re.escape(name), body, text)
            changed = True
    return expand(text, macros, depth + 1) if changed else text


def eval_cond(text, env, macros, depth=0):
    """Fold cond(G, A, B) / macros under env into a concrete type, or None."""
    if depth > 12:
        return None
    t = text.strip()
    while balanced(t):
        t = t[1:-1].strip()
    m = re.match(r'^cond\((.*)\)$', t)
    if m:
        parts = split_top(m.group(1), ',')
        if len(parts) != 3:
            return None
        v = eval_bool(parts[0], env)
        if v is None:
            return None
        return eval_cond(parts[1] if v else parts[2], env, macros, depth + 1)
    for name in macros:
        if t == name:
            return eval_cond(macros[name], env, macros, depth + 1)
    t = expand(t, macros).strip()
    if t in macros:
        return eval_cond(macros[t], env, macros, depth + 1)
    return t if TY.fullmatch(t) else None


# ---------- self-test: the shapes that already bit us -----------------------
def self_test():
    set_universe(['p0:64', 's8', 's16', 's32', 's64', 's128',
                  'v16s8', 'v8s16', 'v4s32'])
    cases = [
        # (shape, expected products)
        ('p0:64, s1, s8, s16, s32, s64', [['p0:64, s1, s8, s16, s32, s64']]),
        ('s8, s16', [['s8, s16']]),
        ('(s32, s32), (s64, s64)', [['s32', 's32'], ['s64', 's64']]),
        ('s8 * p0:64', [['s8', 'p0:64']]),
        ('(s8, p0:64, mem(s8))', [['s8', 'p0:64', 'mem(s8)']]),
        ('(p0:64 + s16 + s32) * s32', [['p0:64 + s16 + s32', 's32']]),
        ('any', [['any']]),
        ('_', [['_']]),
    ]
    fails = 0
    for shape, want in cases:
        got = products(shape)
        if got != want:
            print('  FAIL products(%r) = %r, want %r' % (shape, got, want))
            fails += 1

    d, a = dom_of('p0:64, s1, s8', '')
    if (d, a) != ({0: {'p0:64', 's1', 's8'}}, 1):
        print('  FAIL dom_of one-slot union = %r arity %d' % (d, a))
        fails += 1

    d, a = dom_of('(s8 + s16) * s32', '')
    if (d, a) != ({0: {'s8', 's16'}, 1: {'s32'}}, 2):
        print('  FAIL dom_of two-slot product = %r arity %d' % (d, a))
        fails += 1

    if elem('v8s16') != 's16' or elem('v2p0:64') != 'p0:64' or elem('s8') != 's8':
        print('  FAIL elem()')
        fails += 1
    if [kind('s8'), kind('p0:64'), kind('v8s16')] != ['scalar', 'pointer', 'vector']:
        print('  FAIL kind()')
        fails += 1
    if feature_atoms('bits(ty(0)) < 32 && !pow2') != set():
        print('  FAIL pow2 leaked into feature atoms')
        fails += 1
    if feature_atoms('HasSSE2 && !Is64Bit') != {'HasSSE2', 'Is64Bit'}:
        print('  FAIL feature atom extraction')
        fails += 1
    if not feature_holds('HasSSE2', {'HasSSE2': True}):
        print('  FAIL feature_holds true')
        fails += 1
    if feature_holds('HasSSE2', {'HasSSE2': False}):
        print('  FAIL feature_holds false')
        fails += 1

    print('decl_lib self-test: %d cases, %d failed' % (len(cases) + 7, fails))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(self_test())
