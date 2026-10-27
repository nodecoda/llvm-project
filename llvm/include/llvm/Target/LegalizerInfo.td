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
// Actions
//
// The action a rule implements and the mutation direction that action implies.
// In hand-written C++ these two live in different places -- the builder call
// picks the action, the mutation the rule performs implies the direction --
// and nothing ties them together, which is how
// https://github.com/llvm/llvm-project/issues/220802 happened.  Keeping both
// on one record lets -gen-legalizer reject the disagreement.
//
// Most of these are LegalizeActions::LegalizeAction values.  ClampScalar,
// ScalarSameSizeAs and ClampNumElements are not: they are composites that
// expand to more than one applied action (LegalizerInfo.h: clampScalar and
// clampScalarOrElt -> minScalar + maxScalar, scalarSameSizeAs ->
// minScalarSameAs + maxScalarSameAs, clampNumElements -> clampMinNumElements +
// clampMaxNumElements), so they only exist at this level and accept either
// direction.  Unsupported and NotFound are not rule actions and have no entry.
//===----------------------------------------------------------------------===//

class LegalizeAction<string name, string mutation> {
  /// C++ spelling, for diagnostics.
  string Name = name;

  /// Direction the action implies: "widen", "narrow", "split", "merge",
  /// "bitcast", "neutral", "any", or "" if the action prescribes none.
  string Mutation = mutation;
}

def ActionLegal            : LegalizeAction<"Legal", "">;
def ActionLower            : LegalizeAction<"Lower", "">;
def ActionLibcall          : LegalizeAction<"Libcall", "">;
def ActionCustom           : LegalizeAction<"Custom", "">;
def ActionWidenScalar      : LegalizeAction<"WidenScalar", "widen">;
def ActionNarrowScalar     : LegalizeAction<"NarrowScalar", "narrow">;
def ActionFewerElements    : LegalizeAction<"FewerElements", "split">;
def ActionMoreElements     : LegalizeAction<"MoreElements", "merge">;
def ActionBitcast          : LegalizeAction<"Bitcast", "bitcast">;
def ActionClampScalar      : LegalizeAction<"ClampScalar", "any">;
def ActionScalarSameSizeAs : LegalizeAction<"ScalarSameSizeAs", "any">;
def ActionClampNumElements : LegalizeAction<"ClampNumElements", "any">;

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

  /// Action this rule implements.  Every rule states one: -gen-legalizer
  /// rejects a rule that leaves it out, and rejects a mutation direction that
  /// the action cannot produce.
  LegalizeAction Action = ?;

  /// Mutation direction this rule performs: "widen", "narrow", "split",
  /// "merge", "bitcast", "neutral", "any", or "" for none.
  string Mutation = "";
}

/// A rule built from a builder method, its arguments and an action.
class LegalizeCall<string method, list<string> args, LegalizeAction action,
                   string mutation = ""> : LegalizeRule {
  let Method = method;
  let Args = args;
  let Action = action;
  let Mutation = mutation;
}

/// The same, with a subtarget guard.
class LegalizeGuardedCall<string method, LegalizePredicate P,
                          list<string> args, LegalizeAction action,
                          string mutation = ""> : LegalizeRule {
  let Method = method;
  let Pred = P;
  let Args = args;
  let Action = action;
  let Mutation = mutation;
}

/// A rule whose guard (and possibly mutation) is still raw C++.
class LegalizeOpaqueRule<string method, code body, LegalizeAction action,
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
    : LegalizeCall<"legalFor", [LegalizeTypes<Types, Pairs>.Text], ActionLegal>;

class LegalForIf<LegalizePredicate P, list<string> Types, bit Pairs = 1>
    : LegalizeGuardedCall<"legalFor", P, [LegalizeTypes<Types, Pairs>.Text],
                          ActionLegal>;

class LegalForCartesianProduct<string T0, string T1>
    : LegalizeCall<"legalForCartesianProduct", [T0, T1], ActionLegal>;

