"""Canonical comparison of two X86 legalizer corpus documents.

The comparison key is the query itself, never an array position, an atom id,
or the raw bytes of the file:

    (config_fingerprint, opcode, types, mem type, align bits)

`expected` is compared only where both sides carry one.  A shapes-only corpus
therefore reports `expected_available: false` instead of pretending that the
answers matched.
"""

import hashlib
import json


def load(path):
    with open(path, "r", encoding="utf-8") as handle:
        return json.load(handle)


def config_fingerprint(doc):
    desc = doc.get("config_descriptor")
    if desc is None:
        desc = {"config": doc.get("config")}
    text = json.dumps(desc, sort_keys=True, separators=(",", ":"))
    return hashlib.sha256(text.encode("utf-8")).hexdigest()


def config_id(doc):
    desc = doc.get("config_descriptor") or {}
    return desc.get("id") or doc.get("config")


def _query(atom):
    query = atom.get("query")
    if query is None:
        query = {
            "types": atom.get("types", []),
            "mem": (None if atom.get("mem") is None
                    else {"type": atom["mem"], "align": atom.get("align", 0)}),
        }
    return query


def _norm_type(name):
    """Pointers are spelled with or without their width across artifacts."""
    if name and (name.startswith("p") or "p" in name.split("s", 1)[0]):
        return name.split(":", 1)[0]
    return name


def atom_key(atom, config):
    query = _query(atom)
    mem = query.get("mem") or {}
    return (
        config,
        atom.get("opcode"),
        tuple(_norm_type(t) for t in (query.get("types") or ())),
        _norm_type(mem.get("type")),
        mem.get("align", 0),
    )


def render_key(key):
    config, opcode, types, mem_type, align = key
    out = {"config": config, "opcode": opcode, "types": list(types)}
    if mem_type is not None:
        out["mem"] = {"type": mem_type, "align": align}
    return out


def _index(doc, config):
    index = {}
    for atom in doc.get("atoms") or doc.get("points") or []:
        index.setdefault(atom_key(atom, config), []).append(atom)
    return index


def _expected(atom):
    value = atom.get("expected")
    return None if value is None else json.dumps(value, sort_keys=True)


def _compare_expected(left, right):
    left_values = sorted(v for v in (_expected(a) for a in left) if v is not None)
    right_values = sorted(v for v in (_expected(a) for a in right) if v is not None)
    if not left_values or not right_values:
        return None
    return left_values == right_values, left_values, right_values


def _per_rule(index):
    counts = {}
    for _key, atoms in index.items():
        for atom in atoms:
            name = atom.get("rule_set")
            counts[name] = counts.get(name, 0) + 1
    return counts


def compare(left, right, left_name="left", right_name="right"):
    left_fp, right_fp = config_fingerprint(left), config_fingerprint(right)
    left_index = _index(left, left_fp)
    right_index = _index(right, right_fp)

    left_only = sorted(set(left_index) - set(right_index))
    right_only = sorted(set(right_index) - set(left_index))
    common = sorted(set(left_index) & set(right_index))

    expected_diff = []
    expected_compared = 0
    for key in common:
        got = _compare_expected(left_index[key], right_index[key])
        if got is None:
            continue
        expected_compared += 1
        if not got[0]:
            expected_diff.append({
                "query": render_key(key),
                left_name: got[1],
                right_name: got[2],
            })

    left_rules, right_rules = _per_rule(left_index), _per_rule(right_index)
    per_rule = {
        "left": dict(sorted(left_rules.items())),
        "right": dict(sorted(right_rules.items())),
        "left_only": sorted(set(left_rules) - set(right_rules)),
        "right_only": sorted(set(right_rules) - set(left_rules)),
        "count_diff": [
            {"rule_set": name, "left": left_rules.get(name, 0),
             "right": right_rules.get(name, 0)}
            for name in sorted(set(left_rules) | set(right_rules))
            if left_rules.get(name, 0) != right_rules.get(name, 0)
        ],
    }

    return {
        "schema": "llvm-x86-legalizer-corpus-diff",
        "version": 2,
        "left": {"path": None, "config": config_id(left),
                 "config_fingerprint": left_fp,
                 "atoms": sum(len(v) for v in left_index.values())},
        "right": {"path": None, "config": config_id(right),
                  "config_fingerprint": right_fp,
                  "atoms": sum(len(v) for v in right_index.values())},
        "config_match": left_fp == right_fp,
        "keys": {"left": len(left_index), "right": len(right_index),
                 "common": len(common),
                 "left_only": len(left_only), "right_only": len(right_only)},
        "expected_available": expected_compared > 0,
        "expected_compared": expected_compared,
        "expected_diff": expected_diff,
        "left_only": [render_key(k) for k in left_only],
        "right_only": [render_key(k) for k in right_only],
        "per_rule_coverage": per_rule,
    }


def has_differences(report):
    return bool(report["expected_diff"] or report["left_only"]
                or report["right_only"] or not report["config_match"])
