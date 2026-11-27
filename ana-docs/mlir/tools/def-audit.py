#!/usr/bin/env python3
"""Definition-layer audit: attribute, type and enum declarations.

The op layer has an escape audit; the definition layer had none.  An
`attr_def` or `type_def` could claim any text channel, drop any parameter, or
name a symbol that does not exist, and nothing noticed.  This tool closes that
hole by checking each declaration against the same witness the ops use:
`llvm-tblgen --dump-json`.

Rules for `attr_def` / `type_def`:

  D0 channel   -- every definition declares its text source (`Assembly { ... }`,
                  `Assembly Custom;`, or `Assembly Generic;`).  Omission is not
                  silently read as generic.
  D1 custom    -- `Custom` <=> the upstream record sets
                  `hasCustomAssemblyFormat` (and has no `assemblyFormat`).
  D2 block     -- a block <=> upstream has a declarative `assemblyFormat`.
  D3 generic   -- `Generic` <=> upstream has neither a format nor a custom
                  printer; the fixed `!dialect.mnemonic` spelling is all there
                  is.
  D4 cover     -- every declared parameter is named at least once in the block,
                  and every identifier in the block is a declared parameter (or
                  a directive keyword / hook name).  A parameter that is never
                  printed is a hole exactly like an unprinted operand.
  D7 family    -- `params` / `struct(params)` stand for the whole parameter
                  list.  At most one per body, never alongside a named
                  parameter, never as a hook/oilist argument, and only when the
                  upstream format carries the same directive (struct(params) is
                  accepted in its total form only).
  D8 witness   -- every escape row is consumed and its evidence resolves: an
                  alias target exists in the same kind, and a modeled witness
                  names the quoted symbol on (or beside) the cited line.  A
                  witness without a symbol is a file citation, not evidence.
  D9 modeled   -- `Assembly Modeled;` requires a `def-exceptions.tsv` row with
                  a witness; it is the only channel allowed for a definition
                  with no upstream ODS record, and it cannot be used by a
                  definition that does have one.
  D11 spine    -- a modeled block is a *spine*, not a grammar: only string
                  literals, declared parameters and `params` are legal.  No
                  type(...)/attr-dict/prop-dict/custom<>/if/oilist/else/
                  qualified.  The block records the punctuation a C++ printer
                  emits; it must not re-implement that printer's control flow.
  D12 params   -- `Assembly Modeled;` is opaque, so it cannot declare
                  Parameters.  If the parameter list is known, write the spine.
  D14 hook     -- a `custom<Hook>` in a definition block must appear in that
                  record's upstream assemblyFormat.  A hook inside a type is
                  a named sub-format, so it needs the same evidence an op hook
                  gets from L3.
  D13 channel  -- a modeled definition uses only `Modeled;` or a spine block.
                  `Custom`/`Generic` are claims about an upstream ODS record;
                  with no record they cannot be falsified.

Every modeled row also carries a `category` from a closed vocabulary (rename,
cxx-type, cxx-enum, builtin, constraint, borrowed-enum).  The category decides
what the witness must look like, so "modeled" never becomes a free-text excuse.

Rules for `enum`:

  D5 cases     -- the printed case spellings and their values match upstream
                  exactly; groups are checked by value (the OR of their
                  members) and must exist upstream.  Enum attributes are
                  modeled by their enum domain, so an `EnumAttr` needs no
                  separate `attr_def`.
  D6 kind      -- `: bits` matches a bit-enum record.

`--coverage` additionally classifies every upstream record the corpus does not
declare.  A record the corpus names in a type position is `unaccounted`: a
dangling reference that fails the mode, checked first.  Otherwise, a record no
op reaches through a structured ODS reference is `unreferenced`; a record an op
reaches but the corpus never uses as a type is `substituted` (the corpus types
that field differently, e.g. as i32 or `any`).  So "missing" is never a silent
number: every entry has a class, and `unaccounted == 0` is the gate.  Corpus declarations that are
neither upstream records nor ledger rows are `extra`; a ledger row is licensed,
so `extra` must be empty.  Coverage is still a separate opt-in mode: a green
default run means "everything declared is true", never "everything is
declared".

Usage:
  LLVM_TBLGEN=... LLVM_BUILD_DIR=... tools/def-audit.py [--coverage] [--verbose]
  tools/def-audit.py --selftest

Exit status is 1 on any failed rule.
"""

import importlib.util
import json
import os
import re
import subprocess
import sys
from collections import OrderedDict
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent      # ana-docs/mlir
CORPUS = ROOT / "corpus"

_spec = importlib.util.spec_from_file_location(
    "dialect_coverage", ROOT / "tools/dialect-coverage.py")
coverage = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(coverage)
TD = coverage.TD

# Definition-only entry points.  A dialect's op aggregate sometimes stops at
# the op base file; the type definitions then live in a sibling that the op
# closure never needs (`LLVMTypes.td` is not included by `LLVMOps.td`).
TD_DEFS = {
    "complex": ["Complex/IR/ComplexAttributes.td"],
    "smt": ["SMT/IR/SMTTypes.td"],
    "llvm": ["LLVMIR/LLVMTypes.td"],
    "transform": ["Transform/IR/TransformTypes.td"],
    "mpi": ["MPI/IR/MPITypes.td"],
    "tosa": ["Tosa/IR/TosaTypesBase.td"],
}

KEYWORDS = {
    "if", "else", "custom", "qualified", "type", "attr-dict",
    "attr-dict-with-keyword", "prop-dict", "functional-type", "oilist",
    "ref", "Gap", "successors", "params", "struct",
}

