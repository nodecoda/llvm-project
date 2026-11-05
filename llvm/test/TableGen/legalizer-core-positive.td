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

    // Observers and combinators as guards.
    CoreWidenIf<(Slot 0), (Pow2), (CoreGt (BitsOf (Slot 0)), 8)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreGt (BitsOf (ElemOf (Slot 0))), 8)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreIn (NumElems (Slot 0)), (Clamp 2, 16))>,
    CoreWidenIf<(Slot 0), (Pow2), (CoreGe (SizeOf (Slot 0)), 8)>,
    CoreWidenIf<(Slot 0), (Pow2), (CoreGe (SizeOf (MemSlot 0)), 8)>,
    CoreWidenIf<(Slot 0), (Pow2), (CoreEq (AddrSpace (Slot 0)), 0)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreIn (SortOf (Slot 0)), KindSet)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreEq (MemTy (MemSlot 0)), s32)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreGe (MemOrdering (MemSlot 0)), OrderingAcquire)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreAll Is32, IsVector, (CoreNot (CoreEq (BitsOf (Slot 0)), 8)))>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreAny (CoreLt (BitsOf (Slot 0)), 8),
                         (CoreNe (BitsOf (Slot 1)), 16))>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreIn (Slot 0), IntSet)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreLe (BitsOf (Slot 0)), 32)>,
    CoreWidenIf<(Slot 0), (Pow2),
                (CoreLt (BitsOf (Slot 0)), 32)>
  ];
}

// CHECK:       "core_rules": 32,
// CHECK:       "core_operators": ["widen", "narrow", "more", "fewer", "bitcast"]

// CXX:       .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0)
// CXX:       .coreWidenScalarToNextPow2([](const LegalityQuery &) { return true; }, 0, 64)
// CXX:       .coreWidenScalarToNextMultipleOf([](const LegalityQuery &) { return true; }, 0, 8)
// CXX:       .coreMinScalar([](const LegalityQuery &) { return true; }, 0, s32)
// CXX:       .coreMinScalarSameAs([](const LegalityQuery &) { return true; }, 0, 1)
// CXX:       .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0)
// CXX:       .coreWidenScalarOrEltToNextPow2([](const LegalityQuery &) { return true; }, 0, 16)
// CXX:       .coreWidenVectorEltsToVectorMinSize([](const LegalityQuery &) { return true; }, 0, 64)
// CXX:       .coreWidenScalarOrEltToNextPow2OrMinSize([](const LegalityQuery &) { return true; }, 0, (Is32 ? 16 : 32))
// CXX:       .coreWidenScalarOrEltToNextMultipleOf([](const LegalityQuery &) { return true; }, 0, 4)
// CXX:       .coreMinScalarOrElt([](const LegalityQuery &) { return true; }, 0, s64)
// CXX:       .coreMinScalarSameAs([](const LegalityQuery &) { return true; }, 0, 1)
// CXX:       .coreMaxScalar([](const LegalityQuery &) { return true; }, 0, s32)
// CXX:       .coreMaxScalarSameAs([](const LegalityQuery &) { return true; }, 0, 1)
// CXX:       .coreMaxScalarOrElt([](const LegalityQuery &) { return true; }, 0, s32)
// CXX:       .coreMoreElementsToNextPow2([](const LegalityQuery &) { return true; }, 0)
// CXX:       .coreFewerElementsToType([](const LegalityQuery &) { return true; }, 0, s32)
// CXX:       .coreBitcastTo([](const LegalityQuery &) { return true; }, 0, s32)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() > 8); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (((Query.Types[0]).getElementType()).getScalarSizeInBits() > 8); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getNumElements() >= 2 && (Query.Types[0]).getNumElements() <= 16); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getSizeInBits() >= 8); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.MMODescrs[0]).MemoryTy.getSizeInBits() >= 8); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getAddressSpace() == 0); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (Query.Types[0].isScalar() || Query.Types[0].isVector()); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.MMODescrs[0]).MemoryTy == s32); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return isAtLeastOrStrongerThan((Query.MMODescrs[0]).Ordering, AtomicOrdering::Acquire); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (Is32) && (IsVector) && (!(((Query.Types[0]).getScalarSizeInBits() == 8))); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (((Query.Types[0]).getScalarSizeInBits() < 8)) || (((Query.Types[1]).getScalarSizeInBits() != 16)); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return (Query.Types[0] == s32 || Query.Types[0] == s64); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() <= 32); }, 0)
// CXX:       .coreWidenScalarToNextPow2([=](const LegalityQuery &Query) { return ((Query.Types[0]).getScalarSizeInBits() < 32); }, 0);

// CORE:       "version": "0.4",
// CORE:       "op": "widen", "on": {"kind":"slot","index":0}, "to": {"kind":"pow2"}
// CORE:       "op": "widen", "on": {"kind":"elemOf","args":[{"kind":"slot","index":0}]}, "to": {"kind":"pow2Min","args":[{"kind":"int","value":16}]}
// CORE:       "op": "widen", "on": {"kind":"elemOf","args":[{"kind":"slot","index":0}]}, "to": {"kind":"minVectorSize","args":[{"kind":"int","value":64}]}
// CORE:       "op": "widen", "on": {"kind":"slot","index":0}, "to": {"kind":"select","args":[{"kind":"leaf","name":"hasFeature","args":["Subtarget.is32()"]},{"kind":"int","value":16},{"kind":"int","value":32}]}
// CORE:       "op": "narrow", "on": {"kind":"slot","index":0}, "to": {"kind":"ty","args":[{"kind":"type","value":"s32"}]}
// CORE:       "op": "more", "on": {"kind":"slot","index":0}, "to": {"kind":"pow2"}
// CORE:       "op": "fewer", "on": {"kind":"slot","index":0}, "to": {"kind":"ty","args":[{"kind":"type","value":"s32"}]}
// CORE:       "op": "bitcast", "on": {"kind":"slot","index":0}, "to": {"kind":"ty","args":[{"kind":"type","value":"s32"}]}
// CORE:       "predicate":{"kind":"in","args":[{"kind":"sortOf","args":[{"kind":"slot","index":0}]},{"kind":"set","of":"Kind","elems":["KindScalar","KindVector"]}]}
