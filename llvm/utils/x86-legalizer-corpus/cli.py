"""Command line entry point for the X86 legalizer corpus reader."""

import argparse
import hashlib
import json
import os
import subprocess
import sys

import corpusdiff
import domain
import emit
import expr as E
import mirrun
import oracle
import parse
import target
import tracediff
import verify

CORPUS_SCHEMA = "llvm-x86-legalizer-cpp-corpus"
CORPUS_VERSION = 2


def _source_info(text, spec, revision, path):
    return {
        "rev": spec,
        "revision": revision,
        "path": path,
        "sha256": hashlib.sha256(text.encode("utf-8")).hexdigest(),
        "lines": text.count("\n") + (0 if text.endswith("\n") else 1),
    }


def resolve_revision(repo, revision):
    """The full commit SHA for a symbolic revision, or *revision* if absent."""
    if not revision:
        return None
    proc = subprocess.run(
        ["git", "-C", repo, "rev-parse", "--verify", f"{revision}^{{commit}}"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        return revision
    return proc.stdout.strip()


def load_source(args):
    if args.input:
        with open(args.input, "r", encoding="utf-8") as handle:
            text = handle.read()
        return text, _source_info(text, None, None,
                                  os.path.abspath(args.input))
    revision = args.source_rev or target.DEFAULT_SOURCE_REV
    spec = f"{revision}:{args.source_path}"
    proc = subprocess.run(
        ["git", "-C", args.repo, "show", spec],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        raise SystemExit(proc.stderr.strip() or f"git show {spec} failed")
    text = proc.stdout
    return text, _source_info(text, revision,
                              resolve_revision(args.repo, revision),
                              args.source_path)


def read_revision(repo, revision, path):
    proc = subprocess.run(
        ["git", "-C", repo, "show", f"{revision}:{path}"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        return None
    return proc.stdout


def parse_input(args):
    text, source = load_source(args)
    features = target.CONFIGS[args.config]
    result = parse.parse_source(text, source, args.config, features)
    if result.unknown_methods and not args.allow_opaque:
        for method, line in result.unknown_methods:
            print(f"unknown chain method {method} at line {line}", file=sys.stderr)
        raise SystemExit(2)
    return result, source


def file_sha256(path):
    digest = hashlib.sha256()
    with open(path, "rb") as handle:
        for chunk in iter(lambda: handle.read(1 << 20), b""):
            digest.update(chunk)
    return digest.hexdigest()


def coverage_gate(coverage, allow_gaps):
    gaps = coverage["rules"]["unprobed"]
    uncovered = coverage["opcodes"]["uncovered"]
    if allow_gaps or (not gaps and not uncovered):
        return
    for gap in gaps:
        print(f"coverage gap: {gap['rule_set']} rule {gap['rule_index']} "
              f"{gap['method']} ({gap['kind']}): {gap['reason']}",
              file=sys.stderr)
    for gap in uncovered:
        print(f"coverage gap: {gap['rule_set']} opcode {gap['opcode']} has no "
              f"rule/boundary/AS point", file=sys.stderr)
    raise SystemExit(3)


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
    exceptions = domain.load_coverage_exceptions(args.coverage_exceptions)
    points, classes, mem_classes, panel, pruned, coverage = domain.build_points(
        result, args.tier, exceptions)
    coverage_gate(coverage, args.allow_coverage_gaps)
    doc = emit.points_document(
        result, source, args.tier, points, classes, mem_classes, panel, pruned,
        coverage=coverage, oracle=oracle.oracle_doc("none"),
        config_descriptor=target.config_descriptor(args.config))
    emit.write_json(args.out, doc)
    print(json.dumps({"out": args.out, "tier": args.tier,
                      "points": len(points), "pruned": doc["pruned"],
                      "origins": doc["origin_counts"],
                      "rules_verified": coverage["rules"]["verified"],
                      "rules_unprobed": len(coverage["rules"]["unprobed"])},
                     sort_keys=True))


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


def _cross_revision_delta(args, result, oracle_revision, source):
    text = read_revision(args.repo, oracle_revision, args.source_path)
    if text is None:
        raise SystemExit(
            f"cannot read the oracle source {oracle_revision} from {args.repo}")
    info = _source_info(text, oracle_revision,
                        resolve_revision(args.repo, oracle_revision),
                        args.source_path)
    oracle_result = parse.parse_source(
        text, info, args.config, target.CONFIGS[args.config])
    delta = oracle.rule_delta(result, oracle_result)
    return delta


def _canonical_llt(name, pointer_bits):
    """Match the C++ witnessLLTName spelling: pointers carry their width."""
    if name.startswith("p"):
        return f"{name}:{pointer_bits}"
    if name.startswith("v") and name.find("p") > 0:
        return f"{name}:{pointer_bits}"
    return name


def _corpus_atom(point, pointer_bits=64):
    """The corpus atom shape, shared by shapes-only and filled corpora."""
    query = {"types": [_canonical_llt(t, pointer_bits)
                       for t in point["types"]]}
    if point.get("mem") is not None:
        query["mem"] = {"type": _canonical_llt(point["mem"], pointer_bits),
                        "align": point.get("align", 0)}
    return {
        "id": point["id"],
        "origin": point["origin"],
        "rule_set": point["rule_set"],
        "opcode": point["opcode"],
        "rule_index": point["rule_index"],
        "config": None,
        "query": query,
    }


def _shapes_corpus(args, source, doc, coverage, oracle_doc, points):
    config = target.config_descriptor(args.config)
    pointer_bits = 64 if target.CONFIGS[args.config]["is64Bit"] else 32
    atoms = []
    for point in points:
        atom = _corpus_atom(point, pointer_bits)
        atom["config"] = config["id"]
        atoms.append(atom)
    return {
        "schema": CORPUS_SCHEMA,
        "version": CORPUS_VERSION,
        "source": source,
        "oracle": oracle_doc,
        "config_descriptor": config,
        "coverage": coverage,
        "counts": doc["counts"],
        "points_sha256": None,
        "atom_count": len(atoms),
        "atoms": atoms,
    }


def cmd_corpus(args):
    result, source = parse_input(args)
    exceptions = domain.load_coverage_exceptions(args.coverage_exceptions)
    points, classes, mem_classes, panel, pruned, coverage = domain.build_points(
        result, args.tier, exceptions)
    coverage_gate(coverage, args.allow_coverage_gaps)

    kind = args.oracle
    if kind is None:
        kind = "reference" if args.build else None
    source_revision = source.get("revision")
    cross = False
    delta = None
    oracle_doc = None

    if kind is None:
        oracle_doc = None
    elif kind == "none":
        oracle_doc = oracle.oracle_doc("none")
    elif kind == "reference":
        reference_spec = oracle.reference_revision(args.repo)
        reference = resolve_revision(args.repo, reference_spec)
        if reference is None:
            raise SystemExit(
                "cannot find the frozen reference provenance under "
                f"{args.repo}")
        cross = source_revision != reference
        if cross:
            delta = _cross_revision_delta(args, result, reference, source)
            if not args.allow_cross_revision:
                missing = ", ".join(
                    f"{x['rule_set']}#{x['rule_index']} {x['method']}"
                    for x in delta["oracle_only"])
                raise SystemExit(
                    f"cross-revision corpus: source {source_revision or '<input>'} "
                    f"!= oracle reference {reference}"
                    + (f"; oracle-only rules: {missing}" if missing else "")
                    + "; pass --allow-cross-revision to mark the corpus")
        oracle_doc = oracle.oracle_doc("reference", reference, cross,
                                       spec=reference_spec)
    else:
        raise SystemExit(f"unknown oracle {kind}")

    points_path = os.path.abspath(args.points_out or (args.out + ".points.json"))
    doc = emit.points_document(
        result, source, args.tier, points, classes, mem_classes, panel, pruned,
        coverage=coverage, oracle=oracle_doc,
        config_descriptor=target.config_descriptor(args.config))
    emit.write_json(points_path, doc)

    if kind is None:
        print(f"points written to {points_path}; pass --build to fill expected, "
              "or --oracle=none for a shapes-only corpus", file=sys.stderr)
        return

    if kind == "none":
        corpus = _shapes_corpus(args, source, doc, coverage, oracle_doc, points)
        corpus["points_sha256"] = file_sha256(points_path)
        emit.write_json(args.out, corpus)
        print(json.dumps({
            "points": points_path, "corpus": os.path.abspath(args.out),
            "oracle": "none", "atoms": len(points),
            "rules_verified": coverage["rules"]["verified"],
        }, sort_keys=True))
        return

    if not args.build:
        raise SystemExit("--oracle=reference requires --build")
    oracle.run_reference_filler(args.build, points_path, args.out)

    with open(args.out, "r", encoding="utf-8") as handle:
        filled = json.load(handle)
    if filled.get("schema") != CORPUS_SCHEMA:
        raise SystemExit(f"{args.out} is not a {CORPUS_SCHEMA} document")
    if filled.get("atom_count") != len(points):
        raise SystemExit(
            f"filler wrote {filled.get('atom_count')} atoms, expected "
            f"{len(points)}")

    report = {
        "schema": "llvm-x86-legalizer-corpus-report",
        "version": CORPUS_VERSION,
        "source": source,
        "oracle": oracle_doc,
        "coverage": coverage,
        "counts": doc["counts"],
        "corpus": os.path.abspath(args.out),
        "points_sha256": file_sha256(points_path),
    }
    if delta is not None:
        report["source_oracle_delta"] = delta
    emit.write_json(args.out + ".report.json", report)
    print(json.dumps({
        "points": points_path, "corpus": os.path.abspath(args.out),
        "report": os.path.abspath(args.out + ".report.json"),
        "oracle": "reference", "cross_revision": cross,
        "atoms": filled["atom_count"],
        "rules_verified": coverage["rules"]["verified"],
    }, sort_keys=True))


def cmd_diff(args):
    left = corpusdiff.load(args.left)
    right = corpusdiff.load(args.right)
    report = corpusdiff.compare(left, right, "left", "right")
    report["left"]["path"] = os.path.abspath(args.left)
    report["right"]["path"] = os.path.abspath(args.right)
    emit.write_json(args.out, report)
    summary = {
        "out": os.path.abspath(args.out),
        "config_match": report["config_match"],
        "keys": report["keys"],
        "expected_available": report["expected_available"],
        "expected_diff": len(report["expected_diff"]),
    }
    print(json.dumps(summary, sort_keys=True))
    if args.strict and corpusdiff.has_differences(report):
        raise SystemExit(1)


def cmd_trace_diff(args):
    left = tracediff.load(args.left)
    right = tracediff.load(args.right)
    report = tracediff.compare(left, right, "left", "right", args.max_diffs)
    report["left"]["path"] = os.path.abspath(args.left)
    report["right"]["path"] = os.path.abspath(args.right)
    emit.write_json(args.out, report)
    summary = {
        "out": os.path.abspath(args.out),
        "corpus_match": report["corpus_match"],
        "config_match": report["config_match"],
        "ids": report["ids"],
        "status": report["status_histogram"],
        "longest_rounds": report["longest_rounds"],
        "diff_count": report["diff_count"],
    }
    print(json.dumps(summary, sort_keys=True))
    if args.strict and tracediff.has_differences(report):
        raise SystemExit(1)


def cmd_migrate(args):
    doc = corpusdiff.load(args.input)
    candidates = ([args.source_rev] if args.source_rev
                  else [doc.get("source_commit"),
                        oracle.reference_revision(args.repo),
                        target.DEFAULT_SOURCE_REV])
    source_rev = None
    source = None
    result = None
    last_error = None
    for revision in candidates:
        if not revision:
            continue
        text = read_revision(args.repo, revision, args.source_path)
        if text is None:
            last_error = f"cannot read {revision}:{args.source_path}"
            continue
        info = _source_info(text, revision,
                            resolve_revision(args.repo, revision),
                            args.source_path)
        try:
            result = parse.parse_source(text, info, args.config,
                                        target.CONFIGS[args.config])
        except E.ExprError as err:
            last_error = f"{revision}: {err}"
            continue
        source_rev = revision
        source = info
        break
    if result is None:
        raise SystemExit(last_error or "cannot find a source for the opcode map")
    rule_opcodes = {}
    for builder in result.builders:
        for opcode in builder.opcodes:
            rule_opcodes.setdefault("X86_" + opcode[2:], opcode)
        rule_opcodes.setdefault(builder.name, builder.opcodes[0])
    try:
        migrated = corpusdiff.migrate_v1(doc, rule_opcodes, source)
    except ValueError as err:
        raise SystemExit(str(err))
    if migrated is None:
        raise SystemExit(f"{args.input} is not a version-1 document")
    emit.write_json(args.out, migrated)
    print(json.dumps({
        "out": os.path.abspath(args.out),
        "source_rev": source_rev,
        "atoms": migrated["atom_count"],
        "config": migrated.get("config"),
    }, sort_keys=True))


def cmd_mir_run(args):
    try:
        report, code = mirrun.run(
            args.corpus, args.out, build=args.build, driver=args.driver,
            rules=args.rules, shard_size=args.shard_size,
            timeout=args.timeout, max_steps=args.max_steps,
            only_atom=args.only_atom, a2_probe=args.a2_probe,
            jobs=args.jobs)
    except mirrun.MirRunInfraError as err:
        print(f"mir-run infrastructure error: {err}", file=sys.stderr)
        raise SystemExit(2)
    summary = {
        "out": os.path.abspath(args.out),
        "atoms": report["corpus"]["atoms"],
        "counts": report["counts"],
        "first_anomaly": (None if report["first_anomaly"] is None
                          else report["first_anomaly"]["id"]),
        "a3": report["a3"]["status"],
        "libcalls": report["libcalls"],
    }
    print(json.dumps(summary, sort_keys=True))
    if code:
        raise SystemExit(code)


def build_parser():
    p = argparse.ArgumentParser(
        prog="x86-legalizer-corpus",
        description="Read the hand-written X86 legalizer C++ into test corpora")
    sub = p.add_subparsers(dest="cmd", required=True)

    def common(sp):
        sp.add_argument("--input", help="read this C++ file instead of a revision")
        sp.add_argument("--source-rev", default=target.DEFAULT_SOURCE_REV,
                        help="git revision to read (default: upstream/main)")
        sp.add_argument("--source-path", default=target.DEFAULT_SOURCE_PATH)
        sp.add_argument("--repo", default=".")
        sp.add_argument("--config", default=target.DEFAULT_CONFIG,
                        choices=sorted(target.CONFIGS))
        sp.add_argument("--allow-opaque", action="store_true",
                        help="do not fail on an unknown chain method")

    def coverage(sp):
        sp.add_argument("--coverage-exceptions",
                        help="JSON file of reviewed coverage exceptions")
        sp.add_argument("--allow-coverage-gaps", action="store_true",
                        help="emit points despite unregistered coverage gaps "
                             "(inspection only)")

    sp = sub.add_parser("parse")
    common(sp)
    sp.add_argument("--out", required=True)
    sp.set_defaults(func=cmd_parse)

    sp = sub.add_parser("points")
    common(sp)
    coverage(sp)
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
    coverage(sp)
    sp.add_argument("--tier", default="witness",
                    choices=("witness", "full", "deep"))
    sp.add_argument("--oracle", choices=("none", "reference"),
                    help="expected-action source (default: reference when "
                         "--build is given, otherwise points-only)")
    sp.add_argument("--allow-cross-revision", action="store_true",
                    help="allow source.revision != oracle.revision; the "
                         "corpus is marked cross_revision")
    sp.add_argument("--build", help="LLVM build dir containing X86Tests")
    sp.add_argument("--points-out")
    sp.add_argument("--out", required=True)
    sp.set_defaults(func=cmd_corpus)

    sp = sub.add_parser("migrate")
    sp.add_argument("--input", required=True)
    sp.add_argument("--out", required=True)
    sp.add_argument("--source-rev",
                    help="source for the rule-set -> primary-opcode map "
                         "(default: source_commit, then the frozen reference)")
    sp.add_argument("--source-path", default=target.DEFAULT_SOURCE_PATH)
    sp.add_argument("--repo", default=".")
    sp.add_argument("--config", default=target.DEFAULT_CONFIG,
                    choices=sorted(target.CONFIGS))
    sp.set_defaults(func=cmd_migrate)

    sp = sub.add_parser("trace-diff")
    sp.add_argument("--left", required=True,
                    help="trace to compare from (the reference)")
    sp.add_argument("--right", required=True,
                    help="trace to compare against (the table)")
    sp.add_argument("--out", required=True)
    sp.add_argument("--max-diffs", type=int, default=50,
                    help="cap the per-diff detail and id samples (default: 50)")
    sp.add_argument("--strict", action="store_true",
                    help="exit 1 when ids, statuses, rounds, or traces differ")
    sp.set_defaults(func=cmd_trace_diff)

    sp = sub.add_parser("mir-run")
    sp.add_argument("--corpus", required=True,
                    help="filled corpus JSON from the corpus subcommand")
    sp.add_argument("--build", help="LLVM build dir containing X86Tests")
    sp.add_argument("--driver", help="explicit X86Tests binary")
    sp.add_argument("--rules", default="production",
                    choices=("production", "reference"))
    sp.add_argument("--shard-size", type=int, default=2000)
    sp.add_argument("--timeout", type=float, default=120,
                    help="per-shard timeout in seconds")
    sp.add_argument("--max-steps", type=int, default=10000)
    sp.add_argument("--jobs", type=int, default=1,
                    help="run up to N shards concurrently (default: 1)")
    sp.add_argument("--only-atom", help="run exactly one atom id")
    sp.add_argument("--a2-probe", action="store_true",
                    help="mark this as the fixed-rules A2 probe (A3 exempt)")
    sp.add_argument("--out", required=True)
    sp.set_defaults(func=cmd_mir_run)

    sp = sub.add_parser("diff")
    sp.add_argument("--left", required=True)
    sp.add_argument("--right", required=True)
    sp.add_argument("--out", required=True)
    sp.add_argument("--strict", action="store_true",
                    help="exit 1 when keys, expected actions, or configs differ")
    sp.set_defaults(func=cmd_diff)
    return p


def main(argv=None):
    args = build_parser().parse_args(argv)
    args.func(args)
    return 0
