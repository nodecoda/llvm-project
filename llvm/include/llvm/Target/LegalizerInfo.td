//===- LegalizerInfo.td - GlobalISel legalizer rules ------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// Schema for describing GlobalISel legalizer rules in TableGen.
//
// This is the first increment: it covers the builders used by the X86
// G_CTPOP rule set.  The schema grows with the rules that need it; it does
// not try to describe the whole LegalizerInfo API up front.
//
// Design constraints:
//
//  * One record maps one-to-one onto one existing builder call in
//    LegalizerInfo.h.  The generated fragment is a statement sequence, so a
//    target can convert one rule set at a time and keep the rest hand-written.
//  * The mutation direction is an explicit field, so -gen-legalizer can
//    reject an action/mutation mismatch before any C++ exists.  In the
//    hand-written API these live in two different places and nothing ties
//    them together (PR220802 is exactly that bug class).
//  * Predicates are a closed record tree.  A leaf names a bool the target
//    computes once; a combination is an explicit record.  This is enough for
//    the first increment; follow-ups will use typed dags so that an
//    undeclared leaf is rejected by TableGen instead of C++.
//
// TODO: This increment still spells type lists and builder arguments as
// strings.  Follow-ups will turn types into records and give builders a
// signature, so a typo becomes a TableGen error rather than a C++ error.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Predicate AST
//===----------------------------------------------------------------------===//

class LegalizePredicate {
  string Text = ?;
}

class LegalizeTrue : LegalizePredicate {
  let Text = "true";
}

def True : LegalizeTrue;

/// A subtarget feature, or any per-subtarget boolean computed by the target,
/// e.g. "HasPOPCNT" or "Is64Bit".
class LegalizeFeature<string Name> : LegalizePredicate {
  let Text = Name;
}

class LegalizeAnd<LegalizePredicate A, LegalizePredicate B> : LegalizePredicate {
  let Text = "(" # A.Text # " && " # B.Text # ")";
}

class LegalizeOr<LegalizePredicate A, LegalizePredicate B> : LegalizePredicate {
  let Text = "(" # A.Text # " || " # B.Text # ")";
}

class LegalizeNot<LegalizePredicate A> : LegalizePredicate {
  let Text = "!(" # A.Text # ")";
}

//===----------------------------------------------------------------------===//
// Rules
//===----------------------------------------------------------------------===//

class LegalizeRule {
  string Method = ?;
  list<string> Args = [];

  /// Action this rule implements.  -gen-legalizer checks that the mutation
  /// carried by the rule can actually produce it.
  string Action = "Legal";

  /// Mutation direction: "widen", "narrow", "split", "merge", "bitcast",
  /// "neutral" or "any".  Empty for rules that carry no mutation.
  string Mutation = "";
}

class LegalFor<LegalizePredicate P, list<string> Types, bit Pairs = 1>
    : LegalizeRule {
  let Method = "legalFor";
  let Args = [P.Text,
              !if(Pairs, "{{" # !interleave(Types, "}, {") # "}}",
                        "{" # !interleave(Types, ", ") # "}")];
  let Action = "Legal";
  let Mutation = "";
}

class WidenScalarToNextPow2<string Idx, string Min> : LegalizeRule {
  let Method = "widenScalarToNextPow2";
  let Args = [Idx, "/*Min=*/" # Min];
  let Action = "WidenScalar";
  let Mutation = "widen";
}

class ClampScalar<string Idx, string MinTy, string MaxTy> : LegalizeRule {
  let Method = "clampScalar";
  let Args = [Idx, MinTy, MaxTy];
  let Action = "ClampScalar";
  let Mutation = "any";
}

class ScalarSameSizeAs<string Idx, string OtherIdx> : LegalizeRule {
  let Method = "scalarSameSizeAs";
  let Args = [Idx, OtherIdx];
  let Action = "ScalarSameSizeAs";
  let Mutation = "neutral";
}

//===----------------------------------------------------------------------===//
// Rule sets
//===----------------------------------------------------------------------===//

class LegalizeRuleSet<list<string> opcodes> {
  list<string> Opcodes = opcodes;
  list<LegalizeRule> Rules = [];
}
