//===- LegalizerCore.td - Sort-checked legalizer core IR ---*- tablegen -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// A typed core IR for legalizer rules.
//
// A rule is one operator applied to an observation expression and a target
// constructor:
//
//   CoreWiden<(Slot 0), (Pow2Min 64)>
//   CoreMore<(Slot 1), Pow2>
//
// The operator fixes the sort of its observation (`Type` for all five lattice
// operators) and the sorts it accepts for its target.  The checker in
// LegalizerInfoEmitter.cpp walks the dag, assigns a sort to every node, and
// rejects a rule whose operator/observation/target combination is not in the
// action table.  The table is a total function of (operator, target sort), so
// the action a rule performs is derived from its shape instead of being a
// field that can disagree with it.
//
// The vocabulary is deliberately small:
//
//   sorts         Type, Int, Bool, MemDesc, Opcode, Kind, Ordering, String
//   operators     Widen, Narrow, More, Fewer, Bitcast,
//                 Legal, Custom, Lower, Unsupported (terminal), Scalarize,
//                 IsPow2
//   constructors  Pow2, Pow2Min(n), MultipleOf(n), Ty(t), SameAs(k),
//                 MinVectorSize(n), Clamp(l,h), Pow2Ceil(n)
//   observers     Slot, ElemOf, ScalarOf, NumElems, BitsOf, SizeOf, AlignOf,
//                 AddrSpace, SortOf, MemSlot, MemTy, MemOrdering,
//                 MemFailureOrdering, Feature, IsBFloat16
//   combinators   all/any/not, select, eq/ne/lt/le/gt/ge, in, mul, urem
//
// This is not a shape checker.  `More` on a scalar is sort-correct: that it
// also needs a vector at run time is the guard's business, not the sort's.
//
//===----------------------------------------------------------------------===//

/// A sort.  Sorts are nominal records rather than TableGen types because
/// TableGen has no dependent types: `ElemOf(slot)` cannot be declared as
/// "the sort of slot".  The checker recovers that information by walking the
/// expression, which is why every node record carries its result sort.
class LegalizeSort<string name> {
  string Name = name;
}

def SortType     : LegalizeSort<"Type">;
def SortInt      : LegalizeSort<"Int">;
def SortBool     : LegalizeSort<"Bool">;
def SortMemDesc  : LegalizeSort<"MemDesc">;
def SortOpcode   : LegalizeSort<"Opcode">;
def SortKind     : LegalizeSort<"Kind">;
def SortOrdering : LegalizeSort<"Ordering">;
def SortString   : LegalizeSort<"String">;

/// A finite union of sorts, for the one observation that crosses the two
/// memory axes: SizeOf applies to a type slot and to a memory descriptor.
class LegalizeSortSet<string name, list<LegalizeSort> members>
    : LegalizeSort<name> {
  list<LegalizeSort> Members = members;
}

def SortTypeOrMemDesc : LegalizeSortSet<"Type|MemDesc", [SortType, SortMemDesc]>;

//===----------------------------------------------------------------------===//
// Node expressions
//===----------------------------------------------------------------------===//

class LegalizeCoreNode;

/// An operator, observer, constructor, or combinator.
///
/// `ArgSorts` is the declared signature.  A variadic operator accepts `N`
/// instantiations of its single declared sort.  The comparison operators and
/// `in` are polymorphic; they leave the signature empty and set a flag the
/// checker interprets.
class LegalizeCoreExpr<string name, list<LegalizeSort> argSorts,
                        LegalizeSort result> : LegalizeCoreNode {
  string Name = name;
  list<LegalizeSort> ArgSorts = argSorts;
  LegalizeSort Result = result;

  /// `all`/`any`: the one declared argument sort repeats.
  bit Variadic = 0;

  /// The two operands must have the same sort.  Used by eq/ne/lt/le/gt/ge.
  bit SameSortOperands = 0;

  /// The second operand is a set and the first must be a member of it.
  bit SetMembership = 0;

  /// The operands are two type slots, a memory slot, and a memdesc set.
  bit MemDescMembership = 0;
}