class LowerForIf<LegalizePredicate P, list<string> Types, bit Pairs = 1>
    : LegalizeGuardedCall<"lowerFor", P, [LegalizeTypes<Types, Pairs>.Text],
                          ActionLower>;

class LegalForMemDesc<list<string> Types>
    : LegalizeCall<"legalForTypesWithMemDesc", [LegalizeTypes<Types, 1>.Text],
                   ActionLegal>;

class LegalForMemDescIf<LegalizePredicate P, list<string> Types>
    : LegalizeGuardedCall<"legalForTypesWithMemDesc", P,
                          [LegalizeTypes<Types, 1>.Text], ActionLegal>;

def Lower : LegalizeCall<"lower", [], ActionLower>;

def Libcall : LegalizeCall<"libcall", [], ActionLibcall>;

class LibcallFor<list<string> Types, bit Pairs = 0>
    : LegalizeCall<"libcallFor", [LegalizeTypes<Types, Pairs>.Text],
                   ActionLibcall>;

class CustomFor<list<string> Types, bit Pairs = 0>
    : LegalizeCall<"customFor", [LegalizeTypes<Types, Pairs>.Text],
                   ActionCustom>;

class CustomForCartesianProductIf<LegalizePredicate P, string T0, string T1>
    : LegalizeGuardedCall<"customForCartesianProduct", P, [T0, T1],
                          ActionCustom>;

def AlwaysLegal : LegalizeCall<"alwaysLegal", [], ActionLegal>;

class ClampScalar<string Idx, string MinTy, string MaxTy>
    : LegalizeCall<"clampScalar", [Idx, MinTy, MaxTy], ActionClampScalar,
                   "any">;

class ClampScalarOrElt<string Idx, string MinTy, string MaxTy>
    : LegalizeCall<"clampScalarOrElt", [Idx, MinTy, MaxTy], ActionClampScalar,
                   "any">;

class WidenScalarToNextPow2<string Idx, string Min = "">
    : LegalizeCall<"widenScalarToNextPow2",
                   !if(!eq(Min, ""), [Idx], [Idx, "/*Min=*/" # Min]),
                   ActionWidenScalar, "widen">;

class WidenScalarOrEltToNextPow2<string Idx, string Min = "">
    : LegalizeCall<"widenScalarOrEltToNextPow2",
                   !if(!eq(Min, ""), [Idx], [Idx, "/*Min=*/" # Min]),
                   ActionWidenScalar, "widen">;

class MinScalar<string Idx, string Ty>
    : LegalizeCall<"minScalar", [Idx, Ty], ActionWidenScalar, "widen">;

class MaxScalar<string Idx, string Ty>
    : LegalizeCall<"maxScalar", [Idx, Ty], ActionNarrowScalar, "narrow">;

class ScalarSameSizeAs<string Idx, string OtherIdx>
    : LegalizeCall<"scalarSameSizeAs", [Idx, OtherIdx], ActionScalarSameSizeAs,
                   "neutral">;

class Scalarize<string Idx>
    : LegalizeCall<"scalarize", [Idx], ActionFewerElements, "split">;

class ScalarizeIf<LegalizePredicate P, string Idx>
    : LegalizeGuardedCall<"scalarizeIf", P, [Idx], ActionFewerElements,
                          "split">;

class MoreElementsToNextPow2<string Idx>
    : LegalizeCall<"moreElementsToNextPow2", [Idx], ActionMoreElements,
                   "merge">;

class ClampNumElements<string Idx, string EltTy, string MaxTy>
    : LegalizeCall<"clampNumElements", [Idx, EltTy, MaxTy],
                   ActionClampNumElements, "any">;

class ClampMinNumElements<string Idx, string EltTy, string Num>
    : LegalizeCall<"clampMinNumElements", [Idx, EltTy, Num],
                   ActionClampNumElements, "any">;

class ClampMaxNumElements<string Idx, string EltTy, string Num>
    : LegalizeCall<"clampMaxNumElements", [Idx, EltTy, Num],
                   ActionClampNumElements, "any">;

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