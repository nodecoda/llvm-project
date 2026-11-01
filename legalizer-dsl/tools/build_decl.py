#!/usr/bin/env python3
"""Build 2026-11-01-x86.decl from the Core IR (legal, escapes) and the
builder chains (prefer)."""
import json, os, re, sys, collections

import decl_lib as D
import check_decl as C

CORE = json.load(open('data/x86-core.json'))
CH   = json.load(open('data/x86-rule-chains.json'))

FEAT = {
 'Subtarget.is64Bit()': 'Is64Bit',
 'Subtarget.canUseCMOV()': 'HasCMOV',
 'Subtarget.hasSSE1()': 'HasSSE1',
 'Subtarget.hasSSE2()': 'HasSSE2',
 'Subtarget.hasSSE41()': 'HasSSE41',
 'Subtarget.hasAVX()': 'HasAVX',
 'Subtarget.hasAVX2()': 'HasAVX2',
 'Subtarget.hasAVX512()': 'HasAVX512',
 'Subtarget.hasVLX()': 'HasVLX',
 'Subtarget.hasAVX512() && Subtarget.hasBWI()': 'HasBWI',
 'Subtarget.hasAVX512() && Subtarget.hasDQI()': 'HasDQI',
 'Subtarget.hasDQI()': 'HasDQI',
 'Subtarget.hasPOPCNT()': 'HasPOPCNT',
 'Subtarget.hasLZCNT()': 'HasLZCNT',
 'Subtarget.hasBMI()': 'HasBMI',
 '!Subtarget.useSoftFloat() && Subtarget.hasX87()': 'UseX87',
}
SYM = {'sMaxScalar':'MaxScalar','s8MaxVector':'MaxVector8','s16MaxVector':'MaxVector16',
       's32MaxVector':'MaxVector32','s64MaxVector':'MaxVector64'}

def strip_comments(s): return re.sub(r'/\*.*?\*/', '', s).strip()

def feat(s):
    s = strip_comments(s)
    for k, v in sorted(FEAT.items(), key=lambda kv: -len(kv[0])):
        s = s.replace(k, v)
    return s.replace('&&', 'and').replace('||', 'or')

def sym(s):
    for k, v in SYM.items(): s = re.sub(r'\b%s\b' % k, v, s)
    return s

def ternary(s):
    m = re.match(r'^(?P<c>.+?)\s*\?\s*(?P<a>[^?:]+?)\s*:\s*(?P<b>.+)$', s.strip())
    return 'cond(%s, %s, %s)' % (m.group('c'), m.group('a'), m.group('b')) if m else None

def typ(s):
    s = sym(feat(s.strip()))
    return ternary(s) or s

def leaf_str(l):
    n, a = l['name'], l['args']
    if n == 'typeIs':      return 'ty(%s) = %s' % (a[0], a[1])
    if n == 'hasFeature':  return feat(a[0])
    if n == 'memSize':     return 'bits(mem(%s).ty) = %s and mem(%s).align >= %s' % (a[0], a[1], a[0], a[2])
    if n == 'typeDiffersFromMemDesc': return 'ty(%s) != mem(%s).ty' % (a[0], a[1])
    if n == 'true':        return 'true'
    if n == 'typeIn':      return 'ty(%s) in (%s)' % (a[0], ' + '.join(a[1]))
    if n == 'scalarNarrowerThan':
        # LegalizerInfo.h:266 -- isScalar() && size < N.  Both halves matter:
        # `bits < N` alone would fire on vectors whose total size is small.
        return 'kind(ty(%s)) = scalar && bits(ty(%s)) < %s' % (a[0], a[0], typ(str(a[1])))
    if n == 'sizeIs':      return 'bits(ty(%s)) = %s' % (a[0], a[1])
    return None

def expr(node, neg=False):
    k = node.get('kind')
    if k == 'leaf':
        s = leaf_str(node)
        if s is None: return None
        return ('not (%s)' % s) if neg else s
    if k == 'not':
        return expr(node['arg'], not neg)
    if k == 'nary':
        parts = [expr(a) for a in node['args']]
        if any(p is None for p in parts): return None
        op = ' or ' if node['op'] == 'any_of' else ' and '
        s = op.join('(%s)' % p if (' and ' in p or ' or ' in p) else p for p in parts)
        return ('not (%s)' % s) if neg else s
    return None

TRUE_N = {'kind': 'leaf', 'name': 'true', 'args': []}
SHAPE_LEAVES = ('typeIs', 'memSize', 'sizeIs')

def _is_true(n):
    return n.get('kind') == 'leaf' and n.get('name') == 'true'