# `params` / `struct(params)` are total over the parameter dimension.  They
# are the only escapes on the definition side that stand for "all parameters";
# everything else must name parameters one by one.  The grammar accepts them
# wherever an assembly element is legal, so this tool owns the scope rule: a
# definition body may use one, never both, never alongside a named parameter,
# and never as a hook/oilist argument.
FAMILY_RE = re.compile(r"\bstruct\s*\(\s*params\s*\)|\bparams\b")
BAD_FAMILY = re.compile(r"(?:custom\s*<[^>]*>\s*\([^)]*\bparams\b"
                        r"|oilist\s*(?:<[^>]*>)?\s*\([^)]*\bparams\b)")

DEF_RE = re.compile(r"(?m)^  (type_def|attr_def|enum)\s+([A-Za-z_][\w.]*)")
EXCEPTIONS = ROOT / "tools/def-exceptions.tsv"
POLICY_RE = re.compile(r"(?m)^/// 定义层范围[:：]")
CASES_RE = re.compile(r"(?s)\bCases\s*\[(.*?)\]\s*;")
GROUPS_RE = re.compile(r"(?s)\bGroups\s*\[(.*?)\]\s*;")
PARAMS_RE = re.compile(r"(?s)\bParameters\s*\((.*?)\)\s*;")


def norm(name):
    return re.sub(r"[_\-]", "", name).lower()


def strip_comments(text):
    """Remove // and /* */ comments, preserving newlines and strings."""
    out, i, n = [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            out.append(text[i:j + 1])
            i = j + 1
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            j = text.find("\n", i)
            j = n if j < 0 else j
            out.append(" " * (j - i))
            i = j
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            j = text.find("*/", i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r"[^\n]", " ", text[i:j]))
            i = j
        else:
            out.append(c)
            i += 1
    return "".join(out)


def matching(text, start, open_ch, close_ch):
    """Index of the delimiter matching text[start] (strings/comments skipped)."""
    depth = 0
    i = start
    n = len(text)
    while i < n:
        c = text[i]
        if c == '"':
            i += 1
            while i < n:
                if text[i] == "\\":
                    i += 2
                    continue
                if text[i] == '"':
                    break
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "/":
            i = text.find("\n", i)
            if i < 0:
                return -1
            continue
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            i = text.find("*/", i + 2)
            if i < 0:
                return -1
            i += 1
        elif c == open_ch:
            depth += 1
        elif c == close_ch:
            depth -= 1
            if depth == 0:
                return i
        i += 1
    return -1


def split_top(text, sep=","):
    """Split on a separator at nesting depth zero."""
    parts, depth, cur, i, n = [], 0, [], 0, len(text)
    while i < n:
        c = text[i]
        if c == '"':
            j = i + 1
            while j < n:
                if text[j] == "\\":
                    j += 2
                    continue
                if text[j] == '"':
                    break
                j += 1
            cur.append(text[i:j + 1])
            i = j + 1
            continue
        if c in "<([{":
            depth += 1
        elif c in ">)]}":
            depth -= 1
        if c == sep and depth == 0:
            parts.append("".join(cur))
            cur = []
        else:
            cur.append(c)
        i += 1
    if "".join(cur).strip():
        parts.append("".join(cur))
    return parts


def line_of(text, pos):
    return text.count("\n", 0, pos) + 1


def parse_corpus(path):
    """Return {kind: {name: declaration}} for one corpus file."""
    raw = path.read_text()
    text = strip_comments(raw)
    defs = {"type_def": OrderedDict(), "attr_def": OrderedDict(),
            "enum": OrderedDict()}
    for m in DEF_RE.finditer(text):
        kind, name = m.group(1), m.group(2)
        brace = text.find("{", m.end())
        if brace < 0:
            continue
        end = matching(text, brace, "{", "}")
        if end < 0:
            continue
        body = text[brace + 1:end]
        decl = {
            "name": name,
            "line": line_of(text, m.start()),
            "body": body,
            "params": [],
            "channel": None,
            "block": "",
            "bits": bool(re.match(r"\s*:\s*bits\b", text[m.end():brace])),
        }
        pm = PARAMS_RE.search(body)
        if pm:
            for part in split_top(pm.group(1)):
                colon = part.find(":")
                if colon < 0:
                    continue
                decl["params"].append(part[:colon].strip())
        am = re.search(r"(?m)^\s*Assembly\b", body)
        if am:
            rest = body[am.end():]
            stripped = rest.lstrip()
            if stripped.startswith("{"):
                b = body.find("{", am.end())
                e = matching(body, b, "{", "}")
                decl["channel"] = "block"
                decl["block"] = body[b + 1:e]
            else:
                word = stripped.split(None, 1)[0].rstrip(";") if stripped else ""
                decl["channel"] = word
        defs[kind][name] = decl
    return defs


