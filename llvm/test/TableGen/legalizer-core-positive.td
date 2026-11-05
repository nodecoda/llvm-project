// RUN: llvm-tblgen -gen-legalizer-check -I %p/../../include %s | FileCheck %s --check-prefix=CHECK
// RUN: llvm-tblgen -gen-legalizer -I %p/../../include %s | FileCheck %s --check-prefix=CXX
// RUN: llvm-tblgen -gen-legalizer-core -I %p/../../include %s | FileCheck %s --check-prefix=CORE

// One rule per primitive.  The point of the file is that every operator,
// constructor, observer, and combinator parses, type-checks, serializes, and
// lowers to a C++ shim.  A primitive that gains no shim fails here, not in a
// target build.

include "llvm/Target/LegalizerInfo.td"

def s32 : LegalizeNamedType<"LLT::scalar(32)">;
def s64 : LegalizeNamedType<"LLT::scalar(64)">;

def Is32     : LegalizeFeature<"Subtarget.is32()">;
def IsVector : LegalizeFeature<"Subtarget.isVector()">;

def IntSet  : LegalizeCoreTypeSet<[s32, s64]>;
def KindSet : LegalizeCoreKindSet<[KindScalar, KindVector]>;
def PtrVecSet : LegalizeCoreKindSet<[KindPointerVector]>;

def AllPrimitives : LegalizeRuleSet<["G_ALL"]> {
  let Rules = [
    // Widen over a slot: every Int and Type constructor.
    CoreWiden<(Slot 0), (Pow2)>,
    CoreWiden<(Slot 0), (Pow2Min 64)>,
    CoreWiden<(Slot 0), (MultipleOf 8)>,
    CoreWiden<(Slot 0), (Ty s32)>,
    CoreWiden<(Slot 0), (SameAs 1)>,

    // Widen over an element.
    CoreWiden<(ElemOf (Slot 0)), (Pow2)>,
    CoreWiden<(ElemOf (Slot 0)), (Pow2Min 16)>,
    CoreWiden<(ElemOf (Slot 0)), (MultipleOf 4)>,
    CoreWiden<(ElemOf (Slot 0)), (Ty s64)>,
    CoreWiden<(ElemOf (Slot 0)), (SameAs 1)>,
    CoreWiden<(ElemOf (Slot 0)), (MinVectorSize 64)>,

    // An Int target chosen by a cached feature.
    CoreWiden<(Slot 0), (CoreSelect Is32, 16, 32)>,

    CoreNarrow<(Slot 0), (Ty s32)>,
    CoreNarrow<(Slot 0), (SameAs 1)>,
    CoreNarrow<(ElemOf (Slot 0)), (Ty s32)>,
    CoreMore<(Slot 0), (Pow2)>,
    CoreFewer<(Slot 0), (Ty s32)>,
    CoreBitcast<(Slot 0), (Ty s32)>,

    // Whole-type targets: the rule names the new type instead of a size.  The
    // `Replace` wrapper is what says "assign", not "resize".
    CoreWiden<(Slot 0), (Replace (Ty s32))>,
    CoreNarrow<(Slot 0), (Replace (Ty s32))>,
    CoreWiden<(Slot 0), (Replace (SameAs 1))>,
    CoreWiden<(Slot 0), (Replace (EltTypeTo (Slot 0), (Ty s32)))>,
    CoreWiden<(Slot 0), (Replace (EltSizeTo (Slot 0), (SizeOf (Slot 1))))>,
    CoreNarrow<(Slot 0), (Replace (IntOfSize (SizeOf (Slot 0))))>,
    CoreMore<(Slot 0), (Replace (VecOf (CoreSelect Is32, 4, 2), (Slot 0)))>,
    CoreMore<(Slot 0), (Replace (SameAs 1))>,
    CoreFewer<(Slot 0), (Replace (DivLanes (Slot 0), 2))>,
    CoreBitcast<(Slot 0), (Replace (IntOfSize (SizeOf (Slot 0))))>,

    // Observers and combinators as guards.
    CoreWidenIf<(Slot 0), (Pow2), (CoreGt (BitsOf (Slot 0)), 8)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreGt (BitsOf (ElemOf (Slot 0))), 8)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreIn (NumElems (Slot 0)), (Clamp 2, 16))>,
    CoreWidenIf<(Slot 0), (Pow2), (IsFloat16 (ScalarOf (Slot 0)))>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreNe (CoreUDiv (NumElems (Slot 0)), 2), 0)>,
    CoreWidenIf<(Slot 0), (Pow2), (CoreGe (SizeOf (Slot 0)), 8)>,
    CoreWidenIf<(Slot 0), (Pow2), (CoreGe (SizeOf (MemSlot 0)), 8)>,
    CoreWidenIf<(Slot 0), (Pow2), (CoreEq (AddrSpace (Slot 0)), 0)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreIn (SortOf (Slot 0)), KindSet)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreEq (SortOf (Slot 0)), KindVector)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreEq (MemTy (MemSlot 0)), s32)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreGe (MemOrdering (MemSlot 0)), OrderingAcquire)>,
    CoreWidenIf<(Slot 0), (Pow2), (CoreGe (AlignOf (MemSlot 0)), 32)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreGe (MemFailureOrdering (MemSlot 0)), OrderingAcquire)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreAll Is32, IsVector, (CoreNot (CoreEq (BitsOf (Slot 0)), 8)))>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreAny (CoreLt (BitsOf (Slot 0)), 8),
                         (CoreNe (BitsOf (Slot 1)), 16))>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreIn (Slot 0), IntSet)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreIsPow2 (SizeOf (Slot 0)))>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreIn (SortOf (Slot 0)), PtrVecSet)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreLe (BitsOf (Slot 0)), 32)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreLt (BitsOf (Slot 0)), 32)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreEq (ScalarOf (Slot 0)), s32)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (IsBFloat16 (ScalarOf (Slot 0)))>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreGt (BitsOf (Slot 0)), (CoreMul (BitsOf (Slot 1)), 2))>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreNe (CoreURem (CoreMul (NumElems (Slot 1)),
                                           (Pow2Ceil (NumElems (Slot 0)))),
                                  (NumElems (Slot 0))),
                         0)>,

    // A lane clamp is two core rules, not an operator: the direction the
    // builder picks by comparing the query's lane count is an action, and the
    // element type and lane count are the guard and the target.
    CoreClampMinNumElements<0, s32, 4>,
    CoreClampMaxNumElements<0, s32, 4>,
    CoreFewer<(Slot 0), (Replace (ScalarOrVecOf 4, (Slot 0)))>,

    // Terminal actions.  Libcall is the one that carries no mutation.
    CoreLibcall<(CoreNot Is32)>
  ];
}

