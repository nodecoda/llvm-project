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
//    LegalizerInfo.h.  The X86 conversion was checked rule by rule against the
//    chains it replaced, but that baseline is not kept in the tree, so the
//    comparison cannot be re-run mechanically.
//  * The mutation direction is a field of the action, so -gen-legalizer can
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
//    LegalizeOpaqueRule / LegalizeOpaqueBool, which carry C++ text verbatim.
//    That escape hatch is meant to shrink over time, not to be the normal path.
//  * A cached bool and a query-time predicate are different C++ types, and the
//    schema keeps them apart: LegalizeBoolPredicate is what legalFor() and
//    friends take, LegalizeQueryPredicate is what scalarizeIf() takes.
//
//===----------------------------------------------------------------------===//

include "llvm/Target/TargetSubtarget.td"

// `not` is not a generic dag operator: each domain defines the ones it uses.
// RuntimeLibcallsImpl.td:14 does the same for libcall availability.
def not;

//===----------------------------------------------------------------------===//
// Predicate AST
//===----------------------------------------------------------------------===//

/// A guard accepted by the legalizer builders.
///
/// Two different C++ types stand behind this, and the schema keeps them apart
/// because passing one where the other is expected does not compile: a cached
/// `bool` computed once in the target constructor, and a `LegalityPredicate`
/// (std::function<bool(const LegalityQuery &)>) evaluated per query.
class LegalizePredicate;

/// A guard that is a `bool` computed once, in the target constructor.
class LegalizeBoolPredicate : LegalizePredicate;

/// A subtarget feature, or any per-subtarget boolean computed once in the
/// target constructor.  The record name is the C++ identifier, so
///
///   def HasSSE1 : LegalizeFeature<"Subtarget.hasSSE1()">;
///
/// declares `bool HasSSE1 = Subtarget.hasSSE1();` and lets rules refer to it as
/// plain `HasSSE1`.
class LegalizeFeature<string cond> : LegalizeBoolPredicate {
  string Cond = cond;
}

/// A combination of leaves.  Operators come from TargetSubtarget.td, plus the
/// `not` defined above.
class LegalizePredicateDag<dag cond> : LegalizeBoolPredicate {
  dag CondDag = cond;
}

/// A cached boolean that cannot be modelled as AST nodes, e.g. a comparison.
/// -gen-legalizer rejects one whose text is merely a boolean combination of
/// declared features; those must be written with all_of / any_of / not, so
/// that the escape hatch stays an escape hatch.
class LegalizeOpaqueBool<string expr> : LegalizeBoolPredicate {
  string Expr = expr;
}

/// A query-time predicate: a C++ expression that yields a callable, e.g.
/// "scalarOrEltWiderThan(0, 64)".  This is what scalarizeIf() takes, and it is
/// a different C++ type from LegalizeBoolPredicate, so a cached bool here is
/// rejected while the .td is parsed instead of failing to compile later.
class LegalizeQueryPredicate<string expr> : LegalizePredicate {
  string Expr = expr;
}

//===----------------------------------------------------------------------===//
// Rule arguments
//
// One class per shape a builder argument can take.  A sugar class declares
// which shapes it accepts, so the shape is checked when the .td is parsed
// instead of surfacing as an undeclared identifier in the generated .inc.
// The record name of a LegalizeNamedType is the C++ identifier:
//
//   def s32 : LegalizeNamedType<"LLT::scalar(32)">;
//
// and a rule refers to it as plain `s32`.
//===----------------------------------------------------------------------===//

class LegalizeArg;

/// Anything that fits in a single type position: a named type, or a C++
/// expression that yields one.
class LegalizeArgType : LegalizeArg;

/// A named type: `s32`.  The record name is the C++ identifier, and emitTypes()
/// declares it once ahead of the rules.
class LegalizeNamedType<string expr> : LegalizeArgType {
  string Expr = expr;
}

