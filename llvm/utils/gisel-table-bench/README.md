# gisel-table-bench

Reproducible timing of the X86 GlobalISel legalizer table against the rule
chains it replaces.

Two files:

- `gen_ir.py` writes the corpus.  Every function in it is one the X86
  GlobalISel legalizer can legalize to completion.  That restriction is
  load-bearing: `legalizeMachineFunction` returns at the *first* instruction it
  cannot legalize and drops the rest of the function, so one unsupported shape
  would quietly shorten the measured work.  `bench.py` re-checks it.
- `bench.py` runs the modes, parses the timings and reports paired statistics.

## What is measured

Two levels, because they answer different questions.

**lookup** — `-x86-gisel-table-bench=N` walks the generated check domain (the
2866 questions the rules can be asked) `N` times through both paths a
compilation can take:

- `chain_ns`: `LegalizeRuleSet::apply`, i.e. walk the rules and stop at the
  first whose predicate holds.
- `mixed_ns`: `lookup` through `LegalizerInfo::LegalizeTable` and, when it
  declines, the chain.  This is exactly what `LegalizerInfo::getAction` does,
  including the virtual call.  The call goes through a `noinline` wrapper on
  purpose: calling `lookup` on the concrete object here would let the compiler
  devirtualize and inline the classify, which would time a table that never
  exists in a compilation.

**pass** — `llc -time-passes` reports the Legalizer pass's own wall time, for
the whole pipeline and for `-run-pass=legalizer` on the translated MIR.  This
is where the lookup saving lands after it is diluted by the rest of
legalization.

Both levels run every mode once per repetition in a shuffled order and are
analysed as *paired* differences, so slow drift on a shared machine cancels.
There is no `perf` here: `-time-passes` is an in-process measurement of the
pass, and the lookup level is an in-process measurement of the lookup, which is
more direct than sampling either from outside.

## Modes

| mode    | flag                     | what the table does                 |
| ------- | ------------------------ | ----------------------------------- |
| `table` | (default)                | answers every query                 |
| `chain` | `-x86-gisel-table=false` | not installed                       |
| `once`  | `-x86-gisel-table-once`  | answers one query, then stands aside |

`once` is the negative control.  It costs one lookup plus the whole chain, so
its timing must look like `chain`.  A harness that cannot separate it from
`chain` is not resolving the table, and then a report of "no difference" would
mean nothing.

## Reproduce

```
LLC=/home/ubuntu/llvm/builds/table-classify/bin/llc

python3 llvm/utils/gisel-table-bench/bench.py \
    --llc "$LLC" --copies 32 --reps 21 --warmup 3 \
    --lookup-reps 400 --lookup-runs 7
```

`--filter REGEX` / `--exclude REGEX` build a corpus from a subset of the
templates, which is how the query-mix sweep below was produced.  `--json`
writes the raw per-repetition numbers.

`gen_ir.py --templates` lists the template names; `--split-dir DIR` writes one
file per template, which is how the corpus was validated shape by shape.

## Negative control for the harness itself

`llvm/test/CodeGen/X86/GlobalISel/table-bench.ll` asserts the shape of the two
debug outputs: the domain is answered in full in `table` mode (`hits ==
points`), exactly one query in `once` mode, and the stats line carries the rule
mix.  Each of those checks was confirmed to fail on the wrong input.

## Measured

Build: `RelWithDebInfo`, `LLVM_ENABLE_ASSERTIONS=ON` (so `applyRule`'s asserts
are live), pinned with `taskset` to one CPU on a shared 32-core machine.
Percentages are against the Legalizer pass's own wall time, so assertions
inflate the denominator; the absolute deltas are the robust part.

Corpus: `--copies 32`, 8256 functions,
`sha256 cef46beee085874d29f4aafab58c293c18f9c8056d79a68deb13a73608e6f624`.

### Lookup level, uniform check domain

| mode  | chain_ns | mixed_ns | speedup | hits |
| ----- | -------- | -------- | ------- | ---- |
| table | 98.18    | 42.76    | 2.30    | 2866 |
| once  | 98.29    | 103.74   | 0.95    | 1    |

On the domain the table wins: 56.4% less per question, and the control
separates (with the table answering once, the mixed path rises 142.6% against
table mode).

### Pass level, 21 repetitions

| pipeline              | table      | chain      | once       | table - chain            |
| --------------------- | ---------- | ---------- | ---------- | ------------------------ |
| full (`-O0`)          | 75.300 ms  | 78.600 ms  | 79.300 ms  | -3.16 ms (21/21 win)     |
| `-run-pass=legalizer` | 100.100 ms | 100.800 ms | 102.700 ms | -2.02 ms (16/21 win)     |

Total compile: 1393.4 ms against 1400.0 ms, i.e. -0.47%.

