// RUN: not llvm-tblgen -gen-legalizer-table -I %p/../../include %s 2>&1 | FileCheck %s --check-prefix=STRICT
// RUN: llvm-tblgen -gen-legalizer-table -gen-legalizer-allow-gaps -I %p/../../include %s | FileCheck %s --check-prefix=GAPS

// A row of a table's own domain that the evaluator cannot compute is a cell
// that falls back to the chain at run time, and nothing in the build says so:
// the .inc still names the rule set, so the table looks complete.  The
// generator reports it instead of writing the comment and moving on.
//
// The gap here is one theta assignment: with HasAVX the first rule answers, and
// without it the opaque leaf has no build-time meaning.  Both ways out are in
// the .td -- teach the Core IR the leaf, or make the rule opaque so the whole
// rule set stays out of the table -- so the error names the rule set, the row
// count, and the reason.

include "llvm/Target/LegalizerInfo.td"

def HasAVX : LegalizeFeature<"Subtarget.hasAVX()">;

def s32 : LegalizeNamedType<"LLT::scalar(32)">;

def Test_GAP : LegalizeRuleSet<["G_ADD"]> {
  let Rules = [
    LegalForIf<HasAVX, [s32]>,
    ScalarizeIf<LegalizeQueryPredicate<"scalarOrEltWiderThan(0, 64)">, 0>,
  ];
}

// STRICT: error: rule set Test_GAP cannot answer every row of its domain:
// STRICT: row(s): predicate leaf 'opaque' has no build-time meaning
// STRICT: teach legeval::evalLeaf the leaf

// GAPS: // no frozen answer for 1 row(s) of this table: predicate leaf 'opaque' has no build-time meaning
// GAPS: // Test_GAP