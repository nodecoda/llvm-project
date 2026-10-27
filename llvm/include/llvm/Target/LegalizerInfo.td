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
class LegalizeFeature<string cond> : LegalizePredicate {
  string Cond = cond;
}

/// A C++ expression that cannot be modelled as AST nodes, e.g.
/// "scalarOrEltWiderThan(0, 64)".  -gen-legalizer rejects one whose text is
/// merely a boolean combination of declared features; those must be written
/// with all_of / any_of / not, so that the escape hatch stays an escape hatch.
class LegalizeOpaquePred<string expr> : LegalizePredicate {
  string Expr = expr;
}

/// A combination of leaves.  Operators come from TargetSubtarget.td, plus the
/// `not` defined above.
class LegalizePredicateDag<dag cond> : LegalizePredicate {
  dag CondDag = cond;
}

//===----------------------------------------------------------------------===//
// Rule arguments
//
// One class per shape a builder argument can take.  A sugar class declares
// which shapes it accepts, so the shape is checked when the .td is parsed
// instead of surfacing as an undeclared identifier in the generated .inc.
// The record name of a LegalizeType is the C++ identifier:
//
//   def s32 : LegalizeType<"LLT::scalar(32)">;
//
// and a rule refers to it as plain `s32`.
//===----------------------------------------------------------------------===//

class LegalizeArg;

/// Anything that fits in a single type position: a named type, or a C++
/// expression that yields one.
class LegalizeArgType : LegalizeArg;

/// A named type: `s32`.  The record name is the C++ identifier, and emitTypes()
/// declares it once ahead of the rules.
class LegalizeType<string expr> : LegalizeArgType {
  string Name = NAME;
  string Expr = expr;
}

/// A type position that holds a C++ expression, e.g. `HasSSE2 ? s64 : s32`.
/// Unlike LegalizeType it declares nothing: the text is written straight into
/// the builder call, so it has to be a single expression.
class LegalizeTypeExpr<string text> : LegalizeArgType {
  string Text = text;
}

/// A type list in a builder argument position.  Same split as above: an inline
/// list, whose braces the emitter supplies, and an expression that picks one of
/// the named lists, which cannot carry braces because the choice is made at
/// run time.
class LegalizeArgTypeList : LegalizeArg;

/// `{s8, s16}`, spelled as a list and braced by the emitter.
class LegalizeInlineTypeList<list<LegalizeType> elems> : LegalizeArgTypeList {
  list<LegalizeType> Elems = elems;
}

/// `Is64Bit ? PtrTypes64 : PtrTypes32`.
class LegalizeTypeListExpr<string text> : LegalizeArgTypeList {
  string Text = text;
}

/// A named type list, for builders that take an initializer_list:
///   const std::initializer_list<LLT> PtrTypes32 = {s1, s8, s16, s32};
class LegalizeTypeList<list<LegalizeType> elems> : LegalizeArg {
  string Name = NAME;
  list<LegalizeType> Elems = elems;
}

/// Two types that must be legal together: rendered `s32, s32` as one entry of
/// a {{...}, {...}} list.
class LegalizeTypePair<LegalizeType type0, LegalizeType type1> : LegalizeArg {
  LegalizeType Type0 = type0;
  LegalizeType Type1 = type1;
}

/// One LegalityPredicates::TypePairAndMemDesc entry (LegalizerInfo.h:167):
/// {Type0, Type1, MemTy, Align}.  Align is in bits and is an integer, which is
/// why a memdesc tuple is not a list of four types.
class LegalizeMemDesc<LegalizeType type0, LegalizeType type1,
                      LegalizeType memTy, int align> : LegalizeArg {
  LegalizeType Type0 = type0;
  LegalizeType Type1 = type1;
  LegalizeType MemTy = memTy;
  int Align = align;
}

/// A literal integer: a type index, a bit width, an element count.
class LegalizeArgInt<int value, string comment = ""> : LegalizeArg {
  int Value = value;

  /// Rendered as `/*Name=*/` in front of the value, for builders where a bare
  /// number would not say what it means.
  string Comment = comment;
}

/// C++ text that cannot be typed, e.g. an element count computed from features.
/// An escape hatch that is meant to shrink, not to grow: X86 is down to the
/// element counts and the cartesian-product type lists.
class LegalizeRawArg<string text> : LegalizeArg {
  string Text = text;
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

