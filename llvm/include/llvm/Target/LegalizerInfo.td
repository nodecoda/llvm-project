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
// Design constraints:
//
//  * One record maps one-to-one onto one existing builder call in
//    LegalizerInfo.h.  This keeps the generated C++ comparable with the
//    hand-written code, which is what makes a mechanical equivalence check
//    possible.
//  * The mutation direction is an explicit field, so -gen-legalizer can
//    reject an action/mutation mismatch before any C++ exists.  In the
//    hand-written API these live in two different places and nothing ties
//    them together (PR220802 is exactly that bug class).
//  * Predicates are a closed AST (see LegalizePredicate).  Leaves name
//    existing C++ predicates; composition is TableGen record composition.  An
//    LGExpr that is nothing but a boolean combination of declared features is
//    rejected, so the AST cannot silently degrade into opaque text.
//  * Rules whose guard cannot be expressed in the AST yet use LG_Opaque,
//    which carries the C++ text verbatim.  The escape hatch is meant to
//    shrink over time, not to be the normal path.
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
/// e.g. "HasPOPCNT" or "Is64Bit".  The name is kept as a field so that
/// -gen-legalizer can check it against the target's LegalizeContext.
class LGFeature<string name> : LegalizePredicate {
  string Name = name;
  let Text = name;
}

/// A C++ expression that cannot be modelled as AST nodes, e.g.
/// "scalarOrEltWiderThan(0, 64)".  -gen-legalizer rejects one whose text is
/// merely a boolean combination of declared features; those must be written
/// with LGAnd / LGOr / LGNot, so that the escape hatch stays an escape hatch.
class LGExpr<string expr> : LegalizePredicate {
  string Expr = expr;
  let Text = expr;
}

class LGAnd<LegalizePredicate A, LegalizePredicate B> : LegalizePredicate {
  let Text = "(" # A.Text # " && " # B.Text # ")";
}

class LGOr<LegalizePredicate A, LegalizePredicate B> : LegalizePredicate {
  let Text = "(" # A.Text # " || " # B.Text # ")";
}

/// Compound operands parenthesize themselves, so "!" needs no extra pair and
/// `!HasCMOV` stays readable instead of becoming `!(HasCMOV)`.
class LGNot<LegalizePredicate A> : LegalizePredicate {
  let Text = "!" # A.Text;
}

//===----------------------------------------------------------------------===//
// Rules
//===----------------------------------------------------------------------===//

class LegalizeRule {
  string Method = ?;
  list<string> Args = [];

  /// When non-empty, the rule body is this C++ text, emitted verbatim.  Used
  /// for guards that are still raw lambdas.
  code OpaqueArgs = "";

  /// Action this rule implements.  -gen-legalizer checks that the mutation
  /// carried by the rule can actually produce it.
  string Action = "Legal";

  /// Mutation direction: "widen", "narrow", "split", "merge", "bitcast",
  /// "neutral" or "any".  Empty for rules that carry no mutation.
  string Mutation = "";
}

class LG_Rule<string method, list<string> args, string action = "Legal",
              string mutation = ""> : LegalizeRule {
  let Method = method;
  let Args = args;
  let Action = action;
  let Mutation = mutation;
}

class LG_PredRule<string method, LegalizePredicate P, list<string> args,
                  string action = "Legal", string mutation = "">
    : LG_Rule<method, !listconcat([P.Text], args), action, mutation>;

/// A rule whose guard (and possibly mutation) is still raw C++.
class LG_OpaqueRule<string method, code body, string action = "Legal",
                    string mutation = ""> : LegalizeRule {
  let Method = method;
  let OpaqueArgs = body;
  let Action = action;
  let Mutation = mutation;
}

