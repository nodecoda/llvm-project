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

/// A guard that is a callable evaluated per query, e.g. typeIs(0, s32).
class LegalizeQueryPredicateBase : LegalizePredicate;

/// A query-time predicate written as C++ text.  A different C++ type from
/// LegalizeBoolPredicate, so a cached bool here is rejected while the .td is
/// parsed instead of failing to compile later.
class LegalizeQueryPredicate<string expr> : LegalizeQueryPredicateBase {
  string Expr = expr;
}


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

/// A combination of cached bools: features only.  Renders to a plain `&&` /
/// `||` expression, which is what `legalFor(bool, ...)` takes.
class LegalizeBoolPredicateDag<dag cond> : LegalizeBoolPredicate {
  dag CondDag = cond;
}

/// A combination that may read the query.  Renders to a lambda, which is what
/// `customIf()`, `lowerIf()` and the rest take.
class LegalizeQueryPredicateDag<dag cond> : LegalizeQueryPredicateBase {
  dag CondDag = cond;
}

/// The old name, kept so the existing feature-only dags do not have to be
/// rewritten in the same change that introduces the split.
class LegalizePredicateDag<dag cond> : LegalizeBoolPredicateDag<cond>;

/// A cached boolean that cannot be modelled as AST nodes, e.g. a comparison.
/// -gen-legalizer rejects one whose text is merely a boolean combination of
/// declared features; those must be written with all_of / any_of / not, so
/// that the escape hatch stays an escape hatch.
class LegalizeOpaqueBool<string expr> : LegalizeBoolPredicate {
  string Expr = expr;
}

