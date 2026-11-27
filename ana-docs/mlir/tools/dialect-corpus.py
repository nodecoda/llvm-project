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
# 核心事实词表：语义在 19 章定义，任何方言可用。其余事实是"方言本地事实"，
# 必须在方言文档里解释；同一名字被 3 个以上方言使用仍不在核心表内 → 违规。
# 类型名也是逃逸点：declared（本语料声明）与 builtin（语言/IR 内建）之外的名字
# 必须被报告，否则拼错一个类型名会被静默当成"外部类型"。
BUILTIN_TYPES = {
    "i1", "i8", "i16", "i32", "i64", "f16", "bf16", "f32", "f64",
    "f8e4m3fn", "f8e5m2", "f8e8m0fnu", "f6e2m3fn", "f6e3m2fn", "f4e2m1fn",
    "index", "any", "none", "unit", "string", "bool", "dict", "dictionary",
    "tensor", "vector", "memref", "complex", "tuple", "function_type",
    "flat_symbol_ref", "attribute_value", "successor", "region",
    "integer", "signless_integer", "unsigned_integer", "float", "type",
    "block", "attribute", "affine_map", "affine_set", "symbol_ref",
    "opaque", "i4", "i2", "f80", "f128", "none_type",
}
TYPE_KEYWORDS = {"optional", "variadic", "array", "type_of"}

CORE_FACTS = {
    # 结构 / trait
    "pure", "commutative", "speculatable", "terminator", "elementwise", "constant_like",
    "symbol", "symbol_table", "symbol_visibility", "isolated_from_above",
    "recursive_effects", "no_memory_effect", "automatic_allocation_scope",
    "destination_style", "contraction", "return_like", "no_region_args",
    "operand_segments_by_attribute", "memory_effect",
    "affine_scope", "infer_tensor_type", "same_variadic_operand_size",
    # region
    "single_block", "min_blocks", "max_blocks", "implicit_terminator",
    "no_terminator",
    # 绑定：值域与基数
    "positive", "non_negative", "cardinality", "arity",
    # 关系
    "has_parent", "same_type", "same_shape", "same_rank", "same_element_type",
    "broadcastable", "element_type_matches", "unwraps_element_type",
    "entry_args_unwrap", "yield_types_match",
}

