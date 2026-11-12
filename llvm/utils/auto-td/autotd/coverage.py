"""Coverage report: which instructions exist, and how they were produced."""

from __future__ import annotations

from collections import Counter

from .model import InstrSpec


def render(specs: list[InstrSpec], title: str = "auto-td coverage") -> str:
    counts = Counter(spec.status for spec in specs)
    lines = [f"# {title}", ""]
    for status in ("auto_full", "auto_with_structured_override", "retained_schema_gap"):
        lines.append(f"- {status}: {counts.get(status, 0)}")
    lines += ["", "| instruction | domain | status | opcode source | spec | yaml |", "|---|---|---|---|---|---|"]
    for spec in sorted(specs, key=lambda s: s.mnemonic):
        lines.append(
            f"| {spec.mnemonic} | {spec.domain} | {spec.status} | {spec.ref} | "
            f"{spec.spec_ref} | {spec.source.as_posix()} |"
        )
    return "\n".join(lines) + "\n"