/// A leaf that is a query on the instruction rather than a subtarget feature:
/// typeIs(0, s32), scalarNarrowerThan(1, 64), and the rest of the predicates
/// LegalizerInfo.h exposes.
///
/// This is a *query* predicate, not a cached bool, and the difference is not
/// cosmetic: `typeIs(0, s32)` is a callable, so it cannot appear in
/// `legalFor(bool Pred, ...)`.  Putting it in a dag makes that dag a query
/// predicate too -- which is why the four classes below are split in two.
class LegalizeTypePredicate<string call> : LegalizeQueryPredicateBase {
  string Call = call;
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

/// A type chosen by a priority list of cached-boolean guards:
///
///   def sMaxScalar
///       : LegalizeSelectType<[Is64Bit], [s64], s32>;
///
/// It is still a named type: -gen-legalizer declares one LLT constant whose
/// initializer is the conditional, so every position that accepts a named type
/// accepts it without a signature change.  Core IR keeps the guards as a select
/// node; folding them at build time would pick the type of whichever
/// configuration generated the file and silently lose the others.
class LegalizeSelectType<list<LegalizeBoolPredicate> conds,
                         list<LegalizeNamedType> types,
                         LegalizeNamedType otherwise>
    : LegalizeNamedType<""> {
  list<LegalizeBoolPredicate> Conds = conds;
  list<LegalizeNamedType> Types = types;
  LegalizeNamedType Otherwise = otherwise;
}

/// A type position that holds a C++ expression that is not a type choice.
/// Unlike a named type it declares nothing: the text is written straight into
/// the builder call, so it has to be a single expression.  A choice between
/// types belongs in `LegalizeSelectType`, which keeps its guards in Core IR.
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

/// `Is64Bit ? PtrTypes64 : PtrTypes32`: one of several named lists, chosen by a
/// priority list of cached-boolean guards.
class LegalizeSelectTypeList<list<LegalizeBoolPredicate> conds,
                             list<LegalizeNamedTypeList> lists,
                             LegalizeNamedTypeList otherwise>
    : LegalizeArgTypeList {
  list<LegalizeBoolPredicate> Conds = conds;
  list<LegalizeNamedTypeList> Lists = lists;
  LegalizeNamedTypeList Otherwise = otherwise;
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
class LegalizeTypeTriple<LegalizeNamedType type0, LegalizeNamedType type1,
                         LegalizeNamedType type2> : LegalizeArg {
  LegalizeNamedType Type0 = type0;
  LegalizeNamedType Type1 = type1;
  LegalizeNamedType Type2 = type2;
}

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

/// A lane count chosen by a priority list of cached-boolean guards:
///
///   LegalizeSelectLanes<[HasBWI, HasAVX2], [64, 32], 16>
///
/// is the structured form of `HasBWI ? 64 : HasAVX2 ? 32 : 16`.  Like the
/// select type, the guard list stays in Core IR instead of being folded.
class LegalizeSelectLanes<list<LegalizeBoolPredicate> conds,
                          list<int> values, int otherwise> : LegalizeArg {
  list<LegalizeBoolPredicate> Conds = conds;
  list<int> Values = values;
  int Otherwise = otherwise;
}

/// C++ text that cannot be typed.  An escape hatch that is meant to shrink,
/// not to grow: no rule uses it now that element counts are selects and type
/// lists are named.  It stays for the next argument the vocabulary cannot
/// type, like `LegalizeOpaqueRule` for guards.
class LegalizeRawArg<string text> : LegalizeArg {
  string Text = text;
}

/// A bit width chosen by a priority list of cached-boolean guards, e.g.
/// `HasFP16 ? 16 : 32`.  Like LegalizeSelectLanes, the guards stay structured.
class LegalizeSelectBits<list<LegalizeBoolPredicate> conds,
                         list<int> values, int otherwise> : LegalizeArg {
  list<LegalizeBoolPredicate> Conds = conds;
  list<int> Values = values;
  int Otherwise = otherwise;
}

/// A mutation written as C++ text: a lambda, or a call such as
/// `changeElementTo(0, AArch64_f32)`.  The builder still decides the action and
/// the guard is still a predicate record; only the mutation body is opaque.
class LegalizeRawMutation<string text> : LegalizeArg {
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
/// Not an action: immIdx() marks an immediate operand as covered for the
/// coverage verifier and produces no legalization step.  It gets an action
/// record only so that it can be a rule the emitters can spell.
def ActionCoverage        : LegalizeAction<"Coverage", "">;

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
/// A list of three-type tuples, `{{s16, v8s8, s64}, ...}`.
def ShapeTypeTripleList : LegalizeArgShape;
/// A type slot index.
def ShapeIndex : LegalizeArgShape;
/// A bit width.
def ShapeBits : LegalizeArgShape;
/// A lane count: a literal, or C++ text when the count depends on features.
def ShapeLanes : LegalizeArgShape;
/// A type list, for the cartesian-product builders.
def ShapeTypeList : LegalizeArgShape;
/// A mutation written as C++ text.  It is still a mutation argument, so the
/// builder signature checks it, but the schema cannot type its body.
def ShapeRawMutation : LegalizeArgShape;

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
  let Formals = [[ShapeType], [ShapeTypeList], [ShapeTypePairList],
                 [ShapeTypeTripleList]];
}

def BuilderLegalForCartesianProduct      : LegalizeBuilder<"legalForCartesianProduct", ActionLegal> {
  let Guards = [];
  let Formals = [[ShapeTypeList], [ShapeTypeList, ShapeTypeList],
                 [ShapeTypeList, ShapeTypeList, ShapeTypeList]];
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
  let Formals = [[ShapeType], [ShapeTypePairList]];
}

def BuilderLibcall                       : LegalizeBuilder<"libcall", ActionLibcall> {
  let Guards = [];
  let Formals = [[]];
}
def BuilderLibcallFor                    : LegalizeBuilder<"libcallFor", ActionLibcall> {
  let Guards = [GuardBool];
  let Formals = [[ShapeType], [ShapeTypePairList]];
}

def BuilderCustomFor                     : LegalizeBuilder<"customFor", ActionCustom> {
  let Guards = [GuardBool];
  let Formals = [[ShapeType], [ShapeTypePairList]];
}

def BuilderCustomForCartesianProduct     : LegalizeBuilder<"customForCartesianProduct", ActionCustom> {
  let Guards = [GuardBool];
  let Formals = [[ShapeTypeList], [ShapeTypeList, ShapeTypeList],
                 [ShapeTypeList, ShapeTypeList, ShapeTypeList]];
}

def BuilderCustom                        : LegalizeBuilder<"custom", ActionCustom> {
  let Guards = [];
  let Formals = [[]];
}

def BuilderBitcastIf                     : LegalizeBuilder<"bitcastIf", ActionBitcast> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeRawMutation]];
}

def BuilderWidenScalarFor                : LegalizeBuilder<"widenScalarFor", ActionWidenScalar> {
  let Guards = [];
  let Formals = [[ShapeTypeList, ShapeRawMutation]];
}

def BuilderWidenScalarOrEltToNextPow2OrMinSize
    : LegalizeBuilder<"widenScalarOrEltToNextPow2OrMinSize", ActionWidenScalar> {
  let Guards = [];
  let Formals = [[ShapeIndex], [ShapeIndex, ShapeBits]];
}

def BuilderWidenVectorEltsToVectorMinSize
    : LegalizeBuilder<"widenVectorEltsToVectorMinSize", ActionWidenScalar> {
  let Guards = [];
  let Formals = [[ShapeIndex, ShapeBits]];
}

def BuilderMinScalarOrElt                : LegalizeBuilder<"minScalarOrElt", ActionWidenScalar> {
  let Guards = [];
  let Formals = [[ShapeIndex, ShapeType]];
}

def BuilderMinScalarOrEltIf              : LegalizeBuilder<"minScalarOrEltIf", ActionWidenScalar> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeIndex, ShapeType]];
}