def strip_shape(n):
    """把决定形状的叶子换成 true，剩下的就是 guard —— 保留 and/or 结构。

    原来是「只收集 hasFeature 叶子再用 and 拼」，于是 any_of(hasSSE1, x87)
    被拍成 `HasSSE1 && UseX87`（13 条规则 / 7 个 ruleset）。
    """
    k = n.get('kind')
    if k == 'leaf':
        return TRUE_N if n.get('name') in SHAPE_LEAVES else n
    if k == 'not':
        return {'kind': 'not', 'arg': strip_shape(n['arg'])}
    if k == 'nary':
        args = [strip_shape(a) for a in n['args']]
        if n['op'] == 'all_of':
            args = [a for a in args if not _is_true(a)]
            if not args: return TRUE_N
            return args[0] if len(args) == 1 else {'kind': 'nary', 'op': 'all_of', 'args': args}
        if any(_is_true(a) for a in args): return TRUE_N
        return args[0] if len(args) == 1 else {'kind': 'nary', 'op': 'any_of', 'args': args}
    return n

def shape(node):
    """Flatten a legal predicate into one tuple shape; None if not flat."""
    leaves = []
    def walk(n):
        if isinstance(n, dict):
            if n.get('kind') == 'leaf': leaves.append(n)
            for v in n.values(): walk(v)
        elif isinstance(n, list):
            for v in n: walk(v)
    walk(node)
    ts, mem = {}, None
    for l in leaves:
        # A slot can carry several literals.  `any_of SizeIsIn<0,16>, …
        # SizeIsIn<0,512>` is a *union* on that slot, and using a dict keyed by
        # the slot (last write wins) collapsed G_MERGE_VALUES / G_UNMERGE_VALUES
        # to a single `size(512)` / `size(256)`, i.e. one point of the six.
        if l['name'] == 'typeIs':
            ts.setdefault(int(l['args'][0]), []).append(l['args'][1])
        elif l['name'] == 'memSize': mem = (l['args'][1], l['args'][2])
        elif l['name'] == 'hasFeature': continue
        elif l['name'] == 'true': return 'any'
        elif l['name'] == 'sizeIs':
            ts.setdefault(int(l['args'][0]), []).append('size(%s)' % l['args'][1])
        else: return None
    if not ts and mem is None: return 'any'
    def slot(i):
        v = sorted(set(ts.get(i, [])), key=lambda x: int(re.findall(r'\d+', x)[-1]))
        return ' + '.join(v) if v else '_'
    parts = [slot(i) for i in range(max(ts) + 1)] if ts else []
    if mem: parts.append('mem(%s, %s)' % mem)
    return parts[0] if len(parts) == 1 else '(%s)' % ', '.join(parts)

def split_top(s):
    parts, d, cur = [], 0, ''
    for ch in s:
        if ch in '([': d += 1
        elif ch in ')]': d -= 1
        if ch == ',' and d == 0:
            parts.append(cur); cur = ''
        else:
            cur += ch
    parts.append(cur)
    return [x.strip() for x in parts]

def parse_shape(s):
    t = s.strip()
    if t.startswith('(') and t.endswith(')'): t = t[1:-1]
    return tuple(split_top(t))

def merge_cartesian(shapes):
    """`(a,x),(a,y),(b,x),(b,y)` -> `(a+b) * (x+y)` when the set is a product."""
    if len(shapes) < 2: return shapes
    if any('mem(' in x for x in shapes): return shapes   # mem is a size match
    ts = [parse_shape(x) for x in shapes]
    n = len(ts[0])
    if n < 2 or any(len(t) != n for t in ts): return shapes
    cols = [sorted({t[i] for t in ts}) for i in range(n)]
    total = 0
    for c in cols: total = (total or 1) * len(c)
    if total != len(ts): return shapes
    if total != len(set(ts)): return shapes
    factors = [' + '.join(c) for c in cols]
    return [' * '.join('(%s)' % f if ' + ' in f else f for f in factors)]

def torder(t):
    """类型的全序：位宽, 元素位宽, lane 数。用于稳定排序。

    只按 tbits 排会在同宽时（v16s8 vs v8s16，都是 128）留平局，
    平局由 set 迭代顺序决定 —— 输出随 PYTHONHASHSEED 变，不可复现。
    """
    m = re.fullmatch(r'v(\d+)p\d+:(\d+)', t)
    if m: return (int(m.group(1))*int(m.group(2)), int(m.group(2)), int(m.group(1)))
    m = re.fullmatch(r'v(\d+)s(\d+)', t)
    if m: return (int(m.group(1))*int(m.group(2)), int(m.group(2)), int(m.group(1)))
    m = re.fullmatch(r'p\d+:(\d+)', t)
    if m: return (int(m.group(1)), int(m.group(1)), 1)
    m = re.fullmatch(r's(\d+)', t)
    if m: return (int(m.group(1)), int(m.group(1)), 1)
    return (10**9, 0, 0)

