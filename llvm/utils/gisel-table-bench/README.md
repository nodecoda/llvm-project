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
| table | 97.94    | 73.07    | 1.346   | 2866 |
| once  | 97.91    | 101.31   | 0.964   | 1    |

On the domain the table wins: 26.5% less per question.  The domain's mean
winning rule index is 3.05.  The control separates: with the table answering
once, the mixed path rises 38.4%.

### Pass level, 21 repetitions

| pipeline           | table      | chain      | once       | table - chain       |
| ------------------ | ---------- | ---------- | ---------- | ------------------- |
| full (`-O0`)       | 80.700 ms  | 78.400 ms  | 78.900 ms  | +2.30 ms (0/21 win) |
| `-run-pass=legalizer` | 105.500 ms | 101.800 ms | 102.200 ms | +3.65 ms (0/21 win) |

Total compile: 1403.3 ms against 1398.6 ms, i.e. +0.34%.

Paired 95% bootstrap CI of the mean difference, full pipeline:
[+1.96, +2.75] ms.  It excludes zero, and 21 of 21 pairs favour the chain.
`table - once` is [+1.39, +2.09] ms; `once - chain` is small, as the control
predicts.  The table's contribution is resolved, and it is a cost.

The three modes emit byte-identical assembly (same sha256), so the legalizer
does the same work either way and the difference is only in how it decides.

### Why the two levels disagree

The check domain is uniform over the type classes the rules name.  A
compilation is not.  The table counts the winning rule index on the queries
actually asked (`-x86-gisel-table-stats`):

| corpus                   | winning rule mean | legalizer, table - chain |
| ------------------------ | ----------------- | ------------------------ |
| scalar + memory only     | 0.30              | +5.7% (isolated)         |
| whole corpus             | 0.91              | +3.6% (isolated)         |
| wide/odd/vector only     | 1.29              | +1.7% (isolated)         |

Real queries stop at the first rule.  `G_ADD`'s rule 0 is
`LegalFor<[s8, s16, s32]>` and rule 1 is the 64-bit case, so a scalar add costs
the chain about one predicate.  The table pays a fixed cost -- virtual
dispatch, two type classifications, the cell index, and one `ruleMatches`
call -- and saves one predicate per skipped rule.  On this build that fixed
cost is about 50 ns against roughly 25 ns per rule, so the crossing is near two
rules.  The uniform domain (3.05) is past it; every mix a compilation produces
(0.3 to 1.3) is well short of it.

That is the result: on X86, with the chains ordered the way they are, the table
is slower than the rules, by about 19 to 43 ns per query, or 1.7% to 8.8% of
the legalizer's own time.

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
