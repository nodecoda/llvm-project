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
//  * A predicate is a closed set of records: a leaf names the bool the target
//    caches in its constructor, and a combination is a `dag` of leaves using
//    the same all_of/any_of/none_of/not operators as AssemblerPredicate and
//    RuntimeLibcallAvailability.  TableGen itself rejects a leaf that names
//    something the target never defined, so the AST cannot silently degrade
//    into opaque text.
//  * A combination needs a named record: a `dag` template argument cannot
//    hold a bare record, and wrapping every single-feature guard in
//    `(all_of X)` would be noise.  Leaves are therefore records and only the
//    combinations become dags.
//  * Rules whose guard cannot be expressed in the AST yet use
//    LegalizeOpaqueRule / LegalizeOpaquePred, which carry C++ text verbatim.
//    That escape hatch is meant to shrink over time, not to be the normal path.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/TargetSubtarget.td"

// `not` is not a generic dag operator: each domain defines the ones it uses.
// RuntimeLibcallsImpl.td:14 does the same for libcall availability.
def not;

//===----------------------------------------------------------------------===//
// Predicate AST
//===----------------------------------------------------------------------===//

/// A boolean guard, either a leaf or a combination of leaves.
class LegalizePredicate;

/// A subtarget feature, or any per-subtarget boolean computed once in the
/// target constructor.  The record name is the C++ identifier, so
///
///   def HasSSE1 : LegalizeFeature<"Subtarget.hasSSE1()">;
///
/// declares `bool HasSSE1 = Subtarget.hasSSE1();` and lets rules refer to it as
/// plain `HasSSE1`.
class LegalizeFeature<code cond> : LegalizePredicate {
  code Cond = cond;
}

/// A C++ expression that cannot be modelled as AST nodes, e.g.
/// "scalarOrEltWiderThan(0, 64)".  -gen-legalizer rejects one whose text is
/// merely a boolean combination of declared features; those must be written
/// with all_of / any_of / not, so that the escape hatch stays an escape hatch.
class LegalizeOpaquePred<code expr> : LegalizePredicate {
  code Expr = expr;
}

/// A combination of leaves.  Operators come from TargetSubtarget.td, plus the
/// `not` defined above.
class LegalizePredicateDag<dag cond> : LegalizePredicate {
  dag CondDag = cond;
}

//===----------------------------------------------------------------------===//
// Rules
//===----------------------------------------------------------------------===//

class LegalizeRule {
  string Method = ?;
  list<string> Args = [];

