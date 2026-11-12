"""Unit tests for auto-td.

The fixture is synthetic on purpose: the tool must be verifiable without
checking out any particular target.
"""

from __future__ import annotations

import sys
import tempfile
import textwrap
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from autotd import emit  # noqa: E402
from autotd.check import validate  # noqa: E402
from autotd.encoding import assignments, instruction_format  # noqa: E402
from autotd.model import BitRange, InstrSpec, OpcodeRef, OpcodeRow, record_name_for  # noqa: E402
from autotd.loader import load_instructions  # noqa: E402
from autotd.opcodes import OpcodeDatabase, parse_fixed, read_arg_lut  # noqa: E402

ARG_LUT = textwrap.dedent(
    """\
    "rd", 11, 7
    "rs1", 19, 15
    "rs2", 24, 20
    "imm12", 31, 20
    "imm20", 31, 12
    "imm12hi", 31, 25
    "imm12lo", 11, 7
    "bimm12hi", 31, 25
    "bimm12lo", 11, 7
    "jimm20", 31, 12
    "aq", 26, 26
    "rl", 25, 25
    "nf", 31, 29
    "vm", 25, 25
    """
)

RV_I = textwrap.dedent(
    """\
    add            rd rs1 rs2 31..25=0 14..12=0 6..2=0x0c 1..0=3
    addi           rd rs1 imm12 14..12=0 6..2=0x04 1..0=3
    sw             imm12hi rs1 rs2 imm12lo 14..12=2 6..2=0x08 1..0=3
    lui            rd imm20 6..2=0x0d 1..0=3
    beq            bimm12hi rs1 rs2 bimm12lo 14..12=0 6..2=0x18 1..0=3
    jal            rd jimm20 6..2=0x1b 1..0=3
    amoadd.w       rd rs1 rs2 aq rl 31..29=0 28..27=0 14..12=2 6..2=0x0b 1..0=3
    """
)

RV_X = textwrap.dedent(
    """\
    demo.vreduce  31..26=0x2a vm rs2 19..15=0 14..12=0x1 rd 6..0=0x0b
    """
)

CONFIG = textwrap.dedent(
    """\
    record_prefix: TST_AUTO
    base_class: TestInst
    domains:
      core:
        predicate: HasStdExtCore
      vec:
        predicate: HasStdExtVec
    operand_classes:
      VMaskOp: TestMaskOp
      VMaskCarryInOp: TestCarryMaskOp
    formats:
      lui: InstFormatU
    builtins:
      types:
        i32m1: "_ExtVector<4, int>"
      features: "xtest"
      class: TestBuiltin
    """
)

TAXONOMY = textwrap.dedent(
    """\
    categories:
      r_type:
        operands:
          outs: [{role: dest, field: rd, reg_class: GPR}]
          ins:
            - {role: lhs, field: rs1, reg_class: GPR}
            - {role: rhs, field: rs2, reg_class: GPR}
        effects: {implicit_uses: [VL, VTYPE]}
      i_type:
        operands:
          outs: [{role: dest, field: rd, reg_class: GPR}]
          ins: [{role: lhs, field: rs1, reg_class: GPR}, {role: imm, field: imm12, operand: SImm12}]
        effects: {}
      s_type:
        operands:
          ins:
            - {role: rhs, field: rs2, reg_class: GPR}
            - {role: base, field: rs1, reg_class: GPR}
            - {role: offset, field: imm12, operand: SImm12}
        effects: {may_store: true}
      u_type:
        operands:
          outs: [{role: dest, field: rd, reg_class: GPR}]
          ins: [{role: imm, field: imm20, operand: UImm20}]
        effects: {}
      b_type:
        operands:
          ins:
            - {role: lhs, field: rs1, reg_class: GPR}
            - {role: rhs, field: rs2, reg_class: GPR}
            - {role: offset, field: imm12, operand: SImm13Lsb0}
        effects: {}
      j_type:
        operands:
          outs: [{role: dest, field: rd, reg_class: GPR}]
          ins: [{role: offset, field: imm20, operand: SImm21Lsb0Jal}]
        effects: {}
      amo_type:
        operands:
          outs: [{role: dest, field: rd, reg_class: GPR}]
          ins:
            - {role: rhs, field: rs2, reg_class: GPR}
            - {role: base, field: rs1, reg_class: GPRMemZeroOffset}
        effects: {}
      reduce:
        operands:
          outs: [{role: dest, field: rd, reg_class: GPR}]
          ins:
            - {role: vector, field: rs2, reg_class: GPR}
            - {role: carry, field: vm, operand: VMaskCarryInOp}
        effects: {}
    """
)