/// An operator.  The target sorts are the second operand's admissible sorts;
/// the action table turns each pair into a LegalizeAction.
///
/// A terminal operator has no target at all: the rule is `(Op)` plus a
/// `CoreGuard`, and the action is keyed by the operator alone.  `Legal`,
/// `Custom`, `Lower` and `Unsupported` are terminal because there is nothing
/// to mutate -- the action *is* the answer.  `Scalarize` keeps an observation
/// (the slot it rewrites) but still has no target, because the type it
/// produces is implied by the action rather than written by the rule.
class LegalizeCoreOperator<string name, LegalizeSort domain,
                           list<LegalizeSort> targets> : LegalizeCoreNode {
  string Name = name;
  LegalizeSort Domain = domain;
  list<LegalizeSort> Targets = targets;

  /// False for a terminal action, whose rule carries no observation.
  bit HasObservation = 1;
}

def Widen   : LegalizeCoreOperator<"widen", SortType, [SortType, SortInt]>;
def Narrow  : LegalizeCoreOperator<"narrow", SortType, [SortType, SortInt]>;
def More    : LegalizeCoreOperator<"more", SortType, [SortInt, SortType]>;
def Fewer   : LegalizeCoreOperator<"fewer", SortType, [SortInt, SortType]>;
def Bitcast : LegalizeCoreOperator<"bitcast", SortType, [SortType]>;

/// Terminal actions.  A terminal rule is `(LegalOp)` plus a `CoreGuard`: the
/// guard is the condition under which the action is the answer, and there is
/// nothing else for the rule to write.  `CoreLegal<(CoreTrue)>` states an
/// unconditional `Legal` when one is really wanted.
///
/// The record names carry an `Op` suffix because `LegalizerInfo.td` already
/// defines `Custom`, `Lower` and `Unsupported` as builder sugar for chains, and
/// this file is included after it.  The schema name in the string argument is
/// what the IR and the frontend see; it stays unprefixed.
class LegalizeCoreTerminal<string name, LegalizeSort domain>
    : LegalizeCoreOperator<name, domain, []> {
  let HasObservation = 0;
}

def LegalOp       : LegalizeCoreTerminal<"legal", SortBool>;
def CustomOp      : LegalizeCoreTerminal<"custom", SortBool>;
def LowerOp       : LegalizeCoreTerminal<"lower", SortBool>;
def UnsupportedOp : LegalizeCoreTerminal<"unsupported", SortBool>;
/// `Libcall` is terminal in the same sense as `Lower`: the action is the whole
/// answer and the rule has nothing to mutate.
def LibcallOp     : LegalizeCoreTerminal<"libcall", SortBool>;

/// Drop the lanes of a vector slot.  The observation is the slot; the vector
/// requirement and the result type are part of the action, so the rule writes
/// neither, exactly as `scalarize` does.
def ScalarizeOp : LegalizeCoreOperator<"scalarize", SortType, []> {
  // HasObservation stays set: the slot is the observation.
}

//===----------------------------------------------------------------------===//
// Observers
//===----------------------------------------------------------------------===//

/// A type slot.  `(Slot 0)` and the bare integer `0` are the same node; the
/// bare form is accepted because a rule's observation is usually just its slot.
def Slot      : LegalizeCoreExpr<"slot", [SortInt], SortType>;

/// The element type of a vector slot.  ElemOf on a scalar is sort-correct;
/// the guard decides whether the slot really is a vector.
def ElemOf    : LegalizeCoreExpr<"elemOf", [SortType], SortType>;
/// The scalar type of a slot: the element type of a vector, and the type
/// itself otherwise.  This is `LLT::getScalarType()`, which unlike `ElemOf`
/// is defined for every type; a rule that asks "is the *element* bf16?" needs
/// it for both halves of the answer to be expressible.
def ScalarOf  : LegalizeCoreExpr<"scalarOf", [SortType], SortType>;
def NumElems  : LegalizeCoreExpr<"numElems", [SortType], SortInt>;
def BitsOf    : LegalizeCoreExpr<"bitsOf", [SortType], SortInt>;
def SizeOf    : LegalizeCoreExpr<"sizeOf", [SortTypeOrMemDesc], SortInt>;
def AddrSpace : LegalizeCoreExpr<"addrSpace", [SortType], SortInt>;

