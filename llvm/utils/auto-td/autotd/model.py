"""Data model for auto-td.

An instruction is fully described by three things:

* where its encoding lives        -> :class:`OpcodeRef` + :class:`OpcodeRow`
* which taxonomy category gives
  its operands and effects        -> :class:`Operand` / :class:`Effects`
* the few facts no database holds -> :class:`InstrSpec`

Nothing in this module knows about any particular ISA.
"""

from __future__ import annotations

from dataclasses import dataclass, field
from pathlib import Path

DEFAULT_PREFIX = "AUTO"


def record_name_for(mnemonic: str, prefix: str = DEFAULT_PREFIX) -> str:
    """`vadd.vv` -> `<PREFIX>_VADD_VV`; an empty prefix yields `VADD_VV`."""
    stem = mnemonic.replace(".", "_").replace("-", "_").upper()
    return f"{prefix}_{stem}" if prefix else stem


@dataclass(frozen=True)
class OpcodeRef:
    """Pointer into an opcode database."""

    repo: str
    extension: str
    key: str

    def __str__(self) -> str:  # used in comments and diagnostics
        return f"{self.repo}/{self.extension}/{self.key}"


@dataclass(frozen=True)
class BitRange:
    """Bit range of one encoding field, as declared by the opcode database."""

    name: str
    msb: int
    lsb: int

    @property
    def width(self) -> int:
        return self.msb - self.lsb + 1


@dataclass(frozen=True)
class OpcodeRow:
    """One line of an opcode source file, already split into its parts."""

    mnemonic: str
    fields: tuple[str, ...]
    fixed: tuple[str, ...]
    ranges: dict[str, BitRange]
    origin: Path

    def range_of(self, name: str) -> BitRange | None:
        return self.ranges.get(name)


@dataclass(frozen=True)
class Operand:
    """One operand slot, as declared by a taxonomy category."""

    role: str
    field: str
    reg_class: str | None = None
    operand_class: str | None = None
    is_mask: bool = False
    is_carry_mask: bool = False

    @property
    def td_class(self) -> str:
        """TableGen operand class used in the outs/ins DAG."""
        if self.operand_class:
            return self.operand_class
        if self.reg_class:
            return self.reg_class
        raise ValueError(f"operand '{self.role}' has neither class nor reg_class")


@dataclass(frozen=True)
class Effects:
    may_load: bool = False
    may_store: bool = False
    side_effects: bool = False
    implicit_uses: tuple[str, ...] = ()
    implicit_defs: tuple[str, ...] = ()
    #: extra TableGen instruction properties (``let <flag> = 1;``)
    flags: tuple[str, ...] = ()


@dataclass
class InstrSpec:
    """Everything needed to emit one instruction record."""

    mnemonic: str
    domain: str
    predicate: str
    opcode: OpcodeRow
    ref: OpcodeRef
    spec_ref: str
    source: Path
    status: str = "auto_full"
    outs: tuple[Operand, ...] = ()
    ins: tuple[Operand, ...] = ()
    effects: Effects = field(default_factory=Effects)
    aliases: tuple[str, ...] = ()
    pseudos: dict = field(default_factory=dict)
    patterns: tuple[dict, ...] = ()
    builtin: dict | None = None
    format_override: str | None = None
    retained_owner_files: tuple[str, ...] = ()
    #: database field -> constant the ISA always encodes there
    fixed_fields: dict[str, int] = field(default_factory=dict)
    #: scheduling resources (``let SchedRW = [...]``); ``None`` means the YAML
    #: did not say, ``()`` means "declare an empty list on purpose"
    sched: tuple[str, ...] | None = None
    #: True when the YAML or its domain explicitly declared a predicate, even
    #: an empty one (``predicate: null``)
    predicate_declared: bool = True
    #: an instruction with no operands by design (ecall/ebreak)
    nullary: bool = False
    #: field-name -> bit range for the repo this instruction came from
    arg_lut: dict[str, "BitRange"] = field(default_factory=dict)

    def record_name(self, prefix: str = DEFAULT_PREFIX) -> str:
        return record_name_for(self.mnemonic, prefix)

    @property
    def mask_operand(self) -> Operand | None:
        return next((op for op in self.ins if op.is_mask), None)
