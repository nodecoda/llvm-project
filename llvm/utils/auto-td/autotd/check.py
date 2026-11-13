"""Validation rules.

Two kinds of mistakes are caught here:

* things that are *wrong* (missing opcode, mnemonic mismatch, duplicate keys)
* things that would let a contributor smuggle TableGen back into the YAML

All problems are collected and reported together, so one run fixes them all.
"""

from __future__ import annotations

from collections import defaultdict

from .encoding import FIELD_RULES, LITERAL_ZERO_FIELDS
from .model import InstrSpec
from .taxonomy import category_key

ALLOWED_STATUS = {"auto_full", "auto_with_structured_override", "retained_schema_gap"}
FORBIDDEN_TOKENS = ("raw_td", "def : Pat", "bits<", "let Inst{", "multiclass")
FORBIDDEN_KEYS = {"encoding", "fixed_bits", "raw_td", "raw_cpp"}
CODEGEN_OPS = {"add", "sub", "mul", "ireduce_sum", "freduce_sum", "gather", "slideup", "fexp"}

#: keys the loader actually consumes; anything else is dead weight and is
#: reported so a typo cannot silently disable a declaration.
ALLOWED_KEYS = {
    "mnemonic", "opcode", "opcode_source", "spec", "spec_ref", "domain", "predicate",
    "status", "aliases", "pseudos", "patterns", "builtin", "format",
    "retained_owner_files", "features", "reason", "missing_schema", "fixed", "flags", "lets", "sched", "nullary",
}


def validate(specs: list[InstrSpec], prefix: str = "AUTO", strict: bool = False) -> list[str]:
    problems: list[str] = []
    problems += _check_each(specs)
    problems += _check_uniqueness(specs, prefix)
    unknown = unknown_keys(specs)
    if strict:
        problems += unknown
    return problems


def unknown_keys(specs: list[InstrSpec]) -> list[str]:
    """Top-level YAML keys the loader never reads (typo or dead declaration)."""
    import yaml

    warnings: list[str] = []
    for spec in specs:
        data = yaml.safe_load(spec.source.read_text(encoding="utf-8")) or {}
        extra = sorted(set(data) - ALLOWED_KEYS)
        if extra:
            warnings.append(f"{spec.source}: unused key(s) {extra} (loader ignores them)")
    return warnings


def _check_each(specs: list[InstrSpec]) -> list[str]:
    problems: list[str] = []
    for spec in specs:
        where = spec.source
        text = spec.source.read_text(encoding="utf-8")

        for token in FORBIDDEN_TOKENS:
            if token in text:
                problems.append(f"{where}: forbidden token {token!r} (YAML must not carry TableGen)")
        for key in FORBIDDEN_KEYS:
            if f"{key}:" in text:
                problems.append(f"{where}: forbidden key {key!r} (encoding comes from the opcode database)")

        if not spec.predicate and not spec.predicate_declared:
            problems.append(
                f"{where}: missing 'predicate' (set it in YAML or config.yaml domains; "
                "use 'predicate: null' for none)"
            )
        if spec.status not in ALLOWED_STATUS:
            problems.append(f"{where}: status {spec.status!r} not in {sorted(ALLOWED_STATUS)}")
        if spec.status == "retained_schema_gap" and not spec.retained_owner_files:
            problems.append(f"{where}: retained_schema_gap requires retained_owner_files")
        # RISC-V spells the aq/rl variants of an atomic as the base mnemonic
        # plus a suffix while they share one database row; that is not a typo.
        suffixed = spec.mnemonic.startswith(spec.opcode.mnemonic + ".")
        if spec.mnemonic != spec.opcode.mnemonic and not suffixed and not spec.aliases:
            problems.append(
                f"{where}: mnemonic {spec.mnemonic!r} != opcode {spec.opcode.mnemonic!r} "
                "and no aliases are declared"
            )
        if not spec.outs and not spec.ins and not spec.nullary:
            problems.append(
                f"{where}: taxonomy category '{category_key(spec.spec_ref)}' declares no operands"
            )
        problems += _check_builtin(spec)
        problems += _check_fields(spec)
    return problems


def _check_fields(spec: InstrSpec) -> list[str]:
    """Every database field must be explained, and every operand must exist."""
    problems: list[str] = []
    where = spec.source
    db_fields = set(spec.opcode.fields)
    operand_fields = {op.field for op in (*spec.outs, *spec.ins)}
    synthetic = {rule.var for rule in FIELD_RULES.values()}

    for op in (*spec.outs, *spec.ins):
        if op.field in db_fields or op.field in synthetic or op.is_carry_mask:
            continue
        problems.append(f"{where}: operand field '{op.field}' is not in the opcode row")

    covered = operand_fields | set(spec.fixed_fields) | set(FIELD_RULES) | LITERAL_ZERO_FIELDS
    for name in spec.opcode.fields:
        if name not in covered:
            problems.append(f"{where}: opcode field '{name}' is neither an operand nor fixed")

    for name, value in spec.fixed_fields.items():
        rng = spec.opcode.range_of(name)
        if rng is None:
            problems.append(f"{where}: fixed field '{name}' is not in the opcode row")
            continue
        if not 0 <= value < (1 << rng.width):
            problems.append(f"{where}: fixed value {value} does not fit '{name}' ({rng.width} bits)")
        if name in operand_fields:
            problems.append(f"{where}: field '{name}' is both an operand and fixed")
    return problems


def _check_builtin(spec: InstrSpec) -> list[str]:
    builtin = spec.builtin
    if not builtin or not builtin.get("codegen"):
        return []
    problems: list[str] = []
    where = spec.source

    pattern = next((p for p in spec.patterns if p.get("kind") == "intrinsic_to_pseudo"), None)
    if pattern is None:
        problems.append(f"{where}: builtin.codegen requires an intrinsic_to_pseudo pattern")
        return problems
    if pattern.get("operation") not in CODEGEN_OPS:
        problems.append(f"{where}: unsupported builtin operation {pattern.get('operation')!r}")
    intrinsic = pattern.get("intrinsic")
    if not isinstance(intrinsic, str) or not intrinsic.startswith("ysx."):
        problems.append(f"{where}: builtin intrinsic must use the 'ysx.' prefix")
    for name in builtin.get("names") or []:
        if not str(name).startswith("ysx_"):
            problems.append(f"{where}: builtin name {name!r} must start with 'ysx_'")
    return problems


def _check_uniqueness(specs: list[InstrSpec], prefix: str) -> list[str]:
    problems: list[str] = []
    by_opcode: dict[str, list[InstrSpec]] = defaultdict(list)
    by_record: dict[str, list[InstrSpec]] = defaultdict(list)
    for spec in specs:
        by_opcode[str(spec.ref)].append(spec)
        by_record[spec.record_name(prefix)].append(spec)

    for ref, group in sorted(by_opcode.items()):
        if len(group) < 2:
            continue
        # Atomic aq/rl variants share one database row and differ only in the
        # constant aq/rl bits.  Sharing is fine as long as every claimant
        # pins a different set of bits; anything else is a duplicate.
        base = group[0].opcode.mnemonic
        variants = all(
            s.mnemonic == base or s.mnemonic.startswith(base + ".") for s in group
        )
        distinct = len({tuple(sorted(s.fixed_fields.items())) for s in group}) == len(group)
        if not (variants and distinct):
            files = ", ".join(str(s.source.name) for s in group)
            problems.append(f"opcode {ref} is claimed by {len(group)} instructions ({files})")
    for record, group in sorted(by_record.items()):
        if len(group) > 1:
            problems.append(f"record name collision: {record} from {[s.mnemonic for s in group]}")
    return problems