/// The memory descriptor's alignment, in bits, like `SizeOf`.  It is a
/// property of the descriptor, not of the memory type: a load of `s32` can
/// be 4- or 16-byte aligned, and the rule that cares about that says so.
/// `SizeOf(MemSlot)` reads `MemoryTy`, never this field.
def AlignOf   : LegalizeCoreExpr<"alignOf", [SortMemDesc], SortInt>;

/// The memory type's size in bytes, i.e. its bit size rounded up to whole
/// bytes.  Two questions need it and `SizeOf` cannot answer either: whether
/// the access is a whole number of bytes (`IsByteSized`), and whether the
/// number of bytes is a power of two.
def SizeInBytesOf
    : LegalizeCoreExpr<"sizeInBytesOf", [SortType], SortInt>;

/// Whether the type's bit size is a whole number of bytes.  A separate
/// observation from `SizeInBytesOf` because a 12-bit type is byte-sized and
/// not a power of two, while a 24-bit type is neither.
def IsByteSized : LegalizeCoreExpr<"isByteSized", [SortType], SortBool>;
def SortOf    : LegalizeCoreExpr<"sortOf", [SortType], SortKind>;

/// A memory descriptor, indexed into the query's MMODescs array.  It is a
/// different axis from Type: MMODescs[0] is not Types[0].
def MemSlot       : LegalizeCoreExpr<"memSlot", [SortInt], SortMemDesc>;
def MemTy         : LegalizeCoreExpr<"memTy", [SortMemDesc], SortType>;
/// Success and failure orderings are separate fields on the descriptor.  A
/// cmpxchg's failure ordering is weaker than its success ordering, so a rule
/// that constrains it is not restating the success one.
def MemOrdering   : LegalizeCoreExpr<"memOrdering", [SortMemDesc], SortOrdering>;
def MemFailureOrdering
    : LegalizeCoreExpr<"memFailureOrdering", [SortMemDesc], SortOrdering>;

/// A cached per-subtarget boolean, written as its C++ condition.
def Feature   : LegalizeCoreExpr<"feature", [SortString], SortBool>;

/// A type property: is this the bfloat16 scalar type?  It cannot be written as
/// `(CoreEq (ScalarOf s) (Ty bf16))` because `LLT::operator==` falls back to
/// comparing size and scalar-ness when either side is a plain scalar, and
/// `LLT::scalar(16)` is one: under that fallback `s16 == bfloat16`.  The
/// property has to be asked for by the method that answers it.
def IsBFloat16 : LegalizeCoreExpr<"isBFloat16", [SortType], SortBool>;

/// The same question for float16, and for the same reason.  `isFloat16()` and
/// `LLT::operator==` disagree on a plain scalar: `s16 == f16` is true under
/// the fallback, so a rule that means "is *this* the f16 type" must say so
/// with the method that answers it.
def IsFloat16 : LegalizeCoreExpr<"isFloat16", [SortType], SortBool>;

/// The sort of a slot, as a first-class value.  These are the tags SortOf
/// compares against.
def KindScalar         : LegalizeCoreExpr<"kindScalar", [], SortKind>;
def KindVector         : LegalizeCoreExpr<"kindVector", [], SortKind>;
def KindScalableVector : LegalizeCoreExpr<"kindScalableVector", [], SortKind>;
def KindPointer        : LegalizeCoreExpr<"kindPointer", [], SortKind>;
def KindPointerVector  : LegalizeCoreExpr<"kindPointerVector", [], SortKind>;

/// Atomic orderings, for MemOrdering comparisons.
def OrderingUnordered : LegalizeCoreExpr<"orderingUnordered", [], SortOrdering>;
def OrderingMonotonic : LegalizeCoreExpr<"orderingMonotonic", [], SortOrdering>;
def OrderingAcquire   : LegalizeCoreExpr<"orderingAcquire", [], SortOrdering>;
def OrderingRelease   : LegalizeCoreExpr<"orderingRelease", [], SortOrdering>;
def OrderingAcquireRelease
    : LegalizeCoreExpr<"orderingAcquireRelease", [], SortOrdering>;
def OrderingSequentiallyConsistent
    : LegalizeCoreExpr<"orderingSequentiallyConsistent", [], SortOrdering>;

//===----------------------------------------------------------------------===//
// Constructors
//===----------------------------------------------------------------------===//

