"""Assemble :class:`InstrSpec` objects from YAML + opcode database + taxonomy."""

from __future__ import annotations

from dataclasses import replace
from pathlib import Path

import yaml

from .model import InstrSpec, OpcodeRef
from .opcodes import OpcodeDatabase
from .taxonomy import category_key, effects_of, load_categories, operands_of

REQUIRED_KEYS = ("mnemonic",)


def _read_yaml(path: Path) -> dict:
    data = yaml.safe_load(path.read_text(encoding="utf-8"))
    if not isinstance(data, dict):
        raise ValueError(f"{path}: expected a YAML mapping")
    return data


def _opcode_ref(path: Path, data: dict) -> OpcodeRef:
    raw = data.get("opcode") or data.get("opcode_source")
    if not isinstance(raw, dict):
        raise ValueError(f"{path}: missing 'opcode' mapping (repo/extension/key)")
    try:
        return OpcodeRef(str(raw["repo"]), str(raw["extension"]), str(raw["key"]))
    except KeyError as exc:
        raise ValueError(f"{path}: opcode mapping lacks {exc.args[0]!r}") from None


def _string(path: Path, data: dict, key: str, *aliases: str) -> str:
    for name in (key, *aliases):
        value = data.get(name)
        if isinstance(value, str) and value:
            return value
    raise ValueError(f"{path}: missing required string field '{key}'")


def load_config(target_root: Path) -> dict:
    """Optional ``auto-td/config.yaml``: domain predicates, base class, formats."""
    path = target_root / "auto-td" / "config.yaml"
    return _read_yaml(path) if path.is_file() else {}


def load_instructions(target_root: Path, db: OpcodeDatabase, config: dict | None = None) -> list[InstrSpec]:
    config = config if config is not None else load_config(target_root)
    categories = load_categories(target_root / "auto-td" / "taxonomy")
    domains = config.get("domains") or {}
    specs: list[InstrSpec] = []

    for path in sorted((target_root / "auto-td" / "instructions").glob("*/*.yaml")):
        data = _read_yaml(path)
        ref = _opcode_ref(path, data)
        row = db.lookup(ref)
        if row is None:
            raise ValueError(f"{path}: opcode source {ref} not found in database")

        spec_ref = _string(path, data, "spec", "spec_ref")
        domain = str(data.get("domain") or path.parent.name)
        category = categories.get(category_key(spec_ref), {})
        domain_conf = domains.get(domain) or {}
        # ``predicate: null`` is a deliberate "no predicate"; a missing key is
        # an omission and is reported by the validator.
        if "predicate" in data:
            predicate_declared = True
            raw_predicate = data["predicate"]
        elif "predicate" in domain_conf:
            predicate_declared = True
            raw_predicate = domain_conf["predicate"]
        else:
            predicate_declared = False
            raw_predicate = None
        if raw_predicate is None:
            predicate = ""
        elif isinstance(raw_predicate, list):
            predicate = ", ".join(str(item) for item in raw_predicate)
        else:
            predicate = str(raw_predicate)

        fixed = data.get("fixed") or {}
        if not isinstance(fixed, dict):
            raise ValueError(f"{path}: 'fixed' must be a mapping of field -> constant")

        effects = effects_of(category)
        if data.get("flags"):
            effects = replace(effects, flags=tuple(str(f) for f in data["flags"]))
        if data.get("lets"):
            if not isinstance(data["lets"], dict):
                raise ValueError(f"{path}: 'lets' must be a mapping of TableGen field -> expression")
            merged = dict(effects.lets)
            merged.update({str(k): str(v) for k, v in data["lets"].items()})
            effects = replace(effects, lets=tuple(merged.items()))

        specs.append(
            InstrSpec(
                mnemonic=_string(path, data, "mnemonic"),
                domain=domain,
                predicate=predicate,
                predicate_declared=predicate_declared,
                opcode=row,
                ref=ref,
                spec_ref=spec_ref,
                source=path,
                status=str(data.get("status", "auto_full")),
                outs=operands_of(category, "outs"),
                ins=operands_of(category, "ins"),
                effects=effects,
                aliases=tuple(a["mnemonic"] for a in (data.get("aliases") or []) if isinstance(a, dict) and a.get("mnemonic")),
                pseudos=dict(data.get("pseudos") or {}),
                patterns=tuple(data.get("patterns") or ()),
                builtin=dict(data["builtin"]) if isinstance(data.get("builtin"), dict) else None,
                format_override=data.get("format"),
                retained_owner_files=tuple(data.get("retained_owner_files") or ()),
                fixed_fields={str(k): int(v) for k, v in fixed.items()},
                sched=tuple(str(s) for s in data["sched"]) if "sched" in data else None,
                nullary=bool(data.get("nullary", False)),
                arg_lut=db.lut_for(ref.repo),
            )
        )
    return specs
