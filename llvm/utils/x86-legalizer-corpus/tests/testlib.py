"""Shared helpers for the corpus-reader unit tests."""

import json
import os
import subprocess
import sys

TOOL_DIR = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
if TOOL_DIR not in sys.path:
    sys.path.insert(0, TOOL_DIR)

import parse


def repo_root():
    proc = subprocess.run(
        ["git", "-C", TOOL_DIR, "rev-parse", "--show-toplevel"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        return None
    return proc.stdout.strip()


def reference_revision():
    """The commit the frozen C++ reference was captured from."""
    root = repo_root()
    if root is None:
        return None
    path = os.path.join(root, "llvm", "unittests", "Target", "X86",
                        "x86-legalizer-reference-provenance.json")
    try:
        with open(path, "r", encoding="utf-8") as handle:
            return json.load(handle)["source_commit"]
    except (OSError, KeyError, ValueError):
        return None


def reference_source():
    """Return (source_text, source_info) for the frozen reference revision.

    Returns None when the revision is not available in this checkout, so the
    census test can skip instead of failing on a shallow clone.
    """
    root = repo_root()
    revision = reference_revision()
    if root is None or revision is None:
        return None
    source_path = "llvm/lib/Target/X86/GISel/X86LegalizerInfo.cpp"
    proc = subprocess.run(
        ["git", "-C", root, "show", f"{revision}:{source_path}"],
        stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    if proc.returncode != 0:
        return None
    text = proc.stdout
    return text, {
        "rev": revision,
        "path": source_path,
        "sha256": "test",
        "lines": text.count("\n") + 1,
    }


def parse_text(text, config="C2"):
    from target import CONFIGS
    source = {"rev": None, "path": "<test>", "sha256": "test",
              "lines": text.count("\n") + 1}
    return parse.parse_source(text, source, config, CONFIGS[config])