/// The next power of two.  An Int target.
def Pow2            : LegalizeCoreExpr<"pow2", [], SortInt>;
/// The next power of two that is at least `n`.
def Pow2Min         : LegalizeCoreExpr<"pow2Min", [SortInt], SortInt>;
/// The next multiple of `n`.
def MultipleOf      : LegalizeCoreExpr<"multipleOf", [SortInt], SortInt>;
/// `1 << Log2_32_Ceil(n)`, the value form of what `Pow2Min` asks a mutation
/// for.  The two are separate records because the mutation's argument is a
/// literal size baked into the emitted call, while a rule computing an
/// element count passes an expression.
def Pow2Ceil        : LegalizeCoreExpr<"pow2Ceil", [SortInt], SortInt>;
/// A clamp to [lo, hi].
def Clamp           : LegalizeCoreExpr<"clamp", [SortInt, SortInt], SortInt>;
/// A literal type.  The argument is a LegalizeNamedType def.
def Ty              : LegalizeCoreExpr<"ty", [SortType], SortType>;
/// The type of another slot.
def SameAs          : LegalizeCoreExpr<"sameAs", [SortInt], SortType>;
/// The element size a fixed vector needs to reach `n` bits: `n / NumElts`,
/// with the integer division the C++ mutation performs.  A Type target
/// because it is the new element type, even though the shim also needs the
/// slot's lane count.
def MinVectorSize   : LegalizeCoreExpr<"minVectorSize", [SortInt], SortType>;

/// Type constructors.  A rule that cannot say its new type as "the same type
/// with a different element size" writes the type itself: an integer of some
/// size, a vector of some lane count, an element type, or a lane count.
def IntOfSize       : LegalizeCoreExpr<"intOfSize", [SortInt], SortType>;
def VecOf           : LegalizeCoreExpr<"vecOf", [SortInt, SortType], SortType>;
def EltTypeTo       : LegalizeCoreExpr<"eltTypeTo", [SortType, SortType],
                                       SortType>;
def EltSizeTo       : LegalizeCoreExpr<"eltSizeTo", [SortType, SortInt],
                                       SortType>;
/// `changeElementCount(getElementCount().divideCoefficientBy(n))`, which keeps
/// the scalable flag: a rule that halves a scalable vector wants half its
/// minimum lane count, not a fixed vector.
def DivLanes        : LegalizeCoreExpr<"divLanes", [SortType, SortInt],
                                       SortType>;

/// A fixed vector of `n` lanes whose element type is `ty`, except that `n == 1`
/// is the element type itself.  This is `LLT::scalarOrVector`, which the lane
/// clamp needs: lowering a vector to one lane leaves a scalar, not `<1 x ty>`.
def ScalarOrVecOf  : LegalizeCoreExpr<"scalarOrVecOf", [SortInt, SortType],
                                      SortType>;

/// Assign this whole type to the slot.  `Widen` and `Narrow` over a `Ty`
/// target change the *element size*; `Replace` says the new type outright.
/// The two cannot share a spelling: `changeElementSizeTo(f32)` on `<4 x s16>`
/// is `<4 x s32>`, while `changeTo(f32)` is `f32`.
def Replace         : LegalizeCoreExpr<"replace", [SortType], SortType>;

//===----------------------------------------------------------------------===//
// Boolean combinators and comparisons
//===----------------------------------------------------------------------===//

def CoreTrue  : LegalizeCoreExpr<"true", [], SortBool>;
def CoreAll   : LegalizeCoreExpr<"all", [SortBool], SortBool> { let Variadic = 1; }
def CoreAny   : LegalizeCoreExpr<"any", [SortBool], SortBool> { let Variadic = 1; }
def CoreNot   : LegalizeCoreExpr<"not", [SortBool], SortBool>;

/// Comparisons.  Both operands must have the same sort, and that sort must be
/// Int, Ordering, Kind, or Type.  Which pairs are meaningful in C++ is the
/// shim table's business; the checker only rejects ill-sorted comparisons.
class LegalizeCoreCompare<string name>
    : LegalizeCoreExpr<name, [], SortBool> {
  let SameSortOperands = 1;
}

def CoreEq : LegalizeCoreCompare<"eq">;
def CoreNe : LegalizeCoreCompare<"ne">;
def CoreLt : LegalizeCoreCompare<"lt">;
def CoreLe : LegalizeCoreCompare<"le">;
def CoreGt : LegalizeCoreCompare<"gt">;
def CoreGe : LegalizeCoreCompare<"ge">;