  /// Guard, lowered by -gen-legalizer.  Unset for builders that take none.
  LegalizePredicate Pred = ?;

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

class LegalizeSimpleRule<string method, list<string> args,
                         string action = "Legal", string mutation = "">
    : LegalizeRule {
  let Method = method;
  let Args = args;
  let Action = action;
  let Mutation = mutation;
}

class LegalizePredRule<string method, LegalizePredicate P, list<string> args,
                       string action = "Legal", string mutation = "">
    : LegalizeRule {
  let Method = method;
  let Pred = P;
  let Args = args;
  let Action = action;
  let Mutation = mutation;
}

/// A rule whose guard (and possibly mutation) is still raw C++.
class LegalizeOpaqueRule<string method, code body, string action = "Legal",
                         string mutation = ""> : LegalizeRule {
  let Method = method;
  let OpaqueArgs = body;
  let Action = action;
  let Mutation = mutation;
}

/// Render a `{...}` / `{{...}}` type argument.
class LegalizeTypes<list<string> Types, bit Pairs> {
  string Text = !if(Pairs, "{{" # !interleave(Types, "}, {") # "}}",
                           "{" # !interleave(Types, ", ") # "}");
}

//===----------------------------------------------------------------------===//
// Sugar: one class per builder in LegalizerInfo.h used by X86.
//===----------------------------------------------------------------------===//

class LegalFor<list<string> Types, bit Pairs = 1>
    : LegalizeSimpleRule<"legalFor", [LegalizeTypes<Types, Pairs>.Text], "Legal",
                         "">;

class LegalForIf<LegalizePredicate P, list<string> Types, bit Pairs = 1>
    : LegalizePredRule<"legalFor", P, [LegalizeTypes<Types, Pairs>.Text],
                       "Legal", "">;

class LegalForCartesianProduct<string T0, string T1>
    : LegalizeSimpleRule<"legalForCartesianProduct", [T0, T1], "Legal", "">;

class LowerForIf<LegalizePredicate P, list<string> Types, bit Pairs = 1>
    : LegalizePredRule<"lowerFor", P, [LegalizeTypes<Types, Pairs>.Text],
                       "Lower", "">;

class LegalForMemDesc<list<string> Types>
    : LegalizeSimpleRule<"legalForTypesWithMemDesc",
                         [LegalizeTypes<Types, 1>.Text], "Legal", "">;

class LegalForMemDescIf<LegalizePredicate P, list<string> Types>
    : LegalizePredRule<"legalForTypesWithMemDesc", P,
                       [LegalizeTypes<Types, 1>.Text], "Legal", "">;

def Lower : LegalizeSimpleRule<"lower", [], "Lower", "">;

def Libcall : LegalizeSimpleRule<"libcall", [], "Libcall", "">;

class LibcallFor<list<string> Types, bit Pairs = 0>
    : LegalizeSimpleRule<"libcallFor", [LegalizeTypes<Types, Pairs>.Text],
                         "Libcall", "">;

class CustomFor<list<string> Types, bit Pairs = 0>
    : LegalizeSimpleRule<"customFor", [LegalizeTypes<Types, Pairs>.Text],
                         "Custom", "">;

class CustomForCartesianProductIf<LegalizePredicate P, string T0,
                                        string T1>
    : LegalizePredRule<"customForCartesianProduct", P, [T0, T1], "Custom", "">;

def AlwaysLegal : LegalizeSimpleRule<"alwaysLegal", [], "Legal", "">;

class ClampScalar<string Idx, string MinTy, string MaxTy>
    : LegalizeSimpleRule<"clampScalar", [Idx, MinTy, MaxTy], "ClampScalar",
                         "any">;

class ClampScalarOrElt<string Idx, string MinTy, string MaxTy>
    : LegalizeSimpleRule<"clampScalarOrElt", [Idx, MinTy, MaxTy], "ClampScalar",
                         "any">;

class WidenScalarToNextPow2<string Idx, string Min = "">
    : LegalizeSimpleRule<"widenScalarToNextPow2",
                         !if(!eq(Min, ""), [Idx], [Idx, "/*Min=*/" # Min]),
                         "WidenScalar", "widen">;

class WidenScalarOrEltToNextPow2<string Idx, string Min = "">
    : LegalizeSimpleRule<"widenScalarOrEltToNextPow2",
                         !if(!eq(Min, ""), [Idx], [Idx, "/*Min=*/" # Min]),
                         "WidenScalar", "widen">;

class MinScalar<string Idx, string Ty>
    : LegalizeSimpleRule<"minScalar", [Idx, Ty], "WidenScalar", "widen">;

class MaxScalar<string Idx, string Ty>
    : LegalizeSimpleRule<"maxScalar", [Idx, Ty], "NarrowScalar", "narrow">;

class ScalarSameSizeAs<string Idx, string OtherIdx>
    : LegalizeSimpleRule<"scalarSameSizeAs", [Idx, OtherIdx],
                         "ScalarSameSizeAs", "neutral">;

class Scalarize<string Idx>
    : LegalizeSimpleRule<"scalarize", [Idx], "FewerElements", "split">;

class ScalarizeIf<LegalizePredicate P, string Idx>
    : LegalizePredRule<"scalarizeIf", P, [Idx], "FewerElements", "split">;

class MoreElementsToNextPow2<string Idx>
    : LegalizeSimpleRule<"moreElementsToNextPow2", [Idx], "MoreElements",
                         "merge">;

class ClampNumElements<string Idx, string EltTy, string MaxTy>
    : LegalizeSimpleRule<"clampNumElements", [Idx, EltTy, MaxTy],
                         "ClampNumElements", "any">;

class ClampMinNumElements<string Idx, string EltTy, string Num>
    : LegalizeSimpleRule<"clampMinNumElements", [Idx, EltTy, Num],
                         "ClampNumElements", "any">;

class ClampMaxNumElements<string Idx, string EltTy, string Num>
    : LegalizeSimpleRule<"clampMaxNumElements", [Idx, EltTy, Num],
                         "ClampNumElements", "any">;

//===----------------------------------------------------------------------===//
// Target vocabulary
//
// A target writes its rule sets in terms of LegalizeFeature records (see
// above) and a handful of LLT constants.  Declaring the constants here keeps a
// target's whole legalizer in one file; -gen-legalizer emits them ahead of the
// rules, in the order given.
//===----------------------------------------------------------------------===//

/// An LLT constant:
///   const LLT s32 = LLT::scalar(32);
class LegalizeType<string name, string expr> {
  string Name = name;
  string Expr = expr;
}

/// A named type list, for the builders that take an initializer_list:
///   const std::initializer_list<LLT> PtrTypes32 = {s1, s8, s16, s32};
class LegalizeTypeList<string name, list<string> elems> {
  string Name = name;
  list<string> Elems = elems;
}

/// Everything a target's rule sets are written in terms of, apart from the
/// features themselves.  -gen-legalizer emits these declarations ahead of the
/// rules, in the order given here, so a derived entry may refer to the ones
/// above it.
class LegalizeContext {
  list<string> Comments = [];
  list<LegalizeType> Types = [];
  list<LegalizeTypeList> TypeLists = [];
}

//===----------------------------------------------------------------------===//
// Rule sets
//===----------------------------------------------------------------------===//

class LegalizeRuleSet<list<string> opcodes> {
  list<string> Opcodes = opcodes;
  list<LegalizeRule> Rules = [];
  list<string> Comments = [];
}