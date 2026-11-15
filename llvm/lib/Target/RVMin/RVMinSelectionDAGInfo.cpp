//===-- RVMinSelectionDAGInfo.cpp - RVMin DAG target info -----------------===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//

#include "RVMinSelectionDAGInfo.h"

#define GET_SDNODE_DESC
#include "RVMinGenSDNodeInfo.inc"

using namespace llvm;

RVMinSelectionDAGInfo::RVMinSelectionDAGInfo()
    : SelectionDAGGenTargetInfo(RVMinGenSDNodeInfo) {}

RVMinSelectionDAGInfo::~RVMinSelectionDAGInfo() = default;
