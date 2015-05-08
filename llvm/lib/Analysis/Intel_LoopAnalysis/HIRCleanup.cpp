//===---- HIRCleanup.cpp - Clean up redundant HIR Nodes -----*- C++ -*-----===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the HIR cleanup pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/LoopInfo.h"

#include "llvm/Analysis/Intel_LoopAnalysis/HIRCleanup.h"
#include "llvm/Analysis/Intel_LoopAnalysis/HIRCreation.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Passes.h"

#include "llvm/Transforms/Intel_LoopTransforms/Utils/HLNodeUtils.h"

using namespace llvm;
using namespace llvm::loopopt;

#define DEBUG_TYPE "hir-cleanup"

INITIALIZE_PASS_BEGIN(HIRCleanup, "hir-cleanup", "HIR Cleanup", false, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRCreation)
INITIALIZE_PASS_END(HIRCleanup, "hir-cleanup", "HIR Cleanup", false, true)

char HIRCleanup::ID = 0;

FunctionPass *llvm::createHIRCleanupPass() { return new HIRCleanup(); }

HIRCleanup::HIRCleanup() : FunctionPass(ID) {
  initializeHIRCleanupPass(*PassRegistry::getPassRegistry());
}

void HIRCleanup::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.addRequiredTransitive<HIRCreation>();
}

HLNode *HIRCleanup::findHLNode(const BasicBlock *BB) const {
  auto It = LoopLatchHooks.find(const_cast<BasicBlock *>(BB));

  if (It != LoopLatchHooks.end()) {
    return It->second;
  }
  auto Iter = HIR->Labels.find(const_cast<BasicBlock *>(BB));

  if (Iter != HIR->Labels.end()) {
    return Iter->second;
  }

  llvm_unreachable("Could not find basic block's label!");
  return nullptr;
}

void HIRCleanup::eliminateRedundantGotos() {

  for (auto I = HIR->Gotos.begin(), E = HIR->Gotos.end(); I != E; ++I) {
    auto Goto = *I;

    auto LexSuccessor = HLNodeUtils::getLexicalControlFlowSuccessor(Goto);

    // If Goto's lexical successor is the same as its target then we can remove
    // it.
    if (LexSuccessor && isa<HLLabel>(LexSuccessor) &&
        (Goto->getTargetBBlock() ==
         cast<HLLabel>(LexSuccessor)->getSrcBBlock())) {
      HLNodeUtils::erase(Goto);
    } else {
      // Link Goto to its HLLabel target, if available.
      auto It = HIR->Labels.find(Goto->getTargetBBlock());

      if (It != HIR->Labels.end()) {
        Goto->setTargetLabel(It->second);
        RequiredLabels.insert(It->second);
      }
    }
  }
}

void HIRCleanup::eliminateRedundantLabels() {
  Loop *Lp;

  for (auto I = HIR->Labels.begin(), E = HIR->Labels.end(); I != E; ++I) {
    auto LabelBB = I->first;
    auto Label = I->second;

    // This HLLabel is redundant as no HLGoto is pointing to it.
    if (!RequiredLabels.count(Label)) {

      // This label represents loop latch bblock. We need to store the successor
      // as it is used by LoopFomation pass to find loop's bottom test.
      if ((Lp = LI->getLoopFor(LabelBB)) && (Lp->getLoopLatch() == LabelBB)) {
        auto LexSuccessor = HLNodeUtils::getLexicalControlFlowSuccessor(Label);

        HLContainerTy::iterator It(Label);
        assert(LexSuccessor && ((HLNode *)std::next(It) == LexSuccessor) &&
               "Unexpected loop latch label successor!");

        LoopLatchHooks[LabelBB] = LexSuccessor;
      }

      HLNodeUtils::erase(Label);
    }
  }
}

bool HIRCleanup::runOnFunction(Function &F) {
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  HIR = &getAnalysis<HIRCreation>();

  eliminateRedundantGotos();
  eliminateRedundantLabels();

  return false;
}

void HIRCleanup::releaseMemory() {
  LoopLatchHooks.clear();
  RequiredLabels.clear();
}

void HIRCleanup::print(raw_ostream &OS, const Module *M) const {
  formatted_raw_ostream FOS(OS);

  for (auto I = HIR->begin(), E = HIR->end(); I != E; ++I) {
    FOS << "\n";
    I->print(FOS, 0);
  }
}

void HIRCleanup::verifyAnalysis() const {
  // TODO: Implement later
}