class Upstream:
    """Per-dialect upstream definitions from tblgen JSON."""

    def __init__(self, dialect):
        self.dialect = dialect
        self.json = OrderedDict()
        self.failed = []
        self.types = OrderedDict()     # norm mnemonic -> (mnemonic, record)
        self.attrs = OrderedDict()     # norm mnemonic -> (mnemonic, record)
        self.enums = OrderedDict()     # norm class   -> (class, record)
        self._load()
        self._classify()

    def _load(self):
        tblgen = coverage.find_tblgen()
        if not tblgen:
            raise SystemExit("def-audit: llvm-tblgen not found")
        incs = coverage.include_dirs()
        files = list(TD.get(self.dialect, [])) + TD_DEFS.get(self.dialect, [])
        for rel in files:
            f = coverage.resolve(rel)
            cmd = [str(tblgen)]
            for d in incs:
                cmd += ["-I", str(d)]
            cmd += ["--dump-json", str(f)]
            r = subprocess.run(cmd, capture_output=True, text=True)
            if r.returncode != 0:
                self.failed.append((f.name, r.stderr.strip().splitlines()[:2]))
                continue
            self.json.update(json.loads(r.stdout))

    def _dialect_name(self, record):
        ref = (record.get("dialect") or {}).get("def")
        if not ref:
            return ""
        return ((self.json.get(ref) or {}).get("name") or "").lower()

    def _namespace(self, record):
        return (record.get("cppNamespace") or "").rsplit("::", 1)[-1].lower()

    def _dialect_ns(self):
        for v in self.json.values():
            if (isinstance(v, dict) and "Dialect" in (v.get("!superclasses") or [])
                    and (v.get("name") or "").lower() == self.dialect.lower()):
                return (v.get("cppNamespace") or "").strip(":")
        return ""

    def _owned(self, record):
        name = self._dialect_name(record)
        if name:
            return name == self.dialect.lower()
        ns = (record.get("cppNamespace") or "").strip(":")
        if not ns:
            return False
        if ns.rsplit("::", 1)[-1].lower() == self.dialect.lower():
            return True
        # Sub-namespaces belong to the dialect (`::mlir::LLVM::linkage`).
        # `::mlir` alone (DLTI) is the root, not a dialect namespace.
        dns = self._dialect_ns()
        return bool(dns) and len(dns.split("::")) >= 2 and ns.startswith(dns + "::")

    def _classify(self):
        # An enum that an *owned* EnumAttr points at belongs to the dialect even
        # when its own cppNamespace is shared (`utils::IteratorType` is linalg's
        # iterator domain).  A borrowed record (memref's use of arith's
        # AtomicRMWKind) is not owned: its attr's dialect is arith.
        enum_refs = set()
        for v in self.json.values():
            if not isinstance(v, dict):
                continue
            ref = (v.get("enum") or {}).get("def")
            if ref and self._owned(v):
                enum_refs.add(ref)
        self.enum_attrs = {}
        for v in self.json.values():
            if not isinstance(v, dict):
                continue
            sc = v.get("!superclasses") or []
            mnem = v.get("mnemonic")
            if "TypeDef" in sc and mnem and self._owned(v):
                self.types[norm(mnem)] = (mnem, v)
            elif "AttrDef" in sc and mnem and self._owned(v):
                self.attrs[norm(mnem)] = (mnem, v)
                if v.get("enum"):
                    self.enum_attrs[norm(mnem)] = (v.get("enum") or {}).get("def")
            elif "EnumInfo" in sc and v.get("className"):
                owned = self._owned(v) or v.get("!name") in enum_refs
                if owned:
                    self.enums[norm(v["className"])] = (v["className"], v)


# Closed vocabulary for the `category` column.  Each category fixes what the
# witness has to be; the audit refuses a row whose category does not match its
# kind or its witness shape.  There is no "other".
CATEGORIES = {
    "rename": "the record exists; only the spelling differs",
    "cxx-type": "a hand-written C++ type or value object",
    "cxx-enum": "a hand-written C++ enum",
    "builtin": "a builtin attribute/type used as a payload",
    "constraint": "a TableGen constraint alias, not a def",
    "borrowed-enum": "an enum record owned by another dialect",
}
QUOTED = re.compile(r"`([^`]+)`")
SPINE_STR = re.compile(r'"(?:\\.|[^"])*"')


def norm_ident(text):
    return re.sub(r"[^a-z0-9]", "", text.lower())


def witness_symbol(reason):
    """The symbol a modeled row promises its witness shows.

    The row states it in backticks (`SparseDnTensorHandleType`,
    `TypeAttrOf<Tosa_AccType>`); a template application names its head.
    """
    m = QUOTED.search(reason)
    if not m:
        return None
    return re.sub(r"<.*", "", m.group(1)).strip()


def witness_problem(dialect, kind, witness, category, reason):
    """Every reason a witness can fail to be evidence."""
    path, _, line = witness.partition(":")
    full = ROOT.parent.parent / path
    if not full.exists():
        return "witness %s does not exist" % witness
    if category in ("cxx-type", "builtin", "constraint") and kind not in (
            "type", "attr"):
        return "category %s cannot describe a %s" % (category, kind)
    if category == "cxx-enum" and kind != "enum":
        return "cxx-enum must be an enum"
    if category == "borrowed-enum" and kind != "enum":
        return "borrowed-enum must be an enum"
    if category == "builtin" and not path.startswith("mlir/include/mlir/IR/"):
        return "builtin witness must be under mlir/include/mlir/IR/"
    if category == "constraint" and not path.endswith(".td"):
        return "constraint witness must be a .td"
    if category == "cxx-enum" and not path.endswith(".h"):
        return "cxx-enum witness must be a .h"
    if category == "cxx-type" and not path.endswith((".h", ".td")):
        return "cxx-type witness must be a .h or .td"
    if category == "borrowed-enum" and ("/" + dialect + "/") in path.lower():
        return "borrowed-enum witness is in %s's own tree" % dialect
    symbol = witness_symbol(reason)
    if not symbol:
        return "modeled row must quote the witness symbol in its reason"
    try:
        n = int(line)
    except ValueError:
        return "witness %s has no line" % witness
    lines = full.read_text(errors="replace").splitlines()
    window = "\n".join(lines[max(0, n - 3):n + 2])
    if symbol not in window:
        return "witness line does not mention `%s`" % symbol
    return None


def spine_tokens(block, params):
    """Identifiers in a modeled block that are neither parameters nor literals.

    A modeled block may only be a spine: string literals (punctuation), declared
    parameter references, and `params`/`struct(params)`.  Anything left over is
    a directive or an undeclared name, and either one means the block is a
    grammar, not a spine.
    """
    s = SPINE_STR.sub(" ", block)
    s = re.sub(r"struct\s*\(\s*params\s*\)", " ", s)
    s = re.sub(r"\bparams\b", " ", s)
    for p in sorted(params, key=len, reverse=True):
        s = re.sub(r"\b%s\b" % re.escape(p), " ", s)
    return re.findall(r"[A-Za-z_][\w.]*", s)


