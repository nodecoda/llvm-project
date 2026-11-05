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
//   operators     Widen, Narrow, More, Fewer, Bitcast
//   constructors  Pow2, Pow2Min(n), MultipleOf(n), Ty(t), SameAs(k),
//                 MinVectorSize(n), Clamp(l,h)
//   observers     Slot, ElemOf, NumElems, BitsOf, SizeOf, AddrSpace, SortOf,
//                 MemSlot, MemTy, MemOrdering, Feature
//   combinators   all/any/not, select, eq/ne/lt/le/gt/ge, in
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
}

/// A lattice operator.  The target sorts are the second operand's admissible
/// sorts; the action table turns each pair into a LegalizeAction.
class LegalizeCoreOperator<string name, LegalizeSort domain,
                           list<LegalizeSort> targets> : LegalizeCoreNode {
  string Name = name;
  LegalizeSort Domain = domain;
  list<LegalizeSort> Targets = targets;
}

def Widen   : LegalizeCoreOperator<"widen", SortType, [SortType, SortInt]>;
def Narrow  : LegalizeCoreOperator<"narrow", SortType, [SortType, SortInt]>;
def More    : LegalizeCoreOperator<"more", SortType, [SortInt]>;
def Fewer   : LegalizeCoreOperator<"fewer", SortType, [SortInt, SortType]>;
def Bitcast : LegalizeCoreOperator<"bitcast", SortType, [SortType]>;

//===----------------------------------------------------------------------===//
// Observers
//===----------------------------------------------------------------------===//

/// A type slot.  `(Slot 0)` and the bare integer `0` are the same node; the
/// bare form is accepted because a rule's observation is usually just its slot.
def Slot      : LegalizeCoreExpr<"slot", [SortInt], SortType>;

/// The element type of a vector slot.  ElemOf on a scalar is sort-correct;
/// the guard decides whether the slot really is a vector.
def ElemOf    : LegalizeCoreExpr<"elemOf", [SortType], SortType>;
def NumElems  : LegalizeCoreExpr<"numElems", [SortType], SortInt>;
def BitsOf    : LegalizeCoreExpr<"bitsOf", [SortType], SortInt>;
def SizeOf    : LegalizeCoreExpr<"sizeOf", [SortTypeOrMemDesc], SortInt>;
def AddrSpace : LegalizeCoreExpr<"addrSpace", [SortType], SortInt>;
def SortOf    : LegalizeCoreExpr<"sortOf", [SortType], SortKind>;

/// A memory descriptor, indexed into the query's MMODescs array.  It is a
/// different axis from Type: MMODescs[0] is not Types[0].
def MemSlot       : LegalizeCoreExpr<"memSlot", [SortInt], SortMemDesc>;
def MemTy         : LegalizeCoreExpr<"memTy", [SortMemDesc], SortType>;
def MemOrdering   : LegalizeCoreExpr<"memOrdering", [SortMemDesc], SortOrdering>;

/// A cached per-subtarget boolean, written as its C++ condition.
def Feature   : LegalizeCoreExpr<"feature", [SortString], SortBool>;

/// The sort of a slot, as a first-class value.  These are the tags SortOf
/// compares against.
def KindScalar         : LegalizeCoreExpr<"kindScalar", [], SortKind>;
def KindVector         : LegalizeCoreExpr<"kindVector", [], SortKind>;
def KindScalableVector : LegalizeCoreExpr<"kindScalableVector", [], SortKind>;
def KindPointer        : LegalizeCoreExpr<"kindPointer", [], SortKind>;

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

/// Set membership: `(CoreIn (SortOf (Slot 0)) MyKindSet)`.
def CoreIn : LegalizeCoreExpr<"in", [], SortBool> { let SetMembership = 1; }

/// An integer choice: `(CoreSelect cond, then, else)`.  The condition is a
/// cached subtarget feature, which is why the branches must be Int and the
/// target is still a literal at build time.
def CoreSelect
    : LegalizeCoreExpr<"select", [SortBool, SortInt, SortInt], SortInt>;

/// A set literal.  A type set names LegalizeNamedType defs; a kind set names
/// SortOf tags.
class LegalizeCoreSet<string name, LegalizeSort elemSort>
    : LegalizeCoreNode {
  string Name = name;
  LegalizeSort ElemSort = elemSort;
}

class LegalizeCoreTypeSet<list<LegalizeNamedType> elems>
    : LegalizeCoreSet<"typeSet", SortType> {
  list<LegalizeNamedType> Elems = elems;
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
class CoreNarrow<dag on, dag to>  : LegalizeCoreRule<(Narrow on, to)>;
class CoreMore<dag on, dag to>    : LegalizeCoreRule<(More on, to)>;
class CoreFewer<dag on, dag to>   : LegalizeCoreRule<(Fewer on, to)>;
class CoreBitcast<dag on, dag to> : LegalizeCoreRule<(Bitcast on, to)>;

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
