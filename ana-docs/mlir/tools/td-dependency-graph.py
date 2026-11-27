#!/usr/bin/env python3
"""Build the MLIR TableGen include graph from a source tree.

Usage:
  python3 td-dependency-graph.py --root /path/to/llvm-project [--mermaid out.md]

Scans every .td under mlir/include, mlir/lib and mlir/python, resolves each
`include "..."` against those roots (plus the including file's directory), and
prints degree/depth statistics. TableGen `include` is lexical substitution
(llvm/docs/TableGen/ProgRef.md), so the graph is expected to be a DAG; cycles
are reported if found.

Notes:
  * mlir/test, mlir/examples and mlir/unittests are excluded: they are not
    build inputs for the dialect definitions.
  * llvm/include is scanned only to prove MLIR does not include it; those nodes
    are reported separately and left out of the statistics.
  * Three .td files are generated at build time (OpenMP/OmpCommon.td,
    OpenACC/AccCommon.td, Linalg/.../LinalgNamedStructuredOps.yamlgen.td) and
    therefore stay unresolved here.
"""

import argparse
import collections
import functools
import json
import os
import re
import sys

ROOTS = ["mlir/include", "mlir/lib", "mlir/python"]
LLVM_ROOT = "llvm/include"
INCLUDE_RE = re.compile(r'^\s*include\s+"([^"]+)"', re.M)
GENERATED = {
    "mlir/Dialect/OpenMP/OmpCommon.td": "--gen-directive-decl from llvm/Frontend/OpenMP/OMP.td",
    "mlir/Dialect/OpenACC/AccCommon.td": "--gen-directive-decl from llvm/Frontend/OpenACC/ACC.td",
    "mlir/Dialect/Linalg/IR/LinalgNamedStructuredOps.yamlgen.td": "mlir-linalg-ods-yaml-gen",
}


def collect(root):
    files = {}
    for r in ROOTS:
        for dirpath, _, names in os.walk(os.path.join(root, r)):
            for name in names:
                if name.endswith(".td"):
                    path = os.path.join(dirpath, name)
                    files[os.path.relpath(path, root)] = path
    return files


def resolve(include, current, root):
    for r in ROOTS + [""]:
        candidate = os.path.normpath(os.path.join(r, include) if r else
                                     os.path.join(os.path.dirname(current), include))
        if os.path.isfile(os.path.join(root, candidate)):
            return candidate
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--root", default=os.getcwd())
    ap.add_argument("--json")
    ap.add_argument("--mermaid")
    args = ap.parse_args()

    files = collect(args.root)
    edges, unresolved = [], []
    for rel, path in sorted(files.items()):
        with open(path, encoding="utf-8", errors="replace") as f:
            for include in INCLUDE_RE.findall(f.read()):
                target = resolve(include, rel, args.root)
                if target is None:
                    unresolved.append((rel, include))
                else:
                    edges.append((rel, target))

    nodes = set(files)
    forward = collections.defaultdict(set)
    reverse = collections.defaultdict(set)
    for src, dst in edges:
        forward[src].add(dst)
        reverse[dst].add(src)

    @functools.lru_cache(maxsize=None)
    def depth(node):
        return 0 if not forward[node] else 1 + max(depth(n) for n in forward[node])

    def reachable(start):
        seen, stack = set(), list(reverse[start])
        while stack:
            node = stack.pop()
            if node not in seen:
                seen.add(node)
                stack.extend(reverse[node])
        return seen

    # cycle detection (iterative DFS)
    state = collections.defaultdict(int)
    cycles = []
    for start in sorted(nodes):
        if state[start]:
            continue
        stack = [(start, iter(sorted(forward[start])))]
        path = [start]
        state[start] = 1
        while stack:
            node, children = stack[-1]
            child = next(children, None)
            if child is None:
                state[node] = 2
                stack.pop()
                path.pop()
            elif state[child] == 1:
                cycles.append(path[path.index(child):] + [child])
            elif state[child] == 0:
                state[child] = 1
                stack.append((child, iter(sorted(forward[child]))))
                path.append(child)

    print(f"nodes {len(nodes)}  edges {len(edges)}  cycles {len(cycles)}")
    print("unresolved:", len(unresolved))
    for src, include in unresolved:
        tag = GENERATED.get(include, "missing")
        print(f"  {src} -> {include}  [{tag}]")
    print("depth histogram:", dict(sorted(collections.Counter(depth(n) for n in nodes).items())))
    print("\ntop fan-in:")
    for node in sorted(nodes, key=lambda n: -len(reverse[n]))[:15]:
        print(f"  {len(reverse[node]):4d}  {node}")
    print("\ntop transitive dependents:")
    for node in sorted(nodes, key=lambda n: -len(reachable(n)))[:15]:
        print(f"  {len(reachable(node)):4d}  {node}")

    if args.json:
        with open(args.json, "w") as f:
            json.dump({"nodes": sorted(nodes), "edges": sorted(edges)}, f, indent=1)
    if args.mermaid:
        ids = {node: f"n{index}" for index, node in enumerate(sorted(nodes))}
        with open(args.mermaid, "w") as f:
            f.write("graph TD\n")
            for node, node_id in ids.items():
                f.write(f'  {node_id}["{node}"]\n')
            for src, dst in sorted(edges):
                f.write(f"  {ids[src]} --> {ids[dst]}\n")
    return 1 if cycles else 0


if __name__ == "__main__":
    sys.exit(main())
