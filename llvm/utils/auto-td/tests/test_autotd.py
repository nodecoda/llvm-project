"""Unit tests for auto-td.

The fixture is synthetic on purpose: the tool must be verifiable without
checking out any particular target.
"""

from __future__ import annotations

import sys
import tempfile
import textwrap
import unittest
from dataclasses import replace
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from autotd import emit  # noqa: E402
from autotd.check import validate  # noqa: E402
from autotd.encoding import assignments, instruction_format  # noqa: E402
from autotd.model import BitRange, InstrSpec, OpcodeRef, OpcodeRow, Operand, record_name_for  # noqa: E402
from autotd.loader import load_config, load_instructions  # noqa: E402
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


class OperandVarTest(unittest.TestCase):
    """A database field may be exposed under a different TableGen name."""

    def test_renamed_field_drives_dag_asm_and_bits(self):
        ranges = {
            "vd": BitRange("vd", 11, 7),
            "vs2": BitRange("vs2", 24, 20),
            "zimm5": BitRange("zimm5", 19, 15),
            "vm": BitRange("vm", 25, 25),
        }
        spec = InstrSpec(
            mnemonic="demo.vi",
            domain="d",
            predicate="",
            opcode=OpcodeRow("demo.vi", ("vm", "vs2", "zimm5", "vd"), (), ranges, Path("mem.td")),
            ref=OpcodeRef("repo", "ext", "demo.vi"),
            spec_ref="s",
            source=Path("mem.yaml"),
            outs=(Operand("dest", "vd", reg_class="VR"),),
            ins=(
                Operand("lhs", "vs2", reg_class="VR"),
                Operand("imm", "zimm5", operand_class="simm5", var="imm"),
                Operand("mask", "vm", operand_class="VMaskOp", is_mask=True),
            ),
            arg_lut=dict(ranges),
        )
        self.assertIn("Inst{19-15} = imm", {item.range_text() + " = " + item.expr for item in assignments(spec)})
        self.assertEqual(emit.asm_string(spec), "$vd, $vs2, $imm$vm")
        self.assertIn("simm5:$imm", "\n".join(emit.render_record(spec, {}, "T")))


class RecordPropertyTest(FixtureCase):
    """Value-form ``lets`` and taxonomy ``flags`` must reach the record."""

    def test_category_reads_flags_and_lets(self):
        from autotd.taxonomy import effects_of

        effects = effects_of({"effects": {
            "flags": ["UseNamedOperandTable", "VMConstraint"],
            "lets": {"DestEEW": "EEWSEWx2", "Constraints": '"@earlyclobber $vd"'},
        }})
        self.assertEqual(effects.flags, ("UseNamedOperandTable", "VMConstraint"))
        self.assertEqual(dict(effects.lets)["DestEEW"], "EEWSEWx2")
        self.assertEqual(dict(effects.lets)["Constraints"], '"@earlyclobber $vd"')

    def test_flags_and_lets_are_emitted(self):
        spec = self.specs["add"]
        spec.effects = replace(
            spec.effects,
            flags=("VMConstraint",),
            lets=(("DestEEW", "EEWSEWx2"), ("Constraints", '"@earlyclobber $vd"')),
        )
        text = "\n".join(emit.render_record(spec, {}, "T"))
        self.assertIn("let VMConstraint = 1;", text)
        self.assertIn("let DestEEW = EEWSEWx2;", text)
        self.assertIn('let Constraints = "@earlyclobber $vd";', text)

    def test_lets_uses_wins_over_implicit_uses(self):
        spec = self.specs["add"]
        spec.effects = replace(
            spec.effects,
            implicit_uses=("VL", "VTYPE"),
            lets=(("Uses", "[]"),),
        )
        text = "\n".join(emit.render_record(spec, {}, "T"))
        self.assertIn("let Uses = [];", text)
        self.assertNotIn("let Uses = [VL, VTYPE];", text)

    def test_instruction_lets_override_the_category(self):
        path = self.target / "auto-td" / "instructions" / "core" / "add.yaml"
        path.write_text(path.read_text() + "lets: {VMConstraint: 0}\n")
        specs = {s.mnemonic: s for s in load_instructions(self.target, self.db)}
        self.assertEqual(dict(specs["add"].effects.lets)["VMConstraint"], "0")