// CHECK:       "core_rules": 57,
// CHECK:       "core_operators": ["bitcast", "custom", "fewer", "legal", "libcall", "lower", "more", "narrow", "scalarize", "unsupported", "widen"]

// CXX: .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
// CXX: .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 64)
// CXX: .coreWidenScalarToNextMultipleOf([](const LegalityQuery &) { return true; }, 0, 8)
// CXX: .coreMinScalar([](const LegalityQuery &) { return true; }, 0, s32)
// CXX: .coreMinScalarSameAs([](const LegalityQuery &) { return true; }, 0, 1)
// CXX: .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0)
// CXX: .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0, 16)
// CXX: .coreWidenScalarOrEltToNextMultipleOf([](const LegalityQuery &) { return true; }, 0, 4)
// CXX: .coreMinScalarOrElt([](const LegalityQuery &) { return true; }, 0, s64)
// CXX: .coreMinScalarSameAs([](const LegalityQuery &) { return true; }, 0, 1)
// CXX: .coreWidenVectorEltsToVectorMinSize([](const LegalityQuery &) { return true; }, 0, 64)
// CXX: .coreWidenScalarOrEltToNextPow2OrMinSize([](const LegalityQuery &) { return true; }, 0, (Is32 ? 16 : 32))
// CXX: .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, s32)
// CXX: .coreMaxScalarSameAs([](const LegalityQuery &) { return true; }, 0, 1)
// CXX: .coreMaxScalarOrElt([](const LegalityQuery &) { return true; }, 0, s32)
// CXX: .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
// CXX: .coreFewerElementsToType([](const LegalityQuery &) { return true; }, 0, s32)
// CXX: .coreBitcastTo([](const LegalityQuery &) { return true; }, 0, s32)
// CXX: .coreWidenTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, s32); })
// CXX: .coreNarrowTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, s32); })
// CXX: .coreWidenTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, Query.Types[1]); })
// CXX: .coreWidenTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementType(s32)); })
// CXX: .coreWidenTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementSize((Query.Types[1]).getSizeInBits())); })
// CXX: .coreNarrowTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::integer((Query.Types[0]).getSizeInBits())); })
// CXX: .coreMoreTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::fixed_vector((Is32 ? 4 : 2), Query.Types[0])); })
// CXX: .coreMoreTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, Query.Types[1]); })
// CXX: .coreFewerTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, (Query.Types[0]).changeElementCount((Query.Types[0]).getElementCount().divideCoefficientBy(2))); })
// CXX: .coreBitcastToType([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::integer((Query.Types[0]).getSizeInBits())); })
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 8); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (((Query.Types[0]).getElementType()).getScalarSizeInBits() > 8); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getNumElements() >= 2 && (Query.Types[0]).getNumElements() <= 16); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarType()).isFloat16(); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (((Query.Types[0]).getNumElements() / 2) != 0); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getSizeInBits() >= 8); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.MMODescrs[0]).MemoryTy.getSizeInBits() >= 8); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getAddressSpace() == 0); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (Query.Types[0].isScalar() || Query.Types[0].isVector()); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return Query.Types[0].isVector(); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.MMODescrs[0]).MemoryTy == s32); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return isAtLeastOrStrongerThan((Query.MMODescrs[0]).Ordering, AtomicOrdering::Acquire); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.MMODescrs[0]).AlignInBits >= 32); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return isAtLeastOrStrongerThan((Query.MMODescrs[0]).FailureOrdering, AtomicOrdering::Acquire); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (Is32) && (IsVector) && (!(((Query.Types[0]).getScalarSizeInBits() == 8))); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (((Query.Types[0]).getScalarSizeInBits() < 8)) || (((Query.Types[1]).getScalarSizeInBits() != 16)); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (Query.Types[0] == s32 || Query.Types[0] == s64); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return llvm::isPowerOf2_32((Query.Types[0]).getSizeInBits()); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (Query.Types[0].isPointerVector()); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() <= 32); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() < 32); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarType() == s32); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarType()).isBFloat16(); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > ((Query.Types[1]).getScalarSizeInBits() * 2)); }, 0)
// CXX: .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((((Query.Types[1]).getNumElements() * llvm::PowerOf2Ceil((Query.Types[0]).getNumElements())) % (Query.Types[0]).getNumElements()) != 0); }, 0)
// CXX: .coreMoreTo([=](const LegalityQuery &Query) { return (Query.Types[0].isVector()) && (!(Query.Types[0].isScalableVector())) && (((Query.Types[0]).getElementType() == s32)) && (((Query.Types[0]).getNumElements() < 4)); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::fixed_vector(4, (Query.Types[0]).getElementType())); })
// CXX: .coreFewerTo([=](const LegalityQuery &Query) { return (Query.Types[0].isVector()) && (!(Query.Types[0].isScalableVector())) && (((Query.Types[0]).getElementType() == s32)) && (((Query.Types[0]).getNumElements() > 4)); }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::scalarOrVector(ElementCount::getFixed(4), (Query.Types[0]).getElementType())); })
// CXX: .coreFewerTo([](const LegalityQuery &) { return true; }, 0, [=](const LegalityQuery &Query) { return std::make_pair(0, LLT::scalarOrVector(ElementCount::getFixed(4), Query.Types[0])); })
// CXX: .coreLibcallIf([=](const LegalityQuery &Query) { return !(Is32); });

