//===-- MiniRVSubtarget.cpp - MiniRV Subtarget Information ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVSubtarget.h"
#include "MiniRVSelectionDAGInfo.h"
#include "MiniRVTargetMachine.h"
#include "llvm/Analysis/LibcallLoweringInfo.h"

using namespace llvm;

#define DEBUG_TYPE "minirv-subtarget"

#define GET_SUBTARGETINFO_TARGET_DESC
#define GET_SUBTARGETINFO_CTOR
#include "MiniRVGenSubtargetInfo.inc"

void MiniRVSubtarget::anchor() {}

MiniRVSubtarget::MiniRVSubtarget(const Triple &TT, StringRef CPU, StringRef FS,
                                 const MiniRVTargetMachine &TM)
    : MiniRVGenSubtargetInfo(TT, CPU, /*TuneCPU=*/CPU, FS), InstrInfo(*this),
      TLInfo(TM, *this), FrameLowering(*this) {
  TSInfo = std::make_unique<MiniRVSelectionDAGInfo>();
}

MiniRVSubtarget::~MiniRVSubtarget() = default;

/// The triple is unknown to the runtime libcall table, so no implementation is
/// seeded by default.  Name the C library entry points the memory intrinsics
/// fall back to; without a name the lowering would build a symbol from a null
/// string.
void MiniRVSubtarget::initLibcallLoweringInfo(
    LibcallLoweringInfo &Info) const {
  Info.setLibcallImpl(RTLIB::MEMCPY, RTLIB::impl_memcpy);
  Info.setLibcallImpl(RTLIB::MEMMOVE, RTLIB::impl_memmove);
  Info.setLibcallImpl(RTLIB::MEMSET, RTLIB::impl_memset);

  // The base ISA has no A extension, so AtomicExpandPass turns every atomic
  // load, store, exchange, compare-exchange and read-modify-write into one of
  // libatomic's __atomic_* entry points.  The sized variants exist for the C
  // integer widths; the unsized entry points take a byte count and cover the
  // unaligned and non-power-of-two cases.
  static const struct {
    RTLIB::Libcall Op;
    RTLIB::LibcallImpl Impl;
  } AtomicCalls[] = {
      {RTLIB::ATOMIC_LOAD, RTLIB::impl___atomic_load},
      {RTLIB::ATOMIC_LOAD_1, RTLIB::impl___atomic_load_1},
      {RTLIB::ATOMIC_LOAD_2, RTLIB::impl___atomic_load_2},
      {RTLIB::ATOMIC_LOAD_4, RTLIB::impl___atomic_load_4},
      {RTLIB::ATOMIC_LOAD_8, RTLIB::impl___atomic_load_8},
      {RTLIB::ATOMIC_LOAD_16, RTLIB::impl___atomic_load_16},
      {RTLIB::ATOMIC_STORE, RTLIB::impl___atomic_store},
      {RTLIB::ATOMIC_STORE_1, RTLIB::impl___atomic_store_1},
      {RTLIB::ATOMIC_STORE_2, RTLIB::impl___atomic_store_2},
      {RTLIB::ATOMIC_STORE_4, RTLIB::impl___atomic_store_4},
      {RTLIB::ATOMIC_STORE_8, RTLIB::impl___atomic_store_8},
      {RTLIB::ATOMIC_STORE_16, RTLIB::impl___atomic_store_16},
      {RTLIB::ATOMIC_EXCHANGE, RTLIB::impl___atomic_exchange},
      {RTLIB::ATOMIC_EXCHANGE_1, RTLIB::impl___atomic_exchange_1},
      {RTLIB::ATOMIC_EXCHANGE_2, RTLIB::impl___atomic_exchange_2},
      {RTLIB::ATOMIC_EXCHANGE_4, RTLIB::impl___atomic_exchange_4},
      {RTLIB::ATOMIC_EXCHANGE_8, RTLIB::impl___atomic_exchange_8},
      {RTLIB::ATOMIC_EXCHANGE_16, RTLIB::impl___atomic_exchange_16},
      {RTLIB::ATOMIC_COMPARE_EXCHANGE, RTLIB::impl___atomic_compare_exchange},
      {RTLIB::ATOMIC_COMPARE_EXCHANGE_1,
       RTLIB::impl___atomic_compare_exchange_1},
      {RTLIB::ATOMIC_COMPARE_EXCHANGE_2,
       RTLIB::impl___atomic_compare_exchange_2},
      {RTLIB::ATOMIC_COMPARE_EXCHANGE_4,
       RTLIB::impl___atomic_compare_exchange_4},
      {RTLIB::ATOMIC_COMPARE_EXCHANGE_8,
       RTLIB::impl___atomic_compare_exchange_8},
      {RTLIB::ATOMIC_COMPARE_EXCHANGE_16,
       RTLIB::impl___atomic_compare_exchange_16},
      {RTLIB::ATOMIC_FETCH_ADD_1, RTLIB::impl___atomic_fetch_add_1},
      {RTLIB::ATOMIC_FETCH_ADD_2, RTLIB::impl___atomic_fetch_add_2},
      {RTLIB::ATOMIC_FETCH_ADD_4, RTLIB::impl___atomic_fetch_add_4},
      {RTLIB::ATOMIC_FETCH_ADD_8, RTLIB::impl___atomic_fetch_add_8},
      {RTLIB::ATOMIC_FETCH_ADD_16, RTLIB::impl___atomic_fetch_add_16},
      {RTLIB::ATOMIC_FETCH_SUB_1, RTLIB::impl___atomic_fetch_sub_1},
      {RTLIB::ATOMIC_FETCH_SUB_2, RTLIB::impl___atomic_fetch_sub_2},
      {RTLIB::ATOMIC_FETCH_SUB_4, RTLIB::impl___atomic_fetch_sub_4},
      {RTLIB::ATOMIC_FETCH_SUB_8, RTLIB::impl___atomic_fetch_sub_8},
      {RTLIB::ATOMIC_FETCH_SUB_16, RTLIB::impl___atomic_fetch_sub_16},
      {RTLIB::ATOMIC_FETCH_AND_1, RTLIB::impl___atomic_fetch_and_1},
      {RTLIB::ATOMIC_FETCH_AND_2, RTLIB::impl___atomic_fetch_and_2},
      {RTLIB::ATOMIC_FETCH_AND_4, RTLIB::impl___atomic_fetch_and_4},
      {RTLIB::ATOMIC_FETCH_AND_8, RTLIB::impl___atomic_fetch_and_8},
      {RTLIB::ATOMIC_FETCH_AND_16, RTLIB::impl___atomic_fetch_and_16},
      {RTLIB::ATOMIC_FETCH_OR_1, RTLIB::impl___atomic_fetch_or_1},
      {RTLIB::ATOMIC_FETCH_OR_2, RTLIB::impl___atomic_fetch_or_2},
      {RTLIB::ATOMIC_FETCH_OR_4, RTLIB::impl___atomic_fetch_or_4},
      {RTLIB::ATOMIC_FETCH_OR_8, RTLIB::impl___atomic_fetch_or_8},
      {RTLIB::ATOMIC_FETCH_OR_16, RTLIB::impl___atomic_fetch_or_16},
      {RTLIB::ATOMIC_FETCH_XOR_1, RTLIB::impl___atomic_fetch_xor_1},
      {RTLIB::ATOMIC_FETCH_XOR_2, RTLIB::impl___atomic_fetch_xor_2},
      {RTLIB::ATOMIC_FETCH_XOR_4, RTLIB::impl___atomic_fetch_xor_4},
      {RTLIB::ATOMIC_FETCH_XOR_8, RTLIB::impl___atomic_fetch_xor_8},
      {RTLIB::ATOMIC_FETCH_XOR_16, RTLIB::impl___atomic_fetch_xor_16},
      {RTLIB::ATOMIC_FETCH_NAND_1, RTLIB::impl___atomic_fetch_nand_1},
      {RTLIB::ATOMIC_FETCH_NAND_2, RTLIB::impl___atomic_fetch_nand_2},
      {RTLIB::ATOMIC_FETCH_NAND_4, RTLIB::impl___atomic_fetch_nand_4},
      {RTLIB::ATOMIC_FETCH_NAND_8, RTLIB::impl___atomic_fetch_nand_8},
      {RTLIB::ATOMIC_FETCH_NAND_16, RTLIB::impl___atomic_fetch_nand_16},
  };
  for (const auto &LC : AtomicCalls)
    Info.setLibcallImpl(LC.Op, LC.Impl);

  // The ISA has no 128-bit divide, so the type legalizer asks for the
  // quad-precision helpers from compiler-rt.  A 128-bit multiply is expanded
  // inline from the 64-bit pieces instead of calling __multi3.
  Info.setLibcallImpl(RTLIB::SDIV_I128, RTLIB::impl___divti3);
  Info.setLibcallImpl(RTLIB::UDIV_I128, RTLIB::impl___udivti3);
  Info.setLibcallImpl(RTLIB::SREM_I128, RTLIB::impl___modti3);
  Info.setLibcallImpl(RTLIB::UREM_I128, RTLIB::impl___umodti3);

  // There is no floating-point unit, so every floating-point operation is
  // expanded into a call to the soft-float helpers.
  static const struct {
    RTLIB::Libcall Op;
    RTLIB::LibcallImpl Impl;
  } FloatCalls[] = {
      // Arithmetic.
      {RTLIB::ADD_F32, RTLIB::impl___addsf3},
      {RTLIB::SUB_F32, RTLIB::impl___subsf3},
      {RTLIB::MUL_F32, RTLIB::impl___mulsf3},
      {RTLIB::DIV_F32, RTLIB::impl___divsf3},
      {RTLIB::ADD_F64, RTLIB::impl___adddf3},
      {RTLIB::SUB_F64, RTLIB::impl___subdf3},
      {RTLIB::MUL_F64, RTLIB::impl___muldf3},
      {RTLIB::DIV_F64, RTLIB::impl___divdf3},
      // Conversions between the widths and to/from the integer registers.
      // Half precision has no arithmetic helpers: it is promoted to f32 and
      // converted with __extendhfsf2/__truncsfhf2.
      {RTLIB::FPEXT_F16_F32, RTLIB::impl___extendhfsf2},
      {RTLIB::FPROUND_F32_F16, RTLIB::impl___truncsfhf2},
      {RTLIB::FPEXT_BF16_F32, RTLIB::impl___extendbfsf2},
      {RTLIB::FPROUND_F32_BF16, RTLIB::impl___truncsfbf2},
      {RTLIB::FPROUND_F64_BF16, RTLIB::impl___truncdfbf2},
      {RTLIB::FPEXT_F32_F64, RTLIB::impl___extendsfdf2},
      {RTLIB::FPROUND_F64_F32, RTLIB::impl___truncdfsf2},
      {RTLIB::FPTOSINT_F32_I32, RTLIB::impl___fixsfsi},
      {RTLIB::FPTOSINT_F32_I64, RTLIB::impl___fixsfdi},
      {RTLIB::FPTOSINT_F64_I32, RTLIB::impl___fixdfsi},
      {RTLIB::FPTOSINT_F64_I64, RTLIB::impl___fixdfdi},
      {RTLIB::FPTOUINT_F32_I32, RTLIB::impl___fixunssfsi},
      {RTLIB::FPTOUINT_F32_I64, RTLIB::impl___fixunssfdi},
      {RTLIB::FPTOUINT_F64_I32, RTLIB::impl___fixunsdfsi},
      {RTLIB::FPTOUINT_F64_I64, RTLIB::impl___fixunsdfdi},
      {RTLIB::SINTTOFP_I32_F32, RTLIB::impl___floatsisf},
      {RTLIB::SINTTOFP_I32_F64, RTLIB::impl___floatsidf},
      {RTLIB::SINTTOFP_I64_F32, RTLIB::impl___floatdisf},
      {RTLIB::SINTTOFP_I64_F64, RTLIB::impl___floatdidf},
      {RTLIB::UINTTOFP_I32_F32, RTLIB::impl___floatunsisf},
      {RTLIB::UINTTOFP_I32_F64, RTLIB::impl___floatunsidf},
      {RTLIB::UINTTOFP_I64_F32, RTLIB::impl___floatundisf},
      {RTLIB::UINTTOFP_I64_F64, RTLIB::impl___floatundidf},
      {RTLIB::FPTOSINT_F16_I32, RTLIB::impl___fixhfsi},
      {RTLIB::FPTOSINT_F16_I64, RTLIB::impl___fixhfdi},
      {RTLIB::FPTOUINT_F16_I32, RTLIB::impl___fixunshfsi},
      {RTLIB::FPTOUINT_F16_I64, RTLIB::impl___fixunshfdi},
      {RTLIB::SINTTOFP_I32_F16, RTLIB::impl___floatsihf},
      {RTLIB::SINTTOFP_I64_F16, RTLIB::impl___floatdihf},
      {RTLIB::UINTTOFP_I32_F16, RTLIB::impl___floatunsihf},
      {RTLIB::UINTTOFP_I64_F16, RTLIB::impl___floatundihf},
      {RTLIB::SINTTOFP_I64_BF16, RTLIB::impl___floatdibf},
      {RTLIB::UINTTOFP_I64_BF16, RTLIB::impl___floatundibf},
      {RTLIB::FPTOSINT_F32_I128, RTLIB::impl___fixsfti},
      {RTLIB::FPTOSINT_F64_I128, RTLIB::impl___fixdfti},
      {RTLIB::FPTOUINT_F32_I128, RTLIB::impl___fixunssfti},
      {RTLIB::FPTOUINT_F64_I128, RTLIB::impl___fixunsdfti},
      {RTLIB::SINTTOFP_I128_F32, RTLIB::impl___floattisf},
      {RTLIB::SINTTOFP_I128_F64, RTLIB::impl___floattidf},
      {RTLIB::UINTTOFP_I128_F32, RTLIB::impl___floatuntisf},
      {RTLIB::UINTTOFP_I128_F64, RTLIB::impl___floatuntidf},
      // Comparisons return a three-way -1/0/1 result.
      {RTLIB::FCMP3_PRED_OEQ_F32, RTLIB::impl___eqsf2},
      {RTLIB::FCMP3_PRED_UNE_F32, RTLIB::impl___nesf2},
      {RTLIB::FCMP3_PRED_OGE_F32, RTLIB::impl___gesf2},
      {RTLIB::FCMP3_PRED_OLT_F32, RTLIB::impl___ltsf2},
      {RTLIB::FCMP3_PRED_OLE_F32, RTLIB::impl___lesf2},
      {RTLIB::FCMP3_PRED_OGT_F32, RTLIB::impl___gtsf2},
      {RTLIB::FCMP3_PRED_OEQ_F64, RTLIB::impl___eqdf2},
      {RTLIB::FCMP3_PRED_UNE_F64, RTLIB::impl___nedf2},
      {RTLIB::FCMP3_PRED_OGE_F64, RTLIB::impl___gedf2},
      {RTLIB::FCMP3_PRED_OLT_F64, RTLIB::impl___ltdf2},
      {RTLIB::FCMP3_PRED_OLE_F64, RTLIB::impl___ledf2},
      {RTLIB::FCMP3_PRED_OGT_F64, RTLIB::impl___gtdf2},
      {RTLIB::UO_F32, RTLIB::impl___unordsf2},
      {RTLIB::UO_F64, RTLIB::impl___unorddf2},
      // The libm operations that the DAG can produce.
      {RTLIB::SQRT_F32, RTLIB::impl_sqrtf},
      {RTLIB::SQRT_F64, RTLIB::impl_sqrt},
      {RTLIB::FMA_F32, RTLIB::impl_fmaf},
      {RTLIB::FMA_F64, RTLIB::impl_fma},
      {RTLIB::FLOOR_F32, RTLIB::impl_floorf},
      {RTLIB::FLOOR_F64, RTLIB::impl_floor},
      {RTLIB::CEIL_F32, RTLIB::impl_ceilf},
      {RTLIB::CEIL_F64, RTLIB::impl_ceil},
      {RTLIB::TRUNC_F32, RTLIB::impl_truncf},
      {RTLIB::TRUNC_F64, RTLIB::impl_trunc},
      {RTLIB::ROUND_F32, RTLIB::impl_roundf},
      {RTLIB::ROUND_F64, RTLIB::impl_round},
      {RTLIB::RINT_F32, RTLIB::impl_rintf},
      {RTLIB::RINT_F64, RTLIB::impl_rint},
      {RTLIB::NEARBYINT_F32, RTLIB::impl_nearbyintf},
      {RTLIB::NEARBYINT_F64, RTLIB::impl_nearbyint},
      {RTLIB::FMIN_F32, RTLIB::impl_fminf},
      {RTLIB::FMIN_F64, RTLIB::impl_fmin},
      {RTLIB::FMAX_F32, RTLIB::impl_fmaxf},
      {RTLIB::FMAX_F64, RTLIB::impl_fmax},
      {RTLIB::REM_F32, RTLIB::impl_fmodf},
      {RTLIB::REM_F64, RTLIB::impl_fmod},
  };
  for (const auto &LC : FloatCalls)
    Info.setLibcallImpl(LC.Op, LC.Impl);
}
