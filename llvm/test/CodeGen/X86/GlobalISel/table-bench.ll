; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -x86-gisel-table-bench=1 -o /dev/null 2>&1 \
; RUN:     | FileCheck %s --check-prefix=BENCH
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -x86-gisel-table-bench=1 -x86-gisel-table-once -o /dev/null 2>&1 \
; RUN:     | FileCheck %s --check-prefix=ONCE
; RUN: llc < %s -mtriple=x86_64-unknown-linux-gnu -global-isel -O0 \
; RUN:     -x86-gisel-table-stats -x86-gisel-table-once -o /dev/null 2>&1 \
; RUN:     | FileCheck %s --check-prefix=STATS

; The benchmark times the two paths getAction() can take over the check
; domain.  What is asserted here is only the shape: that the domain is
; answered in full, that repetition count is honoured, and that every field the
; harness parses is present.  The nanoseconds are the point of the tool but not
; the point of the test, so they are matched as numbers, not values.
;
; BENCH: x86-gisel-table-bench: points [[P:[0-9]+]] hits [[P]] reps 1
; BENCH-SAME: chain_ns {{[0-9.]+}} mixed_ns {{[0-9.]+}} speedup {{[0-9.]+}}
; BENCH-SAME: mean_rule {{[0-9.]+}} max_rule {{[0-9]+}} sink {{[0-9]+}}

; -x86-gisel-table-once is the negative control: it must answer exactly one
; query and leave the rest to the chains.  If this regresses to answering all
; of them, the control stops being a control and every "no difference" the
; harness reports becomes unreadable.
;
; ONCE: x86-gisel-table-bench: points [[P:[0-9]+]] hits 1 reps 1

; The stats line carries the query mix a compilation actually produced, which
; is what connects the lookup numbers (uniform domain) to the pass numbers.
;
; STATS: x86-gisel-table: hit 1 {{.*}} rule_mean {{[0-9.]+}} rule_max {{[0-9]+}}

define i32 @f(i32 %a, i32 %b) {
  %r = add i32 %a, %b
  ret i32 %r
}