def load_exceptions():
    """Alias and modeled rows from a seven-column ledger.

    dialect	kind	corpus	upstream	witness	category	reason

    Both are escapes, so both are listed, both are checked for freshness, and
    neither is inferred from the corpus.
    """
    aliases, modeled = {}, {}
    if not EXCEPTIONS.exists():
        return aliases, modeled
    for line in EXCEPTIONS.read_text().splitlines():
        if not line.strip() or line.lstrip().startswith("#"):
            continue
        parts = line.split("\t")
        if len(parts) < 7:
            raise SystemExit("def-audit: bad exception row: %r" % line)
        dialect, kind, corpus, upstream, witness, category, reason = [
            x.strip() for x in parts[:7]]
        if category not in CATEGORIES:
            raise SystemExit("def-audit: bad category %r in %r"
                             % (category, line))
        key = (dialect, kind, norm(corpus))
        if upstream == "-":
            if witness == "-":
                raise SystemExit("def-audit: modeled row needs a witness: %r"
                                 % line)
            if category == "rename":
                raise SystemExit("def-audit: rename row needs an upstream: %r"
                                 % line)
            modeled[key] = (corpus, witness, category, reason)
        else:
            if category != "rename":
                raise SystemExit("def-audit: alias row must be rename: %r"
                                 % line)
            aliases[key] = (corpus, upstream, reason)
    return aliases, modeled


def check_exceptions(aliases, modeled, used, rows):
    """Every row must be consumed by a corpus declaration and still resolve."""
    problems = []
    by_dialect = {r["dialect"]: r for r in rows}
    for (dialect, kind, key), (corpus, upstream, reason) in aliases.items():
        if (dialect, kind, key) not in used:
            problems.append((dialect, 0, "D8 stale",
                             "alias `%s` is not used by the corpus" % corpus))
            continue
        r = by_dialect.get(dialect)
        if not r:
            problems.append((dialect, 0, "D8 stale",
                             "alias `%s` names a missing corpus" % corpus))
            continue
        table = {"type": r["up"].types, "attr": r["up"].attrs,
                 "enum": r["up"].enums}[kind]
        if norm(upstream) not in table:
            problems.append((dialect, 0, "D8 stale",
                             "alias `%s` target `%s` does not exist"
                             % (corpus, upstream)))
    for (dialect, kind, key), (corpus, witness, category, reason) in modeled.items():
        if (dialect, kind, key) not in used:
            problems.append((dialect, 0, "D8 stale",
                             "modeled `%s` is not used by the corpus" % corpus))
            continue
        why = witness_problem(dialect, kind, witness, category, reason)
        if why:
            problems.append((dialect, 0, "D8 witness",
                             "modeled `%s`: %s" % (corpus, why)))
    return problems


def strip_strings(text):
    """Blank double-quoted strings, preserving offsets and newlines.

    Doc prose and Assembly literals are user-facing text, not declarations.
    Leaving them in the type-surface scan lets `Doc "reads: address, ..."`
    look like a use of a type named `address`.
    """
    out, i, n = [], 0, len(text)
    while i < n:
        if text[i] != '"':
            out.append(text[i])
            i += 1
            continue
        j = i + 1
        while j < n:
            if text[j] == "\\":
                j += 2
                continue
            if text[j] == '"':
                break
            j += 1
        out.append('""')
        out.append("\n" * text.count("\n", i, j + 1))
        i = j + 1
    return "".join(out)


def type_surface(text):
    """The part of a corpus file where a type name is a declaration use.

    Type names occur in `Parameters (...)`, clause bindings and constraint
    right-hand sides.  They do not occur in `Assembly { ... }`: there, `type(x)`
    is a directive and `x ":" type(y)` is a format.  Scanning the whole file
    turns both into fake references.
    """
    text = strip_strings(text)
    out, pos = [], 0
    for m in re.finditer(r"\bAssembly\s*\{", text):
        brace = text.find("{", m.start())
        end = matching(text, brace, "{", "}")
        if end < 0:
            break
        out.append(text[pos:brace])
        out.append(" " * (end - brace + 1))
        pos = end + 1
    out.append(text[pos:])
    return "".join(out)


def policy_present(text):
    """True when the file header records its definition-layer scope policy.

    `substituted` is a scope decision, not a debt, but a decision that lives
    only in the audit tool is still a silent black hole for the next reader.
    Require the dialect file to say it in the header.
    """
    return bool(POLICY_RE.search(text))


def upstream_reachable(up):
    """Def keys reachable from an op through structured ODS references.

    Only a `{"def": ...}` reference or a `!superclasses` entry is an edge.
    Descriptions, summaries and `extraClassDeclaration` are prose and C++:
    a type mentioned there is not used by an op.  Counting those strings made
    the `substituted` class claim "an op references it" on the strength of a
    sentence, which is exactly the kind of unearned evidence this audit exists
    to reject.
    """
    keys = set(up.json)
    ops = [k for k, r in up.json.items()
           if isinstance(r, dict)
           and ("Op" in (r.get("!superclasses") or []) or r.get("opName"))]
    seen, stack = set(ops), list(ops)
    prose = {"description", "summary", "extraClassDeclaration",
             "extraClassDefinition", "assemblyFormat", "!name"}

    def walk(x, out, key=None):
        if isinstance(x, dict):
            ref = x.get("def")
            if isinstance(ref, str) and ref in keys:
                out.add(ref)
            for k, v in x.items():
                if k not in prose:
                    walk(v, out, k)
        elif isinstance(x, list):
            for v in x:
                walk(v, out, key)
        elif isinstance(x, str):
            # `!superclasses` is the one string list that names records.
            if key == "!superclasses" and x in keys:
                out.add(x)

    while stack:
        k = stack.pop()
        refs = set()
        walk(up.json.get(k), refs)
        for nxt in refs:
            if nxt not in seen:
                seen.add(nxt)
                stack.append(nxt)
    return seen


