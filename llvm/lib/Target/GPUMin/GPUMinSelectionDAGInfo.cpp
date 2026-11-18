//===-- GPUMinSelectionDAGInfo.cpp - GPUMin DAG target info ---------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "GPUMinSelectionDAGInfo.h"

#define GET_SDNODE_DESC
#include "GPUMinGenSDNodeInfo.inc"

using namespace llvm;

GPUMinSelectionDAGInfo::GPUMinSelectionDAGInfo()
    : SelectionDAGGenTargetInfo(GPUMinGenSDNodeInfo) {}

GPUMinSelectionDAGInfo::~GPUMinSelectionDAGInfo() = default;