/// A type position that holds a C++ expression, e.g. `HasSSE2 ? s64 : s32`.
/// Unlike a named type it declares nothing: the text is written straight into
/// the builder call, so it has to be a single expression.
class LegalizeInlineType<string text> : LegalizeArgType {
  string Text = text;
}

/// A type list in a builder argument position.  A named list and an inline list
/// are both initializer_list<LLT>-shaped, so both are legal wherever a list
/// argument is expected.  A choice between two named lists is the one case that
/// cannot carry braces, because which list it names is decided at run time.
class LegalizeArgTypeList : LegalizeArg;

/// `{s8, s16}`, spelled as a list and braced by the emitter.
class LegalizeInlineTypeList<list<LegalizeNamedType> elems>
    : LegalizeArgTypeList {
  list<LegalizeNamedType> Elems = elems;
}

/// A named type list, for builders that take an initializer_list:
///   const std::initializer_list<LLT> PtrTypes32 = {s1, s8, s16, s32};
/// Also legal in a list position, where the emitter passes it by name.
class LegalizeNamedTypeList<list<LegalizeNamedType> elems>
    : LegalizeArgTypeList {
  list<LegalizeNamedType> Elems = elems;
}

/// `Is64Bit ? PtrTypes64 : PtrTypes32`: one of two named lists, chosen at run
/// time.  Only needed when the choice is a run-time condition.
class LegalizeTypeListChoice<string text> : LegalizeArgTypeList {
  string Text = text;
}

/// Two types that must be legal together: rendered `s32, s32` as one entry of
/// a {{...}, {...}} list.
class LegalizeTypePair<LegalizeNamedType type0, LegalizeNamedType type1>
    : LegalizeArg {
  LegalizeNamedType Type0 = type0;
  LegalizeNamedType Type1 = type1;
}

/// One LegalityPredicates::TypePairAndMemDesc entry (LegalizerInfo.h:167):
/// {Type0, Type1, MemTy, Align}.  Align is in bits and is an integer, which is
/// why a memdesc tuple is not a list of four types.
class LegalizeMemDesc<LegalizeNamedType type0, LegalizeNamedType type1,
                      LegalizeNamedType memTy, int align> : LegalizeArg {
  LegalizeNamedType Type0 = type0;
  LegalizeNamedType Type1 = type1;
  LegalizeNamedType MemTy = memTy;
  int Align = align;
}

/// A literal integer in a builder argument position.
///
/// The number alone does not say what it means: `clampScalar(1, ...)` is a type
/// slot, `widenScalarToNextPow2(0, 16)` is a bit width, and
/// `clampMinNumElements(0, s8, 8)` is a lane count.  All three used to be one
/// class, so a lane count could be written where a slot index belongs and the
/// only symptom was a wrong rule at run time.  The three subclasses below are
/// what the builder signatures are checked against; the value and its rendering
/// stay identical.
class LegalizeArgInt<int value, string comment = ""> : LegalizeArg {
  int Value = value;

  /// Rendered as `/*Name=*/` in front of the value, for builders where a bare
  /// number would not say what it means.
  string Comment = comment;
}

/// A type slot index: which operand's type the rule looks at.
class LegalizeArgIndex<int value> : LegalizeArgInt<value>;

/// A bit width, e.g. the `Min` of widenScalarToNextPow2.  Rendered with the
/// `/*Min=*/` comment every bit-width position in the C++ API carries.
class LegalizeArgBits<int value> : LegalizeArgInt<value, "Min">;