class RecordPropsToolTest(unittest.TestCase):
    """The local property-diff helper must keep parsing tblgen dumps."""

    DUMP = textwrap.dedent(
        """\
        def DEMO_VV {
          list<Register> Uses = [VL, VTYPE];
          list<Register> Defs = [];
          list<SchedReadWrite> SchedRW = [WriteVIALUV_WorstCase, ReadVMask];
          list<Predicate> Predicates = [HasVInstructions];
          string AsmString = "demo.vv\t$vd, $vs2$vm";
          string Constraints = "@earlyclobber $vd";
          bit VMConstraint = 1;
          bit UseNamedOperandTable = 1;
          bit ReadsPastVL = 0;
          RVTrimVConstraint VS1VS2Constraint = WidenV;
          EEW DestEEW = EEWSEWx2;
          EltDeps ElementsDependOn = EltDepsNone;
          bits<3> VLMul = { 0, 0, 0 };
          InstFormat Format = InstFormatR;
          dag OutOperandList = (outs VR:$vd);
          dag InOperandList = (ins VR:$vs2, VMaskOp:$vm);
        }
        def OTHER {
          bit VMConstraint = 0;
        }
        """
    )

    def test_properties_are_extracted(self):
        import compare_record_props

        records = compare_record_props.parse_dump(self.DUMP, {"DEMO_VV"})
        self.assertEqual(set(records), {"DEMO_VV"})
        props = records["DEMO_VV"]
        self.assertEqual(props["Uses"], ["VL", "VTYPE"])
        self.assertEqual(props["Defs"], [])
        self.assertEqual(props["SchedRW"], ["WriteVIALUV_WorstCase", "ReadVMask"])
        self.assertEqual(props["Constraints"], "@earlyclobber $vd")
        self.assertEqual(props["VMConstraint"], 1)
        self.assertEqual(props["VS1VS2Constraint"], "WidenV")
        self.assertEqual(props["DestEEW"], "EEWSEWx2")
        self.assertEqual(props["VLMul"], "0,0,0")

    def test_names_come_from_the_opcode_file(self):
        import compare_record_props

        with tempfile.TemporaryDirectory() as tmp:
            path = Path(tmp) / "rv_v"
            path.write_text("# comment\nvadd.vv 1 2 3\n\nvsetvli 1 2\n")
            self.assertEqual(
                compare_record_props.names_from_opcodes(path), ["VADD_VV", "VSETVLI"]
            )


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



# --- vector encodings -------------------------------------------------------
#
# The V encodings are not hand-written either: they come from the same
# riscv-opcodes rows the backend's own td uses.  The golden bit assignments
# below were compared against a llvm-tblgen -print-records dump of the
# backend's V records (13/13 records bit-identical).

V_ARG_LUT = textwrap.dedent(
    """\
    "rd", 11, 7
    "rs1", 19, 15
    "rs2", 24, 20
    "vd", 11, 7
    "vs3", 11, 7
    "vs1", 19, 15
    "vs2", 24, 20
    "vm", 25, 25
    "nf", 31, 29
    "simm5", 19, 15
    "zimm5", 19, 15
    "zimm10", 29, 20
    "zimm11", 30, 20
    """
)