def used_as_type(text, name):
    """True when `name` appears in a type position in the corpus text.

    Only `: Name`, `<Name>` and `| Name` count.  A bare word is usually the
    attribute's own name (`comdat: string`) or part of an op mnemonic
    (`llvm.func`), and counting those would turn every folding into a
    dangling reference.  `=` is a legal follower because a parameter may carry
    a default (`x: Foo = none`).
    """
    n = re.escape(name)
    return bool(re.search(r":\s*%s(?=\s*[,)>=;\]]|$)|<\s*%s\s*>|\|\s*%s\b"
                          % (n, n, n), text))


def classify_missing(up, missing, text):
    """Split upstream-but-undeclared records into scope classes.

    unaccounted  -- the corpus names it in a type position and does not
                    declare it; a dangling reference that must be fixed.
                    Checked first: ODS reachability cannot excuse a name the
                    corpus itself prints.
    unreferenced -- no op reaches it through a structured ODS reference.  It
                    may still be a parameter of a modeled type (`spirv.Dim`
                    sits inside `!spirv.image<...>`), so the claim is about
                    ODS references, never "never printed".
    substituted  -- an op reaches it, but the corpus types that field as
                    something else (i32, `any`, another constraint); a scope
                    decision, not a forgotten declaration.
    """
    reach = upstream_reachable(up)
    text = type_surface(text)
    tables = {"type": up.types, "attr": up.attrs, "enum": up.enums}
    out = {"unreferenced": [], "substituted": [], "unaccounted": []}
    for kind, names in zip(("type", "attr", "enum"), missing):
        for key in names:
            name, rec = tables[kind][key]
            if used_as_type(text, name):
                out["unaccounted"].append((kind, name))
            elif rec.get("!name") not in reach:
                out["unreferenced"].append((kind, name))
            else:
                out["substituted"].append((kind, name))
    return out


def enum_cases(record, json):
    """({spelling: value}, {group spelling: value}) for an EnumInfo record."""
    cases, groups = OrderedDict(), OrderedDict()
    for ent in record.get("enumerants") or []:
        r = json.get(ent.get("def")) or {}
        sc = r.get("!superclasses") or []
        spelling = r.get("str") or r.get("symbol") or ""
        if "BitEnumAttrCaseGroup" in sc or "BitEnumCaseGroup" in sc:
            groups[spelling] = r.get("value")
        elif "EnumCase" in sc:
            cases[spelling] = r.get("value")
    return cases, groups


def block_refs(block):
    """Identifiers a block names, excluding strings, keywords and hook names."""
    s = re.sub(r'"(?:\\.|[^"])*"', "", block)
    hooks = set(re.findall(r"custom\s*<\s*([A-Za-z_]\w*)\s*>", s))
    ids = set(re.findall(r"[A-Za-z_][\w.]*", s))
    return {i for i in ids if i not in KEYWORDS and i not in hooks}


def corpus_cases(body):
    """({spelling: value}, {group: [member spellings]}) from Cases/Groups."""
    cases, groups = OrderedDict(), OrderedDict()
    for m in CASES_RE.finditer(body):
        for part in split_top(m.group(1)):
            cm = re.match(r'\s*([A-Za-z_]\w*)\s*=\s*(-?\d+)\s*(?:"([^"]*)")?',
                          part)
            if not cm:
                continue
            cases[cm.group(3) if cm.group(3) is not None else cm.group(1)] = int(
                cm.group(2))
    for m in GROUPS_RE.finditer(body):
        for part in split_top(m.group(1)):
            gm = re.match(r"\s*([A-Za-z_]\w*)\s*=\s*([^=]+)$", part, re.S)
            if not gm:
                continue
            members = [x.strip() for x in gm.group(2).split("|") if x.strip()]
            groups[gm.group(1)] = members
    return cases, groups


def check_listing(up, decl, dialect, problems):
    """Return {norm name: declaration} for a kind and emit unresolved errors."""
    listing = {"type_def": (up.types, "type"),
               "attr_def": (up.attrs, "attr")}
    table, label = listing[decl["kind"]]
    out = OrderedDict()
    for name, d in decl["table"].items():
        hit = table.get(norm(name))
        if not hit:
            problems.append((dialect, d["line"], "D0 naming",
                             "%s `%s` has no upstream %s definition"
                             % (decl["kind"], name, label)))
            continue
        d["upstream"] = hit
        out[norm(name)] = d
    return out


