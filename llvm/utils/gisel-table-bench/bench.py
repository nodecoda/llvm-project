#!/usr/bin/env python3
"""Measure the X86 GlobalISel legalizer table against the rule chains.

Two levels, because they answer different questions:

  lookup   ``-x86-gisel-table-bench=N`` times the same N passes over the
           generated check domain three ways -- chain, table, and the
           table-then-fallback path ``getAction()`` takes.  This is the
           mechanism: how long one legality question costs.

  pass     ``llc -time-passes`` reports the Legalizer pass's own wall time.
           This is where the lookup saving lands after it is diluted by the
           rest of legalization (MIR rewriting, CSE, artifact combining).

Both levels run every mode once per repetition, in a shuffled order, and are
analysed as *paired* differences, so slow drift on a shared machine cancels.
There is no ``perf`` here: ``-time-passes`` is an in-process measurement of the
pass, and the lookup level is an in-process measurement of the lookup, which is
more direct than sampling either from outside.

The negative control is ``-x86-gisel-table-once``: the table answers one query
and then stands aside, so the mixed path costs chain + one lookup.  A harness
that cannot tell that apart from ``-x86-gisel-table=false`` is not resolving the
table, and then "no difference" would mean nothing.  The control is run at both
levels and the report states whether it separated.
"""

import argparse
import hashlib
import json
import os
import random
import re
import statistics
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
GEN_IR = os.path.join(HERE, "gen_ir.py")

# Mode name -> extra llc flags.  "table" is the default, so it adds nothing.
MODES = {
    "table": [],
    "chain": ["-x86-gisel-table=false"],
    "once": ["-x86-gisel-table-once"],
}

TIME_ROW = re.compile(r"([-\d.]+)\s+\(\s*[\d.]+%\)")
TOTAL_RE = re.compile(
    r"Total Execution Time:\s*([\d.]+)\s*seconds\s*\(\s*([\d.]+)\s*wall clock\)")
BENCH_RE = re.compile(r"^x86-gisel-table-bench: (.*)$", re.M)


def die(msg):
    sys.stderr.write("gisel-table-bench: " + msg + "\n")
    sys.exit(2)


def parse_time_passes(text):
    """Return (legalizer wall ns, first timer group's total wall ns)."""
    legalizer = None
    for line in text.splitlines():
        if not line.rstrip().endswith("Legalizer"):
            continue
        vals = TIME_ROW.findall(line)
        if len(vals) >= 4:
            legalizer = float(vals[3]) * 1e9  # wall column, seconds -> ns
            break
    total = None
    m = TOTAL_RE.search(text)
    if m:
        total = float(m.group(2)) * 1e9
    return legalizer, total


def parse_bench(text):
    m = BENCH_RE.search(text)
    if not m:
        return None
    out = {}
    for key in ("points", "hits", "reps", "max_rule", "sink"):
        mm = re.search(r"\b" + key + r" (\d+)", m.group(1))
        if mm:
            out[key] = int(mm.group(1))
    for key in ("chain_ns", "mixed_ns", "mean_rule", "speedup"):
        mm = re.search(r"\b" + key + r" ([\d.]+)", m.group(1))
        if mm:
            out[key] = float(mm.group(1))
    return out


def parse_stats(text):
    """Return the -x86-gisel-table-stats counters, including the real rule mix."""
    m = re.search(r"^x86-gisel-table: (.*)$", text, re.M)
    if not m:
        return None
    out = {}
    for key, pat in (("hit", r"hit (\d+)"), ("outside", r"outside (\d+)"),
                     ("ptrvec", r"ptrvec (\d+)"),
                     ("addrspace", r"addrspace (\d+)"),
                     ("untabulated", r"untabulated (\d+)"),
                     ("splits", r"splits (\d+)"),
                     ("declined", r"declined (\d+)"),
                     ("fills", r"fills (\d+)"),
                     ("rule_max", r"rule_max (\d+)")):
        mm = re.search(pat, m.group(1))
        if mm:
            out[key] = int(mm.group(1))
    mm = re.search(r"rule_mean ([\d.]+)", m.group(1))
    if mm:
        out["rule_mean"] = float(mm.group(1))
    return out


def run(cmd, cpu):
    if cpu is not None:
        cmd = ["taskset", "-c", str(cpu)] + cmd
    t0 = time.perf_counter()
    p = subprocess.run(cmd, capture_output=True, text=True)
    return p, time.perf_counter() - t0


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(1 << 20), b""):
            h.update(chunk)
    return h.hexdigest()


