import json
import os
import subprocess
import sys
import tempfile
import unittest

import testlib


MINI = """\
X86LegalizerInfo::X86LegalizerInfo(const X86Subtarget &STI) : Subtarget(STI) {
  const LLT s8 = LLT::scalar(8);
  const LLT s32 = LLT::scalar(32);
  getActionDefinitionsBuilder(G_ADD)
      .legalFor({{s32, s32}})
      .frobnicate({s8});
}
"""

TD_INC = """\
X86Tables[] = {
  /*0*/ {"X86_ADD", "G_ADD", {0}, {24}, false, 0, 3, 1}, // X86_ADD
};
"""


def run_cli(*args):
    proc = subprocess.run(
        [sys.executable, os.path.join(testlib.TOOL_DIR, "__main__.py"), *args],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    return proc


class CliTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.src = os.path.join(self.tmp.name, "mini.cpp")
        with open(self.src, "w", encoding="utf-8") as handle:
            handle.write(MINI)

    def out(self, name):
        return os.path.join(self.tmp.name, name)

    def test_unknown_method_fails_with_line(self):
        proc = run_cli("parse", "--input", self.src, "--out", self.out("a.json"))
        self.assertEqual(proc.returncode, 2)
        self.assertIn("unknown chain method frobnicate at line 6", proc.stderr)
        self.assertFalse(os.path.exists(self.out("a.json")))

    def test_allow_opaque_keeps_going(self):
        proc = run_cli("parse", "--input", self.src, "--allow-opaque",
                       "--out", self.out("a.json"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        with open(self.out("a.json"), "r", encoding="utf-8") as handle:
            doc = json.load(handle)
        self.assertEqual(doc["census"]["builders"], 1)
        self.assertEqual(doc["unknown_methods"][0]["method"], "frobnicate")

    def test_points_are_byte_deterministic(self):
        source = MINI.replace("      .frobnicate({s8});\n", "")
        with open(self.src, "w", encoding="utf-8") as handle:
            handle.write(source)
        blobs = []
        for name in ("p1.json", "p2.json"):
            proc = run_cli("points", "--input", self.src, "--out", self.out(name))
            self.assertEqual(proc.returncode, 0, proc.stderr)
            with open(self.out(name), "rb") as handle:
                blobs.append(handle.read())
        self.assertEqual(blobs[0], blobs[1])
        doc = json.loads(blobs[0])
        self.assertEqual(doc["tier"], "witness")
        self.assertEqual(doc["point_count"], len(doc["points"]))
        self.assertTrue(all("expected" not in p for p in doc["points"]))

    def test_corpus_without_build_writes_only_points(self):
        source = MINI.replace("      .frobnicate({s8});\n", "")
        with open(self.src, "w", encoding="utf-8") as handle:
            handle.write(source)
        proc = run_cli("corpus", "--input", self.src,
                       "--out", self.out("corpus.json"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        self.assertIn("pass --build", proc.stderr)
        self.assertTrue(os.path.exists(self.out("corpus.json.points.json")))
        self.assertFalse(os.path.exists(self.out("corpus.json")))

    def test_verify_rejects_non_witness_config(self):
        proc = run_cli("parse", "--input", self.src, "--allow-opaque",
                       "--config", "C0", "--out", self.out("c0.json"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        inc = self.out("rules.inc")
        with open(inc, "w", encoding="utf-8") as handle:
            handle.write(TD_INC)
        proc = run_cli("verify", "--ast", self.out("c0.json"),
                       "--td-inc", inc, "--out", self.out("rep.json"))
        self.assertNotEqual(proc.returncode, 0)
        self.assertIn("--config C2", proc.stderr)

    def test_verify_needs_an_explicit_delta_registration(self):
        source = MINI.replace(
            "      .frobnicate({s8});",
            "      .legalIf([](unsigned, LLT, const LegalityQuery &Q) "
            "{ return typeIs(0, s32); });")
        with open(self.src, "w", encoding="utf-8") as handle:
            handle.write(source)
        proc = run_cli("parse", "--input", self.src, "--out", self.out("ast.json"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        inc = self.out("rules.inc")
        with open(inc, "w", encoding="utf-8") as handle:
            handle.write(TD_INC)

        proc = run_cli("verify", "--ast", self.out("ast.json"),
                       "--td-inc", inc, "--out", self.out("rep1.json"))
        self.assertEqual(proc.returncode, 3, proc.stderr)

        deltas = self.out("deltas.json")
        with open(deltas, "w", encoding="utf-8") as handle:
            json.dump({"deltas": [{"rule_set": "X86_ADD",
                                   "class": "td_expands_lambda"}]}, handle)
        proc = run_cli("verify", "--ast", self.out("ast.json"),
                       "--td-inc", inc, "--known-deltas", deltas,
                       "--out", self.out("rep2.json"))
        self.assertEqual(proc.returncode, 0, proc.stderr)
        with open(self.out("rep2.json"), "r", encoding="utf-8") as handle:
            report = json.load(handle)
        self.assertEqual(report["unexplained"], [])
        self.assertEqual(report["known_deltas_used"],
                         ["X86_ADD:td_expands_lambda"])


if __name__ == "__main__":
    unittest.main()