def tbits(t):
    m = re.fullmatch(r's(\d+)', t)
    if m: return int(m.group(1))
    m = re.fullmatch(r'v(\d+)s(\d+)', t)
    if m: return int(m.group(1)) * int(m.group(2))
    m = re.fullmatch(r'p\d+:(\d+)', t)
    if m: return int(m.group(1))
    m = re.fullmatch(r'v(\d+)p\d+:(\d+)', t)
    if m: return int(m.group(1)) * int(m.group(2))
    return None

def merge_mem_patterns(shapes):
    """`(v, p, mem(N, A))` points -> `any when bits(ty(0)) R bits(mem(0).ty)`
    guards, one per exact rectangle.  Returns None when any group is not a
    rectangle, so the caller keeps the points."""
    pts = []
    for sh in shapes:
        t = sh.strip()
        if t.startswith('(') and t.endswith(')'): t = t[1:-1]
        parts = split_top(t)
        if len(parts) != 3: return None
        m = re.fullmatch(r'mem\((\d+),\s*(\d+)\)', parts[2])
        if not m: return None
        pts.append((parts[0], parts[1], int(m.group(1)), int(m.group(2))))
    ptrs, aligns = {p for _,p,_,_ in pts}, {a for *_,a in pts}
    if len(ptrs) != 1: return None
    ptr = ptrs.pop()
    # slot of the pointer: the only non-mem slot that is the same everywhere
    guards, leftover = [], []
    for rel, f in (('=', lambda a,b: a==b), ('>', lambda a,b: a>b), ('<', lambda a,b: a<b)):
        G = [(v,m) for v,p,m,a in pts if f(tbits(v), m)]
        if not G: continue
        vs = sorted({v for v,_ in G}, key=lambda t: (torder(t), t))
        ms = sorted({m for _,m in G})
        full = {(v,m) for v in vs for m in ms if f(tbits(v), m)}
        if set(G) != full:
            leftover.extend((v,p,m,a) for v,p,m,a in pts if f(tbits(v), m))
            continue
        vset = ' + '.join(vs)
        vs_part = 'ty(0) in (%s)' % vset if len(vs) > 1 else 'ty(0) = %s' % vs[0]
        mem_part = 'bits(ty(0)) %s bits(mem(0).ty)' % rel
        if rel != '=' and len(ms) > 1:
            mem_part += ' and (%s)' % ' or '.join('bits(mem(0).ty) = %d' % m for m in ms)
        guards.append('legal any when ty(1) = %s and %s and %s;' % (ptr, mem_part, vs_part))
    return guards, leftover

def key(opcodes):
    return tuple(sorted(o.strip() for o in opcodes.replace('{','').replace('}','').split(',')))

def lanes_ternary(expr_s, elem):
    e = strip_comments(expr_s).strip()
    while e.startswith('(') and e.endswith(')'): e = e[1:-1].strip()
    m = re.match(r'^(\w+)\s*\?\s*(.+?)\s*:\s*(.+)$', e)
    if not m: return None
    cond, a, b = m.group(1), m.group(2).strip(), m.group(3).strip()
    def one(x):
        x = strip_comments(x).strip()
        while x.startswith('(') and x.endswith(')'):
            x = x[1:-1].strip()
        # 必须先匹配再剥括号：原来先 strip('()') 再 endswith('.getNumElements()')，
        # 括号已被剥掉，endswith 永远为假（死分支），于是拼出
        # `vs64MaxVector.getNumElementsp0` 这种垃圾。
        m = re.search(r's(\d+)MaxVector\.getNumElements\(\)', x)
        if m:
            return 'MaxVector' + m.group(1)
        x = sym(x)
        if re.match(r'^(MaxVector|MaxScalar|cond\()', x):
            return x
        return 'v%s%s' % (x, elem if elem.startswith(('s','p')) else 's' + elem)
    def sub(x):
        x = strip_comments(x).strip()
        return lanes_ternary(x, elem) if re.match(r'^\w+\s*\?', x.strip('() ')) else one(x)
    aa, bb = sub(a), sub(b)
    return 'cond(%s, %s, %s)' % (cond, aa, bb) if aa and bb else None

def elem_of(t):
    m = re.fullmatch(r'v\d+(s\d+|p\d+:\d+)', t)
    if m: return m.group(1)
    m = re.fullmatch(r's\d+', t)
    return t if m else None

def bits_of(t): return re.fullmatch(r's(\d+)', t).group(1) if re.fullmatch(r's(\d+)', t) else 'bits(%s)' % t

