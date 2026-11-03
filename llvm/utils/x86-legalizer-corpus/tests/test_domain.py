import os
import tempfile
import unittest

import testlib  # noqa: F401  (sets sys.path)
import domain
import emit
from target import CONFIGS
import parse

MINI = """\
X86LegalizerInfo::X86LegalizerInfo(const X86Subtarget &STI) : Subtarget(STI) {
  const LLT s8 = LLT::scalar(8);
  const LLT s32 = LLT::scalar(32);
  const LLT p0 = LLT::pointer(0);

  for (unsigned Op : {G_ADD, G_SUB}) {
    getActionDefinitionsBuilder(Op)
        .legalFor({{s32, s32}})
        .widenScalarToNextPow2(0);
  }

  auto &Action = getActionDefinitionsBuilder(G_LOAD);
  if (Subtarget.hasAVX2())
    Action.legalForTypesWithMemDesc({{s32, p0, s8, 8}});
  else
    Action.legalForTypesWithMemDesc({{s8, p0, s8, 8}});

  getActionDefinitionsBuilder(G_CTPOP)
      .legalIf([](unsigned TypeIdx, LLT Ty, const LegalityQuery &Q) {
        return typeIs(0, s32);
      })
      .lower();
}
"""


PRUNE = """\
X86LegalizerInfo::X86LegalizerInfo(const X86Subtarget &STI) : Subtarget(STI) {
  const LLT s8 = LLT::scalar(8);
  const LLT s32 = LLT::scalar(32);
  const LLT p0 = LLT::pointer(0);
  getActionDefinitionsBuilder(G_CTLZ)
      .legalFor({s32})
      .scalarSameSizeAs(0, 1);
}
"""


def mini_result(config="C2", text=MINI):
    source = {"rev": None, "path": "<test>", "sha256": "test", "lines": 1}
    return parse.parse_source(text, source, config, CONFIGS[config])


class DomainTest(unittest.TestCase):
    def test_witness_points_are_shape_only(self):
        result = mini_result()
        points, _, _, panel, pruned = domain.build_points(result, "witness")
        self.assertEqual(pruned, 0)
        self.assertEqual(len(points), 72)
        self.assertEqual(panel, domain.BOUNDARY_PANEL)
        for p in points:
            self.assertNotIn("expected", p)
            self.assertTrue(p["id"].startswith("cpp/"))
            self.assertGreaterEqual(len(p["types"]), 1)
        self.assertEqual(len({p["id"] for p in points}), len(points))

    def test_origin_counts(self):
        points, _, _, _, _ = domain.build_points(mini_result(), "witness")
        counts = {}
        for p in points:
            counts[p["origin"]] = counts.get(p["origin"], 0) + 1
        self.assertEqual(counts, {"as": 20, "boundary": 48, "rule": 4})

    def test_address_space_points_cover_memory_pointer(self):
        points, _, _, _, _ = domain.build_points(mini_result(), "witness")
        load_as = [p for p in points
                   if p["rule_set"] == "X86_LOAD" and p["origin"] == "as"]
        self.assertEqual(len(load_as), 10)
        self.assertIn((["p1", "p0"], "p0"), [(p["types"], p["mem"]) for p in load_as])
        self.assertIn((["p0", "p1"], "p0"), [(p["types"], p["mem"]) for p in load_as])

    def test_rule_points_carry_the_rule_index(self):
        points, _, _, _, _ = domain.build_points(mini_result(), "witness")
        load = [p for p in points
                if p["rule_set"] == "X86_LOAD" and p["origin"] == "rule"]
        self.assertEqual([p["rule_index"] for p in load], [0])
        self.assertEqual(load[0]["mem"], "s8")

    def test_full_and_deep_add_class_pairs(self):
        witness, _, _, _, _ = domain.build_points(mini_result(), "witness")
        full, _, _, _, _ = domain.build_points(mini_result(), "full")
        deep, _, _, _, _ = domain.build_points(mini_result(), "deep")
        self.assertGreater(len(full), len(witness))
        self.assertGreater(len(deep), len(witness))
        self.assertTrue(any(p["origin"] == "classpair" for p in full))
        self.assertTrue(any(p["origin"] == "classpair" for p in deep))

    def test_pointer_in_mutated_slot_is_pruned(self):
        full, _, _, _, pruned = domain.build_points(
            mini_result(text=PRUNE), "full")
        self.assertGreater(pruned, 0)
        ctlz = [p for p in full
                if p["rule_set"] == "X86_CTLZ" and p["origin"] == "classpair"]
        self.assertTrue(ctlz)
        # scalarSameSizeAs(0, 1) mutates slot 0, so a pointer there is not a
        # well-formed query; a pointer in slot 1 is fine.
        self.assertFalse(any(p["types"][0] == "p0" for p in ctlz))
        self.assertTrue(any(p["types"][1] == "p0" for p in ctlz))

    def test_points_are_byte_deterministic(self):
        result = mini_result()
        with tempfile.TemporaryDirectory() as tmp:
            docs = []
            for name in ("a.json", "b.json"):
                points, classes, mem_classes, panel, pruned = domain.build_points(
                    result, "witness")
                doc = emit.points_document(result, result.source, "witness",
                                           points, classes, mem_classes, panel,
                                           pruned)
                path = os.path.join(tmp, name)
                emit.write_json(path, doc)
                with open(path, "rb") as handle:
                    docs.append(handle.read())
        self.assertEqual(docs[0], docs[1])


if __name__ == "__main__":
    unittest.main()