/// A lane count.
class LegalizeArgLanes<int value, string comment = ""> : LegalizeArgInt<value, comment>;

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
// Most of these are LegalizeActions::LegalizeAction values.  ClampScalar and
// ClampNumElements are not: they are composites that expand to more than one
// applied action (LegalizerInfo.h: clampScalar and clampScalarOrElt ->
// minScalar + maxScalar, clampNumElements -> clampMinNumElements +
// clampMaxNumElements), so the rule picks a direction at query time and they
// accept either one.  ScalarSameSizeAs is a composite too, but its direction is
// always the same on both sides, so it states "neutral" rather than "any".
//
// Unsupported has no entry yet: unsupported() and unsupportedIf() exist in the
// C++ API (LegalizerInfo.h:875) but the schema does not model them.
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
def ActionScalarSameSizeAs : LegalizeAction<"ScalarSameSizeAs", "neutral">;
def ActionClampNumElements : LegalizeAction<"ClampNumElements", "any">;
def ActionUnsupported     : LegalizeAction<"Unsupported", "">;

//===----------------------------------------------------------------------===//
// Rules
//===----------------------------------------------------------------------===//
/// The shape a builder argument position accepts.
///
/// A builder without a signature accepts any argument list, which is how
/// `.clampScalar({s32})` and `.lower({s32})` used to be emitted from a schema
/// that knew better: nothing compared the arguments the rule carried against
/// the arguments the builder declares.  A Formals list is that comparison, one
/// entry per position.
class LegalizeArgShape;

/// One type: a named type, an inline type expression, or a named type list.
def ShapeType : LegalizeArgShape;
/// A list of type pairs, `{{s32, s32}, ...}`.
def ShapeTypePairList : LegalizeArgShape;
/// A list of memdesc tuples, `{{s32, s32, s8, 8}, ...}`.
def ShapeMemDescList : LegalizeArgShape;
/// A type slot index.
def ShapeIndex : LegalizeArgShape;
/// A bit width.
def ShapeBits : LegalizeArgShape;
/// A lane count: a literal, or C++ text when the count depends on features.
def ShapeLanes : LegalizeArgShape;
/// A type list, for the cartesian-product builders.
def ShapeTypeList : LegalizeArgShape;

/// The kind of guard a builder accepts.
class LegalizeGuardKind;
/// `bool Pred` computed once in the target constructor.
def GuardBool : LegalizeGuardKind;
/// `LegalityPredicate`, evaluated per query.
def GuardQuery : LegalizeGuardKind;

/// The builder a rule stands for.  A record rather than a string: the string
/// form let a misspelling (legalForr) through TableGen and through
/// -gen-legalizer, producing C++ that only failed hours later in a full build,
/// with a diagnostic pointing at LegalizerInfo.h instead of the .td line.  Both
/// backends also had to agree, separately, on what each spelling meant; that is
/// how the memdesc expansion came to disagree with the builder it described.
///
/// A builder is now something a rule names, so a name that does not exist is a
/// TableGen error at parse time.
class LegalizeBuilder<string name, LegalizeAction action> {
  /// C++ spelling, written verbatim by -gen-legalizer.
  string Name = name;

  /// The action this builder implements.  It lives here, not on the rule: the
  /// builder keyword already decides the action, so a rule that also named one
  /// could only repeat it or contradict it, and the two emitters read different
  /// fields, so a contradiction produced two different answers for the same
  /// sentence (`.lower()` in C++, {"action":"Lower"} in the Core IR) while
  /// both ignored what the rule said.
  LegalizeAction Action = action;

  /// The guard overloads the C++ builder actually declares.
  ///
  /// There are two, and they take different C++ types:
  ///   legalFor(bool Pred, initializer_list<LLT>)   -- a cached bool
  ///   legalIf(const LegalityPredicate &)           -- a callable
  /// A builder may have neither, one, or (legalFor) both.  Saying which is
  /// what stops a rule from emitting `.lower(HasSSE1, )`, which no overload
  /// declares but which the emitter happily wrote.
  list<LegalizeGuardKind> Guards = [];

  /// The argument positions this builder declares.
  ///
  /// One inner list per overload: an empty inner list means the builder takes
  /// no arguments, and a builder with two overloads (legalFor takes either a
  /// type list or a list of pairs) lists both, because the schema models a
  /// rule rather than a call and either one is a legal rule.
  list<list<LegalizeArgShape>> Formals = [[]];
}