ASM_DIRECTIVES = {"attr-dict", "attr-dict-with-keyword", "prop-dict", "type",
                  "gap",
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
        m = re.search(r"(?m)^[ \t]*(\w+)[ \t]+(?:(\"[^\"]+\")|([A-Za-z_][\w.]*))",
                      text[pos:])
        if not m:
            break
        kind = m.group(1)
        name = m.group(3) if m.group(3) else m.group(2)[1:-1]
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


def split_top(expr):
    """按顶层逗号切分（跳过括号/尖括号内部）。"""
    out, depth, cur = [], 0, ""
    for c in expr:
        if c in "(<[{":
            depth += 1
        elif c in ")>]}":
            depth -= 1
        if c == "," and depth == 0:
            out.append(cur)
            cur = ""
        else:
            cur += c
    out.append(cur)
    return out


def fact_names(body):
    """事实原子：Properties 列表，以及绑定/region 的方括号注解。"""
    out = []
    b = re.sub(r'"(?:\\.|[^"])*"', " ", body)      # 字符串字面量不是事实
    for m in re.finditer(r"Properties\s*\[(.*?)\]\s*;", b, re.S):
        out += top_names(m.group(1))
    for m in re.finditer(r"(?:Operands|Results|Attributes|Successors|Parameters)"
                         r"\s*\((.*?)\)\s*;", b, re.S):
        for ann in re.findall(r"\[([^\[\]]*)\]", m.group(1)):
            out += top_names(ann)
    for m in re.finditer(r"Regions\s*\((.*?)\)\s*;", b, re.S):
        for ann in re.findall(r"\[([^\[\]]*)\]", m.group(1)):
            out += top_names(ann)
    return out


def asm_refs(body):
    refs = set()
    for blk in blocks(body, "Assembly"):
        blk = re.sub(r'"(?:\\.|[^"])*"', " ", blk)   # literals are not refs
        blk = re.sub(r"custom<[^>]*>", " ", blk)     # hook names are not refs
        blk = re.sub(r"\bGap\s*\(\s*[a-z_]\w*\s*\)", " ", blk)  # gap 名不是部件
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

    # 逃逸点：语言表达不了的东西必须显式写在某个点上，并且可数。
    escape = OrderedDict()
    all_facts = Counter()
    per_file_facts = {}
    for path in files:
        text = file_text[path.name]
        ents = [e for e in entities(text) if e[0] != "dialect"]
        custom_asm = len(re.findall(r"Assembly\s+Custom\s*;", text))
        # 通用格式 = 既没有自己的 Assembly，也没有从 schema 继承（没有 from）。
        generic = sum(1 for k, _, h, b in ents
                      if k == "op" and not re.search(r"\bfrom\b", h)
                      and not re.search(r"\bAssembly\b", b))
        hooks = sum(len(re.findall(r"custom<", b)) for _, _, _, b in ents)
        fn = [n for _, _, _, b in ents for n in fact_names(b)]
        all_facts.update(fn)
        per_file_facts[path.name] = set(fn)
        escape[path.name] = {"Custom": custom_asm, "generic": generic,
                             "custom<>": hooks, "facts": len(fn),
                             "distinct": len(set(fn))}

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

    # 死声明检查：dialect 实体的 body 就是整段文本，若把它算进去，每个声明名
    # 都会在自己方言的文本里被数到一次，检查恒为 none（第八轮实证）。因此跳过
    # dialect 容器，并剥掉声明自身的名字一次。
    used_names = Counter()
    for fname, kind, name, header, body in all_entities:
        if kind == "dialect":
            continue
        head = header
        if kind == "schema" or kind == "enum":
            head = re.sub(r"\b%s\b" % re.escape(name), "", head, count=1)
        for w in WORD.findall(body + " " + head):
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
    # 文本通道：每个 op 必须显式声明一种（块 / Custom / Generic）。
    channels = OrderedDict()
    no_channel = []
    for path in files:
        text = file_text[path.name]
        blk = hook = gen = gap = 0
        for kind, name, header, body in entities(text):
            if kind != "op":
                continue
            if re.search(r"Assembly\s+Custom\s*;", body):
                hook += 1
            elif re.search(r"Assembly\s+Generic\s*;", body):
                gen += 1
            elif re.search(r"Assembly\s*\{", body) or re.search(r"\bfrom\b", header):
                blk += 1          # 块格式：直接写，或从 schema 继承
                if re.search(r"\bGap\s*\(\s*([a-z_][\w]*)\s*\)", body):
                    gap += 1
            else:
                no_channel.append("%s:%s" % (path.name, name))
        channels[path.name] = {"block": blk, "Custom": hook,
                               "Generic": gen, "gap": gap}

    local = Counter({k: v for k, v in all_facts.items() if k not in CORE_FACTS})
    spread = Counter()
    for fname, names in per_file_facts.items():
        for n in names:
            spread[n] += 1
    unpromoted = sorted(n for n in CORE_FACTS if n not in all_facts)
    overloaded = sorted(n for n, c in spread.items()
                        if c >= 3 and n not in CORE_FACTS)

    print("\n## escape points")
    print("| file | Assembly Custom | generic | custom<> | facts | distinct |")
    print("| --- | --- | --- | --- | --- | --- |")
    for name, e in escape.items():
        print("| %s | %d | %d | %d | %d | %d |"
              % (name, e["Custom"], e["generic"], e["custom<>"],
                 e["facts"], e["distinct"]))
    tot = {k: sum(e[k] for e in escape.values()) for k in
           ("Custom", "generic", "custom<>", "facts")}
    once = [n for n, c in all_facts.items() if c == 1]
    print()
    print("## escape totals: Custom=%d, generic=%d, custom<>=%d, facts=%d "
          "(distinct %d, single-use %d)"
          % (tot["Custom"], tot["generic"], tot["custom<>"], tot["facts"],
             len(all_facts), len(once)))

    print("\n## text channels")
    print("| file | block | Custom | Generic | gap |")
    print("| --- | --- | --- | --- | --- |")
    for name, c in channels.items():
        print("| %s | %d | %d | %d | %d |" % (name, c["block"], c["Custom"],
                                            c["Generic"], c["gap"]))
    print("ops without a text channel:", no_channel or "none")
    gaps = [n for path in files for k, n, h, body in entities(file_text[path.name])
            if k == "op" and re.search(r"\bGap\s*\(", body)]
    print("ops with a named gap:", len(gaps), ",".join(gaps) or "none")
    print("## fact vocabulary: core=%d used, local(distinct)=%d, "
          "local(single-use)=%d"
          % (len([n for n in CORE_FACTS if n in all_facts]), len(local),
             len([n for n, c in local.items() if c == 1])))
    print("local facts:", ", ".join("%s=%d" % kv for kv in local.most_common()))
    print("core, unused:", unpromoted or "none")
    print("local but used by >=3 dialects (must promote):", overloaded or "none")

    # 类型名解析按 R7 的三层可见性：
    #   1. 本方言声明（本文件）；
    #   2. 内建类型 / 类型构造关键字；
    #   3. 其它方言的声明——允许，但必须单独列出，因为一个方言的拼写错误
    #      会被另一个方言的同名声明掩盖（22 章 §6 的 `scalar` 事故）。
    # 都不命中才是外部署名。
    all_declared = set()
    own_declared = {}
    for path in files:
        own = set()
        for k, n, _, _ in entities(file_text[path.name]):
            if k in ("type_def", "constraint", "enum", "attr_def"):
                all_declared.add(n)
                own.add(n)
        own_declared[path.name] = own
    unresolved = {}
    cross_refs = {}
    for path in files:
        text = file_text[path.name]
        names = set()
        for kind, name, header, body in entities(text):
            if kind == "dialect":
                continue
            # `constraint x = a | b;` 是 ; 结尾的声明：右侧在 header 里，body 为空。
            body = body + "\n" + header
            for m in re.finditer(r"(?:Operands|Results|Attributes|Successors|"
                                 r"Parameters)\s*\((.*?)\)\s*;", body, re.S):
                for part in split_top(m.group(1)):
                    bm = re.match(r"\s*[A-Za-z_][\w.]*\s*:\s*(.+)$", part, re.S)
                    if not bm:
                        continue
                    texpr = bm.group(1)
                    texpr = texpr.split("[")[0]      # 注解不是类型
                    texpr = texpr.split("=")[0]      # 默认值不是类型
                    texpr = re.sub(r"\w+\s*\([^)]*\)", " ", texpr)  # type_of(x)
                    names |= set(re.findall(r"[A-Za-z_][\w.]*", texpr))
            for m in re.finditer(r"constraint\s+\w+\s*=\s*([^;]+);", body):
                names |= set(re.findall(r"[A-Za-z_][\w.]*", m.group(1)))
        own = own_declared[path.name]

        def resolved_here(n):
            if n in own or n in BUILTIN_TYPES or n in TYPE_KEYWORDS:
                return True
            # MLIR 的整型/浮点内建类型是任意位宽的 iN / fN（i14、i128 都真实存在）。
            if re.fullmatch(r"(?:i|f|bf)\d+", n):
                return True
            # 限定名 pdl.range 的头部 pdl 在别处声明，末段是类型名
            return "." in n and n.split(".")[-1] in own

        def resolved_elsewhere(n):
            if n in all_declared:
                return True
            return "." in n and n.split(".")[-1] in all_declared

        unresolved[path.name] = sorted(
            n for n in names if not resolved_here(n) and not resolved_elsewhere(n))
        cross_refs[path.name] = sorted(
            n for n in names if not resolved_here(n) and resolved_elsewhere(n))

    print("\n## external type names (neither declared in corpus nor builtin)")
    print("These must be recognizable domain names; a typo shows up here.")
    for name, rest in unresolved.items():
        if rest:
            print("| %s | %s |" % (name, ", ".join(rest)))

    print("\n## cross-dialect type names (declared in another dialect, not in this one)")
    print("Reuse is allowed; it is listed so a same-spelled local typo cannot hide behind it.")
    cross_total = 0
    for name, rest in cross_refs.items():
        if rest:
            cross_total += len(rest)
            print("| %s | %s |" % (name, ", ".join(rest)))
    print("cross-dialect references: %d" % cross_total)

    print("\n## checks")
    print("missing `from`/`extends` targets:", missing_from or "none")
    print("dangling assembly refs:", dangling or "none")
    print("dead declarations:", unused or "none")
    print("C++ leakage:", leakage or "none")


if __name__ == "__main__":
    main()