def audit(path, verbose=False, aliases=None, modeled=None, used=None):
    text = path.read_text()
    m = re.search(r"(?m)^\s*dialect\s+([a-z0-9_]+)", text)
    if not m:
        return None
    dialect = m.group(1)
    up = Upstream(dialect)
    self_json = up.json
    problems = []
    aliases = aliases or {}
    modeled = modeled or {}
    used = used if used is not None else set()

    defs = parse_corpus(path)
    modeled_seen = []
    for kind in ("type_def", "attr_def"):
        by_up = OrderedDict()
        table = up.types if kind == "type_def" else up.attrs
        short = "type" if kind == "type_def" else "attr"
        for name, d in defs[kind].items():
            d["kind"] = kind
            key = (dialect, short, norm(name))
            hit = table.get(norm(name))
            if not hit and key in aliases:
                used.add(key)
                hit = table.get(norm(aliases[key][1]))
            if not hit and key in modeled:
                used.add(key)
                d["modeled"] = modeled[key]
                by_up[norm(name)] = d
                continue
            if not hit:
                problems.append((dialect, d["line"], "D0 naming",
                                 "%s `%s` has no upstream definition"
                                 % (kind, name)))
                continue
            d["upstream"] = hit
            by_up[norm(name)] = d

        def declarative(rec):
            fmt = rec.get("assemblyFormat")
            if fmt is None:
                return False
            if fmt.strip():
                return True
            # An empty format is declarative only when the framework appends
            # the attribute self type (`#ml_program.extern : tensor<..>`).
            for arg, _ in ((rec.get("parameters") or {}).get("args") or []):
                node = arg if isinstance(arg, dict) else None
                if node and "AttributeSelfTypeParameter" in (
                        (self_json.get(node.get("def")) or {}).get("!superclasses") or []):
                    return True
            return False

        for d in by_up.values():
            if d.get("modeled"):
                ch = d["channel"]
                where = "%s `%s`" % (kind, d["name"])
                modeled_seen.append((d["line"], d["name"], len(d["params"])))
                if ch is None:
                    problems.append((dialect, d["line"], "D1 channel",
                                     where + " declares no Assembly clause"))
                elif ch == "block":
                    spine = spine_tokens(d["block"], d["params"])
                    if spine:
                        problems.append((dialect, d["line"], "D11 spine",
                                         where + " modeled block is not flat: " +
                                         ", ".join(sorted(set(spine)))))
                    refs = block_refs(d["block"])
                    declared = set(d["params"])
                    families = FAMILY_RE.findall(d["block"])
                    unknown = sorted(refs - declared)
                    if unknown:
                        problems.append((dialect, d["line"], "D4 cover",
                                         where + " names unknown " +
                                         ", ".join(unknown)))
                    if not families:
                        dropped = sorted(declared - refs)
                        if dropped:
                            problems.append((dialect, d["line"], "D4 cover",
                                             where + " never prints " +
                                             ", ".join(dropped)))
                elif ch == "Modeled":
                    # Opaque: no text is claimed, so there is nothing to
                    # compare a parameter list against.  Allowed, but bounded
                    # by D12 below.
                    pass
                elif ch in ("Custom", "Generic"):
                    problems.append((dialect, d["line"], "D13 channel",
                                     where + " is modeled; %s claims an upstream"
                                     " record that does not exist" % ch))
                else:
                    problems.append((dialect, d["line"], "D1 channel",
                                     where + " has unknown channel `%s`" % ch))
                continue
            rec = d["upstream"][1]
            fmt = rec.get("assemblyFormat")
            decl = declarative(rec)
            custom = bool(rec.get("hasCustomAssemblyFormat"))
            ch = d["channel"]
            where = "%s `%s`" % (kind, d["upstream"][0])
            if ch == "Modeled":
                problems.append((dialect, d["line"], "D9 modeled",
                                 where + " is Modeled but not registered"))
            if ch is None:
                problems.append((dialect, d["line"], "D1 channel",
                                 where + " declares no Assembly clause"))
            elif ch == "block":
                if not decl:
                    problems.append((dialect, d["line"], "D2 block",
                                     where + " has a block, upstream has no assemblyFormat"))
                else:
                    block = d["block"]
                    declared = set(d["params"])
                    families = FAMILY_RE.findall(block)
                    if len(families) > 1:
                        problems.append((dialect, d["line"], "D7 family",
                                         where + " uses params more than once"))
                    if families and BAD_FAMILY.search(block):
                        problems.append((dialect, d["line"], "D7 family",
                                         where + " passes params into a hook/oilist"))
                    refs = block_refs(block)
                    unknown = sorted(refs - declared)
                    if unknown:
                        problems.append((dialect, d["line"], "D4 cover",
                                         where + " names unknown " + ", ".join(unknown)))
                    for hook in re.findall(
                            r"custom\s*<\s*([A-Za-z_]\w*)\s*>", block):
                        if hook not in (rec.get("assemblyFormat") or ""):
                            problems.append((dialect, d["line"], "D14 hook",
                                             where + " hook `%s` is not in the"
                                             " upstream format" % hook))
                    if families:
                        if refs & declared:
                            problems.append((dialect, d["line"], "D7 family",
                                             where + " mixes params with " +
                                             ", ".join(sorted(refs & declared))))
                        if "params" not in (fmt or ""):
                            problems.append((dialect, d["line"], "D7 family",
                                             where + " uses params, upstream format does not"))
                        if ("struct" in families[0]
                                and "struct(params)" not in (fmt or "").replace(" ", "")):
                            problems.append((dialect, d["line"], "D7 family",
                                             where + " uses struct(params), upstream does not"))
                    else:
                        dropped = sorted(declared - refs)
                        if dropped:
                            problems.append((dialect, d["line"], "D4 cover",
                                             where + " never prints " + ", ".join(dropped)))
            elif ch == "Custom":
                if not custom or decl:
                    problems.append((dialect, d["line"], "D3 custom",
                                     where + " is Custom, upstream is not"))
            elif ch == "Generic":
                if decl or custom:
                    problems.append((dialect, d["line"], "D3 generic",
                                     where + " is Generic, upstream has a format"))
            else:
                problems.append((dialect, d["line"], "D1 channel",
                                 where + " has unknown channel `%s`" % ch))
            if verbose:
                print("  %s %s: %s" % (kind, d["upstream"][0], ch))

    # D12 budget: a modeled declaration has no upstream ODS record, so its
    # parameter list and (spine) text cannot be falsified by tblgen.  The
    # escape stays narrow: at most 12 modeled definitions per dialect, and at
    # most 4 of them may declare parameters.  Beyond that the dialect is
    # hiding structure in the escape instead of modeling it.
    if len(modeled_seen) > 12:
        problems.append((dialect, 0, "D12 budget",
                         "%d modeled definitions > 12" % len(modeled_seen)))
    with_params = [x for x in modeled_seen if x[2]]
    if len(with_params) > 4:
        problems.append((dialect, 0, "D12 budget",
                         "%d modeled definitions declare parameters > 4: %s"
                         % (len(with_params),
                            ", ".join(x[1] for x in with_params))))

    for name, d in defs["enum"].items():
        key = (dialect, "enum", norm(name))
        hit = up.enums.get(norm(name))
        if not hit and key in aliases:
            used.add(key)
            hit = up.enums.get(norm(aliases[key][1]))
        if not hit and key in modeled:
            used.add(key)
            d["modeled"] = modeled[key]
            continue
        if not hit:
            problems.append((dialect, d["line"], "D0 naming",
                             "enum `%s` has no upstream enum" % name))
            continue
        rec = hit[1]
        up_cases, up_groups = enum_cases(rec, up.json)
        c_cases, c_groups = corpus_cases(d["body"])
        where = "enum `%s`" % hit[0]
        bit = any(x in (rec.get("!superclasses") or [])
                  for x in ("BitEnumAttr", "BitEnumBase"))
        if d["bits"] != bit:
            problems.append((dialect, d["line"], "D6 kind",
                             where + (" should be bits" if bit else " should not be bits")))
        if dict(c_cases) != dict(up_cases):
            missing = sorted(set(up_cases) - set(c_cases))
            extra = sorted(set(c_cases) - set(up_cases))
            wrong = sorted(k for k in set(up_cases) & set(c_cases)
                           if up_cases[k] != c_cases[k])
            problems.append((dialect, d["line"], "D5 cases",
                             where + " cases differ: missing=%s extra=%s wrong=%s"
                             % (missing[:6], extra[:6], wrong[:6])))
        if set(c_groups) != set(up_groups):
            problems.append((dialect, d["line"], "D5 cases",
                             where + " groups differ: missing=%s extra=%s"
                             % (sorted(set(up_groups) - set(c_groups))[:6],
                                sorted(set(c_groups) - set(up_groups))[:6])))
        for g, members in c_groups.items():
            if g not in up_groups:
                continue
            vals = [c_cases.get(x) for x in members]
            if any(v is None for v in vals):
                problems.append((dialect, d["line"], "D5 cases",
                                 where + " group `%s` names an unknown case" % g))
            elif sum(vals) != up_groups[g]:
                problems.append((dialect, d["line"], "D5 cases",
                                 where + " group `%s` value %d != upstream %d"
                                 % (g, sum(vals), up_groups[g])))

    # An enum attribute is covered by its mnemonic (`attr_def fastmath`) or by
    # its value domain (`enum FastMathFlags`); both are faithful models, and
    # the corpus uses the second wherever the op field already names the
    # attribute.
    declared_enums = {norm(x) for x in defs["enum"]}
    covered_enum_attrs = set()
    for key, ref in up.enum_attrs.items():
        cls = (up.json.get(ref) or {}).get("className")
        if cls and norm(cls) in declared_enums:
            covered_enum_attrs.add(key)
    # An alias makes the upstream spelling absent from the corpus by design;
    # subtract its target so a rename is not reported as a forgotten record.
    alias_targets = {
        kind: {norm(target) for (d, k, key), (corpus, target, reason)
               in aliases.items() if d == dialect and k == kind}
        for kind in ("type", "attr", "enum")}
    missing = (sorted(set(up.types) - {norm(x) for x in defs["type_def"]}
                      - alias_targets["type"]),
               sorted(set(up.attrs) - {norm(x) for x in defs["attr_def"]}
                      - covered_enum_attrs - alias_targets["attr"]),
               sorted(set(up.enums) - declared_enums - alias_targets["enum"]))
    # A declaration with no upstream record is only "extra" (fabricated) when
    # the ledger does not account for it.  Aliases and modeled rows are the
    # licensed cases; anything else must not hide behind them.
    def licensed(kind, names):
        return {norm(x) for x in names
                if (dialect, kind, norm(x)) in aliases
                or (dialect, kind, norm(x)) in modeled}

    extra = (sorted({norm(x) for x in defs["type_def"]} - set(up.types)
                    - licensed("type", defs["type_def"])),
             sorted({norm(x) for x in defs["attr_def"]} - set(up.attrs)
                    - licensed("attr", defs["attr_def"])),
             sorted({norm(x) for x in defs["enum"]} - set(up.enums)
                    - licensed("enum", defs["enum"])))
    text = path.read_text()
    scope = classify_missing(up, missing, strip_comments(text))
    return {
        "dialect": dialect,
        "file": path.name,
        "text": text,
        "up": up,
        "defs": defs,
        "problems": problems,
        "missing": missing,
        "scope": scope,
        "extra": extra,
    }


