//===- RegionIdentification.cpp - Identifies HIR Regions *- C++ -*---------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
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

#include "llvm/IR/Intel_LoopIR/IRRegion.h"
#include "llvm/IR/Intel_LoopIR/CanonExpr.h"

#include "llvm/Analysis/Intel_LoopAnalysis/RegionIdentification.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Passes.h"

using namespace llvm;
using namespace llvm::loopopt;

#define DEBUG_TYPE "hir-regions"

INITIALIZE_PASS_BEGIN(RegionIdentification, "hir-regions",
                      "HIR Region Identification", false, true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_END(RegionIdentification, "hir-regions",
                    "HIR Region Identification", false, true)

char RegionIdentification::ID = 0;

FunctionPass *llvm::createRegionIdentificationPass() {
  return new RegionIdentification();
}

RegionIdentification::RegionIdentification() : FunctionPass(ID) {
  initializeRegionIdentificationPass(*PassRegistry::getPassRegistry());
}

unsigned RegionIdentification::insertBaseTemp(const Value *Temp) {
  BaseTemps.push_back(Temp);
  // Offset of 1 for "constant" symbase.
  return BaseTemps.size() + 1;
}

const Value *RegionIdentification::getBaseTemp(unsigned Symbase) const {
  assert((Symbase > 0 && Symbase <= (BaseTemps.size() + 1)) &&
         "Symbase is out of range!");

  return BaseTemps[Symbase - 2];
}

void RegionIdentification::insertTempSymbase(const Value *Temp,
                                             unsigned Symbase) {
  assert((Symbase > 0 && Symbase <= (BaseTemps.size() + 1)) &&
         "Symbase is out of range!");

  auto Ret = TempSymbaseMap.insert(std::make_pair(Temp, Symbase));

  assert(Ret.second && "Attempt to overwrite Temp symbase!");
}

unsigned RegionIdentification::findTempSymbase(const Value *Temp) const {

  auto Iter = TempSymbaseMap.find(Temp);

  if (Iter != TempSymbaseMap.end()) {
    return Iter->second;
  }

  return 0;
}

void RegionIdentification::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<DominatorTreeWrapperPass>();
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.addRequiredTransitive<ScalarEvolution>();
}

bool RegionIdentification::isSelfGenerable(const Loop &Lp,
                                           unsigned LoopnestDepth) const {

  // Loop is not in a handleable form.
  if (!Lp.isLoopSimplifyForm()) {
    return false;
  }

  // At least one of this loop's subloops reach MaxLoopNestLevel so we cannot
  // generate this loop.
  if (LoopnestDepth > MaxLoopNestLevel) {
    return false;
  }

  const SCEV *BETC = SE->getBackedgeTakenCount(&Lp);

  // Don't handle unknown loops for now.
  if (isa<SCEVCouldNotCompute>(BETC)) {
    return false;
  }

  for (auto I = Lp.block_begin(), E = Lp.block_end(); I != E; ++I) {

    // Skip this bblock as it has been checked by an inner loop.
    if (!Lp.empty() && LI->getLoopFor(*I) != (&Lp)) {
      continue;
    }

    if ((*I)->isLandingPad()) {
      return false;
    }

    auto Term = (*I)->getTerminator();

    if (isa<IndirectBrInst>(Term) || isa<InvokeInst>(Term) ||
        isa<ResumeInst>(Term)) {
      return false;
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
  this->Func = &F;

  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  SE = &getAnalysis<ScalarEvolution>();

  formRegions();

  return false;
}

void RegionIdentification::releaseMemory() {

  IRRegion::destroyAll();

  IRRegions.clear();

  BaseTemps.clear();
  TempSymbaseMap.clear();
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