# 这些 builder 由 core.json 那条路径产出（legal / except），
# 这里不重复生成，不算「未处理」。
FROM_CORE = {
    'legalFor', 'legalForIf', 'legalForCartesianProduct', 'legalForTypesWithMemDesc',
    # The v6 .td states the memdesc lists once (LegalForMemDesc /
    # LegalForMemDescIf in X86_MemOp) and the pair lists once
    # (LegalForPairsWithAnyImmIf in X86_EXTRACT/X86_INSERT).  They still land in
    # core.json as Legal rules; naming them here keeps the "unhandled builder"
    # counter honest.
    'legalForMemDesc', 'legalForMemDescIf', 'legalForPairsWithAnyImmIf',
    'alwaysLegal', 'legalIf', 'custom', 'customIf', 'customFor',
    'customForCartesianProduct',
    'libcall', 'libcallFor', 'lower', 'lowerFor', 'lowerIf',
}
# 这些按设计不写进 .decl：由 NEAREST 推断覆盖（见 semantics §6.3）。
INFERRED = {'moreElementsToNextPow2', 'fewerElementsToNextPow2', 'moreElements'}

# .td 谓词 -> 声明语言的 guard。`@` 是槽号占位。
PRED = {
    'scalarOrEltWiderThan(@, 64)': 'bits(elem(ty(@))) > 64',
}
UNHANDLED = collections.Counter()

# Builders the language cannot spell yet, with the reason and the evidence
# that dropping them changes no answer.  Anything not listed here makes the
# generator fail: a silent drop is how a rule set stops being a rendering of
# the chain and starts being a subset of it.
ALLOW_UNHANDLED = {
    # `clampMaxNumElements 0, p0, Is64Bit ? 8 : 16` on
    # {G_IMPLICIT_DEF, G_PHI, G_FREEZE, G_CONSTANT_FOLD_BARRIER}.  The target
    # (v8p0:64) is not legal in any configuration, so the chain clamps and then
    # answers Unsupported -- exactly what the .decl answers without the rule.
    # Checked over every reachable env and every pointer-vector type in U
    # (112 combinations, 0 differences).  The language can name `v8p0:64`, but
    # the 32-bit branch needs v16p0, and U stops at 64-bit.
    'clampNumElements(ptr)': 'unobservable: the clamp target is never legal',
}