RV_V = textwrap.dedent(
    """\
    vsetivli     31=1 30=1 zimm10    zimm5 14..12=0x7 rd 6..0=0x57
    vsetvli      31=0 zimm11          rs1 14..12=0x7 rd 6..0=0x57
    vsetvl       31=1 30..25=0x0 rs2  rs1 14..12=0x7 rd 6..0=0x57
    vle32.v        nf 28=0 27..26=0 vm 24..20=0 rs1 14..12=0x6  vd 6..0=0x07
    vse32.v        nf 28=0 27..26=0 vm 24..20=0 rs1 14..12=0x6 vs3 6..0=0x27
    vadd.vx        31..26=0x00 vm vs2 rs1 14..12=0x4 vd 6..0=0x57
    vadd.vv         31..26=0x00 vm vs2 vs1 14..12=0x0 vd 6..0=0x57
    vsub.vv         31..26=0x02 vm vs2 vs1 14..12=0x0 vd 6..0=0x57
    vmseq.vv       31..26=0x18 vm vs2 vs1 14..12=0x0 vd 6..0=0x57
    vadd.vi        31..26=0x00 vm vs2 simm5 14..12=0x3 vd 6..0=0x57
    vmv1r.v        31..26=0x27 25=1 vs2 19..15=0 14..12=0x3 vd 6..0=0x57
    vredsum.vs     31..26=0x00 vm vs2 vs1 14..12=0x2 vd 6..0=0x57
    vmul.vv        31..26=0x25 vm vs2 vs1 14..12=0x2 vd 6..0=0x57
    """
)

V_CONFIG = """\
record_prefix: RVTRIM_AUTO
base_class: RVInst

domains:
  rv-v: {predicate: HasVInstructions}

formats:
  vsetvli: InstFormatI
  vsetivli: InstFormatI
"""

V_TAXONOMY = """\
categories:
  v_vv:
    operands:
      outs: [{role: dest, field: vd, reg_class: VR}]
      ins:
        - {role: lhs, field: vs2, reg_class: VR}
        - {role: rhs, field: vs1, reg_class: VR}
        - {role: mask, field: vm, operand: VMaskOp}
    effects: {}
  v_vx:
    operands:
      outs: [{role: dest, field: vd, reg_class: VR}]
      ins:
        - {role: lhs, field: vs2, reg_class: VR}
        - {role: rhs, field: rs1, reg_class: GPR}
        - {role: mask, field: vm, operand: VMaskOp}
    effects: {}
  v_vi:
    operands:
      outs: [{role: dest, field: vd, reg_class: VR}]
      ins:
        - {role: lhs, field: vs2, reg_class: VR}
        - {role: imm, field: simm5, operand: simm5}
        - {role: mask, field: vm, operand: VMaskOp}
    effects: {}
  v_load_unit:
    operands:
      outs: [{role: dest, field: vd, reg_class: VR}]
      ins:
        - {role: base, field: rs1, operand: GPRMemZeroOffset}
        - {role: mask, field: vm, operand: VMaskOp}
    effects: {may_load: true}
  v_store_unit:
    operands:
      ins:
        - {role: rhs, field: vs3, reg_class: VR}
        - {role: base, field: rs1, operand: GPRMemZeroOffset}
        - {role: mask, field: vm, operand: VMaskOp}
    effects: {may_store: true}
  v_setvli:
    operands:
      outs: [{role: dest, field: rd, reg_class: GPR}]
      ins:
        - {role: lhs, field: rs1, reg_class: GPR}
        - {role: imm, field: zimm11, operand: VTypeIOp11}
    effects: {has_side_effects: true}
  v_setivli:
    operands:
      outs: [{role: dest, field: rd, reg_class: GPR}]
      ins:
        - {role: imm, field: zimm5, operand: uimm5}
        - {role: vtype, field: zimm10, operand: VTypeIOp10}
    effects: {has_side_effects: true}
  v_setvl:
    operands:
      outs: [{role: dest, field: rd, reg_class: GPR}]
      ins:
        - {role: lhs, field: rs1, reg_class: GPR}
        - {role: rhs, field: rs2, reg_class: GPR}
    effects: {has_side_effects: true}
  v_mv1r:
    operands:
      outs: [{role: dest, field: vd, reg_class: VR}]
      ins: [{role: src, field: vs2, reg_class: VR}]
    effects: {}
  v_red:
    operands:
      outs: [{role: dest, field: vd, reg_class: VR}]
      ins:
        - {role: lhs, field: vs2, reg_class: VR}
        - {role: rhs, field: vs1, reg_class: VR}
        - {role: mask, field: vm, operand: VMaskOp}
    effects: {}
  v_cmp:
    operands:
      outs: [{role: dest, field: vd, reg_class: VR}]
      ins:
        - {role: lhs, field: vs2, reg_class: VR}
        - {role: rhs, field: vs1, reg_class: VR}
        - {role: mask, field: vm, operand: VMaskOp}
    effects: {}
"""