/// Integer power-of-two test.  The legalizer vocabulary has always had it
/// (`scalarOrEltSizeNotPow2`, `sizeNotPow2`); a rule that needs the positive
/// form (`isPowerOf2_32`) used to spell a lambda.
def CoreIsPow2 : LegalizeCoreExpr<"isPow2", [SortInt], SortBool>;

/// Set membership: `(CoreIn (SortOf (Slot 0)) MyKindSet)`.
///
/// The tuple forms name one type slot per element of the set:
///
///   (CoreInPairs (Slot 0), (Slot 1), MyPairSet)
///   (CoreInTriples (Slot 0), (Slot 1), (Slot 2), MyTripleSet)
///
/// which is `typePairInSet(0, 1, {...})` and `typeTupleInSet(0, 1, 2, {...})`.
/// The slots are written out because the C++ predicate takes indices, not
/// query values; the set itself is a table, which is why it is not spelled as
/// a disjunction of equalities the way a one-slot set is.
def CoreIn : LegalizeCoreExpr<"in", [], SortBool> { let SetMembership = 1; }
def CoreInPairs
    : LegalizeCoreExpr<"inPairs", [], SortBool> { let SetMembership = 1; }
def CoreInTriples
    : LegalizeCoreExpr<"inTriples", [], SortBool> { let SetMembership = 1; }

/// Memory-descriptor membership:
///
///   (CoreInMemDesc (Slot 0), (Slot 1), (MemSlot 0), MyMemDescSet)
///
/// is `typePairAndMemDescInSet(0, 1, 0, {{value, pointer, memTy, align},...})`.
/// The two type slots and the memory slot are written out for the same reason
/// the pair form writes its slots; the set is a separate axis because the
/// descriptor is not part of `Query.Types`.  The predicate compares with
/// `TypePairAndMemDesc::isCompatible`, so an entry is a lower bound on the
/// alignment and a size for the memory type, not an equality on either.
def CoreInMemDesc
    : LegalizeCoreExpr<"inMemDesc", [], SortBool> { let MemDescMembership = 1; }

/// Integer arithmetic.  These are the two a legalizer guard needs to compare
/// a type against a multiple or a residue of another type's size; there is no
/// intent to grow an expression language, so the shims are the C++ operators
/// themselves and nothing else is added without a rule that needs it.
def CoreMul  : LegalizeCoreExpr<"mul", [SortInt, SortInt], SortInt>;
def CoreURem : LegalizeCoreExpr<"urem", [SortInt, SortInt], SortInt>;
def CoreUDiv : LegalizeCoreExpr<"udiv", [SortInt, SortInt], SortInt>;

/// An integer choice: `(CoreSelect cond, then, else)`.  The condition is a
/// cached subtarget feature, so the value is still fixed at build time; the
/// branches must be non-negative literals or nested selects, which is the
/// shape a chain of feature tests takes.
def CoreSelect
    : LegalizeCoreExpr<"select", [SortBool, SortInt, SortInt], SortInt>;

/// A set literal.  A type set names LegalizeNamedType defs; a kind set names
/// SortOf tags.
///
/// `Arity` is the number of type slots one element occupies.  A one-slot set
/// is a set of types and tests a type expression (`(CoreIn (Slot 0), Set)`);
/// an n-slot set is a set of tuples and is tested by the matching
/// `CoreInPairs`/`CoreInTriples`, which names the slots.  Keeping the arity on
/// the set rather than on the operator is what makes a pair set unusable where
/// a type is expected, and the other way round.
class LegalizeCoreSet<string name, LegalizeSort elemSort>
    : LegalizeCoreNode {
  string Name = name;
  LegalizeSort ElemSort = elemSort;
  int Arity = 1;
}

class LegalizeCoreTypeSet<list<LegalizeNamedType> elems>
    : LegalizeCoreSet<"typeSet", SortType> {
  list<LegalizeNamedType> Elems = elems;
}

/// A set of type pairs, the table form of
/// `typePairInSet(0, 1, {{p0, i64}, {v2p0, v2i64}})`.
class LegalizeCoreTypePairSet<list<LegalizeTypePair> elems>
    : LegalizeCoreSet<"typePairSet", SortType> {
  list<LegalizeTypePair> Elems = elems;
  let Arity = 2;
}