  /// Builder arguments.  A list that is entirely types, type pairs or memdesc
  /// tuples is emitted inside one `{...}`; anything else is a comma list, with
  /// each argument rendering itself.
  list<LegalizeArg> Args = [];

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
class LegalizeCall<string method, list<LegalizeArg> args,
                   LegalizeAction action, string mutation = ""> : LegalizeRule {
  let Method = method;
  let Args = args;
  let Action = action;
  let Mutation = mutation;
}

/// The same, with a subtarget guard.
class LegalizeGuardedCall<string method, LegalizePredicate P,
                          list<LegalizeArg> args, LegalizeAction action,
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


//===----------------------------------------------------------------------===//
// Sugar: one class per builder in LegalizerInfo.h used by X86.
//
// A class name states the shape it accepts, because the shape is the thing
// that used to go wrong: `LegalFor` takes single types, `LegalForPairs` takes
// pairs that must be legal together, and `LegalForMemDesc` takes memdesc
// tuples.  Pick the wrong one and the .td does not parse.
//===----------------------------------------------------------------------===//

class LegalFor<list<LegalizeArgType> Types>
    : LegalizeCall<"legalFor", Types, ActionLegal>;

class LegalForIf<LegalizePredicate P, list<LegalizeArgType> Types>
    : LegalizeGuardedCall<"legalFor", P, Types, ActionLegal>;

class LegalForPairs<list<LegalizeTypePair> Pairs>
    : LegalizeCall<"legalFor", Pairs, ActionLegal>;

class LegalForPairsIf<LegalizePredicate P, list<LegalizeTypePair> Pairs>
    : LegalizeGuardedCall<"legalFor", P, Pairs, ActionLegal>;

class LegalForCartesianProduct<LegalizeArgTypeList T0, LegalizeArgTypeList T1>
    : LegalizeCall<"legalForCartesianProduct", [T0, T1], ActionLegal>;

class LowerForPairsIf<LegalizePredicate P, list<LegalizeTypePair> Pairs>
    : LegalizeGuardedCall<"lowerFor", P, Pairs, ActionLower>;

class LegalForMemDesc<list<LegalizeMemDesc> Descs>
    : LegalizeCall<"legalForTypesWithMemDesc", Descs, ActionLegal>;

class LegalForMemDescIf<LegalizePredicate P, list<LegalizeMemDesc> Descs>
    : LegalizeGuardedCall<"legalForTypesWithMemDesc", P, Descs, ActionLegal>;

def Lower : LegalizeCall<"lower", [], ActionLower>;

def Libcall : LegalizeCall<"libcall", [], ActionLibcall>;

class LibcallFor<list<LegalizeArgType> Types>
    : LegalizeCall<"libcallFor", Types, ActionLibcall>;

class CustomFor<list<LegalizeArgType> Types>
    : LegalizeCall<"customFor", Types, ActionCustom>;

class CustomForCartesianProductIf<LegalizePredicate P, LegalizeArgTypeList T0,
                                  LegalizeArgTypeList T1>
    : LegalizeGuardedCall<"customForCartesianProduct", P, [T0, T1],
                          ActionCustom>;

def AlwaysLegal : LegalizeCall<"alwaysLegal", [], ActionLegal>;

class ClampScalar<int Idx, LegalizeArgType MinTy, LegalizeArgType MaxTy>
    : LegalizeCall<"clampScalar",
                   [LegalizeArgInt<Idx>, MinTy, MaxTy], ActionClampScalar,
                   "any">;

class ClampScalarOrElt<int Idx, LegalizeArgType MinTy,
                       LegalizeArgType MaxTy>
    : LegalizeCall<"clampScalarOrElt",
                   [LegalizeArgInt<Idx>, MinTy, MaxTy], ActionClampScalar,
                   "any">;

/// Min = 0 means "no minimum": a zero-bit minimum is not meaningful, so the
/// sentinel is unambiguous and the argument stays an integer.
class WidenScalarToNextPow2<int Idx, int Min = 0>
    : LegalizeCall<"widenScalarToNextPow2",
                   !if(!eq(Min, 0), [LegalizeArgInt<Idx>],
                       [LegalizeArgInt<Idx>, LegalizeArgInt<Min, "Min">]),
                   ActionWidenScalar, "widen">;

class WidenScalarOrEltToNextPow2<int Idx, int Min = 0>
    : LegalizeCall<"widenScalarOrEltToNextPow2",
                   !if(!eq(Min, 0), [LegalizeArgInt<Idx>],
                       [LegalizeArgInt<Idx>, LegalizeArgInt<Min, "Min">]),
                   ActionWidenScalar, "widen">;

class MinScalar<int Idx, LegalizeArgType Ty>
    : LegalizeCall<"minScalar", [LegalizeArgInt<Idx>, Ty], ActionWidenScalar,
                   "widen">;

class MaxScalar<int Idx, LegalizeArgType Ty>
    : LegalizeCall<"maxScalar", [LegalizeArgInt<Idx>, Ty], ActionNarrowScalar,
                   "narrow">;

class ScalarSameSizeAs<int Idx, int OtherIdx>
    : LegalizeCall<"scalarSameSizeAs",
                   [LegalizeArgInt<Idx>, LegalizeArgInt<OtherIdx>],
                   ActionScalarSameSizeAs, "neutral">;

class Scalarize<int Idx>
    : LegalizeCall<"scalarize", [LegalizeArgInt<Idx>], ActionFewerElements,
                   "split">;

class ScalarizeIf<LegalizePredicate P, int Idx>
    : LegalizeGuardedCall<"scalarizeIf", P, [LegalizeArgInt<Idx>],
                          ActionFewerElements, "split">;

class MoreElementsToNextPow2<int Idx>
    : LegalizeCall<"moreElementsToNextPow2", [LegalizeArgInt<Idx>],
                   ActionMoreElements, "merge">;

class ClampNumElements<int Idx, LegalizeArgType EltTy,
                       LegalizeArgType MaxTy>
    : LegalizeCall<"clampNumElements",
                   [LegalizeArgInt<Idx>, EltTy, MaxTy], ActionClampNumElements,
                   "any">;

/// The element count is a C++ integer expression: targets routinely compute it
/// from features (`HasAVX2 ? 8 : 4`), so it stays text.  The index and the
/// element type are still checked.
class ClampMinNumElements<int Idx, LegalizeArgType EltTy, string Num>
    : LegalizeCall<"clampMinNumElements",
                   [LegalizeArgInt<Idx>, EltTy, LegalizeRawArg<Num>],
                   ActionClampNumElements, "any">;

class ClampMaxNumElements<int Idx, LegalizeArgType EltTy, string Num>
    : LegalizeCall<"clampMaxNumElements",
                   [LegalizeArgInt<Idx>, EltTy, LegalizeRawArg<Num>],
                   ActionClampNumElements, "any">;

//===----------------------------------------------------------------------===//
// Rule sets
//===----------------------------------------------------------------------===//

class LegalizeRuleSet<list<string> opcodes> {
  list<string> Opcodes = opcodes;
  list<LegalizeRule> Rules = [];
  list<string> Comments = [];
}