INSTRUCTIONS = {
    "core/add.yaml": """\
        mnemonic: add
        opcode: {repo: riscv-opcodes, extension: rv_i, key: add}
        spec: core.alu.r_type
        """,
    "core/addi.yaml": """\
        mnemonic: addi
        opcode: {repo: riscv-opcodes, extension: rv_i, key: addi}
        spec: core.alu.i_type
        """,
    "core/sw.yaml": """\
        mnemonic: sw
        opcode: {repo: riscv-opcodes, extension: rv_i, key: sw}
        spec: core.mem.s_type
        """,
    "core/lui.yaml": """\
        mnemonic: lui
        opcode: {repo: riscv-opcodes, extension: rv_i, key: lui}
        spec: core.alu.u_type
        """,
    "core/beq.yaml": """\
        mnemonic: beq
        opcode: {repo: riscv-opcodes, extension: rv_i, key: beq}
        spec: core.ctrl.b_type
        """,
    "core/jal.yaml": """\
        mnemonic: jal
        opcode: {repo: riscv-opcodes, extension: rv_i, key: jal}
        spec: core.ctrl.j_type
        """,
    "core/amoadd_w.yaml": """\
        mnemonic: amoadd.w
        opcode: {repo: riscv-opcodes, extension: rv_i, key: amoadd_w}
        spec: core.amo.amo_type
        fixed: {aq: 0, rl: 0}
        """,
    "vec/demo_vreduce.yaml": """\
        mnemonic: demo.vreduce
        opcode: {repo: ysx-opcodes, extension: rv_x, key: demo_vreduce}
        spec: vec.alu.reduce
        """,
}


def build_fixture(root: Path) -> Path:
    target = root / "Target"
    (target / "auto-td" / "instructions" / "core").mkdir(parents=True)
    (target / "auto-td" / "instructions" / "vec").mkdir(parents=True)
    (target / "auto-td" / "taxonomy").mkdir(parents=True)
    (target / "auto-td" / "config.yaml").write_text(CONFIG)
    (target / "auto-td" / "taxonomy" / "alu.yaml").write_text(TAXONOMY)
    for name, body in INSTRUCTIONS.items():
        (target / "auto-td" / "instructions" / name).write_text(textwrap.dedent(body))

    riscv = root / "riscv-opcodes"
    (riscv / "extensions").mkdir(parents=True)
    (riscv / "arg_lut.csv").write_text(ARG_LUT)
    (riscv / "extensions" / "rv_i").write_text(RV_I)
    local = root / "ysx-opcodes"
    (local / "extensions").mkdir(parents=True)
    (local / "extensions" / "rv_x").write_text(RV_X)
    return target


class FixtureCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.target = build_fixture(self.root)
        self.db = OpcodeDatabase()
        self.db.add_repo("riscv-opcodes", self.root / "riscv-opcodes")
        self.db.add_repo("ysx-opcodes", self.root / "ysx-opcodes", self.db.lut_for("riscv-opcodes"))
        self.specs = {s.mnemonic: s for s in load_instructions(self.target, self.db)}

    def tearDown(self) -> None:
        self._tmp.cleanup()


class OpcodeReaderTest(FixtureCase):
    def test_arg_lut_is_parsed(self):
        lut = read_arg_lut(self.root / "riscv-opcodes" / "arg_lut.csv")
        self.assertEqual((lut["rd"].msb, lut["rd"].lsb), (11, 7))

    def test_fixed_tokens(self):
        self.assertEqual(parse_fixed("31..25=0"), (31, 25, 0))
        self.assertEqual(parse_fixed("6..2=0x0c"), (6, 2, 12))
        self.assertEqual(parse_fixed("25=1"), (25, 25, 1))

    def test_fixed_token_rejects_overflow(self):
        with self.assertRaises(ValueError):
            parse_fixed("6..2=0x20")

    def test_missing_field_range_is_reported(self):
        spec = self.specs["add"]
        spec.opcode = OpcodeRow(
            spec.opcode.mnemonic,
            tuple(spec.opcode.fields) + ("ghost",),
            spec.opcode.fixed,
            dict(spec.opcode.ranges),
            spec.opcode.origin,
        )
        with self.assertRaises(ValueError) as ctx:
            assignments(spec)
        self.assertIn("no bit range", str(ctx.exception))


