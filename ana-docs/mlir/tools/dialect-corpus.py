#!/usr/bin/env python3
"""Corpus statistics and lexical audit for ana-docs/mlir/corpus/*.dialect.

This is not the language parser.  It is a counting/audit helper:
  - entity and clause inventory
  - assembly directive / type constructor / property frequency
  - one-level schema resolution for name-reference checks
  - C++ leakage and dead-declaration checks

The executable grammar is dialect-grammar.y; both are run over the same
corpus so grammar and statistics cannot drift apart.

Usage: tools/dialect-corpus.py [--json]
"""

import json
import re
import sys
from collections import Counter, OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
CORPUS = ROOT / "corpus"

DECL = {"dialect", "schema", "op", "constraint", "enum", "attr_def",
        "type_def", "interface"}
CLAUSES = ["Operands", "Results", "Attributes", "Regions", "Successors",
           "Properties", "Implements", "Assembly", "Doc", "Cases", "Groups",
           "Parameters", "Methods"]
STRUCT = ["Operands", "Results", "Attributes", "Regions", "Successors"]
ASM_DIRECTIVES = {"attr-dict", "attr-dict-with-keyword", "prop-dict", "type",
                  "functional-type",
                  "operands", "results", "regions", "successors", "custom",
                  "ref", "enum", "qualified", "oilist", "params", "struct",
                  "if", "else"}
TYPE_CTORS = {"optional", "variadic", "array"}
WORD = re.compile(r'[A-Za-z_][\w.]*|"(?:\\.|[^"])*"|\d+|[{}\[\]()<>,;:=|]')


def strip_comments(text):
    out = []
    for line in text.splitlines():
        i = line.find("///")
        out.append(line[:i] if i >= 0 else line)
    return "\n".join(out)


def match_brace(text, start):
    """Index of the '}' matching text[start] == '{'."""
    depth = 0
    i = start
    in_str = False
    while i < len(text):
        c = text[i]
        if in_str:
            if c == "\\":
                i += 2
                continue
            if c == '"':
                in_str = False
        elif c == '"':
            in_str = True
        elif c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def blocks(text, keyword):
    """Contents of every `keyword { ... }` block (brace-balanced)."""
    out = []
    for m in re.finditer(r"\b%s\s*\{" % keyword, text):
        b = m.end() - 1
        e = match_brace(text, b)
        if e > 0:
            out.append(text[b + 1:e])
    return out


def entities(text, depth=0):
    """(kind, name, header, body) for every declaration, nested included."""
    text = strip_comments(text)
    out = []
    pos = 0
    while True:
        m = re.search(r"(?m)^[ \t]*(\w+)[ \t]+([A-Za-z_][\w.]*)", text[pos:])
        if not m:
            break
        kind, name = m.group(1), m.group(2)
        if kind not in DECL:
            pos += m.end()
            continue
        head_start = pos + m.start()
        stop = text.find(";", pos + m.end())
        brace = text.find("{", pos + m.end())
        if stop >= 0 and (brace < 0 or stop < brace):
            out.append((kind, name, " ".join(text[head_start:stop].split()), ""))
            pos = stop + 1
            continue
        if brace < 0:
            break
        end = match_brace(text, brace)
        if end < 0:
            break
        header = " ".join(text[head_start:brace].split())
        body = text[brace + 1:end]
        out.append((kind, name, header, body))
        if kind == "dialect":
            out.extend(entities(body, depth + 1))
        pos = end + 1
    return out


def top_names(expr):
    """Identifiers at parenthesis depth 0 of a comma/pipe-separated list."""
    out, depth, cur = [], 0, ""
    for c in expr:
        if c == "(":
            depth += 1
        elif c == ")":
            depth -= 1
        if c in ",|" and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += c
    out.append(cur)
    names = []
    for item in out:
        item = item.strip()
        m = re.match(r"[A-Za-z_][\w.]*", item)
        if m:
            names.append(m.group(0))
    return names


def parts_of(body):
    parts = {}
    for clause in STRUCT:
        for m in re.finditer(r"\b%s\s*\((.*?)\)\s*;" % clause, body, re.S):
            for b in re.finditer(r"([A-Za-z_][\w.]*)\s*:", m.group(1)):
                parts[b.group(1)] = clause
    return parts


def asm_refs(body):
    refs = set()
    for blk in blocks(body, "Assembly"):
        blk = re.sub(r'"(?:\\.|[^"])*"', " ", blk)   # literals are not refs
        blk = re.sub(r"custom<[^>]*>", " ", blk)     # hook names are not refs
        for tok in re.findall(r"[A-Za-z_][\w.-]*", blk):
            if tok not in ASM_DIRECTIVES and tok not in TYPE_CTORS:
                refs.add(tok)
    return refs


