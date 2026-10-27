//===- LegalizerInfo.td - GlobalISel legalizer rules ------*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// POC schema for describing GlobalISel legalizer rules in TableGen.
//
// Design constraints:
//
//  * One record maps one-to-one onto one existing builder call in
//    LegalizerInfo.h.  This keeps the generated C++ textually comparable
//    with the hand-written code, which is what makes a mechanical
//    equivalence check possible.
//  * The mutation direction is an explicit field, so -gen-legalizer can
//    reject an action/mutation mismatch before any C++ exists.  In the
//    hand-written API these live in two different places and nothing ties
//    them together (PR220802 is exactly that bug class).
//  * Predicates are a closed AST (see LegalizePredicate).  Leaves name
//    existing C++ predicates; composition is TableGen record composition.
//
//===----------------------------------------------------------------------===//

//===----------------------------------------------------------------------===//
// Predicate AST
//===----------------------------------------------------------------------===//

class LegalizePredicate {
  string Text = ?;
}

class LGTrue : LegalizePredicate {
  let Text = "true";
}

/// A subtarget feature, or any per-subtarget boolean computed by the target,
/// e.g. "HasPOPCNT" or "Is64Bit".
class LGFeature<string Name> : LegalizePredicate {
  let Text = Name;
}

class LGAnd<LegalizePredicate A, LegalizePredicate B> : LegalizePredicate {
  let Text = "(" # A.Text # " && " # B.Text # ")";
}

class LGOr<LegalizePredicate A, LegalizePredicate B> : LegalizePredicate {
  let Text = "(" # A.Text # " || " # B.Text # ")";
}

class LGNot<LegalizePredicate A> : LegalizePredicate {
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

class LG_LegalFor<LegalizePredicate P, list<string> Types, bit Pairs = 1>
    : LegalizeRule {
  let Method = "legalFor";
  let Args = [P.Text,
              !if(Pairs, "{{" # !interleave(Types, "}, {") # "}}",
                        "{" # !interleave(Types, ", ") # "}")];
  let Action = "Legal";
  let Mutation = "";
}

class LG_WidenScalarToNextPow2<string Idx, string Min> : LegalizeRule {
  let Method = "widenScalarToNextPow2";
  let Args = [Idx, "/*Min=*/" # Min];
  let Action = "WidenScalar";
  let Mutation = "widen";
}

class LG_ClampScalar<string Idx, string MinTy, string MaxTy> : LegalizeRule {
  let Method = "clampScalar";
  let Args = [Idx, MinTy, MaxTy];
  let Action = "ClampScalar";
  let Mutation = "any";
}

class LG_ScalarSameSizeAs<string Idx, string OtherIdx> : LegalizeRule {
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