V_INSTRUCTIONS = {
    "rv-v/vadd.vv.yaml": """\
        mnemonic: vadd.vv
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vadd_vv}
        spec: vector.alu.v_vv
        """,
    "rv-v/vadd.vx.yaml": """\
        mnemonic: vadd.vx
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vadd_vx}
        spec: vector.alu.v_vx
        """,
    "rv-v/vadd.vi.yaml": """\
        mnemonic: vadd.vi
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vadd_vi}
        spec: vector.alu.v_vi
        """,
    "rv-v/vsub.vv.yaml": """\
        mnemonic: vsub.vv
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vsub_vv}
        spec: vector.alu.v_vv
        """,
    "rv-v/vmul.vv.yaml": """\
        mnemonic: vmul.vv
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vmul_vv}
        spec: vector.alu.v_vv
        """,
    "rv-v/vle32.v.yaml": """\
        mnemonic: vle32.v
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vle32_v}
        spec: vector.mem.v_load_unit
        """,
    "rv-v/vse32.v.yaml": """\
        mnemonic: vse32.v
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vse32_v}
        spec: vector.mem.v_store_unit
        """,
    "rv-v/vsetvli.yaml": """\
        mnemonic: vsetvli
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vsetvli}
        spec: vector.ctrl.v_setvli
        """,
    "rv-v/vsetivli.yaml": """\
        mnemonic: vsetivli
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vsetivli}
        spec: vector.ctrl.v_setivli
        """,
    "rv-v/vsetvl.yaml": """\
        mnemonic: vsetvl
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vsetvl}
        spec: vector.ctrl.v_setvl
        """,
    "rv-v/vmv1r.v.yaml": """\
        mnemonic: vmv1r.v
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vmv1r_v}
        spec: vector.move.v_mv1r
        """,
    "rv-v/vredsum.vs.yaml": """\
        mnemonic: vredsum.vs
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vredsum_vs}
        spec: vector.red.v_red
        """,
    "rv-v/vmseq.vv.yaml": """\
        mnemonic: vmseq.vv
        opcode: {repo: riscv-opcodes, extension: rv_v, key: vmseq_vv}
        spec: vector.cmp.v_cmp
        """,
}