def prefers_of(chain):
    """-> [(chain_index, line)]; the index is the rule's position in the chain."""
    out, seen = [], set()
    cur = [0]
    def add(line):
        if line not in seen:
            seen.add(line); out.append((cur[0], line))
    for cur[0], (name, args) in enumerate(chain):
        # widenScalarIf 的 payload 是 `typeIs(1, s16), [=](const LegalityQuery &)
        # { return std::pair<unsigned, LLT>(1, s32); }` —— 里面带逗号，按逗号
        # 切会把它切碎，所以单独按正则取。语义等价于 MinScalar<idx, to>：
        # 目标落在 D 里，用 `< bits(t)` 形式与 minScalar 分支统一。
        if name == 'widenScalarIf':
            mi = re.search(r'typeIs\((\d+),\s*([^)]+)\)', args)
            mt = re.search(r'pair<unsigned,\s*LLT>\((\d+),\s*([^)]+)\)', args)
            if mi and mt:
                i, t = mt.group(1), typ(mt.group(2))
                add('prefer %s on %s when bits(ty(%s)) < %s;' % (t, i, i, bits_of(t)))
            else:
                UNHANDLED['widenScalarIf(unparsed)'] += 1
            continue
        p = [strip_comments(x) for x in args.split(',')]
        # 下面每个 builder 的谓词都必须写全。少了 kind() 这层，
        # `bits(ty(0))` 对 v16s8 是 128，clampScalar 会把向量 clamp 成标量；
        # 少了 elem() 这层，clampNumElements 会对 s64 也触发
        # （lanes(标量) = 1 < 16），把标量改写成向量。两者都是 LegalizerInfo.h
        # 里明确写了的谓词（Ty.isScalar() / isFixedVector() && elem == EltTy）。
        if name in ('clampScalar', 'clampScalarOrElt') and len(p) == 3:
            i, lo, hi = p[0], typ(p[1]), typ(p[2])
            k = 'kind(ty(%s)) = scalar && ' % i if name == 'clampScalar' else ''
            sz = 'bits(ty(%s))' % i if name == 'clampScalar' else 'bits(elem(ty(%s)))' % i
            # clampScalar == minScalar . maxScalar (LegalizerInfo.h:1163), so a
            # degenerate clamp like G_FCMP's ClampScalar<0, s8, s8> has *both*
            # bounds: s1 widens up to s8 and s64 narrows down to s8.  Skipping
            # the lower bound when lo == hi lost the widening half and made the
            # model answer Unsupported on s1 where the chain says Legal(s8).
            add('prefer %s on %s when %s%s < %s;' % (lo,i,k,sz,bits_of(lo)))
            add('prefer %s on %s when %s%s > %s;' % (hi,i,k,sz,bits_of(hi)))
        elif name in ('widenScalarToNextPow2', 'widenScalarOrEltToNextPow2'):
            i = p[0]
            or_elt = name == 'widenScalarOrEltToNextPow2'
            # mutation is max(nextPow2(elt), MinSize): keep MinSize, it is part
            # of the rule.  Dropping it and adding a `&& !pow2` companion rule
            # gets s80 wrong (chain s128, companion gave s64).
            mn = ''
            if len(p) > 1 and not re.search(r'\?|\.', p[1]):
                mn = ' min %s' % p[1]
            g = '' if or_elt else ' when kind(ty(%s)) = scalar' % i
            add('prefer pow2%s on %s%s;' % (mn, i, g))
        elif name == 'scalarize':
            # Slot 0 of an element-wise op: the scalarize is the default's
            # job, so the text says what the op is, not what to do.  Other
            # slots keep the explicit rule -- `elemwise` is about slot 0.
            add('elemwise;' if p[0] == '0' else 'prefer elem on %s;' % p[0])
        elif name == 'scalarizeIf':
            # scalarizeIf(Pred, TypeIdx); P is a .td predicate over the type.
            m = re.match(r'\s*(.+?)\s*,\s*(\d+)\s*$', args)
            if not m:
                UNHANDLED['scalarizeIf(unparsed)'] += 1
                continue
            pred, i = m.group(1), m.group(2)
            guard = PRED.get(re.sub(r'\((\d+),', '(@,', pred))
            if guard is None:
                UNHANDLED['scalarizeIf(%s)' % pred] += 1
                continue
            add('prefer elem on %s when %s;' % (i, guard.replace('@', i)))
        elif name in ('minScalar','maxScalar') and len(p) == 2:
            t = typ(p[1]).replace('LLT::scalar(','s').rstrip(')')
            op = '<' if name == 'minScalar' else '>'
            add('prefer %s on %s when kind(ty(%s)) = scalar && bits(ty(%s)) %s %s;'
                % (t,p[0],p[0],p[0],op,bits_of(t)))
        elif name == 'clampNumElements' and len(p) == 3:
            i, lo, hi = p[0], typ(p[1]), typ(p[2])
            e = elem_of(lo)
            if e is None:
                UNHANDLED['clampNumElements(ptr)'] += 1
                continue
            g = 'kind(ty(%s)) = vector && elem(ty(%s)) = %s && ' % (i,i,e)
            add('prefer %s on %s when %slanes(ty(%s)) < lanes(%s);' % (lo,i,g,i,lo))
            add('prefer %s on %s when %slanes(ty(%s)) > lanes(%s);' % (hi,i,g,i,hi))
        elif name in ('clampMinNumElements','clampMaxNumElements') and len(p) == 3:
            i, e, n = p[0], p[1], p[2]
            e = typ(e)
            if elem_of('v2' + e) is None:
                # 元素是指针：目标应为 v<N>p0:64，而 N 是 lanes(MaxVector*)，
                # 语言里还没有「用表达式拼一个向量类型」的构造子。见 TODO。
                UNHANDLED['clampNumElements(ptr)'] += 1
                continue
            t = lanes_ternary(n, e) if re.search(r'\?|\.', n) else 'v%s%s' % (n, e)
            if t is None:
                UNHANDLED['clampNumElements(%s)' % n] += 1
                continue
            op = '<' if name == 'clampMinNumElements' else '>'
            add('prefer %s on %s when kind(ty(%s)) = vector && elem(ty(%s)) = %s '
                '&& lanes(ty(%s)) %s lanes(%s);' % (t,i,i,i,e,i,op,t))
        elif name == 'scalarSameSizeAs' and len(p) == 2:
            add('follow %s from %s by elem;' % (p[0], p[1]))
        elif name not in FROM_CORE and name not in INFERRED:
            UNHANDLED[name] += 1
    # A plain `< N` rule subsumes `< N and not pow2` with the same target.
    keep = []
    have = {line for _i, line in out}
    for idx, line in out:
        m = re.match(r'prefer (\S+) on (\d+) when kind\(ty\(\2\)\) = scalar && '
                     r'bits\(ty\(\2\)\) (<|>) (\d+) and not pow2', line)
        if m:
            plain = 'prefer %s on %s when kind(ty(%s)) = scalar && bits(ty(%s)) %s %s;' % (
                m.group(1), m.group(2), m.group(2), m.group(2), m.group(3), m.group(4))
            if plain in have: continue
        keep.append((idx, line))
    return keep

CLASS_INT = 'class int = s8, s16, s32, s64;'
CLASS_MEMBERS = ('s8', 's16', 's32', 's64')


