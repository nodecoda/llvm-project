"""Command line entry point for the X86 legalizer corpus reader."""

import argparse
import hashlib
import json
import os
import subprocess
import sys

import domain
import emit
import parse
import target
import verify


def load_source(args):
    if args.input:
        with open(args.input, "r", encoding="utf-8") as handle:
            text = handle.read()
        return text, {
            "rev": None,
            "path": os.path.abspath(args.input),
            "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
            "lines": text.count("\n") + (0 if text.endswith("\n") else 1),
        }
    if not args.source_rev:
        raise SystemExit("either --input or --source-rev is required")
    spec = f"{args.source_rev}:{args.source_path}"
    proc = subprocess.run(
        ["git", "-C", args.repo, "show", spec],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        raise SystemExit(proc.stderr.strip() or f"git show {spec} failed")
    text = proc.stdout
    return text, {
        "rev": args.source_rev,
        "path": args.source_path,
        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "lines": text.count("\n") + (0 if text.endswith("\n") else 1),
    }


def parse_input(args):
    text, source = load_source(args)
    features = target.CONFIGS[args.config]
    result = parse.parse_source(text, source, args.config, features)
    if result.unknown_methods and not args.allow_opaque:
        for method, line in result.unknown_methods:
            print(f"unknown chain method {method} at line {line}", file=sys.stderr)
        raise SystemExit(2)
    return result, source


def cmd_parse(args):
    result, source = parse_input(args)
    emit.write_json(args.out, emit.ast_document(result, source))
    census = result.census()
    print(json.dumps({"out": args.out, **census}, sort_keys=True))
    if result.unknown_refs:
        print(f"{len(result.unknown_refs)} unresolved references; see ast.json",
              file=sys.stderr)


def cmd_points(args):
    result, source = parse_input(args)
    points, classes, mem_classes, panel, pruned = domain.build_points(
        result, args.tier)
    doc = emit.points_document(result, source, args.tier, points, classes,
                               mem_classes, panel, pruned)
    emit.write_json(args.out, doc)
    print(json.dumps({"out": args.out, "tier": args.tier,
                      "points": len(points), "pruned": doc["pruned"],
                      "origins": doc["origin_counts"]}, sort_keys=True))


def cmd_verify(args):
    with open(args.ast, "r", encoding="utf-8") as handle:
        ast = json.load(handle)
    if ast.get("config") != "C2":
        raise SystemExit(
            "verify compares the unconditional TableGen rule emission; "
            f"re-run parse with --config C2 (got {ast.get('config')})")
    with open(args.td_inc, "r", encoding="utf-8") as handle:
        td = verify.parse_td_inc(handle.read())
    report = verify.build_report(ast, td)
    known = set()
    registry = args.known_deltas
    if registry is None:
        registry = os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                "known_deltas.json")
        if not os.path.exists(registry):
            registry = None
    if registry:
        with open(registry, "r", encoding="utf-8") as handle:
            data = json.load(handle)
        known = {(x["rule_set"], x["class"]) for x in data.get("deltas", [])}
    report["known_deltas_used"] = sorted(f"{a}:{b}" for a, b in known)
    unexplained = verify.only_allowed_deltas(report, known)
    report["unexplained"] = unexplained
    emit.write_json(args.out, report)
    print(json.dumps({
        "out": args.out,
        "census": report["census"],
        "opcodes": report["opcodes_vs_td"],
        "classes_cpp": len(report["classes_vs_td"]["cpp"]),
        "classes_td": len(report["classes_vs_td"]["td"]),
        "unexplained_deltas": len(unexplained),
    }, sort_keys=True))
    if unexplained:
        raise SystemExit(3)


def cmd_corpus(args):
    result, source = parse_input(args)
    points, classes, mem_classes, panel, pruned = domain.build_points(
        result, args.tier)
    points_path = os.path.abspath(args.points_out or (args.out + ".points.json"))
    emit.write_json(points_path, emit.points_document(
        result, source, args.tier, points, classes, mem_classes, panel, pruned))
    if not args.build:
        print(f"points written to {points_path}; pass --build to fill expected",
              file=sys.stderr)
        return
    binary = os.path.join(args.build, "unittests", "Target", "X86", "X86Tests")
    if not os.path.exists(binary):
        raise SystemExit(f"X86Tests not found under {args.build}")
    env = dict(os.environ)
    env["X86_LEGALIZER_CPP_POINTS"] = points_path
    env["X86_LEGALIZER_CPP_CORPUS_OUT"] = os.path.abspath(args.out)
    proc = subprocess.run(
        [binary, "--gtest_also_run_disabled_tests",
         "--gtest_filter=*GenerateCorpusFromCppPoints*"],
        cwd=args.build, env=env)
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)
    print(json.dumps({"points": points_path, "corpus": os.path.abspath(args.out)},
                     sort_keys=True))


def build_parser():
    p = argparse.ArgumentParser(
        prog="x86-legalizer-corpus",
        description="Read the hand-written X86 legalizer C++ into test corpora")
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--input", help="read this C++ file instead of a revision")
        sp.add_argument("--source-rev", help="e.g. ebac17721d12 or upstream/main")
        sp.add_argument("--source-path", default=target.DEFAULT_SOURCE_PATH)
        sp.add_argument("--repo", default=".")
        sp.add_argument("--config", default=target.DEFAULT_CONFIG,
                        choices=sorted(target.CONFIGS))
        sp.add_argument("--allow-opaque", action="store_true",
                        help="do not fail on an unknown chain method")

    sp = sub.add_parser("parse")
    common(sp)
    sp.add_argument("--out", required=True)
    sp.set_defaults(func=cmd_parse)

    sp = sub.add_parser("points")
    common(sp)
    sp.add_argument("--tier", default="witness",
                    choices=("witness", "full", "deep"))
    sp.add_argument("--out", required=True)
    sp.set_defaults(func=cmd_points)

    sp = sub.add_parser("verify")
    sp.add_argument("--ast", required=True)
    sp.add_argument("--td-inc", required=True)
    sp.add_argument("--known-deltas")
    sp.add_argument("--out", required=True)
    sp.set_defaults(func=cmd_verify)

    sp = sub.add_parser("corpus")
    common(sp)
    sp.add_argument("--tier", default="witness",
                    choices=("witness", "full", "deep"))
    sp.add_argument("--build", help="LLVM build dir containing X86Tests")
    sp.add_argument("--points-out")
    sp.add_argument("--out", required=True)
    sp.set_defaults(func=cmd_corpus)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.func(args)
    return 0