Paired 95% bootstrap CI of the mean difference, full pipeline:
[-3.52, -2.79] ms.  It excludes zero on the winning side, and 21 of 21 pairs
favour the table.  The isolated-legalizer delta is noisier (sd 2.1-2.7 ms):
16 of 21 pairs favour the table, CI [-3.52, -0.50] ms.  `once - chain` is
+0.63 ms on the full pipeline, i.e. the control costs what its one lookup
costs, as it should.

The three modes emit byte-identical assembly (same sha256), so the legalizer
does the same work either way and the difference is only in how it decides.

A second run of the same tree, same protocol, reproduced the sign and the size:
42.31 ns per lookup (2.34x), -4.45% full pipeline (21/21 pairs, CI
[-3.86, -3.15] ms), -2.32% isolated (16/21 pairs, CI [-4.19, -1.12] ms).

### Real queries, all of them

The domain check only asks the questions the key was built from.
`-x86-gisel-table-check` with the table installed also compares every answer a
compilation receives.  Over the whole corpus:

```
llc -mtriple=x86_64 -global-isel -O0 -x86-gisel-table-check -x86-gisel-table-stats corpus.ll
x86-gisel-table: hit 92352 outside 672 ... checked 92352 rule_mean 0.91 rule_max 13
```

92352 real queries, 0 mismatches against the chain.  The 672 `outside` are
queries whose keyed type is not a class the table was built for; the chain
answers those, and the check does not count them as verified answers.

### What changed: the cell answers instead of replaying the rule

The first measurement of this harness found the table *slower* than the chains
by 2.9% at pass level (full pipeline, 21/21 pairs), even though it won on the
lookup domain by 1.35x.  The reason is visible in the rule mix: real queries
stop at the first rule (`rule_mean 0.91`), so the chain costs about one
predicate, while the table paid a fixed cost -- virtual dispatch, two
classifications, the cell index -- *plus* one `ruleMatches` call, which is the
same predicate call the chain makes.  The table was doing the chain's work and
its own on top.

A cell of a table with no mem axis and whose key reads exactly slots
0..N-1 holds exactly one question, so its answer is a constant.  The generator
now says which tables those are (`OneQuestion` in `LegalizerTableDesc`; 56 of
X86's 63), the runtime stores the answer at fill time, and the lookup returns
it without matching anything.  The other seven keep replaying the winning rule
from the query, which is still correct, just slower.

Re-measured with the same protocol, same corpus, same build:

| level                        | before      | after        |
| ---------------------------- | ----------- | ------------ |
| lookup, per query            | 73.07 ns    | 42.76 ns     |
| full pipeline legalizer      | +2.93%      | -4.20%       |
| isolated legalizer           | +3.63%      | -0.69%       |
| total compile                | +0.34%      | -0.47%       |

The 5.4 ms the full pipeline gains is consistent with 92352 table hits times
the 56 ns the lookup saves: 5.2 ms.

### Negative controls

- `-x86-gisel-table-once`: the harness still separates it from `chain` and
  from `table` at both levels (see the tables above).  Without that, a "no
  difference" result would be unreadable.
- `llvm/test/CodeGen/X86/GlobalISel/table-check.ll` runs real functions
  through the pipeline with the table installed and asserts
  `checked > 0`, i.e. that the check actually compared real queries.  Forcing
  `checked` to 0 (by checking a function with no instructions) fails the test.
- Corrupting the cached answer (returning `Legal` for every hit) makes the
  check fail on `add i128`, which is the shape the gate used to lack: with only
  "legal" questions in the test, both paths say `Legal` and the check cannot
  see a stale answer.
- Forcing `OneQuestion` on for all 63 tables does *not* fail, and that is
  expected: the seven excluded tables are structurally not one-question (mem
  axis, or a key that reads slot 1 only), but on the questions X86 can be
  asked here their answers are consistent anyway.  The predicate is
  conservative, not tight.

## Caveats

- Absolute nanoseconds are from a shared, loaded machine (`load1` ranged from
  0.4 to 10.5 during these runs).  The paired differences and the sign are
  stable across runs; the percentages are not to three significant figures.
- Assertions are on.  `applyRule`'s `assert(Rule.match(Query))` calls the
  predicate a second time in both paths, so it cancels in the difference but
  inflates the legalizer total and shrinks the percentages.  A release build
  would move the percentages, not the sign.
- The corpus is synthetic.  It is deliberately broad (scalar at every width,
  vectors, floats, memory of every size) rather than shaped like one
  application, and `--filter` exists so a different mix can be tested.
- `-x86-gisel-table-bench` runs inside the `LegalizerInfo` constructor, so it
  prints once per constructed `LegalizerInfo`, and it must not be combined with
  `-x86-gisel-table-once` in a pass-level measurement (it changes what the
  table answers; the lookup level uses that on purpose).