/// Render a `{...}` / `{{...}}` type argument.
class LGTypes<list<string> Types, bit Pairs> {
  string Text = !if(Pairs, "{{" # !interleave(Types, "}, {") # "}}",
                           "{" # !interleave(Types, ", ") # "}");
}

//===----------------------------------------------------------------------===//
// Sugar: one class per builder in LegalizerInfo.h used by X86.
//===----------------------------------------------------------------------===//

class LG_LegalForTypes<list<string> Types, bit Pairs = 1>
    : LG_Rule<"legalFor", [LGTypes<Types, Pairs>.Text], "Legal", "">;

class LG_LegalFor<LegalizePredicate P, list<string> Types, bit Pairs = 1>
    : LG_PredRule<"legalFor", P, [LGTypes<Types, Pairs>.Text], "Legal", "">;

class LG_LegalForCartesianProduct<string T0, string T1>
    : LG_Rule<"legalForCartesianProduct", [T0, T1], "Legal", "">;

class LG_LowerFor<LegalizePredicate P, list<string> Types, bit Pairs = 1>
    : LG_PredRule<"lowerFor", P, [LGTypes<Types, Pairs>.Text], "Lower", "">;

class LG_LegalForMemDesc<list<string> Types>
    : LG_Rule<"legalForTypesWithMemDesc", [LGTypes<Types, 1>.Text], "Legal",
              "">;

class LG_LegalForMemDescIf<LegalizePredicate P, list<string> Types>
    : LG_PredRule<"legalForTypesWithMemDesc", P, [LGTypes<Types, 1>.Text],
                  "Legal", "">;

def LG_Lower : LG_Rule<"lower", [], "Lower", "">;

def LG_Libcall : LG_Rule<"libcall", [], "Libcall", "">;

class LG_LibcallFor<list<string> Types, bit Pairs = 0>
    : LG_Rule<"libcallFor", [LGTypes<Types, Pairs>.Text], "Libcall", "">;

class LG_CustomFor<list<string> Types, bit Pairs = 0>
    : LG_Rule<"customFor", [LGTypes<Types, Pairs>.Text], "Custom", "">;

class LG_CustomForCartesianProduct<LegalizePredicate P, string T0, string T1>
    : LG_PredRule<"customForCartesianProduct", P, [T0, T1], "Custom", "">;

def LG_AlwaysLegal : LG_Rule<"alwaysLegal", [], "Legal", "">;

class LG_ClampScalar<string Idx, string MinTy, string MaxTy>
    : LG_Rule<"clampScalar", [Idx, MinTy, MaxTy], "ClampScalar", "any">;

class LG_ClampScalarOrElt<string Idx, string MinTy, string MaxTy>
    : LG_Rule<"clampScalarOrElt", [Idx, MinTy, MaxTy], "ClampScalar", "any">;

class LG_WidenScalarToNextPow2<string Idx, string Min = "">
    : LG_Rule<"widenScalarToNextPow2",
              !if(!eq(Min, ""), [Idx], [Idx, "/*Min=*/" # Min]), "WidenScalar",
              "widen">;

class LG_WidenScalarOrEltToNextPow2<string Idx, string Min = "">
    : LG_Rule<"widenScalarOrEltToNextPow2",
              !if(!eq(Min, ""), [Idx], [Idx, "/*Min=*/" # Min]), "WidenScalar",
              "widen">;

class LG_MinScalar<string Idx, string Ty>
    : LG_Rule<"minScalar", [Idx, Ty], "WidenScalar", "widen">;

class LG_MaxScalar<string Idx, string Ty>
    : LG_Rule<"maxScalar", [Idx, Ty], "NarrowScalar", "narrow">;

class LG_ScalarSameSizeAs<string Idx, string OtherIdx>
    : LG_Rule<"scalarSameSizeAs", [Idx, OtherIdx], "ScalarSameSizeAs",
              "neutral">;

class LG_Scalarize<string Idx>
    : LG_Rule<"scalarize", [Idx], "FewerElements", "split">;

class LG_ScalarizeIf<LegalizePredicate P, string Idx>
    : LG_PredRule<"scalarizeIf", P, [Idx], "FewerElements", "split">;

class LG_MoreElementsToNextPow2<string Idx>
    : LG_Rule<"moreElementsToNextPow2", [Idx], "MoreElements", "merge">;

class LG_ClampNumElements<string Idx, string EltTy, string MaxTy>
    : LG_Rule<"clampNumElements", [Idx, EltTy, MaxTy], "ClampNumElements",
              "any">;

class LG_ClampMinNumElements<string Idx, string EltTy, string Num>
    : LG_Rule<"clampMinNumElements", [Idx, EltTy, Num], "ClampNumElements",
              "any">;

class LG_ClampMaxNumElements<string Idx, string EltTy, string Num>
    : LG_Rule<"clampMaxNumElements", [Idx, EltTy, Num], "ClampNumElements",
              "any">;

//===----------------------------------------------------------------------===//
// Target vocabulary
//
// The rules below are written in terms of one boolean per subtarget feature
// and a handful of LLT constants.  Declaring them here as well keeps a
// target's whole legalizer in one file, and lets -gen-legalizer check that a
// predicate names something that exists.
//===----------------------------------------------------------------------===//

/// A subtarget feature cached in the constructor:
///   bool HasAVX2 = Subtarget.hasAVX2();
class LG_Bool<string name, string expr> {
  string Name = name;
  string Expr = expr;
}

/// An LLT constant:
///   const LLT s32 = LLT::scalar(32);
class LG_Type<string name, string expr> {
  string Name = name;
  string Expr = expr;
}

/// A named type list, for the builders that take an initializer_list:
///   const std::initializer_list<LLT> PtrTypes32 = {s1, s8, s16, s32};
class LG_TypeList<string name, list<string> elems> {
  string Name = name;
  list<string> Elems = elems;
}

/// Everything a target's rule sets are written in terms of.  -gen-legalizer
/// emits these declarations ahead of the rules, in the order given here, so a
/// derived entry may refer to the ones above it.
class LegalizeContext {
  list<string> Comments = [];
  list<LG_Bool> Bools = [];
  list<LG_Type> Types = [];
  list<LG_TypeList> TypeLists = [];
}

//===----------------------------------------------------------------------===//
// Rule sets
//===----------------------------------------------------------------------===//

class LegalizeRuleSet<list<string> opcodes> {
  list<string> Opcodes = opcodes;
  list<LegalizeRule> Rules = [];
  list<string> Comments = [];
}