/// A set of type triples, the table form of
/// `typeTupleInSet(0, 1, 2, {{s16, nxv16s8, s64}})`.
class LegalizeCoreTypeTripleSet<list<LegalizeTypeTriple> elems>
    : LegalizeCoreSet<"typeTripleSet", SortType> {
  list<LegalizeTypeTriple> Elems = elems;
  let Arity = 3;
}

/// A set of memory descriptors: {Type0, Type1, MemTy, Align}.  The arity is
/// the two type slots; the memory type and alignment belong to the descriptor
/// the query carries beside `Types`.
class LegalizeCoreMemDescSet<list<LegalizeMemDesc> elems>
    : LegalizeCoreSet<"memDescSet", SortType> {
  list<LegalizeMemDesc> Elems = elems;
  let Arity = 2;
}

class LegalizeCoreKindSet<list<LegalizeCoreNode> elems>
    : LegalizeCoreSet<"kindSet", SortKind> {
  list<LegalizeCoreNode> Elems = elems;
}

//===----------------------------------------------------------------------===//
// Rules
//===----------------------------------------------------------------------===//

/// One core rule.  `Builder` stays unset: the action, mutation projection, and
/// C++ shim are all derived from `CoreExpr`, so a rule cannot name a builder
/// that disagrees with its operator.
///
/// `CoreGuard` is optional.  `Pred` (the legacy predicate record) and
/// `CoreGuard` are mutually exclusive; both are checked.
class LegalizeCoreRule<dag expr> : LegalizeRule {
  dag CoreExpr = expr;
  dag CoreGuard = ?;
}

class CoreWiden<dag on, dag to>   : LegalizeCoreRule<(Widen on, to)>;

/// `widenScalarOrEltToNextPow2OrMinSize(TypeIdx, Min)`: widen the scalar or
/// element to the next power of two that is at least `Min`, when it is either
/// below `Min` *or* not a power of two.  `Min` is a literal here because the
/// legacy construct bakes it into the emitted call; the feature-dependent form
/// of the same mutation writes `(CoreSelect Feature, A, B)` in the target of
/// `CoreWiden` instead, which is `widenScalarOrEltToNextPow2` and therefore
/// lacks the "below Min" half of the condition.  The two are separate
/// spellings because they are separate predicates, not because of the
/// payload.
class CoreWidenPow2OrMinSize<dag on, int min>
    : LegalizeCoreRule<(Widen on, min)>;
class CoreNarrow<dag on, dag to>  : LegalizeCoreRule<(Narrow on, to)>;
class CoreMore<dag on, dag to>    : LegalizeCoreRule<(More on, to)>;
class CoreFewer<dag on, dag to>   : LegalizeCoreRule<(Fewer on, to)>;
class CoreBitcast<dag on, dag to> : LegalizeCoreRule<(Bitcast on, to)>;

/// Terminal rules.  The guard is the whole condition; there is no observation
/// and no target, so a rule cannot disagree with its action about what it
/// produces.
class CoreLegal<dag guard>       : LegalizeCoreRule<(LegalOp)> {
  let CoreGuard = guard;
}
class CoreCustom<dag guard>      : LegalizeCoreRule<(CustomOp)> {
  let CoreGuard = guard;
}
class CoreLower<dag guard>       : LegalizeCoreRule<(LowerOp)> {
  let CoreGuard = guard;
}
class CoreUnsupported<dag guard> : LegalizeCoreRule<(UnsupportedOp)> {
  let CoreGuard = guard;
}
class CoreLibcall<dag guard> : LegalizeCoreRule<(LibcallOp)> {
  let CoreGuard = guard;
}

/// The unconditional form: no guard, so the C++ is the predicate-free method
/// (`alwaysLegal`, `custom`, `lower`, `unsupported`) rather than an `*If` with
/// an always-true lambda.  `[CoreAlwaysLower]` in a Rules list is `[Lower]`.
def CoreAlwaysLegal       : LegalizeCoreRule<(LegalOp)>;
def CoreAlwaysCustom      : LegalizeCoreRule<(CustomOp)>;
def CoreAlwaysLower       : LegalizeCoreRule<(LowerOp)>;
def CoreAlwaysUnsupported : LegalizeCoreRule<(UnsupportedOp)>;
def CoreAlwaysLibcall     : LegalizeCoreRule<(LibcallOp)>;