class EncodingTest(FixtureCase):
    def bits(self, mnemonic):
        return {item.range_text(): item.expr for item in assignments(self.specs[mnemonic])}

    def test_r_type_fields_and_constants(self):
        bits = self.bits("add")
        self.assertEqual(bits["Inst{24-20}"], "rs2")
        self.assertEqual(bits["Inst{19-15}"], "rs1")
        self.assertEqual(bits["Inst{31-25}"], "0b0000000")

    def test_imm12_is_kept_whole_for_i_type(self):
        self.assertEqual(self.bits("addi")["Inst{31-20}"], "imm12")

    def test_s_type_splits_the_immediate(self):
        bits = self.bits("sw")
        self.assertEqual(bits["Inst{31-25}"], "imm12{11-5}")
        self.assertEqual(bits["Inst{11-7}"], "imm12{4-0}")

    def test_nf_field_is_forced_to_zero(self):
        spec = self.specs["demo.vreduce"]
        # vreduce has no nf; synthesize one to exercise the rule
        ranges = dict(spec.opcode.ranges)
        ranges["nf"] = spec.arg_lut["nf"]
        spec.opcode = OpcodeRow("demo.vreduce", ("vm", "rs2", "nf", "rd"), spec.opcode.fixed, ranges, spec.opcode.origin)
        self.assertEqual({i.range_text(): i.expr for i in assignments(spec)}["Inst{31-29}"], "0b000")

    def test_carry_mask_replaces_constant_zero(self):
        bits = self.bits("demo.vreduce")
        self.assertEqual(bits["Inst{25}"], "vm")

    def test_b_type_scatters_the_immediate(self):
        bits = self.bits("beq")
        self.assertEqual(bits["Inst{31}"], "imm12{11}")
        self.assertEqual(bits["Inst{30-25}"], "imm12{9-4}")
        self.assertEqual(bits["Inst{11-8}"], "imm12{3-0}")
        self.assertEqual(bits["Inst{7}"], "imm12{10}")

    def test_j_type_scatters_the_immediate(self):
        bits = self.bits("jal")
        self.assertEqual(bits["Inst{31}"], "imm20{19}")
        self.assertEqual(bits["Inst{30-21}"], "imm20{9-0}")
        self.assertEqual(bits["Inst{20}"], "imm20{10}")
        self.assertEqual(bits["Inst{19-12}"], "imm20{18-11}")

    def test_fixed_fields_pin_database_bits(self):
        bits = self.bits("amoadd.w")
        self.assertEqual(bits["Inst{26}"], "0b0")
        self.assertEqual(bits["Inst{25}"], "0b0")

    def test_rule_supplies_declared_width(self):
        text = "\n".join(emit.render_record(self.specs["beq"], {}, "T"))
        self.assertIn("bits<12> imm12;", text)

    def test_format_inference(self):
        self.assertEqual(instruction_format(self.specs["add"]), "InstFormatR")
        self.assertEqual(instruction_format(self.specs["addi"]), "InstFormatI")
        self.assertEqual(instruction_format(self.specs["sw"]), "InstFormatS")
        self.assertEqual(instruction_format(self.specs["lui"], {"lui": "InstFormatU"}), "InstFormatU")


class EmitTest(FixtureCase):
    def setUp(self) -> None:
        super().setUp()
        from autotd.loader import load_config

        self.config = load_config(self.target)
        self.text = emit.render_instrinfo(list(self.specs.values()), self.config)

    def test_records_are_flat_and_prefixed(self):
        self.assertIn("def TST_AUTO_ADD : TestInst<", self.text)
        self.assertNotIn("multiclass", self.text)

    def test_operand_class_mapping_is_applied(self):
        self.assertIn("TestCarryMaskOp:$vm", self.text)

    def test_store_folds_offset_into_address(self):
        self.assertIn('"$rs2, ${imm12}(${rs1})"', self.text)

    def test_output_is_deterministic(self):
        again = emit.render_instrinfo(list(reversed(list(self.specs.values()))), self.config)
        self.assertEqual(again, self.text)


class GuardTest(FixtureCase):
    def test_clean_fixture_has_no_problems(self):
        self.assertEqual(validate(list(self.specs.values()), "TST_AUTO"), [])

    def test_duplicate_opcode_is_rejected(self):
        clone = self.specs["add"]
        clone.mnemonic = "add.clone"
        problems = validate(list(self.specs.values()) + [clone], "TST_AUTO")
        self.assertTrue(any("claimed by" in p for p in problems), problems)

    def test_forbidden_token_is_rejected(self):
        spec = self.specs["add"]
        spec.source.write_text(spec.source.read_text() + "\nraw_td: whatever\n")
        problems = validate(list(self.specs.values()), "TST_AUTO")
        self.assertTrue(any("forbidden token" in p for p in problems), problems)

    def test_missing_predicate_is_rejected(self):
        spec = self.specs["add"]
        spec.predicate = ""
        spec.predicate_declared = False
        problems = validate(list(self.specs.values()), "TST_AUTO")
        self.assertTrue(any("missing 'predicate'" in p for p in problems), problems)

    def test_status_must_be_known(self):
        spec = self.specs["add"]
        spec.status = "hand_written"
        problems = validate(list(self.specs.values()), "TST_AUTO")
        self.assertTrue(any("status" in p for p in problems), problems)


