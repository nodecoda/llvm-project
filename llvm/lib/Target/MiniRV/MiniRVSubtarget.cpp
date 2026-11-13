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
}
