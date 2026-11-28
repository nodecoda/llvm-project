#!/usr/bin/env python3
"""arith-check.py - 用真实语料验证 arith-grammar.y，并跑 24 例自测。

两个检查：

1. 语料：把 `mlir/test/Dialect/Arith/*.mlir` 全部喂给参考解析器。要求
   - 没有任何"意外"失败：每一条被拒的语句，上游自己也在同一处留了
     `expected-error`（这就是负语料，本来就该拒）；
   - 不静默丢语句：逐文件比较"文本里的语句头行数"与"解析出的语句数 + 诊断数"，
     两者必须相等。语句头用正则从源文本数，与解析器无关。这一条抓到过两个
     参考词法器 bug：`{` 紧跟 SSA 被当成 attr-dict，吞掉 `scf.for` 的循环体；
     `({ ... })` 被当成函数类型，吞掉别的方言的 region 体（共 12 条语句）。

2. 自测：26 章 §4 的事实各有一个正例/负例，外加 attr-dict、loc、
   dense_resource、函数类型、`({ ... })` region、`{-# ... #-}` 资源块、
   generic 形式等边界。

用法：
    python3 tools/arith-check.py            # 语料 + 自测
    python3 tools/arith-check.py --corpus   # 只跑语料
    python3 tools/arith-check.py --selftest # 只跑自测
"""

import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
DOC = os.path.dirname(HERE)                      # ana-docs/mlir
ROOT = os.path.dirname(os.path.dirname(DOC))     # 仓库根
ARITH_TESTS = os.path.join(ROOT, "mlir", "test", "Dialect", "Arith")
GRAMMAR = os.path.join(DOC, "arith-grammar.y")
LEXER = os.path.join(HERE, "arith-lex.c")


def build(tmp):
    """bison + cc，返回参考解析器路径。"""
    subprocess.run(
        ["bison", "-Wall", "-Wcounterexamples", "-Werror",
         "-d", "-o", os.path.join(tmp, "ag.c"), GRAMMAR],
        check=True, capture_output=True, text=True,
    )
    binary = os.path.join(tmp, "arith")
    subprocess.run(
        ["cc", "-I" + tmp, "-O1", "-Wall", "-Wextra", "-Werror",
         "-o", binary, os.path.join(tmp, "ag.c"), LEXER],
        check=True, capture_output=True, text=True,
    )
    return binary


def run(binary, path):
    """返回 (语句数, [(行号, 消息)])。"""
    p = subprocess.run([binary, path], capture_output=True, text=True)
    stmts, errors = 0, []
    for line in p.stdout.splitlines():
        if " statements, " in line:
            stmts = int(line.split(": ")[-1].split(" ")[0])
    for line in p.stderr.splitlines():
        # <input>:128: error: syntax error
        parts = line.split(":")
        if len(parts) >= 3 and parts[0] == "<input>":
            errors.append((int(parts[1]), line.split("error: ", 1)[-1]))
    return stmts, errors


# 语句头：可选的结果前缀（`%a =`、`%a, %b =`、`%x:2 =`）加 `arith.<助记符>`。
# 行首限定是刻意的：多行语句的续行以 `:` 或逗号开头，不会重复计数；
# 注释行（`//`）在上游是见证文本，不是语句。
HEAD_RE = re.compile(
    r"^\s*(?:%[A-Za-z0-9_.$-]+(?::\d+)?"
    r"(?:\s*,\s*%[A-Za-z0-9_.$-]+(?::\d+)?)*\s*=\s*)?arith\.([a-z_0-9]+)\b"
)


def count_heads(path):
    """源文本里的语句头数量，与解析器无关。"""
    n = 0
    for line in open(path, errors="ignore"):
        if line.lstrip().startswith("//"):
            continue
        if HEAD_RE.match(line):
            n += 1
    return n


def expected_error_near(path, line):
    """上游是否在 line 附近声明了 expected-error（多行语句留 3 行余量）。"""
    lines = open(path, errors="ignore").read().splitlines()
    lo = max(0, line - 4)
    return any("expected-error" in lines[i] for i in range(lo, min(len(lines), line)))


def check_corpus(binary):
    files = sorted(
        os.path.join(ARITH_TESTS, f)
        for f in os.listdir(ARITH_TESTS) if f.endswith(".mlir")
    )
    total, negative, unexpected, dropped = 0, 0, [], []
    for path in files:
        stmts, errors = run(binary, path)
        total += stmts
        heads = count_heads(path)
        if heads != stmts + len(errors):
            dropped.append((os.path.basename(path), heads, stmts, len(errors)))
        for line, msg in errors:
            if expected_error_near(path, line):
                negative += 1
            else:
                unexpected.append((os.path.basename(path), line, msg))
    heads = sum(count_heads(p) for p in files)
    print(f"语料: {len(files)} 文件, {heads} 个语句头, {total} 条语句, "
          f"{negative} 条负语料被拒, {len(unexpected)} 条意外失败, "
          f"{len(dropped)} 个文件丢语句")
    for name, line, msg in unexpected:
        print(f"  意外: {name}:{line}: {msg}")
    for name, h, s_, e in dropped:
        print(f"  丢语句: {name}: 语句头 {h} != 语句 {s_} + 诊断 {e}")
    return not unexpected and not dropped