def BuilderMinScalarSameAs               : LegalizeBuilder<"minScalarSameAs", ActionWidenScalar> {
  let Guards = [];
  let Formals = [[ShapeIndex, ShapeIndex]];
}

def BuilderMinScalarEltSameAsIf          : LegalizeBuilder<"minScalarEltSameAsIf", ActionWidenScalar> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeIndex, ShapeIndex]];
}

def BuilderMaxScalarEltSameAsIf          : LegalizeBuilder<"maxScalarEltSameAsIf", ActionNarrowScalar> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeIndex, ShapeIndex]];
}

def BuilderUnsupportedIfMemSizeNotPow2
    : LegalizeBuilder<"unsupportedIfMemSizeNotPow2", ActionUnsupported> {
  let Guards = [];
  let Formals = [[]];
}

def BuilderLowerIfMemSizeNotPow2
    : LegalizeBuilder<"lowerIfMemSizeNotPow2", ActionLower> {
  let Guards = [];
  let Formals = [[]];
}

def BuilderLowerIfMemSizeNotByteSizePow2
    : LegalizeBuilder<"lowerIfMemSizeNotByteSizePow2", ActionLower> {
  let Guards = [];
  let Formals = [[]];
}

def BuilderImmIdx                        : LegalizeBuilder<"immIdx", ActionCoverage> {
  let Guards = [];
  let Formals = [[ShapeIndex]];
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
  let Formals = [[], [ShapeRawMutation]];
}

def BuilderCustomIf                      : LegalizeBuilder<"customIf", ActionCustom> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderWidenScalarIf                 : LegalizeBuilder<"widenScalarIf", ActionWidenScalar> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeRawMutation]];
}

def BuilderNarrowScalarIf                : LegalizeBuilder<"narrowScalarIf", ActionNarrowScalar> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeRawMutation]];
}

def BuilderLibcallIf                     : LegalizeBuilder<"libcallIf", ActionLibcall> {
  let Guards = [GuardQuery];
  let Formals = [[]];
}

def BuilderFewerElementsIf               : LegalizeBuilder<"fewerElementsIf", ActionFewerElements> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeRawMutation]];
}