def _slot0_legal(legal_lines, env, macros):
    """Slot-0 scalar types of these `legal` lines under one feature env.

    Only slot 0: the class default is a slot-0 rule, and the other slots are
    what the rest of the block's rules pin.  A block whose legal set depends on
    the memdesc never gets here (see _fold_class).
    """
    out = set()
    for line in legal_lines:
        shape, guard = D.clause_of(line.rstrip(';'))
        if guard and not D.feature_holds(guard, env):
            continue
        for prod in D.products(shape):
            if not prod:
                continue
            c = prod[0].strip()
            if c.startswith('mem(') or c in ('any', '_', ''):
                continue
            out |= D.types_of(c)
    return {t for t in out if D.kind(t) == 'scalar'}


CLAMP = re.compile(r'^prefer .+ on (\d+) when .*lanes\(ty\(\1\)\) [<>] lanes\(.+\);$')


def _fold_shapes(lines):
    """Replace the lane-clamp pairs with one `shapes on i;`.

    The hull is not invented here: `shapes` reads it off the block's own
    admissible shapes, so the generator writes the declaration, not the eight
    numbers that realise it.  Verified equivalent on the reachable domain
    (infer_diff: 0 disagreements both ways) -- the explicit lines also encode
    the chain's cond(HasBWI, ...) bounds, which differ from the legal hull in
    feature assignments the target cannot be in.
    """
    idx = [i for i, l in enumerate(lines) if CLAMP.match(l)]
    if not idx:
        return lines, 0
    slots = {CLAMP.match(lines[i]).group(1) for i in idx}
    if len(slots) != 1:
        return lines, 0
    slot = slots.pop()
    first = idx[0]
    out = [l for i, l in enumerate(lines) if i not in set(idx)]
    out.insert(first, 'shapes on %s;' % slot)
    return out, len(idx)


def _fold_class(lines, macros):
    """Turn the authored (pow2? + promote + truncate) run into `class int`.

    The replacement is allowed only when, under every feature assignment, the
    promote target is the smallest legal member of the class and the truncate
    target is the largest.  That is the definition of the default; when the
    authored bounds are something else (G_FCMP clamps to s8/s8 while its legal
    set is wider) the run stays spelled out.  The differential is still the
    gate; this check is why the generator cannot silently claim the default.
    """
    # The final text pass rewrites `not (X)` to `!(X)`; feature_holds only
    # understands the operator form, so normalize here too.
    def norm(x):
        x = re.sub(r'\bnot\s+\(', '!(', x)
        return x.replace(' and ', ' && ').replace(' or ', ' || ')
    legal = [norm(x) for x in lines if x.startswith('legal ')]
    if not legal or any('mem(' in x for x in legal):
        return lines, False
    p2 = re.compile(r'prefer pow2(?: min (\d+))? on 0(?: when kind\(ty\(0\)\) = scalar)?;$')
    half = re.compile(r'prefer (.+?) on 0 when (?:kind\(ty\(0\)\) = scalar && )?'
                      r'(bits\(ty\(0\)\)) ([<>]) (.+?);$')
    i = 0
    while i < len(lines):
        m0 = p2.match(lines[i])
        j = i + 1 if m0 else i
        if j + 1 >= len(lines):
            break
        a, b = half.match(lines[j]), half.match(lines[j + 1])
        if not (a and b and a.group(3) == '<' and b.group(3) == '>' and
                a.group(2) == b.group(2)):
            i += 1
            continue
        tgt = a.group(1)
        # `prefer MaxScalar ... > bits(MaxScalar)` names the type inside bits();
        # `prefer s8 ... > 8` names it at the front.  Both are the clamp's hi.
        rhs = b.group(4)
        mb = re.fullmatch(r'bits\((.+)\)', rhs)
        bound = mb.group(1) if mb else b.group(1)
        atoms = C.feature_atoms_of({'stmts': [x.rstrip(';') for x in legal]}, macros)
        if 2 ** len(atoms) > 512:
            return lines, False
        ok = True
        for vals in __import__('itertools').product([False, True], repeat=len(atoms)):
            env = dict(zip(atoms, vals))
            scal = _slot0_legal(legal, env, macros) & set(CLASS_MEMBERS)
            pt, ht = D.eval_cond(tgt, env, macros), D.eval_cond(bound, env, macros)
            if not scal or pt is None or ht is None:
                ok = False
                break
            if D.bits(pt) != min(D.bits(t) for t in scal):
                ok = False
                break
            if D.bits(ht) != max(D.bits(t) for t in scal):
                ok = False
                break
        if not ok:
            i += 1
            continue
        n = m0.group(1) if m0 else None
        new = 'class int pow2%s;' % (' ' + n if n else '') if m0 else 'class int;'
        return lines[:i] + [new] + lines[i + (3 if m0 else 2):], True
    return lines, False


