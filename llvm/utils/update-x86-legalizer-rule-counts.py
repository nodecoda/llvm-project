#!/usr/bin/env python3
"""Record the X86 GlobalISel legalizer rule-count denominators.

The regression suite needs a stable denominator: how many TableGen rules the
X86 legalizer describes.  The Core IR (``X86LegalizerInfo.core.json``) is the
build product that enumerates those rules, so the counts below are derived
from it rather than from the ``.td`` text.

Field definitions (all counts are derived from the Core IR):

  literal_td_rule_count
      Number of distinct ``(def, rule_index)`` pairs over every
      ``rulesets[].rules[]`` entry.  A literal rule is one that survives in
      Core IR form; the distinct pair, not ``len(rules)``, is the identity.

  core_entry_count
      Total number of ``rules[]`` entries across all ``rulesets[]``.  This is
      informational only: a single literal rule can expand into several Core
      IR entries, so this is never a denominator.

  literal_bearing_rule_set_count
      Number of entries in ``rulesets[]`` (rule sets that carry at least one
      literal rule).

  escaped_td_rule_count
      Number of entries in ``escapes[]``.

  escaped_in_literal_sets
      Escaped rules whose ``ruleset`` also appears in ``rulesets[]``.

  escaped_in_all_escaped_sets
      Escaped rules whose ``ruleset`` does not appear in ``rulesets[]``.

  all_escaped_rule_set_count
      Number of distinct rule-set names that occur only in ``escapes[]``.

  table_count
      Distinct rule sets in total: ``literal_bearing_rule_set_count`` plus
      ``all_escaped_rule_set_count``.

  mutation_target_count
      Number of distinct ``(def, rule_index)`` pairs whose rule carries
      mutation metadata in Core IR.  This is the denominator the corpus
      generator uses to decide which rules can mutate a query slot; it is not
      a rule count.

  td_rule_count
      Total TableGen rules: ``literal_td_rule_count`` plus
      ``escaped_td_rule_count``.

Invariants (checked below; a failure exits non-zero):

  td_rule_count          == literal_td_rule_count + escaped_td_rule_count
  escaped_td_rule_count  == escaped_in_literal_sets
                            + escaped_in_all_escaped_sets
  table_count            == literal_bearing_rule_set_count
                            + all_escaped_rule_set_count
"""

import argparse
import json
import sys

SCHEMA = "llvm.gisel.legalizer-rule-counts"
VERSION = 1
COMMENT = (
    "Coverage denominators for the X86 GlobalISel legalizer regression "
    "suite. Core entries are an expansion of the literal rules and are "
    "never a denominator."
)


def compute_counts(core):
    """Return the nine manifest fields for a parsed Core IR object."""
    rulesets = core["rulesets"]
    escapes = core["escapes"]

    literal_keys = set()
    mutation_keys = set()
    core_entry_count = 0
    literal_set_names = set()
    for ruleset in rulesets:
        def_name = ruleset["def"]
        literal_set_names.add(def_name)
        for rule in ruleset["rules"]:
            literal_keys.add((def_name, rule["rule_index"]))
            if "mutation" in rule:
                mutation_keys.add((def_name, rule["rule_index"]))
            core_entry_count += 1

    escaped_set_names = {escape["ruleset"] for escape in escapes}
    all_escaped_set_names = escaped_set_names - literal_set_names

    escaped_in_literal_sets = sum(
        1 for escape in escapes if escape["ruleset"] in literal_set_names
    )
    escaped_in_all_escaped_sets = len(escapes) - escaped_in_literal_sets

    literal_td_rule_count = len(literal_keys)
    escaped_td_rule_count = len(escapes)
    literal_bearing_rule_set_count = len(rulesets)
    all_escaped_rule_set_count = len(all_escaped_set_names)

    return {
        "schema": SCHEMA,
        "version": VERSION,
        "comment": COMMENT,
        "table_count": literal_bearing_rule_set_count + all_escaped_rule_set_count,
        "literal_bearing_rule_set_count": literal_bearing_rule_set_count,
        "literal_td_rule_count": literal_td_rule_count,
        "core_entry_count": core_entry_count,
        "all_escaped_rule_set_count": all_escaped_rule_set_count,
        "escaped_td_rule_count": escaped_td_rule_count,
        "escaped_in_literal_sets": escaped_in_literal_sets,
        "escaped_in_all_escaped_sets": escaped_in_all_escaped_sets,
        "td_rule_count": literal_td_rule_count + escaped_td_rule_count,
        "mutation_target_count": len(mutation_keys),
    }


def check_invariants(counts):
    """Return a list of human-readable invariant violations."""
    problems = []
    if counts["td_rule_count"] != (
        counts["literal_td_rule_count"] + counts["escaped_td_rule_count"]
    ):
        problems.append(
            "td_rule_count ({}) != literal_td_rule_count ({}) + "
            "escaped_td_rule_count ({})".format(
                counts["td_rule_count"],
                counts["literal_td_rule_count"],
                counts["escaped_td_rule_count"],
            )
        )
    if counts["escaped_td_rule_count"] != (
        counts["escaped_in_literal_sets"] + counts["escaped_in_all_escaped_sets"]
    ):
        problems.append(
            "escaped_td_rule_count ({}) != escaped_in_literal_sets ({}) + "
            "escaped_in_all_escaped_sets ({})".format(
                counts["escaped_td_rule_count"],
                counts["escaped_in_literal_sets"],
                counts["escaped_in_all_escaped_sets"],
            )
        )
    if counts["table_count"] != (
        counts["literal_bearing_rule_set_count"]
        + counts["all_escaped_rule_set_count"]
    ):
        problems.append(
            "table_count ({}) != literal_bearing_rule_set_count ({}) + "
            "all_escaped_rule_set_count ({})".format(
                counts["table_count"],
                counts["literal_bearing_rule_set_count"],
                counts["all_escaped_rule_set_count"],
            )
        )
    return problems


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="Write the X86 legalizer rule-count manifest from Core IR."
    )
    parser.add_argument(
        "--core", required=True, help="path to X86LegalizerInfo.core.json"
    )
    parser.add_argument("--out", required=True, help="output manifest path")
    args = parser.parse_args(argv)

    with open(args.core, "r", encoding="utf-8") as handle:
        core = json.load(handle)

    counts = compute_counts(core)
    problems = check_invariants(counts)
    if problems:
        for problem in problems:
            print("error: invariant violated: " + problem, file=sys.stderr)
        return 1

    with open(args.out, "w", encoding="utf-8") as handle:
        json.dump(counts, handle, indent=2)
        handle.write("\n")
    return 0


if __name__ == "__main__":
    sys.exit(main())