def BuilderLegalFor                      : LegalizeBuilder<"legalFor", ActionLegal> {
  let Guards = [GuardBool];
  let Formals = [[ShapeType], [ShapeTypePairList]];
}

def BuilderLegalForCartesianProduct      : LegalizeBuilder<"legalForCartesianProduct", ActionLegal> {
  let Guards = [];
  let Formals = [[ShapeTypeList, ShapeTypeList]];
}
def BuilderLegalForTypesWithMemDesc      : LegalizeBuilder<"legalForTypesWithMemDesc", ActionLegal> {
  let Guards = [GuardBool];
  let Formals = [[ShapeMemDescList]];
}

def BuilderLower                         : LegalizeBuilder<"lower", ActionLower> {
  let Guards = [];
  let Formals = [[]];
}
def BuilderLowerFor                      : LegalizeBuilder<"lowerFor", ActionLower> {
  let Guards = [GuardBool];
  let Formals = [[ShapeTypePairList]];
}

def BuilderLibcall                       : LegalizeBuilder<"libcall", ActionLibcall> {
  let Guards = [];
  let Formals = [[]];
}
def BuilderLibcallFor                    : LegalizeBuilder<"libcallFor", ActionLibcall> {
  let Guards = [GuardBool];
  let Formals = [[ShapeType]];
}

def BuilderCustomFor                     : LegalizeBuilder<"customFor", ActionCustom> {
  let Guards = [GuardBool];
  let Formals = [[ShapeType]];
}

def BuilderCustomForCartesianProduct     : LegalizeBuilder<"customForCartesianProduct", ActionCustom> {
  let Guards = [GuardBool];
  let Formals = [[ShapeTypeList, ShapeTypeList]];
}

def BuilderAlwaysLegal                   : LegalizeBuilder<"alwaysLegal", ActionLegal> {
  let Guards = [];
  let Formals = [[]];
}
def BuilderClampScalar                   : LegalizeBuilder<"clampScalar", ActionClampScalar> {
  let Guards = [GuardBool];
  let Formals = [[ShapeIndex, ShapeType, ShapeType]];
}

def BuilderClampScalarOrElt              : LegalizeBuilder<"clampScalarOrElt", ActionClampScalar> {
  let Guards = [];
  let Formals = [[ShapeIndex, ShapeType, ShapeType]];
}
def BuilderWidenScalarToNextPow2         : LegalizeBuilder<"widenScalarToNextPow2", ActionWidenScalar> {
  let Guards = [];
  let Formals = [[ShapeIndex], [ShapeIndex, ShapeBits]];
}
def BuilderWidenScalarOrEltToNextPow2    : LegalizeBuilder<"widenScalarOrEltToNextPow2", ActionWidenScalar> {
  let Guards = [];
  let Formals = [[ShapeIndex], [ShapeIndex, ShapeBits]];
}
def BuilderMinScalar                     : LegalizeBuilder<"minScalar", ActionWidenScalar> {
  let Guards = [GuardBool];
  let Formals = [[ShapeIndex, ShapeType]];
}

