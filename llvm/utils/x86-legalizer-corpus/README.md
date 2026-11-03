# x86-legalizer-corpus

Build legalizer query corpora from the **hand-written** X86 legalizer C++
(`llvm/lib/Target/X86/GISel/X86LegalizerInfo.cpp`), then let the C++ reference
fill in the expected answers.

This tool is deliberately not a TableGen reader.  It parses the source file
that a human wrote, so the corpus describes the chain semantics that the
generated table is supposed to replace.  Generated `.inc` files are read by
`verify` only, as a cross-check.

## Oracle boundary

The Python side never writes `expected`.  It emits query shapes only:

```
C++ source --parse--> rule ledger --points--> query shapes
                                                  |
                              X86Tests (reference) fills the answers
                                                  |
                                            corpus JSON
```

The only producer of an expected action is
`X86LegalizerInfoReference::getAction()` in the C++ unit test driver.
`points` output is therefore safe to diff and review: it contains no answers
that the table could have influenced.

## Quick start

Run from a checkout that has the frozen reference revision (see
`llvm/unittests/Target/X86/x86-legalizer-reference-provenance.json`):

```bash
# 1. Parse the hand-written chain into a rule ledger.
python3 llvm/utils/x86-legalizer-corpus parse \
    --source-rev ebac17721d12 --config C2 --out /tmp/x86-ast.json

# 2. Turn the ledger into query shapes (witness tier by default).
python3 llvm/utils/x86-legalizer-corpus points \
    --source-rev ebac17721d12 --tier witness --out /tmp/x86-points.json

# 3. Cross-check the hand-written chain against the TableGen emission.
python3 llvm/utils/x86-legalizer-corpus verify \
    --ast /tmp/x86-ast.json \
    --td-inc build/unittests/Target/X86/X86LegalizerRuleIndex.inc \
    --out /tmp/x86-report.json

# 4. Fill the expected answers from the C++ reference.
python3 llvm/utils/x86-legalizer-corpus corpus \
    --source-rev ebac17721d12 --tier witness \
    --build /path/to/build --out /tmp/x86-corpus.json
```

`corpus` without `--build` writes only the points file (`<out>.points.json`)
and tells you what to do next.  Environment overrides for step 4 are
`X86_LEGALIZER_CPP_POINTS` and `X86_LEGALIZER_CPP_CORPUS_OUT`; they select the
input points and the output corpus for the gtest driver.

## Tiers

| Tier | What it adds | Points on the frozen revision |
|---|---|---|
| `witness` | reviewed 12-point boundary panel, address-space probes, per-rule witnesses | 1246 |
| `full` | every class pair over the C++ class axis, plus the witness panel | 197896 |
| `deep` | `full` plus the reviewed 60-point deep panel | 288280 |

Tiers only change the query shapes.  They never change the expected answers,
which always come from the reference.

`full` and `deep` prune class-pair and boundary shapes that put a pointer (or
pointer vector) in a slot targeted by an element-size mutation
(`clampScalar`, `scalarSameSizeAs`, `widenScalarOrEltToNextPow2`, ...).  The
reference applies the first match's mutation, and `LLT::changeElementSize`
rejects pointers with an assertion, so those are not well-formed legalizer
queries.  The points document records the count under
`"pruned": {"pointer_element_size": N}` (10884 for `full`, 17508 for `deep`).
This is a query-shape safety filter, not an oracle: it never writes an
expected action.

## Configs

`--config` selects a row of the `CONFIGS` table in `target.py`; the rows mirror
the four `FullConfigs` in `X86LegalizerInfoDifferentialTest.cpp`.

| Config | Shape |
|---|---|
| `C0` | 32-bit, no SSE/AVX, no x87 |
| `C1` | 64-bit, SSE2 only |
| `C2` | 64-bit, AVX-512 + BWI/DQI/VLX (witness config; every guard is taken) |
| `C3` | 32-bit with POPCNT/LZCNT/BMI, no SSE |

`verify` currently accepts only a `C2` AST.  The emitted rule index is the
unconditional TableGen translation, while `C0`/`C1`/`C3` prune guarded rules;
comparing another config would report every pruned rule as a delta.

## What the reader understands

The parser is a strict subset of C++, not a general interpreter:

- declarations of `LLT` aliases and integer constants;
- nested ternaries, arithmetic, comparisons and `Subtarget.<predicate>()`;
- `if`/`else` with literal pruning, including `auto &Action = ...` chains;
- `for (unsigned Op : {G_...})` expansion;
- the builder chain methods listed in `KNOWN_METHODS`;
- lambdas are recorded as opaque rules (never executed).

Anything else is a hard error with a line number.  `--allow-opaque` downgrades
an unknown chain method to a recorded warning; use it only when you are
inspecting a new source revision, never for a committed corpus.

## Extending the tool

- **New chain method**: add it to `KNOWN_METHODS`, teach `record_shape()` which
  slots it reads, and add a case to `domain.probe_for_rule()`.  The point of the
  ledger is that a silent no-op is impossible.
- **New `Subtarget` predicate**: add it to `target.FEATURE_KEYS` and to every
  `CONFIGS` row.  The parser fails if a guard mentions a predicate that the
  feature environment does not define.
- **New LLT spelling**: extend `expr.llt_name()` / `llt_bits()`; the corpus
  format uses those strings, so a rename is a corpus-format change.
- **New boundary panel**: change `target.BOUNDARY_PANEL` or `DEEP_PANEL`.
  These are review constants, intentionally not derived from the source.

## Deltas and verification

`verify` compares the C++ rule ledger with the TableGen-emitted rule index and
classifies every difference:

| Class | Meaning |
|---|---|
| `match` | same rule count |
| `td_expands_lambda` | TableGen expanded one predicate over several classes |
| `td_only` | TableGen has rules the C++ chain did not record |
| `input_missing` | the C++ chain has more rules than TableGen |
| `no_cpp_builder` | a TableGen row has no builder in the source |

Every non-`match` class must be registered in `known_deltas.json` as a
`(rule_set, class)` pair with a reason.  The default registry is read
automatically; pass `--known-deltas` to point at another file.

Exit codes:

| Code | Meaning |
|---|---|
| 0 | every delta is registered |
| 2 | the source has an unknown chain method (use `--allow-opaque` to inspect) |
| 3 | at least one delta is not registered |

Running `verify` against `upstream/main` is a useful negative test: the main
branch has two fewer `G_CTPOP` rules, so the run exits 3 with
`X86_CTPOP:td_only`.  That is the tool detecting a real input difference, not
a corpus bug.

## Determinism and tests

Same input, same bytes: every collection is sorted by a stable key before JSON
emission, and points are serialized with `sort_keys=True`.

```bash
python3 -m unittest discover -s llvm/utils/x86-legalizer-corpus/tests -v
```

The suite covers the lexer, the expression evaluator, branch/loop expansion,
the frozen-reference census, point determinism, the oracle boundary, and the
delta registry.  Tests that need the frozen revision or a built
`X86LegalizerRuleIndex.inc` skip when those artifacts are absent.

Frozen-revision census (a change here means the reader or the source moved):

```
call_sites=58  builders=62  opcodes=115  classes=24  mem_classes=23  aligns=1
chain_rules=277  tablegen_rows=63
```
