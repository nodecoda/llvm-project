"""Derive concrete bit assignments from an opcode row.

The rule is: *every* bit of the instruction comes from the database.  Nothing
here may consult a hand-written table of encodings.  The only target knowledge
allowed is a small, declarative set of field rules (``FIELD_RULES``) plus
whatever the opcode row itself says.
"""

from __future__ import annotations

from dataclasses import dataclass

from .model import BitRange, InstrSpec
from .opcodes import parse_fixed


@dataclass(frozen=True)
class BitAssignment:
    msb: int
    lsb: int
    expr: str

    @property
    def width(self) -> int:
        return self.msb - self.lsb + 1

    def range_text(self) -> str:
        return f"Inst{{{self.msb}}}" if self.msb == self.lsb else f"Inst{{{self.msb}-{self.lsb}}}"


def literal(value: int, width: int) -> str:
    return "0b" + format(value, f"0{width}b")


@dataclass(frozen=True)
class FieldRule:
    """How one database field maps onto TableGen bit slices.

    Some ISA fields are not one contiguous slice: a RISC-V B-type immediate is
    scattered over two fields and four slices, and a J-type immediate over one
    field and four slices.  A rule names the operand variable that carries the
    value, its declared width, and the (inst, value) slice pairs.
    """

    var: str
    width: int
    pieces: tuple[tuple[int, int, int, int], ...]

    def expand(self) -> list["BitAssignment"]:
        out: list["BitAssignment"] = []
        for inst_msb, inst_lsb, val_msb, val_lsb in self.pieces:
            if val_msb == self.width - 1 and val_lsb == 0:
                expr = self.var  # the whole variable, no slice needed
            elif val_msb == val_lsb:
                expr = f"{self.var}{{{val_msb}}}"
            else:
                expr = f"{self.var}{{{val_msb}-{val_lsb}}}"
            out.append(BitAssignment(inst_msb, inst_lsb, expr))
        return out


#: Fields that are not "the operand of the same name".
#:
#: RISC-V keeps the immediate in the *instruction* bit order in TableGen
#: (``RVInstB`` declares ``bits<12> imm12`` and scatters it itself), so the
#: rules below use the 12-bit / 20-bit variable, not the 13-bit / 21-bit
#: value with its implicit zero LSB.  That keeps generated records slice-for-
#: slice identical to the hand-written backend.
FIELD_RULES: dict[str, FieldRule] = {
    # S-format immediate halves (contiguous slices of imm12).
    "imm12hi": FieldRule("imm12", 12, ((31, 25, 11, 5),)),
    "imm12lo": FieldRule("imm12", 12, ((11, 7, 4, 0),)),
    # B-format immediate: {imm[12], imm[10:5]} and {imm[4:1], imm[11]}.
    "bimm12hi": FieldRule("imm12", 12, ((31, 31, 11, 11), (30, 25, 9, 4))),
    "bimm12lo": FieldRule("imm12", 12, ((11, 8, 3, 0), (7, 7, 10, 10))),
    # J-format immediate: {imm[20], imm[10:1], imm[11], imm[19:12]}.
    "jimm20": FieldRule("imm20", 20, ((31, 31, 19, 19), (30, 21, 9, 0), (20, 20, 10, 10), (19, 12, 18, 11))),
    # Shift amounts: the database splits RV64 (6-bit) from RV32 (5-bit) and
    # the backend names both operands "shamt".
    "shamtd": FieldRule("shamt", 6, ((25, 20, 5, 0),)),
    "shamtw": FieldRule("shamt", 5, ((24, 20, 4, 0),)),
    # Zicsr encodes the CSR number in the I-format immediate slot, and the
    # backend names that operand "imm12" even though the database calls it
    # "csr".
    "csr": FieldRule("imm12", 12, ((31, 20, 11, 0),)),
}

#: Fields this backend always encodes as zero (no assembly surface).
LITERAL_ZERO_FIELDS = frozenset({"nf"})


