#!/usr/bin/env python3
"""Escape-point licenses: every escape is a claim, and a claim needs evidence.

Five channels are recognized in the corpus:

    Assembly { ... }        block      -- the DSL's own description (normal form)
    Assembly Custom;        custom     -- upstream hands the text to a printer
    Assembly Generic;       generic    -- upstream has no custom format
    custom<Hook>(...)       hook       -- a named sub-format inside a block
    Gap(name)               gap        -- the DSL cannot say it; a registered debt

The first three of the *claim* channels are checked against upstream TableGen:

  L1  Custom  => the op has hasCustomAssemblyFormat (own or inherited)
  L2  Generic => the op has neither assemblyFormat nor a custom printer
  L3  hook    => the hook name exists upstream (custom<Hook> in the TD closure),
                 or is registered as a corpus-local label in hook-registry.tsv

Gap names are checked against tools/gap-registry.tsv, and facts against
tools/fact-glossary.tsv.  Budgets are hard limits, not advice.

Usage: tools/escape-audit.py [--verbose]
Exit status is 1 on any failed rule.
"""

import csv
import importlib.util
import re
import sys
from collections import Counter, OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent      # ana-docs/mlir
CORPUS = ROOT / "corpus"
REPO = ROOT.parent.parent                          # llvm-project worktree
INC = REPO / "mlir/include"
DIALECT_DIR = INC / "mlir/Dialect"