def BuilderMoreElementsIf                : LegalizeBuilder<"moreElementsIf", ActionMoreElements> {
  let Guards = [GuardQuery];
  let Formals = [[ShapeRawMutation]];
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
class LegalizeQueryGuardedCall<LegalizeBuilder builder,
                               LegalizeQueryPredicateBase P,
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

class LegalForList<LegalizeArgTypeList Types>
    : LegalizeCall<BuilderLegalFor, [Types]>;

class LegalIf<LegalizeQueryPredicateBase P>
    : LegalizeQueryGuardedCall<BuilderLegalIf, P, []>;

class LowerIf<LegalizeQueryPredicateBase P>
    : LegalizeQueryGuardedCall<BuilderLowerIf, P, []>;

class CustomIf<LegalizeQueryPredicateBase P>
    : LegalizeQueryGuardedCall<BuilderCustomIf, P, []>;

class LibcallIf<LegalizeQueryPredicateBase P>
    : LegalizeQueryGuardedCall<BuilderLibcallIf, P, []>;

class LegalForListIf<LegalizeBoolPredicate P, LegalizeArgTypeList Types>
    : LegalizeGuardedCall<BuilderLegalFor, P, [Types]>;

class LegalForIf<LegalizeBoolPredicate P, list<LegalizeArgType> Types>
    : LegalizeGuardedCall<BuilderLegalFor, P, Types>;

class LegalForPairs<list<LegalizeTypePair> Pairs>
    : LegalizeCall<BuilderLegalFor, Pairs>;

class LegalForPairsIf<LegalizeBoolPredicate P, list<LegalizeTypePair> Pairs>
    : LegalizeGuardedCall<BuilderLegalFor, P, Pairs>;

class LegalForTriples<list<LegalizeTypeTriple> Triples>
    : LegalizeCall<BuilderLegalFor, Triples>;

class LegalForTriplesIf<LegalizeBoolPredicate P,
                        list<LegalizeTypeTriple> Triples>
    : LegalizeGuardedCall<BuilderLegalFor, P, Triples>;

class LegalForCartesianProduct<LegalizeArgTypeList T0, LegalizeArgTypeList T1>
    : LegalizeCall<BuilderLegalForCartesianProduct, [T0, T1]>;

class LegalForCartesianProduct1<LegalizeArgTypeList T0>
    : LegalizeCall<BuilderLegalForCartesianProduct, [T0]>;

class LegalForCartesianProduct3<LegalizeArgTypeList T0, LegalizeArgTypeList T1,
                                LegalizeArgTypeList T2>
    : LegalizeCall<BuilderLegalForCartesianProduct, [T0, T1, T2]>;

class LowerForPairsIf<LegalizeBoolPredicate P, list<LegalizeTypePair> Pairs>
    : LegalizeGuardedCall<BuilderLowerFor, P, Pairs>;

class LowerForIf<LegalizeBoolPredicate P, list<LegalizeArgType> Types>
    : LegalizeGuardedCall<BuilderLowerFor, P, Types>;

class LegalForMemDesc<list<LegalizeMemDesc> Descs>
    : LegalizeCall<BuilderLegalForTypesWithMemDesc, Descs>;

class LegalForMemDescIf<LegalizeBoolPredicate P, list<LegalizeMemDesc> Descs>
    : LegalizeGuardedCall<BuilderLegalForTypesWithMemDesc, P, Descs>;

def Lower : LegalizeCall<BuilderLower, []>;

def Libcall : LegalizeCall<BuilderLibcall, []>;

class LibcallFor<list<LegalizeArgType> Types>
    : LegalizeCall<BuilderLibcallFor, Types>;

class LibcallForIf<LegalizeBoolPredicate P, list<LegalizeArgType> Types>
    : LegalizeGuardedCall<BuilderLibcallFor, P, Types>;

class CustomFor<list<LegalizeArgType> Types>
    : LegalizeCall<BuilderCustomFor, Types>;

class CustomForIf<LegalizeBoolPredicate P, list<LegalizeArgType> Types>
    : LegalizeGuardedCall<BuilderCustomFor, P, Types>;

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
class UnsupportedIf<LegalizeQueryPredicateBase P>
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

/// Both halves of the clamp, with a query-time guard.  The predicate is the
/// one the C++ builder's lambda adds: the slot has to be a scalar narrower
/// (wider) than the bound, and the guard has to hold.
class MinScalarIf<LegalizeQueryPredicateBase P, int Idx, LegalizeArgType Ty>
    : LegalizeQueryGuardedCall<BuilderMinScalarIf, P,
                               [LegalizeArgIndex<Idx>, Ty]>;

class MaxScalarIf<LegalizeQueryPredicateBase P, int Idx, LegalizeArgType Ty>
    : LegalizeQueryGuardedCall<BuilderMaxScalarIf, P,
                               [LegalizeArgIndex<Idx>, Ty]>;

class MaxScalar<int Idx, LegalizeArgType Ty>
    : LegalizeCall<BuilderMaxScalar, [LegalizeArgIndex<Idx>, Ty]>;

class ScalarSameSizeAs<int Idx, int OtherIdx>
    : LegalizeCall<BuilderScalarSameSizeAs,
                   [LegalizeArgIndex<Idx>, LegalizeArgIndex<OtherIdx>]>;

class Scalarize<int Idx>
    : LegalizeCall<BuilderScalarize, [LegalizeArgIndex<Idx>]>;

class ScalarizeIf<LegalizeQueryPredicateBase P, int Idx>
    : LegalizeQueryGuardedCall<BuilderScalarizeIf, P, [LegalizeArgIndex<Idx>]>;

class MoreElementsToNextPow2<int Idx>
    : LegalizeCall<BuilderMoreElementsToNextPow2, [LegalizeArgIndex<Idx>]>;

class ClampNumElements<int Idx, LegalizeArgType EltTy,
                       LegalizeArgType MaxTy>
    : LegalizeCall<BuilderClampNumElements,
                   [LegalizeArgIndex<Idx>, EltTy, MaxTy]>;

/// A literal element count.  The lane-count position is `ShapeLanes`, so the
/// number is typed the same way `WidenScalarToNextPow2`'s bit width is; that is
/// what lets Core IR read the count instead of treating the rule as an escape.
///
/// The direction is not "any": clampMinNumElements() raises a too-small vector
/// (MoreElements -> merge) and clampMaxNumElements() splits a too-large one
/// (FewerElements -> split).  Only clampNumElements() is the composite that
/// picks a direction at query time.
class ClampMinNumElements<int Idx, LegalizeArgType EltTy, int Num>
    : LegalizeCall<BuilderClampMinNumElements,
                   [LegalizeArgIndex<Idx>, EltTy, LegalizeArgLanes<Num>]>;

class ClampMaxNumElements<int Idx, LegalizeArgType EltTy, int Num>
    : LegalizeCall<BuilderClampMaxNumElements,
                   [LegalizeArgIndex<Idx>, EltTy, LegalizeArgLanes<Num>]>;

/// An element count computed from features, as a structured select.  The lane
/// counts stay in Core IR so the table can bind them to the query's subtarget.
class ClampMinNumElementsSel<int Idx, LegalizeArgType EltTy,
                             LegalizeSelectLanes Lanes>
    : LegalizeCall<BuilderClampMinNumElements,
                   [LegalizeArgIndex<Idx>, EltTy, Lanes]>;

class ClampMaxNumElementsSel<int Idx, LegalizeArgType EltTy,
                             LegalizeSelectLanes Lanes>
    : LegalizeCall<BuilderClampMaxNumElements,
                   [LegalizeArgIndex<Idx>, EltTy, Lanes]>;

def Custom : LegalizeCall<BuilderCustom, []>;

class BitcastIf<LegalizeQueryPredicateBase P, LegalizeRawMutation M>
    : LegalizeQueryGuardedCall<BuilderBitcastIf, P, [M]>;

class WidenScalarIf<LegalizeQueryPredicateBase P, LegalizeRawMutation M>
    : LegalizeQueryGuardedCall<BuilderWidenScalarIf, P, [M]>;

class NarrowScalarIf<LegalizeQueryPredicateBase P, LegalizeRawMutation M>
    : LegalizeQueryGuardedCall<BuilderNarrowScalarIf, P, [M]>;

class MoreElementsIf<LegalizeQueryPredicateBase P, LegalizeRawMutation M>
    : LegalizeQueryGuardedCall<BuilderMoreElementsIf, P, [M]>;

class FewerElementsIf<LegalizeQueryPredicateBase P, LegalizeRawMutation M>
    : LegalizeQueryGuardedCall<BuilderFewerElementsIf, P, [M]>;

class LowerIfRaw<LegalizeQueryPredicateBase P, LegalizeRawMutation M>
    : LegalizeQueryGuardedCall<BuilderLowerIf, P, [M]>;

class WidenScalarFor<LegalizeArgTypeList Types, LegalizeRawMutation M>
    : LegalizeCall<BuilderWidenScalarFor, [Types, M]>;

class LowerFor<list<LegalizeArgType> Types>
    : LegalizeCall<BuilderLowerFor, Types>;

class LowerForPairs<list<LegalizeTypePair> Pairs>
    : LegalizeCall<BuilderLowerFor, Pairs>;

class LibcallForPairs<list<LegalizeTypePair> Pairs>
    : LegalizeCall<BuilderLibcallFor, Pairs>;

class LibcallForPairsIf<LegalizeBoolPredicate P, list<LegalizeTypePair> Pairs>
    : LegalizeGuardedCall<BuilderLibcallFor, P, Pairs>;

class CustomForPairs<list<LegalizeTypePair> Pairs>
    : LegalizeCall<BuilderCustomFor, Pairs>;

class CustomForPairsIf<LegalizeBoolPredicate P, list<LegalizeTypePair> Pairs>
    : LegalizeGuardedCall<BuilderCustomFor, P, Pairs>;

class CustomForCartesianProduct<LegalizeArgTypeList T0, LegalizeArgTypeList T1>
    : LegalizeCall<BuilderCustomForCartesianProduct, [T0, T1]>;

class CustomForCartesianProduct1<LegalizeArgTypeList T0>
    : LegalizeCall<BuilderCustomForCartesianProduct, [T0]>;

class CustomForCartesianProduct3<LegalizeArgTypeList T0, LegalizeArgTypeList T1,
                                 LegalizeArgTypeList T2>
    : LegalizeCall<BuilderCustomForCartesianProduct, [T0, T1, T2]>;

class WidenScalarOrEltToNextPow2OrMinSize<int Idx, int Min = 0>
    : LegalizeCall<BuilderWidenScalarOrEltToNextPow2OrMinSize,
                   !if(!eq(Min, 0), [LegalizeArgIndex<Idx>],
                       [LegalizeArgIndex<Idx>, LegalizeArgBits<Min>])>;

class WidenScalarOrEltToNextPow2OrMinSizeSel<int Idx, LegalizeSelectBits Min>
    : LegalizeCall<BuilderWidenScalarOrEltToNextPow2OrMinSize,
                   [LegalizeArgIndex<Idx>, Min]>;

class WidenVectorEltsToVectorMinSize<int Idx, int Size>
    : LegalizeCall<BuilderWidenVectorEltsToVectorMinSize,
                   [LegalizeArgIndex<Idx>, LegalizeArgBits<Size>]>;

class MinScalarOrElt<int Idx, LegalizeArgType Ty>
    : LegalizeCall<BuilderMinScalarOrElt, [LegalizeArgIndex<Idx>, Ty]>;

class MinScalarOrEltIf<LegalizeQueryPredicateBase P, int Idx,
                       LegalizeArgType Ty>
    : LegalizeQueryGuardedCall<BuilderMinScalarOrEltIf, P,
                               [LegalizeArgIndex<Idx>, Ty]>;

class MinScalarSameAs<int Idx, int OtherIdx>
    : LegalizeCall<BuilderMinScalarSameAs,
                   [LegalizeArgIndex<Idx>, LegalizeArgIndex<OtherIdx>]>;

class MinScalarEltSameAsIf<LegalizeQueryPredicateBase P, int Idx, int OtherIdx>
    : LegalizeQueryGuardedCall<BuilderMinScalarEltSameAsIf, P,
                               [LegalizeArgIndex<Idx>, LegalizeArgIndex<OtherIdx>]>;

class MaxScalarEltSameAsIf<LegalizeQueryPredicateBase P, int Idx, int OtherIdx>
    : LegalizeQueryGuardedCall<BuilderMaxScalarEltSameAsIf, P,
                               [LegalizeArgIndex<Idx>, LegalizeArgIndex<OtherIdx>]>;

def UnsupportedIfMemSizeNotPow2
    : LegalizeCall<BuilderUnsupportedIfMemSizeNotPow2, []>;
def LowerIfMemSizeNotPow2 : LegalizeCall<BuilderLowerIfMemSizeNotPow2, []>;
def LowerIfMemSizeNotByteSizePow2
    : LegalizeCall<BuilderLowerIfMemSizeNotByteSizePow2, []>;

class ImmIdx<int Idx>
    : LegalizeCall<BuilderImmIdx, [LegalizeArgIndex<Idx>]>;

//===----------------------------------------------------------------------===//

// Rule sets
//===----------------------------------------------------------------------===//

class LegalizeRuleSet<list<string> opcodes> {
  list<string> Opcodes = opcodes;
  list<LegalizeRule> Rules = [];
  list<string> Comments = [];
}

//===----------------------------------------------------------------------===//
// Lowering registry: one entry per (builder, overload).
//
// A builder's expansion is not a function of its action alone: `legalFor` has
// four overloads and `legalForCartesianProduct` three, and they differ in how
// many Core IR rows one rule produces, not in which node kind it produces.
// Keying a registry by node kind therefore cannot describe them.  Each entry
// below names the expansion algorithm, the actions it may emit, and (when the
// overload can produce no row at all) the escape reasons it is allowed to
// report.
//
// `TableTranslation` records whether the C++ rule table can answer for this
// overload today.  A zero means the table evaluator falls back to the rule
// chain for the whole rule set; it is data so that the fallback is counted
// rather than silent.  P1 flips the remaining zeroes to one.
//
// The emitter checks, for every target and before emitting anything:
//   1. builder defs <-> entries, one entry per declared overload;
//   2. every emitted action is declared by the entry;
//   3. the union of declared actions covers every LegalizeAction def;
//   4. every LegalizeExpansion def is known to the expander;
//   5. a rule that escapes names a reason the entry declares (global
//      argument-shape reasons excepted);
//   6. no shim is named unless the shim table declares it.
//===----------------------------------------------------------------------===//

class LegalizeExpansion<string name> {
  /// Spelling used in diagnostics and in the -gen-legalizer-check report.
  string Name = name;
}

/// Exactly one Core IR row per rule.
def ExOne : LegalizeExpansion<"ExOne">;
/// One row per type, per pair, or per triple in the argument list.
def ExPerType : LegalizeExpansion<"ExPerType">;
/// One row per point of a cartesian product.
def ExPerPoint : LegalizeExpansion<"ExPerPoint">;
/// Two rows, one per direction (min + max).
def ExClamp2 : LegalizeExpansion<"ExClamp2">;
/// Always zero rows: the payload is C++ the schema cannot literalize.
def ExRawMutation : LegalizeExpansion<"ExRawMutation">;
/// Always zero rows: a coverage marker, not a legalization step.
def ExCoverage : LegalizeExpansion<"ExCoverage">;
/// One row, or zero when the bit width is itself a feature-dependent select.
def ExSelectBits : LegalizeExpansion<"ExSelectBits">;

class LegalizeLowering<LegalizeBuilder builder, int overload,
                       LegalizeExpansion expansion,
                       list<LegalizeAction> actions,
                       list<string> escapes = [],
                       bit tableTranslation = 1> {
  LegalizeBuilder Builder = builder;
  int Overload = overload;
  LegalizeExpansion Expansion = expansion;

  /// Actions this overload may emit.  Empty only for the two zero-row
  /// expansions; otherwise the emitter rejects a row with an undeclared action.
  list<LegalizeAction> Actions = actions;

  /// Reasons this overload may report zero rows.  A zero-row result whose
  /// reason is not listed (and is not a global argument-shape reason) is fatal.
  list<string> Escapes = escapes;

  /// False when -gen-legalizer-rule-index cannot answer for this overload and
  /// the reader falls back to the rule chain.
  bit TableTranslation = tableTranslation;
}

def LoweringLegalFor0 : LegalizeLowering<BuilderLegalFor, 0, ExPerType, [ActionLegal]>;
def LoweringLegalFor1 : LegalizeLowering<BuilderLegalFor, 1, ExPerType, [ActionLegal]>;
def LoweringLegalFor2 : LegalizeLowering<BuilderLegalFor, 2, ExPerType, [ActionLegal]>;
def LoweringLegalFor3 : LegalizeLowering<BuilderLegalFor, 3, ExPerType, [ActionLegal]>;
def LoweringLegalForCartesianProduct0 : LegalizeLowering<BuilderLegalForCartesianProduct, 0, ExPerPoint, [ActionLegal], []>;
def LoweringLegalForCartesianProduct1 : LegalizeLowering<BuilderLegalForCartesianProduct, 1, ExPerPoint, [ActionLegal]>;
def LoweringLegalForCartesianProduct2 : LegalizeLowering<BuilderLegalForCartesianProduct, 2, ExPerPoint, [ActionLegal], []>;
def LoweringLegalForTypesWithMemDesc0 : LegalizeLowering<BuilderLegalForTypesWithMemDesc, 0, ExPerType, [ActionLegal]>;
def LoweringLower0 : LegalizeLowering<BuilderLower, 0, ExOne, [ActionLower]>;
def LoweringLowerFor0 : LegalizeLowering<BuilderLowerFor, 0, ExPerType, [ActionLower]>;
def LoweringLowerFor1 : LegalizeLowering<BuilderLowerFor, 1, ExPerType, [ActionLower]>;
def LoweringLibcall0 : LegalizeLowering<BuilderLibcall, 0, ExOne, [ActionLibcall]>;
def LoweringLibcallFor0 : LegalizeLowering<BuilderLibcallFor, 0, ExPerType, [ActionLibcall]>;
def LoweringLibcallFor1 : LegalizeLowering<BuilderLibcallFor, 1, ExPerType, [ActionLibcall]>;
def LoweringCustomFor0 : LegalizeLowering<BuilderCustomFor, 0, ExPerType, [ActionCustom]>;
def LoweringCustomFor1 : LegalizeLowering<BuilderCustomFor, 1, ExPerType, [ActionCustom]>;
def LoweringCustomForCartesianProduct0 : LegalizeLowering<BuilderCustomForCartesianProduct, 0, ExPerPoint, [ActionCustom], []>;
def LoweringCustomForCartesianProduct1 : LegalizeLowering<BuilderCustomForCartesianProduct, 1, ExPerPoint, [ActionCustom]>;
def LoweringCustomForCartesianProduct2 : LegalizeLowering<BuilderCustomForCartesianProduct, 2, ExPerPoint, [ActionCustom], []>;
def LoweringCustom0 : LegalizeLowering<BuilderCustom, 0, ExOne, [ActionCustom]>;
def LoweringBitcastIf0 : LegalizeLowering<BuilderBitcastIf, 0, ExRawMutation, [], ["raw_mutation"]>;
def LoweringWidenScalarFor0 : LegalizeLowering<BuilderWidenScalarFor, 0, ExRawMutation, [], ["raw_mutation"]>;
def LoweringWidenScalarOrEltToNextPow2OrMinSize0 : LegalizeLowering<BuilderWidenScalarOrEltToNextPow2OrMinSize, 0, ExOne, [ActionWidenScalar], []>;
def LoweringWidenScalarOrEltToNextPow2OrMinSize1 : LegalizeLowering<BuilderWidenScalarOrEltToNextPow2OrMinSize, 1, ExSelectBits, [ActionWidenScalar], ["nonliteral_bits"]>;
def LoweringWidenVectorEltsToVectorMinSize0 : LegalizeLowering<BuilderWidenVectorEltsToVectorMinSize, 0, ExOne, [ActionWidenScalar], ["no_core_expansion"]>;
def LoweringMinScalarOrElt0 : LegalizeLowering<BuilderMinScalarOrElt, 0, ExOne, [ActionWidenScalar], []>;
def LoweringMinScalarOrEltIf0 : LegalizeLowering<BuilderMinScalarOrEltIf, 0, ExOne, [ActionWidenScalar], []>;
def LoweringMinScalarSameAs0 : LegalizeLowering<BuilderMinScalarSameAs, 0, ExOne, [ActionWidenScalar], []>;
def LoweringMinScalarEltSameAsIf0 : LegalizeLowering<BuilderMinScalarEltSameAsIf, 0, ExOne, [ActionWidenScalar], []>;
def LoweringMaxScalarEltSameAsIf0 : LegalizeLowering<BuilderMaxScalarEltSameAsIf, 0, ExOne, [ActionNarrowScalar], []>;
def LoweringUnsupportedIfMemSizeNotPow20 : LegalizeLowering<BuilderUnsupportedIfMemSizeNotPow2, 0, ExOne, [ActionUnsupported], []>;
def LoweringLowerIfMemSizeNotPow20 : LegalizeLowering<BuilderLowerIfMemSizeNotPow2, 0, ExOne, [ActionLower], []>;
def LoweringLowerIfMemSizeNotByteSizePow20 : LegalizeLowering<BuilderLowerIfMemSizeNotByteSizePow2, 0, ExOne, [ActionLower], []>;
def LoweringImmIdx0 : LegalizeLowering<BuilderImmIdx, 0, ExCoverage, [], ["coverage_marker"]>;
def LoweringAlwaysLegal0 : LegalizeLowering<BuilderAlwaysLegal, 0, ExOne, [ActionLegal]>;
def LoweringClampScalar0 : LegalizeLowering<BuilderClampScalar, 0, ExClamp2, [ActionWidenScalar, ActionNarrowScalar]>;
def LoweringClampScalarOrElt0 : LegalizeLowering<BuilderClampScalarOrElt, 0, ExClamp2, [ActionWidenScalar, ActionNarrowScalar]>;
def LoweringWidenScalarToNextPow20 : LegalizeLowering<BuilderWidenScalarToNextPow2, 0, ExOne, [ActionWidenScalar]>;
def LoweringWidenScalarToNextPow21 : LegalizeLowering<BuilderWidenScalarToNextPow2, 1, ExOne, [ActionWidenScalar]>;
def LoweringWidenScalarOrEltToNextPow20 : LegalizeLowering<BuilderWidenScalarOrEltToNextPow2, 0, ExOne, [ActionWidenScalar]>;
def LoweringWidenScalarOrEltToNextPow21 : LegalizeLowering<BuilderWidenScalarOrEltToNextPow2, 1, ExOne, [ActionWidenScalar]>;
def LoweringMinScalar0 : LegalizeLowering<BuilderMinScalar, 0, ExOne, [ActionWidenScalar]>;
def LoweringMaxScalar0 : LegalizeLowering<BuilderMaxScalar, 0, ExOne, [ActionNarrowScalar]>;
def LoweringScalarSameSizeAs0 : LegalizeLowering<BuilderScalarSameSizeAs, 0, ExClamp2, [ActionWidenScalar, ActionNarrowScalar]>;
def LoweringScalarize0 : LegalizeLowering<BuilderScalarize, 0, ExOne, [ActionFewerElements]>;
def LoweringMoreElementsToNextPow20 : LegalizeLowering<BuilderMoreElementsToNextPow2, 0, ExOne, [ActionMoreElements]>;
def LoweringClampNumElements0 : LegalizeLowering<BuilderClampNumElements, 0, ExClamp2, [ActionMoreElements, ActionFewerElements]>;
def LoweringClampMinNumElements0 : LegalizeLowering<BuilderClampMinNumElements, 0, ExOne, [ActionMoreElements]>;
def LoweringClampMaxNumElements0 : LegalizeLowering<BuilderClampMaxNumElements, 0, ExOne, [ActionFewerElements]>;
def LoweringLegalIf0 : LegalizeLowering<BuilderLegalIf, 0, ExOne, [ActionLegal]>;
def LoweringLowerIf0 : LegalizeLowering<BuilderLowerIf, 0, ExOne, [ActionLower]>;
def LoweringLowerIf1 : LegalizeLowering<BuilderLowerIf, 1, ExRawMutation, [], ["raw_mutation"]>;
def LoweringCustomIf0 : LegalizeLowering<BuilderCustomIf, 0, ExOne, [ActionCustom]>;
def LoweringWidenScalarIf0 : LegalizeLowering<BuilderWidenScalarIf, 0, ExRawMutation, [], ["raw_mutation"]>;
def LoweringNarrowScalarIf0 : LegalizeLowering<BuilderNarrowScalarIf, 0, ExRawMutation, [], ["raw_mutation"]>;
def LoweringLibcallIf0 : LegalizeLowering<BuilderLibcallIf, 0, ExOne, [ActionLibcall]>;
def LoweringFewerElementsIf0 : LegalizeLowering<BuilderFewerElementsIf, 0, ExRawMutation, [], ["raw_mutation"]>;
def LoweringMoreElementsIf0 : LegalizeLowering<BuilderMoreElementsIf, 0, ExRawMutation, [], ["raw_mutation"]>;
def LoweringMinScalarIf0 : LegalizeLowering<BuilderMinScalarIf, 0, ExOne, [ActionWidenScalar]>;
def LoweringMaxScalarIf0 : LegalizeLowering<BuilderMaxScalarIf, 0, ExOne, [ActionNarrowScalar]>;
def LoweringScalarizeIf0 : LegalizeLowering<BuilderScalarizeIf, 0, ExOne, [ActionFewerElements]>;
def LoweringUnsupported0 : LegalizeLowering<BuilderUnsupported, 0, ExOne, [ActionUnsupported]>;
def LoweringUnsupportedFor0 : LegalizeLowering<BuilderUnsupportedFor, 0, ExOne, [ActionUnsupported], []>;
def LoweringUnsupportedIf0 : LegalizeLowering<BuilderUnsupportedIf, 0, ExOne, [ActionUnsupported]>;

include "llvm/Target/LegalizerCore.td"
