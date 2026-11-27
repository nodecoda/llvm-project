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
HOOK_CALL_RE = re.compile(r"custom\s*<\s*([A-Za-z_]\w*)\s*>\s*\(")
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
            "text": full,
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


# ------------------------------------------------- hook argument shapes
SIMPLE_RE = re.compile(r"^\$?([A-Za-z_]\w*)$")
TYPE_RE = re.compile(r"^type\s*\(\s*\$?([A-Za-z_]\w*)\s*\)$")
REF_RE = re.compile(r"^ref\s*\((.+)\)$", re.S)


def split_top(text):
    """Split at commas that are not inside (), [], <> or a string."""
    out, cur, depth, q = [], "", 0, False
    for c in text:
        if q:
            cur += c
            if c == '"':
                q = False
            continue
        if c == '"':
            q = True
        elif c in "([<{":
            depth += 1
        elif c in ")]>}":
            depth -= 1
        elif c == "," and depth == 0:
            out.append(cur)
            cur = ""
            continue
        cur += c
    if cur.strip():
        out.append(cur)
    return out


def arg_shape(arg):
    """Normalize one hook argument to V / T / R(..).  Literals return None.

    A quoted string is hook configuration (a delimiter, a builder name), not
    data from the op; it is dropped, and the corpus is allowed to omit it.
    """
    a = arg.strip()
    m = REF_RE.match(a)
    if m:
        inner = arg_shape(m.group(1))
        return "R(%s)" % inner if inner else "R?"
    if TYPE_RE.match(a):
        return "T"
    if SIMPLE_RE.match(a):
        return "V"
    return None


def hook_calls(text):
    """[(hook, [shapes])] for every custom<Hook>(...) call in text, in order."""
    out = []
    for m in HOOK_CALL_RE.finditer(text):
        i = m.end()
        depth, j = 1, i
        while j < len(text) and depth:
            if text[j] == "(":
                depth += 1
            elif text[j] == ")":
                depth -= 1
            j += 1
        shapes = [s for s in (arg_shape(a) for a in split_top(text[i:j - 1])) if s]
        out.append((m.group(1), shapes))
    return out


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
            "block": block,
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


def effective_block(table, name):
    """The nearest Assembly block on the inheritance chain (schema templates)."""
    seen, cur = set(), name
    while cur and cur not in seen:
        seen.add(cur)
        e = table.get(cur)
        if not e:
            return ""
        if e["block"]:
            return e["block"]
        cur = e["parent"]
    return ""


def fmt_text(records, name):
    """The nearest upstream record text that actually carries a format."""
    seen, cur = set(), name
    while cur and cur not in seen:
        seen.add(cur)
        r = records.get(cur)
        if not r:
            return ""
        if r["fmt"] or r["custom"]:
            return r["text"]
        cur = r["base"]
    return ""


def gap_positions(block):
    """(context, index) for each Gap(): context is '' only on the text spine."""
    regions = []
    for m in re.finditer(r"\bif\s+[A-Za-z_]\w*\s*\{", block):
        b = m.end() - 1
        e = matching(block, b, "{", "}")
        if e > 0:
            regions.append(("if-group", b, e))
    for m in re.finditer(r"\boilist\s*(?:<[^>]*>)?\s*\(", block):
        b = m.end() - 1
        e = matching(block, b, "(", ")")
        if e > 0:
            regions.append(("oilist-group", b, e))
    for m in HOOK_CALL_RE.finditer(block):
        b = m.end() - 1
        e = matching(block, b, "(", ")")
        if e > 0:
            regions.append(("hook-args", b, e))
    out = []
    for m in re.finditer(r"\bGap\s*\(", block):
        ctx = next((k for k, b, e in regions if b < m.start() < e), "")
        out.append((ctx, m.start()))
    return out


def matching(text, start, op, cl):
    depth = 0
    for i in range(start, len(text)):
        if text[i] == op:
            depth += 1
        elif text[i] == cl:
            depth -= 1
            if depth == 0:
                return i
    return -1


