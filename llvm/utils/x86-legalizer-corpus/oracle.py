"""Oracle adapters for the X86 legalizer corpus tool.

Query shapes come from the parsed C++ source.  Expected actions come only
from an oracle, and the oracle revision is recorded next to the source
revision.  The Python side never invents an expected action.
"""

import json
import os
import subprocess

REFERENCE_PROVENANCE = os.path.join(
    "llvm", "unittests", "Target", "X86",
    "x86-legalizer-reference-provenance.json")


def reference_revision(repo="."):
    """The frozen C++ reference commit, or None when provenance is missing."""
    path = os.path.join(repo, REFERENCE_PROVENANCE)
    try:
        with open(path, "r", encoding="utf-8") as handle:
            data = json.load(handle)
    except (OSError, ValueError):
        return None
    return data.get("source_commit")


def oracle_doc(kind, revision=None, cross_revision=False, spec=None):
    doc = {
        "kind": kind,
        "revision": revision,
        "cross_revision": bool(cross_revision),
    }
    if spec is not None and spec != revision:
        doc["spec"] = spec
    return doc


def _rule_keys(result):
    out = {}
    for builder in result.builders:
        for rule in builder.rules:
            if rule.active is False:
                continue
            key = (builder.name, rule.method,
                   rule.arg_text[0] if rule.arg_text else "")
            out.setdefault(key, []).append(rule.index)
    return out


def rule_delta(source, oracle):
    """Rules that exist on one side only, keyed by text, not by index."""
    left, right = _rule_keys(source), _rule_keys(oracle)

    def render(key, indexes):
        return {
            "rule_set": key[0],
            "method": key[1],
            "rule_index": indexes[0],
            "arg": key[2],
        }

    source_only = [render(k, v) for k, v in left.items() if k not in right]
    oracle_only = [render(k, v) for k, v in right.items() if k not in left]
    source_only.sort(key=lambda x: (x["rule_set"], x["rule_index"]))
    oracle_only.sort(key=lambda x: (x["rule_set"], x["rule_index"]))
    return {
        "source_rules": sum(len(v) for v in left.values()),
        "oracle_rules": sum(len(v) for v in right.values()),
        "source_only": source_only,
        "oracle_only": oracle_only,
    }


def filler_binary(build):
    return os.path.join(build, "unittests", "Target", "X86", "X86Tests")


def run_reference_filler(build, points_path, out_path):
    """Fill expected actions with the frozen C++ reference.

    The X86Tests driver reads the points file, asks
    X86LegalizerInfoReference::getAction(), and writes the corpus.  This
    function only launches it and reports its exit code.
    """
    binary = filler_binary(build)
    if not os.path.exists(binary):
        raise SystemExit(f"X86Tests not found under {build}")
    env = dict(os.environ)
    env["X86_LEGALIZER_CPP_POINTS"] = os.path.abspath(points_path)
    env["X86_LEGALIZER_CPP_CORPUS_OUT"] = os.path.abspath(out_path)
    proc = subprocess.run(
        [binary, "--gtest_also_run_disabled_tests",
         "--gtest_filter=*GenerateCorpusFromCppPoints*"],
        cwd=build, env=env)
    if proc.returncode != 0:
        raise SystemExit(proc.returncode)