def load(name, path):
    spec = importlib.util.spec_from_file_location(name, path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


coverage = load("dialect_coverage", ROOT / "tools/dialect-coverage.py")
corpus_mod = load("dialect_corpus", ROOT / "tools/dialect-corpus.py")
TD = coverage.TD

# ---------------------------------------------------------------- upstream TD
HEAD = re.compile(r"(?m)^\s*(def|defm|class)\s+([A-Za-z_]\w*)")
CUSTOM_RE = re.compile(r"hasCustomAssemblyFormat\s*=\s*(?:1|true)")
FORMAT_RE = re.compile(r"assemblyFormat\s*=")
HOOK_RE = re.compile(r"custom\s*<\s*([A-Za-z_]\w*)\s*>")
INCLUDE_RE = re.compile(r'(?m)^\s*include\s+"([^"]+)"')


def header_end(text, i):
    """Return the index of the '{' opening a record body, or a line boundary."""
    depth = 0
    while i < len(text):
        c = text[i]
        if c in "<[(":
            depth += 1
        elif c in ">])":
            depth -= 1
        elif c == "{" and depth == 0:
            return i
        elif c == "\n" and depth == 0:
            nxt = text[i + 1:].lstrip(" \t\r\n")
            if not nxt or re.match(r"(?:def|defm|class|include|//|#|\})", nxt):
                return i
        i += 1
    return len(text)


def body_text(text, k):
    if k >= len(text) or text[k] != "{":
        return ""
    depth = 0
    e = k
    while e < len(text):
        if text[e] == "{":
            depth += 1
        elif text[e] == "}":
            depth -= 1
            if depth == 0:
                return text[k:e + 1]
        e += 1
    return text[k:]


def parse_records(text, out):
    for m in HEAD.finditer(text):
        kind, name = m.groups()
        q, depth, colon, stop = m.end(), 0, None, m.end() + 600
        while q < len(text) and q < stop:
            c = text[q]
            if c in "<[(":
                depth += 1
            elif c in ">])":
                depth -= 1
            elif c == ":" and depth == 0:
                colon = q
                break
            q += 1
        if colon is None:
            continue
        hs = colon + 1
        while hs < len(text) and text[hs] in " \t\r\n":
            hs += 1
        he = header_end(text, hs)
        bm = re.match(r"\s*([A-Za-z_][\w:]*)", text[colon + 1:])
        base = bm.group(1).split("::")[-1] if bm else None
        body = body_text(text, he)
        full = text[m.start():he] + body
        out[name] = {
            "kind": kind,
            "base": base,
            "custom": bool(CUSTOM_RE.search(full)),
            "fmt": bool(FORMAT_RE.search(full)),
            "hooks": set(HOOK_RE.findall(full)),
        }


def include_closure(start):
    seen, order, stack = set(), [], list(start)
    while stack:
        p = stack.pop()
        if p in seen or not p.exists():
            continue
        seen.add(p)
        order.append(p)
        text = p.read_text(errors="ignore")
        for rel in INCLUDE_RE.findall(text):
            cand = INC / rel
            stack.append(cand if cand.exists() else p.parent / rel)
    return order


def upstream_for(keyword):
    files = []
    for rel in TD[keyword]:
        p = DIALECT_DIR / rel
        files += sorted(p.parent.glob(p.name))
    records, closure_text = {}, []
    for f in include_closure(files):
        text = f.read_text(errors="ignore")
        closure_text.append(text)
        parse_records(text, records)
    hooks = set()
    for text in closure_text:
        hooks |= set(HOOK_RE.findall(text))
    mnemonic = {}
    for f in files:
        text = f.read_text(errors="ignore")
        for m in re.finditer(r'(?ms)\b(?:def|defm)\s+([A-Za-z_]\w*)\s*:\s*'
                             r'([A-Za-z_][\w:]*)\s*<\s*(?:[A-Za-z_][\w:]*\s*,\s*)?"([^"]+)"',
                             text):
            mnemonic[m.group(3)] = m.group(1)
    return records, mnemonic, hooks


def resolve(records, name):
    seen, cur = set(), name
    custom = fmt = False
    hooks = set()
    while cur and cur not in seen and len(seen) < 20:
        seen.add(cur)
        r = records.get(cur)
        if not r:
            break
        custom |= r["custom"]
        fmt |= r["fmt"]
        hooks |= r["hooks"]
        cur = r["base"]
    if custom:
        return "custom", hooks
    if not fmt:
        return "generic", hooks
    return ("block-hook" if hooks else "block"), hooks


# ---------------------------------------------------------------- corpus
OP_RE = re.compile(r'(?m)^  (?:(op) "?([a-z0-9_.]+)"?|(schema) ([A-Za-z_][\w.]*))')
NEXT_RE = re.compile(r'(?m)^  (?:op|schema|enum|constraint|type_def|attr_def|dialect)\b')
PARENT_RE = re.compile(r"\b(?:from|extends)\s+([A-Za-z_][\w.]*)")


def assembly_block(seg):
    i = seg.find("Assembly")
    if i < 0:
        return ""
    return seg[i:]


def parse_corpus(path):
    text = path.read_text()
    dialect = re.search(r"(?m)^\s*dialect\s+([a-z0-9_]+)", text).group(1)
    ops, schemas = OrderedDict(), OrderedDict()
    for m in OP_RE.finditer(text):
        kind = "op" if m.group(1) else "schema"
        name = m.group(2) or m.group(4)
        nxt = NEXT_RE.search(text, m.end())
        seg = text[m.end(): nxt.start() if nxt else len(text)]
        parent = PARENT_RE.search(seg[:300])
        block = assembly_block(seg)
        gap = re.search(r"\bGap\s*\(\s*([a-z_]\w*)\s*\)", block)
        if re.search(r"Assembly\s+Custom\s*;", seg):
            channel = "custom"
        elif re.search(r"Assembly\s+Generic\s*;", seg):
            channel = "generic"
        elif re.search(r"Assembly\s*\{", seg):
            channel = "block"
        else:
            channel = "none"
        entry = {
            "channel": channel,
            "parent": parent.group(1) if parent else None,
            "gap": gap.group(1) if gap else None,
            "hooks": set(HOOK_RE.findall(block)),
            "facts": set(corpus_mod.fact_names(seg)),
            "has_assembly": bool(block),
        }
        (ops if kind == "op" else schemas)[name] = entry
    return dialect, ops, schemas


def channel_of(table, name):
    seen, cur, gap = set(), name, None
    while cur and cur not in seen:
        seen.add(cur)
        e = table.get(cur)
        if not e:
            return "none", None
        if e["channel"] != "none":
            return e["channel"], e["gap"]
        gap = gap or e["gap"]
        cur = e["parent"]
    return "none", gap


# ---------------------------------------------------------------- registries
def read_tsv(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    return rows


def main():
    verbose = "--verbose" in sys.argv
    fails, notes = [], []

    glossary = {r["fact"]: r for r in read_tsv(ROOT / "tools/fact-glossary.tsv")}
    registry = {r["gap"]: r for r in read_tsv(ROOT / "tools/gap-registry.tsv")}
    hook_registry = {r["hook"]: r for r in read_tsv(ROOT / "tools/hook-registry.tsv")}

    rows, gap_users, fact_users, unbacked_hooks = [], {}, {}, {}
    fact_occ = Counter()
    totals = Counter()
    for path in sorted(CORPUS.glob("*.dialect")):
        dialect, ops, schemas = parse_corpus(path)
        if dialect not in TD:
            notes.append("%s: no TD mapping, licenses not checked" % dialect)
            continue
        records, mnemonic, upstream_hooks = upstream_for(dialect)
        all_hooks_upstream = upstream_hooks
        table = dict(schemas)
        table.update(ops)
        for name, e in ops.items():
            ch, gap = channel_of(table, name)
            totals["ops"] += 1
            totals[ch] += 1
            if gap:
                totals["gap_ops"] += 1
                gap_users.setdefault(gap, set()).add("%s.%s" % (dialect, name))
            for h in e["hooks"]:
                if h in all_hooks_upstream:
                    continue
                entry = hook_registry.get(h)
                if not entry:
                    fails.append(["L3", dialect, name,
                                  "hook %s is neither upstream nor registered" % h])
                    continue
                unbacked_hooks.setdefault(h, []).append("%s.%s" % (dialect, name))
                if dialect not in entry["dialects"].split(","):
                    fails.append(["L3", dialect, name,
                                  "hook %s is registered for %s"
                                  % (h, entry["dialects"])])
            if not e["has_assembly"] and not e["parent"] and ch == "none":
                fails.append(["L0", dialect, name, "no text channel"])
            if ch == "none":
                totals["no_channel"] += 1
            rec = mnemonic.get(name) or mnemonic.get(name.split(".", 1)[-1]) or \
                next((v for k, v in mnemonic.items()
                      if name.endswith("." + k) or k.endswith("." + name)), None)
            if not rec or rec not in records:
                continue
            up, _ = resolve(records, rec)
            if ch == "custom" and up != "custom":
                fails.append(["L1", dialect, name, "Custom, but upstream is %s" % up])
            if ch == "generic" and up != "generic":
                fails.append(["L2", dialect, name, "Generic, but upstream is %s" % up])
            if ch in ("block", "block-hook") and up in ("custom", "generic"):
                totals["divergence"] += 1
                rows.append([dialect, name, ch, up])
            if verbose and ch != "none":
                rows.append([dialect, name, ch, up])

        for kind, name, header, body in corpus_mod.entities(path.read_text()):
            if kind == "dialect":
                continue
            for f in corpus_mod.fact_names(body):
                fact_users.setdefault(f, set()).add(dialect)
                fact_occ[f] += 1

    # L4: gaps are registered, and the registry is exact (no stale witness).
    for gap, users in sorted(gap_users.items()):
        entry = registry.get(gap)
        if not entry:
            fails.append(["L4", "-", "-", "gap %s is not registered" % gap])
            continue
        claimed = {w.strip() for w in entry["witness"].split(",") if w.strip()}
        if claimed != users:
            fails.append(["L4", "-", "-",
                          "gap %s witness mismatch: registry=%s corpus=%s"
                          % (gap, sorted(claimed), sorted(users))])
    for gap in registry:
        if gap not in gap_users:
            fails.append(["L4", "-", "-", "gap %s is stale (no corpus user)" % gap])

    # L5: facts are registered; local facts stay in their owning dialect.
    local_single = 0
    for fact, dialects in sorted(fact_users.items()):
        entry = glossary.get(fact)
        if not entry:
            fails.append(["L5", ",".join(sorted(dialects)), fact, "unregistered fact"])
            continue
        if entry["scope"] == "local":
            bad = dialects - {entry["owner"]}
            if bad:
                fails.append(["L5", ",".join(sorted(bad)), fact,
                              "local fact of %s used elsewhere" % entry["owner"]])
            if fact_occ[fact] == 1:
                local_single += 1
    for fact, entry in glossary.items():
        if entry["scope"] == "local" and fact not in fact_users:
            notes.append("glossary: local fact %s is unused" % fact)

    # L6: budgets.
    ops = max(totals["ops"], 1)
    custom_share = 100.0 * totals["custom"] / ops
    local_total = sum(1 for e in glossary.values() if e["scope"] == "local")
    single_share = 100.0 * local_single / max(local_total, 1)
    budgets = [
        ("ops without a text channel", totals["no_channel"], "== 0",
         totals["no_channel"] == 0),
        ("Custom share", "%.1f%%" % custom_share, "<= 25%", custom_share <= 25.0),
        ("named gaps", totals["gap_ops"], "<= 12", totals["gap_ops"] <= 12),
        ("single-use local facts", "%.0f%%" % single_share, "<= 50%", single_share <= 50.0),
        ("design divergences (block vs upstream)", totals["divergence"], "<= 30",
         totals["divergence"] <= 30),
    ]
    for label, value, limit, ok in budgets:
        if not ok:
            fails.append(["L6", "-", "-", "%s = %s (limit %s)" % (label, value, limit)])

    print("## corpus: %d ops, custom=%d, generic=%d, gap=%d, divergence=%d"
          % (totals["ops"], totals["custom"], totals["generic"],
             totals["gap_ops"], totals["divergence"]))
    print("\n## budgets")
    print("| metric | value | limit | verdict |")
    print("| --- | --- | --- | --- |")
    for label, value, limit, ok in budgets:
        print("| %s | %s | %s | %s |" % (label, value, limit, "ok" if ok else "FAIL"))
    print("\n## gap registry")
    for gap in sorted(registry):
        print("- %s: %s" % (gap, ", ".join(sorted(gap_users.get(gap, []))) or "unused"))
    if rows:
        print("\n## channel comparisons" + ("" if verbose else " (only divergences)"))
        print("| dialect | op | corpus | upstream |")
        print("| --- | --- | --- | --- |")
        for r in rows:
            print("| %s | %s | %s | %s |" % tuple(r))
    if unbacked_hooks:
        print("\n## hook names with no upstream custom<> (labels, check for typos)")
        for h in sorted(unbacked_hooks):
            print("- %s: %s" % (h, ", ".join(sorted(set(unbacked_hooks[h]))[:6])))
    if notes:
        print("\n## notes")
        for n in notes:
            print("- " + n)
    print("\n## checks")
    if fails:
        for rule, dialect, name, why in fails:
            print("FAIL %s %s %s: %s" % (rule, dialect, name, why))
    else:
        print("all escape licenses hold")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
