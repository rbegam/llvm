//===--- IRRegion.cpp - Implements the IRRegion class ---------------------===//
//
// Copyright (C) 2015 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements the IRRegion class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Intel_LoopIR/IRRegion.h"

#include "llvm/Transforms/Intel_LoopTransforms/Utils/CanonExprUtils.h"

using namespace llvm;
using namespace llvm::loopopt;

std::set<IRRegion *> IRRegion::Objs;

void IRRegion::destroyAll() {
  for (auto &I : Objs) {
    delete I;
  }

  Objs.clear();
}

IRRegion::IRRegion(BasicBlock *EntryBB, const RegionBBlocksTy &BBs)
    : EntryBBlock(EntryBB), ExitBBlock(nullptr), BBlocks(BBs) {
  assert(EntryBB && "Entry basic block cannot be null!");

  Objs.insert(this);
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void IRRegion::dump() const { print(dbgs(), 0); }
#endif

void IRRegion::print(raw_ostream &OS, unsigned IndentWidth) const {
  OS.indent(IndentWidth) << "EntryBB: " << EntryBBlock->getName() << "\n";
  OS.indent(IndentWidth) << "ExitBB: ";

  if (ExitBBlock) {
    OS << ExitBBlock->getName();
  } else {
    OS << ExitBBlock;
  }

  OS << "\n";
  OS.indent(IndentWidth) << "Member BBlocks: ";

  for (auto I = BBlocks.begin(), E = BBlocks.end(); I != E; ++I) {
    if (I != BBlocks.begin()) {
      OS << ", ";
    }
    OS << (*I)->getName();
  }

  OS << "\n";
  OS.indent(IndentWidth) << "LiveIns: ";

  for (auto I = LiveInSet.begin(), E = LiveInSet.end(); I != E; ++I) {
    if (I != LiveInSet.begin()) {
      OS << ", ";
    }
    CanonExprUtils::printScalar(OS, I->first);
    OS << "(";
    I->second->printAsOperand(OS, false);
    OS << ")";
  }

  OS << "\n";
  OS.indent(IndentWidth) << "LiveOuts: ";

  for (auto I = LiveOutSet.begin(), E = LiveOutSet.end(); I != E; ++I) {
    if (I != LiveOutSet.begin()) {
      OS << ", ";
    }
    I->second->printAsOperand(OS, false);
    OS << "(sym:" << I->first << ")";
  }

  OS << "\n";
}

BasicBlock *IRRegion::getPredBBlock() const {
  auto PredI = pred_begin(EntryBBlock);

  /// In some cases the entry bblock is also the loop header, so the predecessor
  /// can be the loop latch. We need to skip it, if that is the case.
  if (BBlocks.count(*PredI)) {
    PredI++;
    auto TempPredI = PredI;

    (void)TempPredI;
    assert(!BBlocks.count(*PredI) &&
           "Both region predecessors lie inside the reigon!");
    assert((++TempPredI == pred_end(EntryBBlock)) &&
           "Region has more than two predecessors!");
  }

  return *PredI;
}

BasicBlock *IRRegion::getSuccBBlock() const {
  auto SuccI = succ_begin(ExitBBlock);

  /// In some cases the exit bblock is also the loop latch, so the successor
  /// can be the loop header. We need to skip it, if that is the case.
  if (BBlocks.count(*SuccI)) {
    SuccI++;
    auto TempSuccI = SuccI;

    (void)TempSuccI;
    assert(!BBlocks.count(*SuccI) &&
           "Both region successors lie inside the reigon!");
    assert((++TempSuccI == succ_end(ExitBBlock)) &&
           "Region has more than two successors!");
  }

  return *SuccI;
}