def assignments(spec: InstrSpec) -> list[BitAssignment]:
    """Return the full bit-assignment list for one instruction, MSB first."""
    result: list[BitAssignment] = []
    # A taxonomy operand may name its TableGen variable differently from the
    # database field it consumes (vsetivli's ``zimm5`` is ``$uimm``).
    renamed = {op.field: op.var_name for op in (*spec.outs, *spec.ins) if op.var_name != op.field}

    for name in spec.opcode.fields:
        rng = spec.opcode.range_of(name)
        if rng is None:
            raise ValueError(
                f"{spec.source}: field '{name}' has no bit range in arg_lut.csv"
            )
        if name in spec.fixed_fields:
            result.append(
                BitAssignment(rng.msb, rng.lsb, literal(spec.fixed_fields[name], rng.width))
            )
            continue
        if name in LITERAL_ZERO_FIELDS:
            result.append(BitAssignment(rng.msb, rng.lsb, literal(0, rng.width)))
            continue
        rule = FIELD_RULES.get(name)
        if rule is not None:
            result.extend(rule.expand())
            continue
        result.append(BitAssignment(rng.msb, rng.lsb, renamed.get(name, name)))

    carry = _carry_mask(spec)
    for token in spec.opcode.fixed:
        msb, lsb, value = parse_fixed(token)
        if carry and (msb, lsb) == (carry[1].msb, carry[1].lsb):
            # The database marks this bit as constant 0, but the taxonomy says
            # the slot carries a register (e.g. v0 as a carry input).
            result.append(BitAssignment(msb, lsb, carry[0]))
        else:
            result.append(BitAssignment(msb, lsb, literal(value, msb - lsb + 1)))

    return sorted(result, key=lambda item: (-item.msb, -item.lsb))


def _carry_mask(spec: InstrSpec):
    """(field-name, BitRange) of a carry-input mask operand, if any."""
    for operand in spec.ins:
        if not operand.is_carry_mask:
            continue
        rng = spec.opcode.range_of(operand.field) or spec.arg_lut.get(operand.field)
        if rng is not None:
            return operand.field, rng
    return None


def field_declarations(spec: InstrSpec) -> list[str]:
    """``bit vm;`` / ``bits<5> vs2;`` for every variable used by the bits.

    The declared width is the *field's* width from the opcode database, not the
    width of one slice: an S-format store uses two slices of the same 12-bit
    ``imm12`` operand and must declare it once, at 12 bits.  Synthetic operand
    names introduced by :data:`FIELD_RULES` (``bimm12``) take their width from
    the rule.
    """
    rule_widths = {rule.var: rule.width for rule in FIELD_RULES.values()}
    widths: dict[str, int] = {}
    for item in assignments(spec):
        name, width = _declared_name(item)
        if not name:
            continue
        full = spec.arg_lut.get(name)
        declared = max(width, rule_widths.get(name, 0))
        widths[name] = max(widths.get(name, 0), full.width if full else declared)
    return [f"bit {n};" if w == 1 else f"bits<{w}> {n};" for n, w in widths.items()]


def _declared_name(item: BitAssignment) -> tuple[str | None, int]:
    expr = item.expr
    if expr.isidentifier():
        return expr, item.width
    if expr.endswith("}") and "{" in expr:
        base = expr.split("{", 1)[0]
        if base.isidentifier():
            return base, item.width
    return None, 0


DEFAULT_FORMATS = (
    (("imm12hi", "imm12lo"), "InstFormatS"),
    (("bimm12hi", "bimm12lo"), "InstFormatB"),
    (("jimm20",), "InstFormatJ"),
    (("imm12",), "InstFormatI"),
    (("imm20",), "InstFormatU"),
    (("shamtd",), "InstFormatI"),
    (("shamtw",), "InstFormatI"),
    (("csr",), "InstFormatI"),
)


def instruction_format(spec: InstrSpec, overrides: dict[str, str] | None = None) -> str:
    overrides = overrides or {}
    if spec.format_override:
        return str(spec.format_override)
    if spec.mnemonic in overrides:
        return overrides[spec.mnemonic]
    fields = set(spec.opcode.fields)
    for required, name in DEFAULT_FORMATS:
        if set(required) <= fields:
            return name
    return "InstFormatR"