def main():
    as_json = "--json" in sys.argv
    files = sorted(CORPUS.glob("*.dialect"))
    report = OrderedDict()
    clause_matrix = Counter()
    asm_dirs = Counter()
    type_ctors = Counter()
    props = Counter()
    facts = Counter()
    region_facts = Counter()
    ifaces = Counter()
    declared = {k: set() for k in DECL}
    all_entities = []
    file_text = {}
    leakage = []

    for path in files:
        text = path.read_text()
        clean = strip_comments(text)
        file_text[path.name] = clean
        ents = entities(text)
        kinds = Counter(k for k, _, _, _ in ents)
        report[path.name] = {"lines": len(text.splitlines()),
                             "entities": dict(kinds)}
        all_entities += [(path.name,) + e for e in ents]
        for kind, name, header, body in ents:
            if kind != "dialect":
                declared[kind].add(name)
            if kind == "dialect":      # a dialect body is a container
                continue
            for clause in CLAUSES:
                n = len(re.findall(r"\b%s\b" % clause, body))
                if n:
                    clause_matrix[(kind, clause)] += n
            for blk in blocks(body, "Assembly"):
                for w in re.findall(r"[A-Za-z_][\w-]*", blk):
                    if w in ASM_DIRECTIVES:
                        asm_dirs[w] += 1
            for w in re.findall(r"\b(optional|variadic|array)\s*<", body):
                type_ctors[w] += 1
            for m in re.finditer(r"Regions\s*\((.*?)\)\s*;", body, re.S):
                for ann in re.findall(r"\[([^\[\]]*)\]", m.group(1)):
                    for item in top_names(ann):
                        region_facts[item] += 1
            for m in re.finditer(r"Properties\s*\[(.*?)\]\s*;", body, re.S):
                for item in top_names(m.group(1)):
                    if item in ("has_parent", "parent_one_of", "parent_is"):
                        facts[item] += 1
                    else:
                        props[item] += 1
            for m in re.finditer(r"Implements\s*\[(.*?)\]\s*;", body, re.S):
                for item in top_names(m.group(1)):
                    ifaces[item] += 1
        for pat in (r"::", r"\bnamespace\b", r"#include", r"\$\w", r"\[\{"):
            if re.search(pat, clean):
                leakage.append({"file": path.name, "pattern": pat})

    schemas = {}
    for _, kind, name, header, body in all_entities:
        if kind == "schema":
            pm = re.search(r"\bextends\s+([A-Za-z_][\w.]*)", header)
            schemas[name] = {"parts": parts_of(body), "parent": pm.group(1) if pm else None,
                             "asm": asm_refs(body)}

    def fold(name, seen=()):
        s = schemas[name]
        parts = {}
        if s["parent"] in schemas and s["parent"] not in seen:
            parts.update(fold(s["parent"], seen + (name,))[0])
        parts.update(s["parts"])
        return parts, s["asm"]

    resolved = {n: fold(n)[0] for n in schemas}
    for n in schemas:
        if schemas[n]["parent"] and schemas[n]["parent"] not in schemas:
            pass

    dangling = []
    for fname, kind, name, header, body in all_entities:
        if kind not in ("op", "schema"):
            continue
        own = parts_of(body)
        pm = re.search(r"\b(?:extends|from)\s+([A-Za-z_][\w.]*)", header)
        inherited = resolved.get(pm.group(1), {}) if pm else {}
        known = set(inherited) | set(own)
        for r in asm_refs(body):
            if r not in known:
                dangling.append([fname, name, r])

    missing_from = sorted({t for _, kind, name, h, b in all_entities if kind == "op"
                           for t in re.findall(r"\bfrom\s+([A-Za-z_][\w.]*)", h)
                           if t not in schemas})

    used_names = Counter()
    for fname, kind, name, header, body in all_entities:
        for w in WORD.findall(body + " " + header):
            if w in declared.get("schema", ()) or w in declared.get("constraint", ()) \
               or w in declared.get("enum", ()):
                used_names[w] += 1
    unused = {}
    for k in ("schema", "constraint", "enum"):
        dead = [n for n in sorted(declared[k]) if used_names[n] == 0]
        if dead:
            unused[k] = dead

    if as_json:
        print(json.dumps({"files": report,
                          "clauses": {"%s.%s" % k: v for k, v in clause_matrix.items()},
                          "assembly": asm_dirs, "type_ctors": type_ctors,
                          "region_facts": region_facts,
                          "properties": props, "facts": facts,
                          "interfaces": ifaces, "missing_from": missing_from,
                          "dangling": dangling, "unused": unused,
                          "leakage": leakage}, indent=2))
        return

    cols = sorted({k for r in report.values() for k in r["entities"]})
    print("## corpus")
    print("| file | lines | " + " | ".join(cols) + " |")
    print("| --- | --- | " + " | ".join("---" for _ in cols) + " |")
    for f, r in report.items():
        print(f"| {f} | {r['lines']} | " +
              " | ".join(str(r["entities"].get(c, "")) for c in cols) + " |")

    kinds = sorted({k for k, _ in clause_matrix})
    used_clauses = sorted({c for _, c in clause_matrix},
                          key=lambda c: CLAUSES.index(c))
    print("\n## clause x entity")
    print("| entity | " + " | ".join(used_clauses) + " |")
    print("| --- | " + " | ".join("---" for _ in used_clauses) + " |")
    for k in kinds:
        print(f"| {k} | " + " | ".join(str(clause_matrix.get((k, c), ""))
                                       for c in used_clauses) + " |")

    print("\n## assembly: " + ", ".join(f"{k}={v}" for k, v in asm_dirs.most_common()))
    print("## type ctors: " + ", ".join(f"{k}={v}" for k, v in type_ctors.most_common()))
    print("## region facts: " + ", ".join(f"{k}={v}" for k, v in region_facts.most_common()))
    print("## properties: " + ", ".join(f"{k}={v}" for k, v in props.most_common()))
    print("## facts: " + ", ".join(f"{k}={v}" for k, v in facts.most_common()))
    print("## interfaces: " + ", ".join(f"{k}={v}" for k, v in ifaces.most_common()))
    print("\n## checks")
    print("missing `from`/`extends` targets:", missing_from or "none")
    print("dangling assembly refs:", dangling or "none")
    print("dead declarations:", unused or "none")
    print("C++ leakage:", leakage or "none")


if __name__ == "__main__":
    main()