def BuilderMaxScalar                     : LegalizeBuilder<"maxScalar", ActionNarrowScalar> {
  let Guards = [];
  let Formals = [[ShapeIndex, ShapeType]];
}
def BuilderScalarSameSizeAs              : LegalizeBuilder<"scalarSameSizeAs", ActionScalarSameSizeAs> {
  let Guards = [];
  let Formals = [[ShapeIndex, ShapeIndex]];
}
def BuilderScalarize                     : LegalizeBuilder<"scalarize", ActionFewerElements> {
  let Guards = [];
  let Formals = [[ShapeIndex]];
}
def BuilderMoreElementsToNextPow2        : LegalizeBuilder<"moreElementsToNextPow2", ActionMoreElements> {
  let Guards = [];
  let Formals = [[ShapeIndex]];
}
def BuilderClampNumElements              : LegalizeBuilder<"clampNumElements", ActionClampNumElements> {
  let Guards = [];
  let Formals = [[ShapeIndex, ShapeType, ShapeType]];
}
def BuilderClampMinNumElements           : LegalizeBuilder<"clampMinNumElements", ActionMoreElements> {
  let Guards = [];
  let Formals = [[ShapeIndex, ShapeType, ShapeLanes]];
}
def BuilderClampMaxNumElements           : LegalizeBuilder<"clampMaxNumElements", ActionFewerElements> {
  let Guards = [];
  let Formals = [[ShapeIndex, ShapeType, ShapeLanes]];
}
def BuilderLegalIf                       : LegalizeBuilder<"legalIf", ActionLegal> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderLowerIf                       : LegalizeBuilder<"lowerIf", ActionLower> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderCustomIf                      : LegalizeBuilder<"customIf", ActionCustom> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderWidenScalarIf                 : LegalizeBuilder<"widenScalarIf", ActionWidenScalar> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderNarrowScalarIf                : LegalizeBuilder<"narrowScalarIf", ActionNarrowScalar> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderLibcallIf                     : LegalizeBuilder<"libcallIf", ActionLibcall> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderFewerElementsIf               : LegalizeBuilder<"fewerElementsIf", ActionFewerElements> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderMoreElementsIf                : LegalizeBuilder<"moreElementsIf", ActionMoreElements> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderMinScalarIf                   : LegalizeBuilder<"minScalarIf", ActionWidenScalar> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeIndex, ShapeType]];
}

def BuilderMaxScalarIf                   : LegalizeBuilder<"maxScalarIf", ActionNarrowScalar> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeIndex, ShapeType]];
}

def BuilderScalarizeIf                   : LegalizeBuilder<"scalarizeIf", ActionFewerElements> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeIndex]];
}

def BuilderUnsupported                   : LegalizeBuilder<"unsupported", ActionUnsupported> {
  let Guards = [];
  let Formals = [[]];
}
def BuilderUnsupportedFor                : LegalizeBuilder<"unsupportedFor", ActionUnsupported> {
  let Guards = [];
  let Formals = [[ShapeType]];
}
def BuilderUnsupportedIf                 : LegalizeBuilder<"unsupportedIf", ActionUnsupported> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

class LegalizeRule {
  /// Builder this rule stands for.  Every rule names one: a record cannot be
  /// misspelled into a different builder the way a string could, and the
  /// builder decides the action, so a rule cannot contradict it.
  LegalizeBuilder Builder = ?;

  /// Builder arguments.  A list that is entirely types, type pairs or memdesc
  /// tuples is emitted inside one `{...}`; anything else is a comma list, with
  /// each argument rendering itself.
  list<LegalizeArg> Args = [];

  /// Guard, lowered by -gen-legalizer.  Only builders with TakesGuard accept
  /// one; the rest take no guard at all.
  LegalizePredicate Pred = ?;

  /// When non-empty, the rule body is this C++ text, emitted verbatim.  Used
  /// for guards that are still raw lambdas.
  code OpaqueArgs = "";

  /// Mutation direction this rule performs.  Inherited from the builder's
  /// action; the few builders that pick a direction at query time override it.
  string Mutation = "";
}

/// A rule built from a builder and its arguments.  The action comes from the
/// builder, so nothing here can disagree with it: before this, the rule named
/// its own action and both emitters ignored it, so a rule could say "Legal"
/// while emitting .lower() and while the Core IR said Lower.
class LegalizeCall<LegalizeBuilder builder, list<LegalizeArg> args>
    : LegalizeRule {
  let Builder = builder;
  let Args = args;
  let Mutation = builder.Action.Mutation;
}

/// The same, with a cached-boolean guard.  Only builders that take a guard may
/// be reached this way; the rest have no such overload, and saying otherwise
/// used to emit `.lower(HasSSE1, )`, which no builder declares.
class LegalizeGuardedCall<LegalizeBuilder builder, LegalizeBoolPredicate P,
                          list<LegalizeArg> args> : LegalizeRule {
  let Builder = builder;
  let Pred = P;
  let Args = args;
  let Mutation = builder.Action.Mutation;
}