# ---------------------------------------------------------------- registries
def read_tsv(path):
    with open(path, newline="") as f:
        rows = list(csv.DictReader(f, delimiter="\t"))
    return rows


def gap_findings(registry, gap_users, gap_blocks):
    """L4: the gap registry is a debt ledger.

    Open gaps match the corpus exactly; closed gaps keep their closure
    evidence and must not reappear in the corpus.
    """
    fails = []
    SCOPE_FEATURE = {"optional-group": "if", "hook-args": "custom<",
                     "oilist": "oilist"}
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
        feat = SCOPE_FEATURE.get(entry["scope"])
        if feat:
            for dialect, name, block in gap_blocks.get(gap, []):
                if feat not in block:
                    fails.append(["L4", dialect, name,
                                  "Gap(%s) scope=%s but no %s in the text"
                                  % (gap, entry["scope"], feat)])
    for gap, entry in registry.items():
        if entry["status"] not in ("open", "closed"):
            fails.append(["L4", "-", "-",
                          "gap %s has bad status %s" % (gap, entry["status"])])
        for col in ("scope", "missing", "retire_when"):
            if not entry.get(col, "").strip():
                fails.append(["L4", "-", "-", "gap %s lacks %s" % (gap, col)])
        if entry["status"] == "open" and gap not in gap_users:
            fails.append(["L4", "-", "-", "gap %s is stale (no corpus user)" % gap])
        if entry["status"] == "closed":
            if gap in gap_users:
                fails.append(["L4", "-", "-",
                              "gap %s is closed but still used" % gap])
            if not entry.get("closed_by", "").strip():
                fails.append(["L4", "-", "-",
                              "gap %s is closed without closed_by" % gap])
    return fails




def selftest():
    """Exercise the contract guards with synthetic inputs.

    The corpus currently has zero open gaps, which would leave L4 vacuously
    green.  These cases pin each guard: a broken registry must fail, and the
    exact witness must pass.
    """
    cases, bad = [], 0

    def check(label, got, want):
        nonlocal bad
        ok = got == want
        bad += 0 if ok else 1
        cases.append((label, "ok" if ok else "FAIL: %r != %r" % (got, want)))

    check("ref(type(x))", arg_shape("ref(type($x))"), "R(T)")
    check("ref(x)", arg_shape("ref($x)"), "R(V)")
    check("type(x)", arg_shape("type($x)"), "T")
    check("x", arg_shape("$x"), "V")
    check("config dropped", arg_shape('"{}"'), None)
    check("hook call shapes",
          [s for _, s in hook_calls(
              "custom<A>(ref(type(x)), type(y)) custom<A>(z)")],
          [["R(T)", "T"], ["V"]])

    def ctx(block):
        return [c for c, _ in gap_positions(block)]

    check("gap on spine", ctx("Gap(g) a b"), [""])
    check("gap in if", ctx("a if x { Gap(g) }"), ["if-group"])
    check("gap in oilist", ctx("oilist(Gap(g) | a)"), ["oilist-group"])
    check("gap in hook", ctx("custom<H>(Gap(g))"), ["hook-args"])

    reg = {"g": {"gap": "g", "scope": "optional-group", "witness": "d.o",
                 "missing": "m", "retire_when": "r", "closed_by": "",
                 "status": "open"}}
    users = {"g": {"d.o"}}
    blocks = {"g": [("d", "o", "if x { x } else { \"undef\" }")]}
    check("open gap exact", gap_findings(reg, users, blocks), [])
    check("unregistered", len(gap_findings({}, users, blocks)), 1)
    check("witness drift",
          len(gap_findings(reg, {"g": {"d.other"}}, blocks)), 1)
    closed = {"g": dict(reg["g"], status="closed", closed_by="grammar.y")}
    check("closed but used", len(gap_findings(closed, users, blocks)), 1)
    check("open but stale", len(gap_findings(reg, {}, {})), 1)
    nosc = {"g": dict(reg["g"], scope="")}
    check("missing scope", len(gap_findings(nosc, users, blocks)), 1)
    wrong = {"g": [("d", "o", "Gap(g) a")]}
    check("scope mismatch", len(gap_findings(reg, users, wrong)), 1)

    print("## selftest")
    for label, verdict in cases:
        print("| %s | %s |" % (label, verdict))
    print("| --- | --- |")
    print("%d cases, %d failed" % (len(cases), bad))
    return 1 if bad else 0


