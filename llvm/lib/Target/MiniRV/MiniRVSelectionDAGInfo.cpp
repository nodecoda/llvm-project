//===-- MiniRVSelectionDAGInfo.cpp - MiniRV SelectionDAG Info -------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "MiniRVSelectionDAGInfo.h"

#define GET_SDNODE_DESC
#include "MiniRVGenSDNodeInfo.inc"

using namespace llvm;

MiniRVSelectionDAGInfo::MiniRVSelectionDAGInfo()
    : SelectionDAGGenTargetInfo(MiniRVGenSDNodeInfo) {}

MiniRVSelectionDAGInfo::~MiniRVSelectionDAGInfo() = default;