/// The same, with a query-time guard.  scalarizeIf() takes a LegalityPredicate,
/// which is a different C++ type from the cached bool the other guards use, so
/// it gets its own base class instead of widening LegalizeGuardedCall.
class LegalizeQueryGuardedCall<LegalizeBuilder builder, LegalizeQueryPredicate P,
                               list<LegalizeArg> args> : LegalizeRule {
  let Builder = builder;
  let Pred = P;
  let Args = args;
  let Mutation = builder.Action.Mutation;
}

/// A rule whose guard is still raw C++, for a builder that takes one.
class LegalizeOpaqueRule<LegalizeBuilder builder, code body> : LegalizeRule {
  let Builder = builder;
  let OpaqueArgs = body;
  let Mutation = builder.Action.Mutation;
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
    : LegalizeCall<BuilderLegalFor, Types>;

class LegalForIf<LegalizeBoolPredicate P, list<LegalizeArgType> Types>
    : LegalizeGuardedCall<BuilderLegalFor, P, Types>;

class LegalForPairs<list<LegalizeTypePair> Pairs>
    : LegalizeCall<BuilderLegalFor, Pairs>;

class LegalForPairsIf<LegalizeBoolPredicate P, list<LegalizeTypePair> Pairs>
    : LegalizeGuardedCall<BuilderLegalFor, P, Pairs>;

class LegalForCartesianProduct<LegalizeArgTypeList T0, LegalizeArgTypeList T1>
    : LegalizeCall<BuilderLegalForCartesianProduct, [T0, T1]>;

class LowerForPairsIf<LegalizeBoolPredicate P, list<LegalizeTypePair> Pairs>
    : LegalizeGuardedCall<BuilderLowerFor, P, Pairs>;

class LegalForMemDesc<list<LegalizeMemDesc> Descs>
    : LegalizeCall<BuilderLegalForTypesWithMemDesc, Descs>;

class LegalForMemDescIf<LegalizeBoolPredicate P, list<LegalizeMemDesc> Descs>
    : LegalizeGuardedCall<BuilderLegalForTypesWithMemDesc, P, Descs>;

def Lower : LegalizeCall<BuilderLower, []>;

def Libcall : LegalizeCall<BuilderLibcall, []>;

class LibcallFor<list<LegalizeArgType> Types>
    : LegalizeCall<BuilderLibcallFor, Types>;

class CustomFor<list<LegalizeArgType> Types>
    : LegalizeCall<BuilderCustomFor, Types>;

class CustomForCartesianProductIf<LegalizeBoolPredicate P,
                                  LegalizeArgTypeList T0,
                                  LegalizeArgTypeList T1>
    : LegalizeGuardedCall<BuilderCustomForCartesianProduct, P, [T0, T1]>;

def AlwaysLegal : LegalizeCall<BuilderAlwaysLegal, []>;

/// "This operation cannot be done on this target", stated rather than implied.
///
/// Without these the only way to say it was to omit every rule and let the
/// fallthrough produce Unsupported, which is a different observation: the Core
/// IR keeps "no rule matched" (result: no_match) apart from "a rule said
/// Unsupported" (result: rule, action: Unsupported), because the two are
/// different facts about the target.  The schema and the evaluator already
/// modelled both; only the .td could not write the second one.
class UnsupportedFor<list<LegalizeArgType> Types>
    : LegalizeCall<BuilderUnsupportedFor, Types>;

/// unsupportedIf() takes a LegalityPredicate, not a cached bool: it is the
/// query-time guard, like scalarizeIf().
class UnsupportedIf<LegalizeQueryPredicate P>
    : LegalizeQueryGuardedCall<BuilderUnsupportedIf, P, []>;

def Unsupported : LegalizeCall<BuilderUnsupported, []>;

class ClampScalar<int Idx, LegalizeArgType MinTy, LegalizeArgType MaxTy>
    : LegalizeCall<BuilderClampScalar,
                   [LegalizeArgIndex<Idx>, MinTy, MaxTy]>;

class ClampScalarOrElt<int Idx, LegalizeArgType MinTy,
                       LegalizeArgType MaxTy>
    : LegalizeCall<BuilderClampScalarOrElt,
                   [LegalizeArgIndex<Idx>, MinTy, MaxTy]>;

/// Min = 0 means "no minimum": a zero-bit minimum is not meaningful, so the
/// sentinel is unambiguous and the argument stays an integer.
class WidenScalarToNextPow2<int Idx, int Min = 0>
    : LegalizeCall<BuilderWidenScalarToNextPow2,
                   !if(!eq(Min, 0), [LegalizeArgIndex<Idx>],
                       [LegalizeArgIndex<Idx>, LegalizeArgBits<Min>])>;

class WidenScalarOrEltToNextPow2<int Idx, int Min = 0>
    : LegalizeCall<BuilderWidenScalarOrEltToNextPow2,
                   !if(!eq(Min, 0), [LegalizeArgIndex<Idx>],
                       [LegalizeArgIndex<Idx>, LegalizeArgBits<Min>])>;

class MinScalar<int Idx, LegalizeArgType Ty>
    : LegalizeCall<BuilderMinScalar, [LegalizeArgIndex<Idx>, Ty]>;

class MaxScalar<int Idx, LegalizeArgType Ty>
    : LegalizeCall<BuilderMaxScalar, [LegalizeArgIndex<Idx>, Ty]>;

class ScalarSameSizeAs<int Idx, int OtherIdx>
    : LegalizeCall<BuilderScalarSameSizeAs,
                   [LegalizeArgIndex<Idx>, LegalizeArgIndex<OtherIdx>]>;

class Scalarize<int Idx>
    : LegalizeCall<BuilderScalarize, [LegalizeArgIndex<Idx>]>;

class ScalarizeIf<LegalizeQueryPredicate P, int Idx>
    : LegalizeQueryGuardedCall<BuilderScalarizeIf, P, [LegalizeArgIndex<Idx>]>;

class MoreElementsToNextPow2<int Idx>
    : LegalizeCall<BuilderMoreElementsToNextPow2, [LegalizeArgIndex<Idx>]>;

class ClampNumElements<int Idx, LegalizeArgType EltTy,
                       LegalizeArgType MaxTy>
    : LegalizeCall<BuilderClampNumElements,
                   [LegalizeArgIndex<Idx>, EltTy, MaxTy]>;

/// The element count is a C++ integer expression: targets routinely compute it
/// from features (`HasAVX2 ? 8 : 4`), so it stays text.  The index and the
/// element type are still checked.
///
/// The direction is not "any": clampMinNumElements() raises a too-small vector
/// (MoreElements -> merge) and clampMaxNumElements() splits a too-large one
/// (FewerElements -> split).  Only clampNumElements() is the composite that
/// picks a direction at query time.
class ClampMinNumElements<int Idx, LegalizeArgType EltTy, string Num>
    : LegalizeCall<BuilderClampMinNumElements,
                   [LegalizeArgIndex<Idx>, EltTy, LegalizeRawArg<Num>]>;

class ClampMaxNumElements<int Idx, LegalizeArgType EltTy, string Num>
    : LegalizeCall<BuilderClampMaxNumElements,
                   [LegalizeArgIndex<Idx>, EltTy, LegalizeRawArg<Num>]>;

//===----------------------------------------------------------------------===//

// Rule sets
//===----------------------------------------------------------------------===//

class LegalizeRuleSet<list<string> opcodes> {
  list<string> Opcodes = opcodes;
  list<LegalizeRule> Rules = [];
  list<string> Comments = [];
}