def main():
    if "--selftest" in sys.argv:
        return selftest()
    verbose = "--verbose" in sys.argv
    fails, notes = [], []

    glossary = {r["fact"]: r for r in read_tsv(ROOT / "tools/fact-glossary.tsv")}
    registry = {r["gap"]: r for r in read_tsv(ROOT / "tools/gap-registry.tsv")}
    hook_registry = {r["hook"]: r for r in read_tsv(ROOT / "tools/hook-registry.tsv")}

    rows, gap_users, fact_users, unbacked_hooks = [], {}, {}, {}
    gap_blocks, dstat = {}, {}
    fact_occ = Counter()
    totals = Counter()
    for path in sorted(CORPUS.glob("*.dialect")):
        dialect, ops, schemas = parse_corpus(path)
        if dialect not in TD:
            notes.append("%s: no TD mapping, licenses not checked" % dialect)
            continue
        dstat[dialect] = Counter()
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
                gap_blocks.setdefault(gap, []).append(
                    (dialect, name, effective_block(table, name)))
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
            eff = effective_block(table, name)
            dstat[dialect]["ops"] += 1
            dstat[dialect][ch] += 1
            dstat[dialect]["gaps"] += 1 if gap else 0
            dstat[dialect]["hooks"] += len(HOOK_RE.findall(eff))

            # L7: one escape per op, and a gap sits on the text spine.
            if eff.count("Gap(") > 1:
                fails.append(["L7", dialect, name, "more than one Gap()"])
            for ctx, _ in gap_positions(eff):
                if ctx:
                    fails.append(["L7", dialect, name, "Gap() inside %s" % ctx])

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

            # L3b: a hook call mirrors the upstream call's data arguments.
            # Quoted configuration (delimiters, builder names) is dropped on
            # both sides; ref()/type() keep their shape.
            up_calls = hook_calls(fmt_text(records, rec))
            for h, shapes in hook_calls(eff):
                ups = [sh for hh, sh in up_calls if hh == h]
                if ups and shapes not in ups:
                    fails.append(["L3", dialect, name,
                                  "hook %s(%s), upstream %s"
                                  % (h, ", ".join(shapes), ups)])
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

    fails += gap_findings(registry, gap_users, gap_blocks)

    # L3c: a registered label states its scope and when it can be retired.
    for hook, entry in hook_registry.items():
        for col in ("scope", "retire_when"):
            if not entry.get(col, "").strip():
                fails.append(["L3", "-", hook, "registry lacks %s" % col])

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

    # L9: each dialect's escape ledger is visible and bounded.  A global
    # share can hide one dialect that escapes everything; the caps are
    # per dialect so that outlier is named.
    for dialect, st in sorted(dstat.items()):
        if not st["ops"]:
            continue
        share = 100.0 * st["custom"] / st["ops"]
        dstat[dialect]["custom_share"] = share
        if st["ops"] >= 8 and share > 80.0:
            fails.append(["L9", dialect, "-",
                          "custom share %.0f%% > 80%%" % share])
        if st["gaps"] > 3:
            fails.append(["L9", dialect, "-", "%d gaps > 3" % st["gaps"]])

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
    print("\n## per-dialect escapes")
    print("| dialect | ops | custom | share | gaps | hooks |")
    print("| --- | --- | --- | --- | --- | --- |")
    for dialect, st in sorted(dstat.items()):
        if not st["ops"]:
            continue
        print("| %s | %d | %d | %.0f%% | %d | %d |"
              % (dialect, st["ops"], st["custom"],
                 100.0 * st["custom"] / st["ops"], st["gaps"], st["hooks"]))
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
