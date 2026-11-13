"""Taxonomy: shared operand/effect categories referenced by instructions.

Splitting operands out of the per-instruction file is what keeps 50 YAMLs from
duplicating the same three operand lists.  A category is looked up by the last
segment of an instruction's ``spec`` coordinate::

    tinyv.vector-alu.int_add  ->  category "int_add"
"""

from __future__ import annotations

from pathlib import Path

import yaml

from .model import Effects, Operand

# Operand classes that consume a mask slot rather than a value slot.
MASK_CLASSES = {"VMaskOp"}
CARRY_MASK_CLASSES = {"VMaskCarryInOp", "VMaskCarryIn"}


def _read_yaml(path: Path) -> dict:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a YAML mapping")
    return data


def load_categories(root: Path) -> dict[str, dict]:
    """Read every ``taxonomy/*.yaml`` into category-name -> raw mapping."""
    categories: dict[str, dict] = {}
    if not root.is_dir():
        return categories
    for path in sorted(root.glob("*.yaml")):
        for name, body in (_read_yaml(path).get("categories") or {}).items():
            if not isinstance(body, dict):
                raise ValueError(f"{path}: category '{name}' must be a mapping")
            categories[name] = body
    return categories


def category_key(spec_ref: str) -> str:
    return spec_ref.rsplit(".", 1)[-1]


def _operand(raw: dict) -> Operand:
    operand_class = raw.get("operand")
    return Operand(
        role=str(raw.get("role", "")),
        field=str(raw.get("field", "")),
        reg_class=raw.get("reg_class"),
        operand_class=operand_class,
        is_mask=operand_class in MASK_CLASSES,
        is_carry_mask=operand_class in CARRY_MASK_CLASSES,
    )


def operands_of(category: dict, direction: str) -> tuple[Operand, ...]:
    spec = ((category.get("operands") or {}).get(direction)) or []
    return tuple(_operand(item) for item in spec)


def _lets(raw: dict) -> tuple[tuple[str, str], ...]:
    lets = raw.get("lets") or {}
    if not isinstance(lets, dict):
        raise ValueError("'lets' must be a mapping of TableGen field -> expression")
    return tuple((str(key), str(value)) for key, value in lets.items())


def effects_of(category: dict) -> Effects:
    raw = category.get("effects") or {}
    return Effects(
        may_load=bool(raw.get("may_load", False)),
        may_store=bool(raw.get("may_store", False)),
        side_effects=bool(raw.get("has_side_effects", False)),
        implicit_uses=tuple(raw.get("implicit_uses") or ()),
        implicit_defs=tuple(raw.get("implicit_defs") or ()),
        flags=tuple(str(flag) for flag in (raw.get("flags") or ())),
        lets=_lets(raw),
    )
