#!/usr/bin/env python3
"""Turn `llc -debug-only=legalizer` output into a query trace.

    llc -global-isel -debug-only=legalizer in.ll -o /dev/null 2>&1 \
      | python3 trace-from-log.py --ast /tmp/ast-C2.json > trace.json

The legalizer prints one `Legalizing: <MI>` line per step, in worklist order.
That order is what a real compile asks the rules in, so replaying it measures
the decision path under the opcode interleaving of a compile instead of the
rule-set-at-a-time order a generated corpus uses.

MIR elides a vreg's type after its first mention and the worklist is LIFO, so
a type can be introduced after the line that uses it.  Types are therefore
collected per function first, then the queries are emitted in step order.

Two things the log cannot tell us, handled conservatively:

  * a function argument is defined before legalization starts, so its type is
    never printed; the query loses that slot.  When the rule set has two type
    slots and only one type survived, the last type is repeated so the chain
    does not read a slot that is not there;
  * the log does not print TypeIdx, so types are deduplicated by value.  A
    rule set with two slots that both hold the same LLT (G_MUL s64, s64) is
    the case this approximates.

Both are recorded in the envelope's `approximations` field, so a trace is
never mistaken for a faithful recording.
"""

import argparse
import collections
import json
import re
import sys

TYPE = r'(s\d+|p\d+(?::\d+)?|v\d+s\d+|v\d+p\d+(?::\d+)?)'
DEF_RE = re.compile(r'%(\d+):_\(' + TYPE + r'\)')
USE_RE = re.compile(r'%(\d+):_')
MEM_RE = re.compile(r':: \((?:load|store|loadstore)\s+\(?' + TYPE + r'')
OP_RE = re.compile(r'\bG_[A-Z0-9_]+\b')
BANNER = 'Legalize Machine IR for:'


def read_functions(stream):
    funcs, cur = [], None
    for line in stream:
        if line.startswith(BANNER):
            cur = []
            funcs.append(cur)
            continue
        if line.startswith('Legalizing: ') and cur is not None:
            cur.append(line[len('Legalizing: '):])
    return funcs


def type_map(lines):
    known = {}
    for body in lines:
        for reg, ty in DEF_RE.findall(body):
            known[reg] = ty
    return known


def query_of(body, known, slots, stats):
    op = OP_RE.search(body)
    if not op:
        stats['no opcode'] += 1
        return None
    types, seen = [], set()
    for reg in USE_RE.findall(body):
        ty = known.get(reg)
        if ty is None:
            stats['unresolved operand'] += 1
            continue
        if ty not in seen:
            seen.add(ty)
            types.append(ty)
    if slots.get(op.group(0)) == 2 and len(types) == 1:
        types.append(types[0])
        stats['slot repeated for a two-slot rule set'] += 1
    q = {'opcode': op.group(0), 'types': types}
    mem = MEM_RE.search(body)
    if mem:
        align = re.search(r'align (\d+)', body)
        q['mem'] = {'type': mem.group(1),
                    'align': int(align.group(1)) * 8 if align else 1}
    return q


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--ast', help='parse output, for the per-opcode slot count')
    ap.add_argument('--out', default='-')
    args = ap.parse_args()

    slots = {}
    if args.ast:
        ast = json.load(open(args.ast))
        for builder in ast['builders']:
            for op in builder['opcodes']:
                slots[op] = builder['num_slots']

    stats = collections.Counter()
    queries = []
    for lines in read_functions(sys.stdin):
        known = type_map(lines)
        for body in lines:
            q = query_of(body, known, slots, stats)
            if q:
                queries.append(q)

    trace = {
        'schema': 'x86-legalizer-trace',
        'source': 'llc -debug-only=legalizer',
        'approximations': [
            'types deduplicated by value: a two-slot rule set holding the '
            'same LLT in both slots is recorded as one type and repeated',
            'a function argument type is never printed, so that slot is '
            'missing from the query',
        ],
        'queries': queries,
    }
    out = sys.stdout if args.out == '-' else open(args.out, 'w')
    json.dump(trace, out)
    out.write('\n')
    print(f'queries={len(queries)} shapes={len({json.dumps(q, sort_keys=True) for q in queries})} '
          f'stats={dict(stats)}', file=sys.stderr)


if __name__ == '__main__':
    main()