# (片段, 期望解析成功, 说明)
CASES = [
    ("%s, %o = arith.subui_extended %a, %a\n    : tensor<?xi32>, tensor<?xi1>", True,
     "语句边界由文法定，不由换行/括号平衡（canonicalize.mlir:5607）"),
    ("arith.addf (%a, %a) : f32", False,
     "操作数列表不能加括号（invalid.mlir:135）"),
    ("arith.addf {%a, %a} : f32", False,
     "操作数列表不能加花括号（invalid.mlir:142）"),
    ("arith.addf %a, %a, %a : f32", False,
     "操作数个数精确（invalid.mlir:128）"),
    ("arith.cmpi %a, %b : i32", False,
     "比较语句必须有谓词（invalid.mlir:245）"),
    ('arith.cmpi "eq", %a, %b : i8', True,
     "谓词可以加引号（canonicalize.mlir:3120）"),
    ("arith.cmpf true, %a, %b : f32", True,
     "true/false 也是 CmpFPredicate 的拼写（canonicalize.mlir:4572）"),
    ("arith.cmpi eq, %a, %b : () -> ()", True,
     "类型可以是函数类型（invalid.mlir:294 的文本必须能解析）"),
    ("arith.constant true", True,
     "布尔常量的类型在值里，没有 `: type`"),
    ('arith.constant {crazy = "func.foo"} 43 : index', True,
     "常量的 attr-dict 在值之前（ops.mlir:1204）"),
    ("%c = arith.constant %x", False,
     "常量值不能是 SSA 名（invalid.mlir:925）"),
    ("arith.constant dense_resource<blob1> : tensor<i8>", True,
     "dense_resource 是值的一种（canonicalize.mlir:5376）"),
    ("%l, %h = arith.mulsi_extended %a, %b : i32", True,
     "2 结果 1 类型；结果个数不由类型列表推（ArithOps.td:mulsi_extended）"),
    ("arith.cmpi slt, %inner#0, %n : i32", True,
     "SSA 名可以带结果号（int-range-analysis-convergence.mlir:78）"),
    ("arith.truncf %a to_nearest_away : f32 to bf16", True,
     "舍入模式有 5 个，含 to_nearest_away（ops.mlir:804）"),
    ("arith.divsi %a, %b exact {test.marker} : i32", True,
     "attr-dict 可以跟在旗标之后（unsigned-when-equivalent.mlir:53）"),
    ("arith.subi %a, %b overflow<nsw, nuw> : index", True,
     "overflow 旗标列表"),
    ("arith.addi %a, %b : i32 loc(#loc1)", True,
     "loc 后缀可选"),
    ("arith.addi %a, %b fastmath<bogus> : f32", False,
     "旗标名由词法器校验"),
    ('%0 = "arith.constant"(){value = 42 : i32} : () -> i32', None,
     "generic 形式不属本方言语句，整体当注释"),
    ("scf.for %i = %c0 to %n step %c1 {\n"
     "  %0 = arith.subi %n, %i : index\n"
     "  %1 = arith.cmpi slt, %0, %c0 : index\n"
     "}", True,
     "别的方言的 region 体不吞语句：`{` 后有 SSA 就不是 attr-dict\n"
     "     （int-range-narrowing.mlir:728 起 4 条）"),
    ('%0 = "test.region"(%a) ({\n  ^bb0(%v: f32):\n'
     "    %s = arith.addf %v, %v : f32\n"
     '    "test.return"(%s) : (f32) -> ()\n'
     "  }) : (f32) -> f32", True,
     "`({ ... })` 是 region，不是函数类型（select-common-elementwise.mlir:62）"),
    ("{-#\n  dialect_resources: { builtin: { blob1: \"0x01\" } }\n#-}\n"
     "arith.constant 1 : i32", True,
     "`{-# ... #-}` 是资源块注释，不是语句（canonicalize.mlir:5518）"),
    ('arith.constant {foo = "100%"} 1 : i32', True,
     "attr-dict 里的字符串可以含 `%`，不能被当成 region"),
]


def check_selftest(binary):
    failed = 0
    with tempfile.TemporaryDirectory() as tmp:
        for idx, (src, want_ok, why) in enumerate(CASES):
            path = os.path.join(tmp, f"case{idx}.mlir")
            with open(path, "w") as f:
                f.write(src + "\n")
            stmts, errors = run(binary, path)
            if want_ok is None:
                ok = stmts == 0 and not errors
            else:
                ok = (not errors) if want_ok else bool(errors)
            if not ok:
                failed += 1
                print(f"  失败 {idx}: {why}")
                print(f"        片段: {src!r}")
                print(f"        实际: {stmts} statements, {len(errors)} errors")
    print(f"自测: {len(CASES)} 例, {failed} 例失败")
    return failed == 0


def main():
    only_corpus = "--corpus" in sys.argv
    only_self = "--selftest" in sys.argv
    with tempfile.TemporaryDirectory() as tmp:
        binary = build(tmp)
        ok = True
        if not only_self:
            ok = check_corpus(binary) and ok
        if not only_corpus:
            ok = check_selftest(binary) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