class FieldRuleTest(unittest.TestCase):
    """RISC-V fields whose name or shape differs from the operand they carry."""

    def _spec(self, mnemonic, fields, ranges, fixed=()):
        opcode = OpcodeRow(mnemonic, tuple(fields), tuple(fixed), dict(ranges), Path("mem.td"))
        return InstrSpec(
            mnemonic=mnemonic,
            domain="d",
            predicate="",
            opcode=opcode,
            ref=OpcodeRef("repo", "ext", mnemonic),
            spec_ref="s",
            source=Path("mem.yaml"),
            arg_lut=dict(ranges),
        )

    def _bits(self, spec):
        return {item.range_text(): item.expr for item in assignments(spec)}

    def test_rv64_shift_uses_a_six_bit_shamt(self):
        ranges = {"rd": BitRange("rd", 11, 7), "rs1": BitRange("rs1", 19, 15),
                  "shamtd": BitRange("shamtd", 25, 20)}
        spec = self._spec("slli", ("rd", "rs1", "shamtd"), ranges)
        self.assertEqual(self._bits(spec)["Inst{25-20}"], "shamt")
        self.assertIn("bits<6> shamt;", emit.field_declarations(spec))

    def test_rv32_shift_uses_a_five_bit_shamt(self):
        ranges = {"rd": BitRange("rd", 11, 7), "rs1": BitRange("rs1", 19, 15),
                  "shamtw": BitRange("shamtw", 24, 20)}
        spec = self._spec("slliw", ("rd", "rs1", "shamtw"), ranges)
        self.assertEqual(self._bits(spec)["Inst{24-20}"], "shamt")
        self.assertIn("bits<5> shamt;", emit.field_declarations(spec))

    def test_csr_number_fills_the_imm12_slot(self):
        ranges = {"rd": BitRange("rd", 11, 7), "rs1": BitRange("rs1", 19, 15),
                  "csr": BitRange("csr", 31, 20)}
        spec = self._spec("csrrw", ("rd", "rs1", "csr"), ranges)
        self.assertEqual(self._bits(spec)["Inst{31-20}"], "imm12")
        self.assertIn("bits<12> imm12;", emit.field_declarations(spec))


class RecordNamingTest(unittest.TestCase):
    def test_default_prefix(self):
        self.assertEqual(record_name_for("lr.w.aq"), "AUTO_LR_W_AQ")

    def test_empty_prefix_is_canonical(self):
        self.assertEqual(record_name_for("lr.w.aq", ""), "LR_W_AQ")


class DeclaredEmptyTest(FixtureCase):
    """`sched: []` and `predicate: null` are deliberate, not omissions."""

    def test_empty_sched_declares_an_empty_list(self):
        spec = self.specs["add"]
        spec.sched = ()
        text = "\n".join(emit.render_record(spec, {"emit_scheduling_stub": True}, "T"))
        self.assertIn("let SchedRW = [];", text)
        self.assertNotIn("hasNoSchedulingInfo", text)

    def test_absent_sched_emits_the_stub(self):
        spec = self.specs["add"]
        spec.sched = None
        text = "\n".join(emit.render_record(spec, {"emit_scheduling_stub": True}, "T"))
        self.assertIn("let hasNoSchedulingInfo = 1;", text)

    def test_declared_null_predicate_validates(self):
        spec = self.specs["add"]
        spec.predicate = ""
        spec.predicate_declared = True
        text = "\n".join(emit.render_record(spec, {}, "T"))
        self.assertIn("let Predicates = [];", text)
        self.assertEqual(validate([spec], "T"), [])


class CliTest(FixtureCase):
    def test_check_only_passes(self):
        from autotd.cli import main

        code = main([
            "--target-root", str(self.target),
            "--riscv-opcodes", str(self.root / "riscv-opcodes"),
            "--ysx-opcodes", str(self.root / "ysx-opcodes"),
            "--out-dir", str(self.root / "out"),
            "--check-only", "--quiet",
        ])
        self.assertEqual(code, 0)

    def test_generate_writes_all_artifacts(self):
        from autotd.cli import main

        out = self.root / "out"
        code = main([
            "--target-root", str(self.target),
            "--riscv-opcodes", str(self.root / "riscv-opcodes"),
            "--ysx-opcodes", str(self.root / "ysx-opcodes"),
            "--out-dir", str(out),
            "--clang-builtins-td", str(out / "Builtins.td"),
            "--quiet",
        ])
        self.assertEqual(code, 0)
        for name in ("AutoTdInstrInfo.inc", "AutoTdManifests.inc", "AutoTdCoverage.md"):
            self.assertTrue((out / name).is_file(), name)


if __name__ == "__main__":
    unittest.main()
