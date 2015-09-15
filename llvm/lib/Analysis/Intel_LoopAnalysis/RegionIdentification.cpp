//===- RegionIdentification.cpp - Identifies HIR Regions ------------------===//
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
// This file implements the HIR Region Identification pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"

#include "llvm/IR/Dominators.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"

#include "llvm/Support/Debug.h"

#include "llvm/IR/Intel_LoopIR/IRRegion.h"
#include "llvm/IR/Intel_LoopIR/CanonExpr.h"

#include "llvm/Analysis/Intel_LoopAnalysis/RegionIdentification.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Passes.h"

using namespace llvm;
using namespace llvm::loopopt;

#define DEBUG_TYPE "hir-region-identification"

INITIALIZE_PASS_BEGIN(RegionIdentification, "hir-region-identification",
                      "HIR Region Identification", false, true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_END(RegionIdentification, "hir-region-identification",
                    "HIR Region Identification", false, true)

char RegionIdentification::ID = 0;

FunctionPass *llvm::createRegionIdentificationPass() {
  return new RegionIdentification();
}

RegionIdentification::RegionIdentification() : FunctionPass(ID) {
  initializeRegionIdentificationPass(*PassRegistry::getPassRegistry());
}

void RegionIdentification::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<DominatorTreeWrapperPass>();
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.addRequiredTransitive<ScalarEvolution>();
}

bool RegionIdentification::isSelfGenerable(const Loop &Lp,
                                           unsigned LoopnestDepth) const {

  // At least one of this loop's subloops reach MaxLoopNestLevel so we cannot
  // generate this loop.
  if (LoopnestDepth > MaxLoopNestLevel) {
    DEBUG(dbgs() << "LOOPOPT_OPTREPORT: Loopnest is more than "
                 << MaxLoopNestLevel << " deep.\n");
    return false;
  }

  // Loop is not in a handleable form.
  if (!Lp.isLoopSimplifyForm()) {
    DEBUG(dbgs() << "LOOPOPT_OPTREPORT: Loop structure is not handleable.\n");
    return false;
  }

  // Don't handle multi-exit loops for now.
  if (!Lp.getExitingBlock()) {
    DEBUG(dbgs()
          << "LOOPOPT_OPTREPORT: Multi-exit loops currently not supported.\n");
    return false;
  }

  // Don't handle unknown loops for now.
  if (!SE->hasLoopInvariantBackedgeTakenCount(&Lp)) {
    DEBUG(dbgs()
          << "LOOPOPT_OPTREPORT: Unknown loops currently not supported.\n");
    return false;
  }

  // Check that the loop backedge is a conditional branch.
  auto LatchBB = Lp.getLoopLatch();

  auto Term = LatchBB->getTerminator();
  auto BrInst = dyn_cast<BranchInst>(Term);

  if (!BrInst) {
    DEBUG(dbgs() << "LOOPOPT_OPTREPORT: Non-branch instrcutions in loop latch "
                    "currently not supported.\n");
    return false;
  }

  if (BrInst->isUnconditional()) {
    DEBUG(dbgs() << "LOOPOPT_OPTREPORT: Unconditional branch instrcutions in "
                    "loop latch currently not supported.\n");
    return false;
  }

  // Check instructions inside the loop.
  for (auto I = Lp.block_begin(), E = Lp.block_end(); I != E; ++I) {

    // Skip this bblock as it has been checked by an inner loop.
    if (!Lp.empty() && LI->getLoopFor(*I) != (&Lp)) {
      continue;
    }

    if ((*I)->isLandingPad()) {
      DEBUG(dbgs() << "LOOPOPT_OPTREPORT: Exception handling currently not "
                      "supported.\n");
      return false;
    }

    Term = (*I)->getTerminator();

    if (isa<IndirectBrInst>(Term)) {
      DEBUG(dbgs() << "LOOPOPT_OPTREPORT: Indirect branches currently not "
                      "supported.\n");
      return false;
    }

    if (isa<InvokeInst>(Term) || isa<ResumeInst>(Term)) {
      DEBUG(dbgs() << "LOOPOPT_OPTREPORT: Exception handling currently not "
                      "supported.\n");
      return false;
    }

    for (auto InstIt = (*I)->begin(), EndIt = (*I)->end(); InstIt != EndIt;
         ++InstIt) {
      if (InstIt->getType()->isVectorTy()) {
        DEBUG(dbgs()
              << "LOOPOPT_OPTREPORT: Vector types currently not supported.\n");
        return false;
      }

      if (auto GEPInst = dyn_cast<GetElementPtrInst>(InstIt)) {
        auto SrcTy = GEPInst->getSourceElementType();

        while (auto ArrTy = dyn_cast<ArrayType>(SrcTy)) {
          SrcTy = ArrTy->getElementType();
        }

        if (SrcTy->isStructTy()) {
          DEBUG(dbgs()
                << "LOOPOPT_OPTREPORT: Struct GEPs currently not supported.\n");
          return false;
        }
      }
    }
  }

  return true;
}

void RegionIdentification::createRegion(const Loop &Lp) {
  IRRegion *Reg = new IRRegion(
      Lp.getHeader(),
      IRRegion::RegionBBlocksTy(Lp.getBlocks().begin(), Lp.getBlocks().end()));

  IRRegions.push_back(Reg);
}

bool RegionIdentification::formRegionForLoop(const Loop &Lp,
                                             unsigned *LoopnestDepth) {
  SmallVector<Loop *, 8> GenerableLoops;
  bool Generable = true;

  *LoopnestDepth = 0;

  // Check which sub loops are generable.
  for (auto I = Lp.begin(), E = Lp.end(); I != E; ++I) {
    unsigned SubLoopnestDepth;

    if (formRegionForLoop(**I, &SubLoopnestDepth)) {
      GenerableLoops.push_back(*I);

      // Set maximum sub-loopnest depth
      *LoopnestDepth = std::max(*LoopnestDepth, SubLoopnestDepth);
    } else {
      Generable = false;
    }
  }

  // Check whether Lp is generable.
  if (Generable && !isSelfGenerable(Lp, ++(*LoopnestDepth))) {
    Generable = false;
  }

  // Lp itself is not generable so create regions for generable sub loops.
  if (!Generable) {
    // TODO: add logic to merge fuseable loops. This might also require
    // recognition of ztt and splitting basic blocks which needs to be done
    // in a transformation pass.
    for (auto I = GenerableLoops.begin(), E = GenerableLoops.end(); I != E;
         ++I) {
      createRegion(**I);
    }
  }

  return Generable;
}

void RegionIdentification::formRegions() {
  // LoopInfo::iterator visits loops in reverse program order so we need to use
  // reverse_iterator here.
  for (LoopInfo::reverse_iterator I = LI->rbegin(), E = LI->rend(); I != E;
       ++I) {
    unsigned Depth;
    if (formRegionForLoop(**I, &Depth)) {
      createRegion(**I);
    }
  }
}

bool RegionIdentification::runOnFunction(Function &F) {
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  SE = &getAnalysis<ScalarEvolution>();

  formRegions();

  return false;
}

void RegionIdentification::releaseMemory() {
  IRRegion::destroyAll();
  IRRegions.clear();
}

void RegionIdentification::print(raw_ostream &OS, const Module *M) const {
  for (auto I = IRRegions.begin(), E = IRRegions.end(); I != E; ++I) {
    OS << "\nRegion " << I - IRRegions.begin() + 1 << "\n";
    (*I)->print(OS, 3);
    OS << "\n";
  }
}

void RegionIdentification::verifyAnalysis() const {
  /// TODO: implement later
}