class CoreScalarize<dag on> : LegalizeCoreRule<(ScalarizeOp on)>;
class CoreScalarizeIf<dag on, dag guard>
    : LegalizeCoreRule<(ScalarizeOp on)> {
  let CoreGuard = guard;
}

class CoreWidenIf<dag on, dag to, dag guard>
    : LegalizeCoreRule<(Widen on, to)> {
  let CoreGuard = guard;
}
class CoreNarrowIf<dag on, dag to, dag guard>
    : LegalizeCoreRule<(Narrow on, to)> {
  let CoreGuard = guard;
}
class CoreMoreIf<dag on, dag to, dag guard>
    : LegalizeCoreRule<(More on, to)> {
  let CoreGuard = guard;
}
class CoreFewerIf<dag on, dag to, dag guard>
    : LegalizeCoreRule<(Fewer on, to)> {
  let CoreGuard = guard;
}
class CoreBitcastIf<dag on, dag to, dag guard>
    : LegalizeCoreRule<(Bitcast on, to)> {
  let CoreGuard = guard;
}

/// A lane-count clamp is not an operator: a fixed vector of `Elt` is raised to
/// `Num` lanes by `more` over a whole type, and lowered to `Num` lanes by
/// `fewer` over one.  The guard is what the builder used to spell inline -- the
/// vector must be fixed (a scalable vector's lane count is a minimum), hold
/// `Elt`, and be on the wrong side of `Num`.  Writing the two directions as
/// two rules is what the builders do as well, so a clamp is two core rules.
class CoreClampMinNumElements<int Idx, LegalizeNamedType Elt, int Num>
    : CoreMoreIf<(Slot Idx), (Replace (VecOf Num, (ElemOf (Slot Idx)))),
                 (CoreAll (CoreEq (SortOf (Slot Idx)), KindVector),
                          (CoreNot (CoreEq (SortOf (Slot Idx)),
                                            KindScalableVector)),
                          (CoreEq (ElemOf (Slot Idx)), (Ty Elt)),
                          (CoreLt (NumElems (Slot Idx)), Num))>;
class CoreClampMaxNumElements<int Idx, LegalizeNamedType Elt, int Num>
    : CoreFewerIf<(Slot Idx),
                  (Replace (ScalarOrVecOf Num, (ElemOf (Slot Idx)))),
                  (CoreAll (CoreEq (SortOf (Slot Idx)), KindVector),
                           (CoreNot (CoreEq (SortOf (Slot Idx)),
                                             KindScalableVector)),
                           (CoreEq (ElemOf (Slot Idx)), (Ty Elt)),
                           (CoreGt (NumElems (Slot Idx)), Num))>;

/// The same two clamps with the lane count chosen by a cached feature, which
/// is how a target writes "the widest vector this subtarget can build".  They
/// are separate classes because `Num` is a `dag` here and a literal in the
/// forms above; a dag parameter cannot take a bare integer, and an integer
/// parameter cannot take a select.
class CoreClampMinNumElementsSel<int Idx, LegalizeNamedType Elt, dag Num>
    : CoreMoreIf<(Slot Idx), (Replace (VecOf Num, (ElemOf (Slot Idx)))),
                 (CoreAll (CoreEq (SortOf (Slot Idx)), KindVector),
                          (CoreNot (CoreEq (SortOf (Slot Idx)),
                                            KindScalableVector)),
                          (CoreEq (ElemOf (Slot Idx)), (Ty Elt)),
                          (CoreLt (NumElems (Slot Idx)), Num))>;
class CoreClampMaxNumElementsSel<int Idx, LegalizeNamedType Elt, dag Num>
    : CoreFewerIf<(Slot Idx),
                  (Replace (ScalarOrVecOf Num, (ElemOf (Slot Idx)))),
                  (CoreAll (CoreEq (SortOf (Slot Idx)), KindVector),
                           (CoreNot (CoreEq (SortOf (Slot Idx)),
                                             KindScalableVector)),
                           (CoreEq (ElemOf (Slot Idx)), (Ty Elt)),
                           (CoreGt (NumElems (Slot Idx)), Num))>;