# The declared environment (v8).  The three-way split is not cosmetic: a
# `mode` changes the type universe and the macros, a `feature` is a
# per-function bit, and a `let` is a *definition* that is substituted away
# before enumeration -- so `{HasCMOV=0, Is64Bit=1}` stops being buildable
# instead of being filtered afterwards.  canUseCMOV() is
# `hasCMOV() || hasSSE1() || is64Bit()` (X86Subtarget.h:208).
ENV_HEADER = [
    'env {',
    '  mode    Is64Bit from triple;',
    '  feature HasSSE1, HasSSE2, HasSSE41, HasAVX, HasAVX2, HasAVX512,',
    '          HasVLX, HasPOPCNT, HasLZCNT, HasBMI from mattr;',
    '  feature cmov, bwi, dqi, x87, softfloat from mattr;',
    '',
    '  let HasCMOV = cmov || HasSSE1 || Is64Bit;',
    '  let UseX87  = !softfloat && x87;',
    '  let HasBWI  = HasAVX512 && bwi;',
    '  let HasDQI  = HasAVX512 && dqi;',
    '}',
    '',
    '// The subtarget classes the generated table is built for.  The C++ gate',
    '// must be generated from this clause, not written by hand:',
    '// `if (X86GISelTable && STI.is64Bit())` is the same fact in a second',
    '// place; the two can drift.',
    'table over Is64Bit;',
    '',
]


def main(env=False):
    D.set_universe(CORE['domain']['types'])
    chains = {key(r['opcodes']): r['chain'] for r in CH}
    o = ['/* X86 legalizer in the v5 declaration language.',
         ' * Generated from data/x86-core.json (legal, escapes) and',
         ' * data/x86-rule-chains.json (prefer, follow).',
         ' * Core rules that are neither Legal nor a delegate (WidenScalar /',
         ' * NarrowScalar / MoreElements / ...) are deliberately not emitted:',
         ' * the inference covers them; the count is reported on stderr. */', '']
    if env:
        o += ENV_HEADER
    o += ['macro MaxScalar  = cond(Is64Bit, s64, s32);',
          'macro MaxVector8  = cond(HasAVX512, v64s8,  cond(HasAVX, v32s8,  v16s8));',
          'macro MaxVector16 = cond(HasAVX512, v32s16, cond(HasAVX, v16s16, v8s16));',
          'macro MaxVector32 = cond(HasAVX512, v16s32, cond(HasAVX, v8s32,  v4s32));',
          'macro MaxVector64 = cond(HasAVX512, v8s64,  cond(HasAVX, v4s64,  v2s64));',
          '',
          CLASS_INT, '']
    macros = {}
    for line in o:
        m = re.fullmatch(r'macro (\w+)\s*=\s*(.+);', line)
        if m:
            macros[m.group(1)] = m.group(2)
    st = collections.Counter()
    for rs in CORE['rulesets']:
        k = key(','.join(rs['opcodes']))
        o.append('// %s' % rs['def'])
        o.append('op %s {' % ', '.join(rs['opcodes']))
        # Rules are an ORDERED list in the chain, and `legal` is just one kind
        # of rule.  G_MERGE_VALUES is the proof: its legalIf sits *after*
        # widenScalarToNextPow2/minScalar, so the chain rewrites (s16, s8) to
        # (s32, s8) even though (s16, s8) satisfies the legalIf.  Emitting
        # every legal clause first (which this did) made that unreachable and
        # cost 160 disagreements.  core.json's rule_index is the authoring
        # position, and it matches the chain index (checked for all 58 chain
        # entries), so the block can be re-emitted in that order.
        groups, escapes = collections.OrderedDict(), []
        for r in rs['rules']:
            ri = r.get('rule_index', 0)
            if r['action'] == 'Legal':
                sh = shape(r.get('predicate') or {})
                gn = expr(strip_shape(r.get('predicate') or {}))
                if gn in (None, '', 'true'): gn = None
                g_prev = groups.get(gn)
                groups[gn] = (min(ri, g_prev[0]) if g_prev else ri,
                              (g_prev[1] if g_prev else []) + [sh])
            elif r['action'] in ('Custom','CustomIf','Libcall','Lower'):
                kind = {'Custom':'custom','CustomIf':'custom','Libcall':'libcall','Lower':'lower'}[r['action']]
                g = expr(r.get('predicate') or {})
                escapes.append((ri, g or 'true', kind))
                st['except'] += 1
            else:
                st['inferred'] += 1

        items = []          # (rule_index, tie-break, [lines])
        for g, (ri, shapes) in groups.items():
            uniq = [x for x in dict.fromkeys(shapes) if x is not None]
            if not uniq:
                st['unmapped'] += len(shapes); continue
            lines = []
            pat = merge_mem_patterns(uniq) if any('mem(' in x for x in uniq) else None
            if pat:
                guards, leftover = pat
                for line in guards:
                    lines.append(line[:-1] + ' and %s;' % g if g else line)
                    st['legal'] += 1; st['pattern'] += 1
                if leftover:
                    pts = ['(%s, %s, mem(%d, %d))' % x for x in leftover]
                    lines.append('legal %s%s;' % (', '.join(pts), ' when %s' % g if g else ''))
                    st['legal'] += 1; st['residual_points'] += len(leftover)
                st['points_merged'] += len(uniq) - len(leftover)
            else:
                uniq = merge_cartesian(uniq)
                lines.append('legal %s%s;' % (', '.join(uniq), ' when %s' % g if g else ''))
                st['legal'] += 1
            items.append((ri, 1, lines))
        for cidx, line in prefers_of(chains.get(k, [])):
            items.append((cidx, 0, [line]))
            if line == 'elemwise;':
                st['elemwise'] += 1
            else:
                st['follow' if line.startswith('follow ') else 'prefer'] += 1
        for ri, g, kind in escapes:
            items.append((ri, 2, ['except %s -> delegate(%s);' % (g, kind)]))
        items.sort(key=lambda x: (x[0], x[1]))
        lines = [line for _ri, _tb, ls in items for line in ls]
        if any('mem(' in x for x in lines if x.startswith('legal ')):
            # A load or store is not element-wise: collapsing the value slot to
            # one lane drops the other lanes, so it stays an explicit rule even
            # though the chain spells it `Scalarize<0>`.  `elemwise;` claims a
            # property of the op, and this op does not have it.
            n = sum(1 for x in lines if x == 'elemwise;')
            lines = ['prefer elem on 0;' if x == 'elemwise;' else x for x in lines]
            st['elemwise'] -= n
            st['prefer'] += n
        removed, shaped = 0, 0
        before = sum(1 for l in lines if CLAMP.match(l))
        lines, removed = _fold_shapes(lines)
        shaped = 1 if removed else 0
        st['shapes'] += shaped
        st['prefer'] -= removed
        lines, fused = _fold_class(lines, macros)
        if fused:
            st['class'] += 1
            st['prefer'] -= 1          # pow2+promote+truncate -> one line
        for line in lines:
            o.append('  ' + line)
        o.append('}'); o.append('')
    text = '\n'.join(o)
    text = text.replace(' and ', ' && ').replace(' or ', ' || ')
    text = re.sub(r'\bnot\s+\(', '!(', text)
    text = re.sub(r'\bnot\s+pow2', '!pow2', text)
    text = re.sub(r'!\(([A-Za-z_][A-Za-z0-9_]*)\)', r'!\1', text)
    text = text.replace('!(', '!(').replace(' && (', ' && (').replace(' || (', ' || (')
    print(text)
    sys.stderr.write('rulesets %d legal %d (patterns %d, folded %d, residual pts %d) '
                     'prefer %d class %d elemwise %d shapes %d follow %d except %d '
                     'inferred %d unmapped %d\n' %
                     (len(CORE['rulesets']), st['legal'], st['pattern'], st['points_merged'],
                      st['residual_points'], st['prefer'], st['class'], st['elemwise'],
                      st['shapes'], st['follow'], st['except'], st['inferred'], st['unmapped']))
    return unhandled_guard(UNHANDLED, ALLOW_UNHANDLED)