def audit_all():
    coverage_mode = "--coverage" in sys.argv
    verbose = "--verbose" in sys.argv
    rows, problems, failed = [], [], []
    aliases, modeled = load_exceptions()
    used = set()
    for path in sorted(CORPUS.glob("*.dialect")):
        r = audit(path, verbose, aliases, modeled, used)
        if not r:
            continue
        if r["up"].failed:
            failed.append((r["dialect"], r["up"].failed[0]))
        rows.append(r)
        problems += r["problems"]
    if "--coverage" not in sys.argv:
        problems += check_exceptions(aliases, modeled, used, rows)

    print("## definition audit")
    print("| dialect | type | attr | enum | problems |")
    print("| --- | --- | --- | --- | --- |")
    for r in rows:
        print("| %s | %d | %d | %d | %d |"
              % (r["dialect"], len(r["up"].types), len(r["up"].attrs),
                 len(r["up"].enums), len(r["problems"])))

    if coverage_mode:
        print("\n## definition coverage")
        print("| dialect | declared | unreferenced | substituted | unaccounted | extra |")
        print("| --- | --- | --- | --- | --- | --- |")
        total = {"unreferenced": 0, "substituted": 0, "unaccounted": 0}
        for r in rows:
            sc = r["scope"]
            e = r["extra"]
            for k in total:
                total[k] += len(sc[k])
            if any(sc.values()) or any(e):
                extra = e[0] + e[1] + e[2]
                print("| %s | %d | %d | %d | %d | %s |"
                      % (r["dialect"],
                         sum(len(r["defs"][k]) for k in ("type_def", "attr_def", "enum")),
                         len(sc["unreferenced"]), len(sc["substituted"]),
                         len(sc["unaccounted"]),
                         ", ".join(extra) or "-"))
        print("\nmissing definitions: %d (unreferenced %d, substituted %d,"
              " unaccounted %d)"
              % (sum(total.values()), total["unreferenced"],
                 total["substituted"], total["unaccounted"]))
        for r in rows:
            for kind, name in r["scope"]["unaccounted"]:
                problems.append((r["dialect"], 0, "C1 dangling",
                                 "%s `%s` is used as a type but not declared"
                                 % (kind, name)))
            sub = len(r["scope"]["substituted"])
            if sub and not policy_present(r["text"]):
                problems.append((r["dialect"], 0, "C2 policy",
                                 "%d substituted definitions but no "
                                 "`/// 定义层范围:` header note" % sub))
        if verbose:
            print("\n## scope classes")
            for r in rows:
                for cls in ("unreferenced", "substituted"):
                    for kind, name in r["scope"][cls]:
                        print("- %s %s %s `%s`"
                              % (r["dialect"], cls, kind, name))

    if problems:
        print("\n## problems")
        for d, line, rule, msg in problems:
            print("- %s:%d: %s: %s" % (d, line, rule, msg))
    if failed:
        print("\n## tblgen failures")
        for d, f in failed:
            print("- %s: %s: %s" % (d, f[0], f[1][0] if f[1] else ""))
    print("\n%d definitions checked, %d problems, %d tblgen failures"
          % (sum(len(d["type_def"]) + len(d["attr_def"]) + len(d["enum"])
                 for r in rows for d in [r["defs"]]), len(problems), len(failed)))
    return 1 if problems or failed else 0