def summarize(xs):
    return {
        "n": len(xs),
        "median": statistics.median(xs),
        "mean": statistics.fmean(xs),
        "sd": statistics.stdev(xs) if len(xs) > 1 else 0.0,
        "min": min(xs),
        "max": max(xs),
    }


def bootstrap_ci(values, iters=10000, alpha=0.05, seed=0x5EED):
    """Percentile bootstrap CI for the mean of *values*."""
    if len(values) < 2:
        return (float("nan"), float("nan"))
    rng = random.Random(seed)
    n = len(values)
    means = sorted(statistics.fmean(rng.choices(values, k=n))
                   for _ in range(iters))
    lo = means[int(alpha / 2 * iters)]
    hi = means[int((1 - alpha / 2) * iters) - 1]
    return (lo, hi)


def fmt_ns(x):
    return f"{x:9.2f}"


def report_pairs(a_label, a_values, b_label, b_values, unit="ns"):
    """Paired difference a - b, with a percentile bootstrap CI for the mean."""
    diffs = [a - b for a, b in zip(a_values, b_values)]
    med = statistics.median(diffs)
    mean = statistics.fmean(diffs)
    lo, hi = bootstrap_ci(diffs)
    wins = sum(1 for d in diffs if d < 0)
    losses = sum(1 for d in diffs if d > 0)
    ties = len(diffs) - wins - losses
    print(f"  paired {a_label:6s} - {b_label:6s}: median {med:+10.1f} {unit}  "
          f"mean {mean:+10.1f} {unit}  95% CI [{lo:+.1f}, {hi:+.1f}]")
    print(f"      pairs: {wins} favor {a_label}, {losses} favor {b_label}, "
          f"{ties} tied")
    return {"median": med, "mean": mean, "ci": [lo, hi],
            "wins": wins, "losses": losses, "ties": ties}


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--llc", default="/home/ubuntu/llvm/builds/table-classify/bin/llc")
    ap.add_argument("--copies", type=int, default=32,
                    help="copies of each corpus template (default 32)")
    ap.add_argument("--filter", default=None,
                    help="pass a template-name regex to gen_ir.py")
    ap.add_argument("--exclude", default=None,
                    help="pass a template-name exclude regex to gen_ir.py")
    ap.add_argument("--reps", type=int, default=21,
                    help="measured repetitions per mode (default 21)")
    ap.add_argument("--warmup", type=int, default=3,
                    help="discarded repetitions per mode (default 3)")
    ap.add_argument("--cpu", type=int, default=0,
                    help="CPU to pin to with taskset (default 0; -1 = no pin)")
    ap.add_argument("--modes", default="table,chain,once")
    ap.add_argument("--lookup-reps", type=int, default=300,
                    help="passes over the domain inside -x86-gisel-table-bench")
    ap.add_argument("--lookup-runs", type=int, default=7,
                    help="process repetitions of the lookup benchmark")
    ap.add_argument("--workdir", default=None,
                    help="where to put the corpus (default: a fresh temp dir)")
    ap.add_argument("--json", default=None, help="write raw numbers here")
    ap.add_argument("--skip-pass", action="store_true")
    ap.add_argument("--skip-lookup", action="store_true")
    args = ap.parse_args()

    modes = [m.strip() for m in args.modes.split(",") if m.strip()]
    for m in modes:
        if m not in MODES:
            die(f"unknown mode {m!r}; known: {', '.join(MODES)}")
    cpu = None if args.cpu < 0 else args.cpu

    if not os.path.exists(args.llc):
        die(f"no llc at {args.llc}")

    workdir = args.workdir
    if workdir is None:
        workdir = os.path.join("/tmp", f"gisel-table-bench-{os.getpid()}")
    os.makedirs(workdir, exist_ok=True)
    corpus = os.path.join(workdir, "corpus.ll")
    mir = os.path.join(workdir, "corpus.mir")

    ver = subprocess.run([args.llc, "--version"], capture_output=True, text=True)
    version = (ver.stdout.splitlines() or ["llc: unknown"])[0]
    load = open("/proc/loadavg").read().split()[0]

    print("=== environment ===")
    print(f"  llc      {args.llc}")
    print(f"  {version}")
    print(f"  cpus     {os.cpu_count()}  load1 {load}  pinned {cpu}")

    # ---- corpus ----
    gen_cmd = [sys.executable, GEN_IR, "--copies", str(args.copies)]
    if args.filter:
        gen_cmd += ["--filter", args.filter]
    if args.exclude:
        gen_cmd += ["--exclude", args.exclude]
    with open(corpus, "w") as f:
        p = subprocess.run(gen_cmd, stdout=f)
    if p.returncode:
        die("gen_ir.py failed")
    nfunc = sum(1 for line in open(corpus) if line.startswith("define "))
    digest = sha256(corpus)
    print("\n=== corpus ===")
    print(f"  {corpus}")
    print(f"  copies {args.copies}  functions {nfunc}  sha256 {digest}")

    base = [args.llc, "-mtriple=x86_64", "-global-isel", "-O0"]

    # Every function has to legalize to completion, or the legalizer stops at
    # the first unsupported instruction and the measured work is a prefix.
    for m in modes:
        p, _ = run(base + MODES[m] + [corpus, "-o", "/dev/null"], cpu)
        if p.returncode:
            die(f"corpus does not compile in mode {m}:\n{p.stderr[-2000:]}")

    p, _ = run(base + ["-stop-after=ir-translator", corpus, "-o", mir], cpu)
    if p.returncode:
        die("could not translate the corpus to MIR:\n" + p.stderr[-2000:])
    print(f"  MIR      {mir}  ({sum(1 for _ in open(mir))} lines)")

    # The query mix a compilation actually produces.  The check domain is
    # uniform over type classes and real code is not, so this is the number
    # that connects the lookup-level and pass-level results.
    p, _ = run(base + ["-x86-gisel-table-stats", corpus, "-o", "/dev/null"], cpu)
    stats = parse_stats(p.stderr)
    if p.returncode or stats is None:
        die("could not read -x86-gisel-table-stats:\n" + p.stderr[-2000:])
    print(f"  queries  hit {stats.get('hit', 0)} outside {stats.get('outside', 0)} "
          f"fills {stats.get('fills', 0)}  winning rule: mean "
          f"{stats.get('rule_mean', 0):.2f} max {stats.get('rule_max', 0)}")

    out = {"environment": {"llc": args.llc, "version": version, "cpu": cpu,
                           "load1": load},
           "corpus": {"path": corpus, "copies": args.copies,
                      "functions": nfunc, "sha256": digest, "stats": stats}}

    # ---- lookup level ----
    if not args.skip_lookup:
        print("\n=== lookup level "
              f"(-x86-gisel-table-bench={args.lookup_reps}, "
              f"{args.lookup_runs} processes) ===")
        got = {}
        for m in ("table", "once"):
            rows = []
            for _ in range(args.lookup_runs):
                cmd = base + [f"-x86-gisel-table-bench={args.lookup_reps}"] + \
                    MODES[m] + [corpus, "-o", "/dev/null"]
                p, _ = run(cmd, cpu)
                if p.returncode:
                    die(f"lookup benchmark failed in mode {m}:\n{p.stderr[-2000:]}")
                b = parse_bench(p.stderr)
                if b is None:
                    die(f"no x86-gisel-table-bench line in mode {m}")
                rows.append(b)
            got[m] = rows
            sinks = {r["sink"] for r in rows}
            if len(sinks) != 1:
                die(f"checksum changed between runs in mode {m}: {sinks}")
            if m == "table" and not all(r["hits"] == r["points"] for r in rows):
                die("the table did not answer every domain point")
            if m == "once" and not all(r["hits"] == 1 for r in rows):
                die("-x86-gisel-table-once did not answer exactly one query")

        print(f"  points {got['table'][0]['points']}  "
              f"mean winning rule index {got['table'][0]['mean_rule']:.2f}  "
              f"max {got['table'][0]['max_rule']}")
        print()
        print(f"  {'mode':6s} {'chain_ns':>10s} {'mixed_ns':>10s} "
              f"{'speedup':>8s} {'hits':>7s}")
        for m in ("table", "once"):
            r = got[m]
            print(f"  {m:6s} {statistics.median(x['chain_ns'] for x in r):10.2f} "
                  f"{statistics.median(x['mixed_ns'] for x in r):10.2f} "
                  f"{statistics.median(x['speedup'] for x in r):8.3f} "
                  f"{r[0]['hits']:7d}")

        t = got["table"][0]
        o = got["once"][0]
        print()
        print(f"  table mode: chain {t['chain_ns']:.2f} ns -> table-then-fallback "
              f"{t['mixed_ns']:.2f} ns per query "
              f"({(t['chain_ns'] - t['mixed_ns']) / t['chain_ns'] * 100:+.1f}%, "
              f"{t['speedup']:.2f}x)")
        change = (o["mixed_ns"] - t["mixed_ns"]) / t["mixed_ns"] * 100
        print(f"  negative control: with the table answering once, mixed is "
              f"{o['mixed_ns']:.2f} ns ({change:+.1f}% against table mode) and "
              f"hits is {o['hits']}")
        separated = abs(change) > 5.0
        print(f"  the control {'SEPARATED' if separated else 'DID NOT SEPARATE'}"
              " the table's contribution")
        out["lookup"] = {
            m: {"chain_ns": statistics.median(x["chain_ns"] for x in rows),
                "mixed_ns": statistics.median(x["mixed_ns"] for x in rows),
                "speedup": statistics.median(x["speedup"] for x in rows),
                "hits": rows[0]["hits"], "points": rows[0]["points"],
                "mean_rule": rows[0]["mean_rule"], "max_rule": rows[0]["max_rule"],
                "raw": rows}
            for m, rows in got.items()
        }
        out["lookup"]["separated"] = separated

    # ---- pass level ----
    if not args.skip_pass:
        for label, extra in (("full pipeline", []),
                             ("isolated legalizer", ["-run-pass=legalizer"])):
            path = corpus if not extra else mir
            print(f"\n=== {label} (-time-passes, {args.reps} repetitions) ===")
            legalizer = {m: [] for m in modes}
            total = {m: [] for m in modes}
            wall = {m: [] for m in modes}
            for i in range(args.warmup + args.reps):
                order = list(modes)
                random.Random(i).shuffle(order)
                for m in order:
                    cmd = base + extra + MODES[m] + ["-time-passes", path,
                                                     "-o", "/dev/null"]
                    p, w = run(cmd, cpu)
                    if p.returncode:
                        die(f"{label} failed in mode {m}:\n{p.stderr[-2000:]}")
                    leg, tot = parse_time_passes(p.stderr)
                    if leg is None:
                        die(f"no Legalizer row in -time-passes output ({m})")
                    if i < args.warmup:
                        continue
                    legalizer[m].append(leg)
                    total[m].append(tot)
                    wall[m].append(w * 1e9)

            print(f"  {'mode':6s} {'n':>3s} {'legalizer median':>18s} "
                  f"{'mean':>12s} {'sd':>10s} {'min':>11s} {'max':>11s}")
            for m in modes:
                s = summarize(legalizer[m])
                print(f"  {m:6s} {s['n']:3d} {s['median']/1e6:16.3f}ms "
                      f"{s['mean']/1e6:10.3f}ms {s['sd']/1e6:8.3f}ms "
                      f"{s['min']/1e6:9.3f}ms {s['max']/1e6:9.3f}ms")
            print()
            if "table" in modes and "chain" in modes:
                d1 = report_pairs("table", legalizer["table"], "chain", legalizer["chain"])
            else:
                d1 = None
            if "table" in modes and "once" in modes:
                d2 = report_pairs("table", legalizer["table"], "once", legalizer["once"])
            else:
                d2 = None
            if "chain" in modes and "once" in modes:
                d3 = report_pairs("once", legalizer["once"], "chain", legalizer["chain"])
            else:
                d3 = None

            if "table" in modes and "chain" in modes:
                tm = statistics.median(legalizer["table"])
                cm = statistics.median(legalizer["chain"])
                sep = d1["ci"][1] < 0 or d1["ci"][0] > 0
                print(f"      legalizer: table is {(cm - tm) / cm * 100:+.2f}% "
                      f"against the chain; the paired CI "
                      f"{'excludes' if sep else 'includes'} zero")
                print(f"      total compile: "
                      f"{statistics.median(total['table'])/1e6:.3f}ms vs "
                      f"{statistics.median(total['chain'])/1e6:.3f}ms")

            key = label.split()[0]
            out[f"pass_{key}"] = {
                "legalizer_ns": {m: legalizer[m] for m in modes},
                "total_ns": {m: total[m] for m in modes},
                "process_wall_ns": {m: wall[m] for m in modes},
                "summary": {m: summarize(legalizer[m]) for m in modes},
                "table_minus_chain": d1,
                "table_minus_once": d2,
                "once_minus_chain": d3,
            }

    if args.json:
        with open(args.json, "w") as f:
            json.dump(out, f, indent=2)
        print(f"\nraw numbers: {args.json}")


if __name__ == "__main__":
    main()