V_GOLDEN = {
    "VADD_VV": [
        "let Inst{31-26} = 0b000000;",
        "let Inst{25} = vm;",
        "let Inst{24-20} = vs2;",
        "let Inst{19-15} = vs1;",
        "let Inst{14-12} = 0b000;",
        "let Inst{11-7} = vd;",
        "let Inst{6-0} = 0b1010111;",
    ],
    "VADD_VX": [
        "let Inst{19-15} = rs1;",
        "let Inst{14-12} = 0b100;",
    ],
    "VADD_VI": [
        "let Inst{19-15} = simm5;",
        "let Inst{14-12} = 0b011;",
    ],
    "VSUB_VV": ["let Inst{31-26} = 0b000010;"],
    "VMUL_VV": [
        "let Inst{31-26} = 0b100101;",
        "let Inst{14-12} = 0b010;",
    ],
    "VLE32_V": [
        "let Inst{31-29} = 0b000;",
        "let Inst{28} = 0b0;",
        "let Inst{27-26} = 0b00;",
        "let Inst{25} = vm;",
        "let Inst{24-20} = 0b00000;",
        "let Inst{19-15} = rs1;",
        "let Inst{14-12} = 0b110;",
        "let Inst{11-7} = vd;",
        "let Inst{6-0} = 0b0000111;",
    ],
    "VSE32_V": [
        "let Inst{11-7} = vs3;",
        "let Inst{6-0} = 0b0100111;",
    ],
    "VSETVLI": [
        "let Inst{31} = 0b0;",
        "let Inst{30-20} = zimm11;",
        "let Inst{19-15} = rs1;",
        "let Inst{14-12} = 0b111;",
        "let Inst{11-7} = rd;",
    ],
    "VSETIVLI": [
        "let Inst{31} = 0b1;",
        "let Inst{30} = 0b1;",
        "let Inst{29-20} = zimm10;",
        "let Inst{19-15} = zimm5;",
    ],
    "VSETVL": [
        "let Inst{31} = 0b1;",
        "let Inst{30-25} = 0b000000;",
        "let Inst{24-20} = rs2;",
        "let Inst{19-15} = rs1;",
    ],
    "VMV1R_V": [
        "let Inst{31-26} = 0b100111;",
        "let Inst{25} = 0b1;",
        "let Inst{24-20} = vs2;",
        "let Inst{19-15} = 0b00000;",
        "let Inst{14-12} = 0b011;",
    ],
    "VREDSUM_VS": ["let Inst{14-12} = 0b010;"],
    "VMSEQ_VV": [
        "let Inst{31-26} = 0b011000;",
        "let Inst{25} = vm;",
    ],
}


def build_vector_fixture(root: Path) -> Path:
    target = root / "Target"
    (target / "auto-td" / "instructions" / "rv-v").mkdir(parents=True)
    (target / "auto-td" / "taxonomy").mkdir(parents=True)
    (target / "auto-td" / "config.yaml").write_text(V_CONFIG)
    (target / "auto-td" / "taxonomy" / "vector.yaml").write_text(V_TAXONOMY)
    for name, body in V_INSTRUCTIONS.items():
        (target / "auto-td" / "instructions" / name).write_text(textwrap.dedent(body))

    repo = root / "riscv-opcodes"
    (repo / "extensions").mkdir(parents=True)
    (repo / "arg_lut.csv").write_text(V_ARG_LUT)
    (repo / "extensions" / "rv_v").write_text(RV_V)
    return target


class VectorFixtureCase(unittest.TestCase):
    def setUp(self) -> None:
        self._tmp = tempfile.TemporaryDirectory()
        self.root = Path(self._tmp.name)
        self.target = build_vector_fixture(self.root)
        self.db = OpcodeDatabase()
        self.db.add_repo("riscv-opcodes", self.root / "riscv-opcodes")
        self.specs = load_instructions(self.target, self.db)
        self.records = emit.render_instrinfo(self.specs, load_config(self.target))

    def tearDown(self) -> None:
        self._tmp.cleanup()

    def record_body(self, name: str) -> str:
        start = self.records.index(f"def RVTRIM_AUTO_{name} :")
        return self.records[start:self.records.index("\n}", start)]


class VectorEncodingTest(VectorFixtureCase):
    def test_fixture_validates(self):
        self.assertEqual(len(self.specs), 13)
        self.assertEqual(validate(self.specs, "RVTRIM_AUTO"), [])

    def test_golden_encodings(self):
        for name, lines in V_GOLDEN.items():
            body = self.record_body(name)
            for line in lines:
                self.assertIn(line, body, f"{name}: {line}")

    def test_nf_is_forced_to_zero(self):
        for name in ("VLE32_V", "VSE32_V"):
            self.assertIn("let Inst{31-29} = 0b000;", self.record_body(name))

    def test_mask_bit_is_a_variable(self):
        for name in ("VADD_VV", "VLE32_V", "VMSEQ_VV"):
            self.assertIn("let Inst{25} = vm;", self.record_body(name))

    def test_vsetivli_pins_bits_31_and_30(self):
        body = self.record_body("VSETIVLI")
        self.assertIn("let Inst{31} = 0b1;", body)
        self.assertIn("let Inst{30} = 0b1;", body)


if __name__ == "__main__":
    unittest.main()