def selftest():
    """Pin the definition-layer guards that a clean corpus cannot prove."""
    cases, bad = [], 0

    def check(label, got, want):
        nonlocal bad
        ok = got == want
        bad += 0 if ok else 1
        cases.append((label, "ok" if ok else "FAIL: %r != %r" % (got, want)))

    check("spine: literals and params",
          spine_tokens('"<" a "," b ">"', ["a", "b"]), [])
    check("spine: struct(params)",
          spine_tokens('"<" struct(params) ">"', ["a"]), [])
    check("spine: rejects if",
          spine_tokens("a if b { b }", ["a", "b"]), ["if"])
    check("spine: rejects directive",
          spine_tokens("type(a)", ["a"]), ["type"])
    check("witness: missing file",
          witness_problem("gpu", "type", "no/such/file.h:1", "cxx-type", "`X`"),
          "witness no/such/file.h:1 does not exist")
    check("witness: category vs kind",
          witness_problem("gpu", "type",
                          "mlir/include/mlir/Dialect/GPU/IR/GPUBase.td:183",
                          "borrowed-enum", "`SparseDnTensorHandleType`"),
          "borrowed-enum must be an enum")
    check("witness: builtin path",
          witness_problem("gpu", "type",
                          "mlir/include/mlir/Dialect/GPU/IR/GPUBase.td:183",
                          "builtin", "`SparseDnTensorHandleType`"),
          "builtin witness must be under mlir/include/mlir/IR/")
    check("witness: symbol absent",
          witness_problem("gpu", "type",
                          "mlir/include/mlir/Dialect/GPU/IR/GPUBase.td:183",
                          "cxx-type", "`NotOnThisLine`"),
          "witness line does not mention `NotOnThisLine`")
    check("witness: symbol present",
          witness_problem("gpu", "type",
                          "mlir/include/mlir/Dialect/GPU/IR/GPUBase.td:183",
                          "cxx-type", "`SparseDnTensorHandleType`"), None)
    check("type position: attribute",
          used_as_type("Attributes (x: Foo);", "Foo"), True)
    check("type position: generic", used_as_type("optional<array<Foo>>", "Foo"), True)
    check("type position: union", used_as_type("constraint c = a | Foo;", "Foo"), True)
    check("type position: own name", used_as_type("Attributes (foo: string);", "foo"), False)
    check("type position: op mnemonic", used_as_type("op llvm.func {", "func"), False)
    check("type position: default value",
          used_as_type("Parameters (x: Foo = none);", "Foo"), True)
    check("type position: doc prose is not a use",
          used_as_type(type_surface('Doc "reads: Foo, i1 mask";'), "Foo"), False)
    check("type position: assembly directive is not a use",
          used_as_type(type_surface('Assembly { x ":" type(y) }'), "type"), False)
    check("type position: outside assembly still counts",
          used_as_type(type_surface('Doc "x"; Assembly { "!" } Parameters (e: Foo);'),
                       "Foo"), True)

    class _Up:
        json = {
            "OpA": {"!superclasses": ["Op"],
                    "description": "mentions TypeB in prose",
                    "arguments": {"args": [[{"def": "TypeC", "kind": "def"},
                                            "x"]]}},
            "TypeB": {"!superclasses": ["TypeDef"]},
            "TypeC": {"!superclasses": ["TypeDef"]},
        }
    reach = upstream_reachable(_Up())
    check("reach: structured reference", "TypeC" in reach, True)
    check("reach: prose is not an edge", "TypeB" in reach, False)
    check("policy: header marker", policy_present("/// 定义层范围：x\n"), True)
    check("policy: body mention only", policy_present("op x { Doc \"定义层范围：\"; }"), False)
    check("witness: no quoted symbol",
          witness_problem("gpu", "type",
                          "mlir/include/mlir/Dialect/GPU/IR/GPUBase.td:183",
                          "cxx-type", "a handle type"),
          "modeled row must quote the witness symbol in its reason")

    print("## selftest")
    for label, verdict in cases:
        print("| %s | %s |" % (label, verdict))
    print("| --- | --- |")
    print("%d cases, %d failed" % (len(cases), bad))
    return 1 if bad else 0


def main():
    if "--selftest" in sys.argv:
        return selftest()
    return audit_all()


if __name__ == "__main__":
    sys.exit(main())