def unhandled_guard(counter, allow, out=sys.stderr):
    """Refuse to emit a .decl that silently drops a chain builder."""
    if not counter:
        return 0
    out.write('unhandled chain builders: %s\n' % dict(counter))
    stray = sorted(k for k in counter if k not in allow)
    if stray:
        out.write('build_decl: refusing to emit a .decl that silently drops %s; '
                  'encode the rule or add it to ALLOW_UNHANDLED with the evidence '
                  'that dropping it changes no answer\n' % ', '.join(stray))
        return 1
    for k in sorted(counter):
        out.write('  allowed: %s (%s)\n' % (k, allow[k]))
    return 0


def self_test():
    fails = 0
    if unhandled_guard(collections.Counter(), ALLOW_UNHANDLED,
                       out=open(os.devnull, 'w')) != 0:
        print('  FAIL empty counter rejected'); fails += 1
    if unhandled_guard(collections.Counter({'mystery': 1}), ALLOW_UNHANDLED,
                       out=open(os.devnull, 'w')) != 1:
        print('  FAIL an unlisted builder was accepted'); fails += 1
    if unhandled_guard(collections.Counter({'clampNumElements(ptr)': 1}),
                       ALLOW_UNHANDLED, out=open(os.devnull, 'w')) != 0:
        print('  FAIL the listed builder was rejected'); fails += 1
    print('build_decl self-test: 3 cases, %d failed' % fails)
    return 1 if fails else 0

if __name__ == '__main__':
    if '--self-test' in sys.argv[1:]:
        sys.exit(self_test())
    sys.exit(main(env='--env' in sys.argv[1:]))
