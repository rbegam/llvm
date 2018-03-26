//===------- WRegionInfo.cpp - Build WRegion Graph ---------*- C++ -*------===//
//
//   Copyright (C) 2016 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation. and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements the W-Region Information Graph build pass.
//
//===----------------------------------------------------------------------===//
#include "llvm/Pass.h"
#include "llvm/Analysis/Intel_VPO/WRegionInfo/WRegion.h"
#include "llvm/Analysis/Intel_VPO/WRegionInfo/WRegionCollection.h"
#include "llvm/Analysis/Intel_VPO/WRegionInfo/WRegionInfo.h"
#include "llvm/Analysis/Intel_VPO/WRegionInfo/WRegionPasses.h"
#include "llvm/ADT/DepthFirstIterator.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/PostDominators.h"

using namespace llvm;
using namespace llvm::vpo;

#define DEBUG_TYPE "vpo-wrninfo"

AnalysisKey WRegionInfoAnalysis::Key;

WRegionInfo WRegionInfoAnalysis::run(Function &F, FunctionAnalysisManager &AM) {

  DEBUG(dbgs() << "\nENTER WRegionInfoAnalysis::run: " << F.getName() << "{\n");

  auto &WRC  = AM.getResult<WRegionCollectionAnalysis>(F);
  auto *DT   = WRC.getDomTree();
  auto *LI   = WRC.getLoopInfo();
  auto *SE   = WRC.getSE();
  auto *TTI  = WRC.getTargetTransformInfo();
  auto *AC   = WRC.getAssumptionCache();
  auto *TLI  = WRC.getTargetLibraryInfo();

  WRegionInfo WRI(&F, DT, LI, SE, TTI, AC, TLI, &WRC);

  DEBUG(dbgs() << "\n}EXIT WRegionInfoAnalysis::run: " << F.getName() << "\n");
  return WRI;
}

INITIALIZE_PASS_BEGIN(WRegionInfoWrapperPass, "vpo-wrninfo",
                      "VPO Work-Region Information", false, true)
INITIALIZE_PASS_DEPENDENCY(WRegionCollectionWrapperPass)
INITIALIZE_PASS_END(WRegionInfoWrapperPass, "vpo-wrninfo",
                    "VPO Work-Region Information", false, true)

char WRegionInfoWrapperPass::ID = 0;

FunctionPass *llvm::createWRegionInfoWrapperPassPass() {
  return new WRegionInfoWrapperPass();
}

WRegionInfoWrapperPass::WRegionInfoWrapperPass() : FunctionPass(ID) {
  // DEBUG(dbgs() << "\nStart W-Region Information Collection Pass\n\n");
  initializeWRegionInfoWrapperPassPass(*PassRegistry::getPassRegistry());
}

void WRegionInfoWrapperPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<WRegionCollectionWrapperPass>();
}

bool WRegionInfoWrapperPass::runOnFunction(Function &F) {
  DEBUG(dbgs() << "\nENTER WRegionInfoWrapperPass::runOnFunction: "
               << F.getName() << "{\n");

  auto &WRC  = getAnalysis<WRegionCollectionWrapperPass>().getWRegionCollection();
  auto *DT   = WRC.getDomTree();
  auto *LI   = WRC.getLoopInfo();
  auto *SE   = WRC.getSE();
  auto *TTI  = WRC.getTargetTransformInfo();
  auto *AC   = WRC.getAssumptionCache();
  auto *TLI  = WRC.getTargetLibraryInfo();

  WRI.reset(new WRegionInfo(&F, DT, LI, SE, TTI, AC, TLI, &WRC));

  DEBUG(dbgs() << "\n}EXIT WRegionInfoWrapperPass::runOnFunction: "
               << F.getName() << "\n");
  return false;
}

void WRegionInfoWrapperPass::releaseMemory() { WRI.reset(); }

WRegionInfo::WRegionInfo(Function *F, DominatorTree *DT, LoopInfo *LI,
                         ScalarEvolution *SE, const TargetTransformInfo *TTI,
                         AssumptionCache *AC, const TargetLibraryInfo *TLI,
                         WRegionCollection *WRC)
    : Func(F), DT(DT), LI(LI), SE(SE), TTI(TTI), AC(AC), TLI(TLI), WRC(WRC) {}

void WRegionInfo::buildWRGraph(WRegionCollection::InputIRKind IR) {
  DEBUG(dbgs() << "\nENTER WRegionInfo::buildWRGraph(InpuIR="
               << IR <<"){\n");

  WRC->buildWRGraph(IR);

  DEBUG(dbgs() << "\nRC Size = " << WRC->getWRGraphSize() << "\n");
  for (auto I = WRC->begin(), E = WRC->end(); I != E; ++I)
    DEBUG((*I)->dump());

  DEBUG(dbgs() << "\n}EXIT WRegionInfo::buildWRGraph\n");
}

void WRegionInfo::print(raw_ostream &OS) const {
#if !INTEL_PRODUCT_RELEASE
  formatted_raw_ostream FOS(OS);

  for (auto I = begin(), E = end(); I != E; ++I) {
    FOS << "\n";
    (*I)->print(FOS, 0);
  }
#endif // !INTEL_PRODUCT_RELEASE
}