// CORE:       "version": "0.4",
// CORE:       "op": "widen", "on": {"kind":"slot","index":0}, "to": {"kind":"pow2"}
// CORE:       "op": "widen", "on": {"kind":"elemOf","args":[{"kind":"slot","index":0}]}, "to": {"kind":"pow2Min","args":[{"kind":"int","value":16}]}
// CORE:       "op": "widen", "on": {"kind":"elemOf","args":[{"kind":"slot","index":0}]}, "to": {"kind":"minVectorSize","args":[{"kind":"int","value":64}]}
// CORE:       "op": "widen", "on": {"kind":"slot","index":0}, "to": {"kind":"select","args":[{"kind":"leaf","name":"hasFeature","args":["Subtarget.is32()"]},{"kind":"int","value":16},{"kind":"int","value":32}]}
// CORE:       "op": "narrow", "on": {"kind":"slot","index":0}, "to": {"kind":"ty","args":[{"kind":"type","value":"s32"}]}
// CORE:       "op": "more", "on": {"kind":"slot","index":0}, "to": {"kind":"pow2"}
// CORE:       "op": "fewer", "on": {"kind":"slot","index":0}, "to": {"kind":"ty","args":[{"kind":"type","value":"s32"}]}
// CORE:       "op": "bitcast", "on": {"kind":"slot","index":0}, "to": {"kind":"ty","args":[{"kind":"type","value":"s32"}]}
// CORE:       "op": "widen", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"ty","args":[{"kind":"type","value":"s32"}]}]}
// CORE:       "op": "narrow", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"ty","args":[{"kind":"type","value":"s32"}]}]}
// CORE:       "op": "widen", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"sameAs","args":[{"kind":"int","value":1}]}]}
// CORE:       "to": {"kind":"replace","args":[{"kind":"eltTypeTo","args":[{"kind":"slot","index":0},{"kind":"ty","args":[{"kind":"type","value":"s32"}]}]}]}
// CORE:       "to": {"kind":"replace","args":[{"kind":"eltSizeTo","args":[{"kind":"slot","index":0},{"kind":"sizeOf","args":[{"kind":"slot","index":1}]}]}]}
// CORE:       "to": {"kind":"replace","args":[{"kind":"intOfSize","args":[{"kind":"sizeOf","args":[{"kind":"slot","index":0}]}]}]}
// CORE:       "op": "more", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"vecOf","args":[{"kind":"select","args":[{"kind":"leaf","name":"hasFeature","args":["Subtarget.is32()"]},{"kind":"int","value":4},{"kind":"int","value":2}]},{"kind":"slot","index":0}]}]}
// CORE:       "op": "more", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"sameAs","args":[{"kind":"int","value":1}]}]}
// CORE:       "op": "fewer", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"divLanes","args":[{"kind":"slot","index":0},{"kind":"int","value":2}]}]}
// CORE:       "op": "bitcast", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"intOfSize","args":[{"kind":"sizeOf","args":[{"kind":"slot","index":0}]}]}]}
// CORE:       "predicate":{"kind":"isFloat16","args":[{"kind":"scalarOf","args":[{"kind":"slot","index":0}]}]}
// CORE:       "predicate":{"kind":"ne","args":[{"kind":"udiv","args":[{"kind":"numElems","args":[{"kind":"slot","index":0}]},{"kind":"int","value":2}]},{"kind":"int","value":0}]}
// CORE:       "predicate":{"kind":"ge","args":[{"kind":"sizeOf","args":[{"kind":"slot","index":0}]},{"kind":"int","value":8}]}
// CORE:       "predicate":{"kind":"in","args":[{"kind":"sortOf","args":[{"kind":"slot","index":0}]},{"kind":"set","of":"Kind","elems":["kindScalar","kindVector"]}]}
// CORE:       "predicate":{"kind":"eq","args":[{"kind":"sortOf","args":[{"kind":"slot","index":0}]},{"kind":"kindVector"}]}
// CORE:       "predicate":{"kind":"in","args":[{"kind":"sortOf","args":[{"kind":"slot","index":0}]},{"kind":"set","of":"Kind","elems":["kindPointerVector"]}]}
// CORE:       "predicate":{"kind":"eq","args":[{"kind":"scalarOf","args":[{"kind":"slot","index":0}]},{"kind":"type","value":"s32"}]}
// CORE:       "predicate":{"kind":"isBFloat16","args":[{"kind":"scalarOf","args":[{"kind":"slot","index":0}]}]}
// CORE:       "predicate":{"kind":"gt","args":[{"kind":"bitsOf","args":[{"kind":"slot","index":0}]},{"kind":"mul","args":[{"kind":"bitsOf","args":[{"kind":"slot","index":1}]},{"kind":"int","value":2}]}]}
// CORE:       "predicate":{"kind":"ne","args":[{"kind":"urem","args":[{"kind":"mul","args":[{"kind":"numElems","args":[{"kind":"slot","index":1}]},{"kind":"pow2Ceil","args":[{"kind":"numElems","args":[{"kind":"slot","index":0}]}]}]},{"kind":"numElems","args":[{"kind":"slot","index":0}]}]},{"kind":"int","value":0}]}
// CORE:       "op": "more", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"vecOf","args":[{"kind":"int","value":4},{"kind":"elemOf","args":[{"kind":"slot","index":0}]}]}]}
// CORE:       "op": "fewer", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"scalarOrVecOf","args":[{"kind":"int","value":4},{"kind":"elemOf","args":[{"kind":"slot","index":0}]}]}]}
// CORE:       "op": "fewer", "on": {"kind":"slot","index":0}, "to": {"kind":"replace","args":[{"kind":"scalarOrVecOf","args":[{"kind":"int","value":4},{"kind":"slot","index":0}]}]}
// CORE:       "op": "libcall", "ruleset": "AllPrimitives"
