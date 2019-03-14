//===---- Intel_LoopCarriedCSE.cpp - Implements Loop Carried CSE Pass -----===//
//
// Copyright (C) 2018-2019 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This pass groups two Phi Nodes in a binary operation by a new Phi Node if
// their latch values have the same binary operation.
//
// For example:
// Convert
//
// for.preheader:
//   %gepload =
//   %gepload37 =
//   br %loop.25
//
// loop.25:
//   %t32.0 = phi i32 [ %gepload37, %for.preheader ], [ %gepload41, %loop.25 ]
//   %t30.0 = phi i32 [ %gepload, %for.preheader ], [%gepload39, %loop.25 ]
//   %1 = add i32 %t30.0, %t32.0
//   %4 = add i32 %gepload39, %gepload41
//
// To -
//
// for.preheader:
//   %gepload =
//   %gepload37 =
//   %1 = add i32 %gepload37, %gepload
//   br %loop.25
//
// loop.25:
//   %t32.0.lccse = phi i32 [ %1, %for.preheader ], [ %4, %loop.25 ]
//   %4 = add i32 %gepload39, %gepload41
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Scalar/Intel_LoopCarriedCSE.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/PassManager.h"
#include "llvm/IR/Value.h"
#include "llvm/Pass.h"
#include "llvm/Support/Casting.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/LoopUtils.h"

using namespace llvm;

#define LDIST_NAME "loop-carried-cse"
#define DEBUG_TYPE LDIST_NAME

// Returns user instruction which has opcode \p OpCode and operands \p LatchVal1
// and \p LatchVal2.
static User *findMatchedLatchUser(Value *LatchVal1, Value *LatchVal2,
                                  FPMathOperator *FPOp,
                                  Instruction::BinaryOps OpCode,
                                  bool IsSwappedOrder, BasicBlock *LoopLatch,
                                  DominatorTree *DT) {
  User *MatchedLatchUser = nullptr;

  for (User *U : LatchVal1->users()) {
    BinaryOperator *LatchBOp = dyn_cast<BinaryOperator>(U);

    if (!LatchBOp) {
      continue;
    }

    unsigned LatchOpCode = LatchBOp->getOpcode();

    if (LatchOpCode != OpCode) {
      continue;
    }

    FPMathOperator *LatchFPOp = dyn_cast<FPMathOperator>(U);

    if (FPOp && LatchFPOp && FPOp->isFast() != LatchFPOp->isFast()) {
      continue;
    }

    Value *V0 = LatchBOp->getOperand(0);
    Value *V1 = LatchBOp->getOperand(1);

    bool LatchIsSwappedOrder = V0 != LatchVal1;
    Value *LatchVal2Use = LatchIsSwappedOrder ? V0 : V1;

    if (LatchVal2Use != LatchVal2) {
      continue;
    }

    if (LatchIsSwappedOrder != IsSwappedOrder && !LatchBOp->isCommutative()) {
      continue;
    }

    if (!DT->dominates(LatchBOp->getParent(), LoopLatch)) {
      continue;
    }

    MatchedLatchUser = U;
    break;
  }
  return MatchedLatchUser;
}

static bool processLoop(Loop *L, DominatorTree *DT) {
  assert(L->empty() && "Only process inner loops.");

  LLVM_DEBUG(dbgs() << "\nLDist: In \""
                    << L->getHeader()->getParent()->getName() << "\" checking "
                    << *L << "\n");

  bool Modified = false;
  BasicBlock *Preheader = L->getLoopPreheader();
  BasicBlock *LoopLatch = L->getLoopLatch();

  if (!Preheader || !LoopLatch) {
    return false;
  }

  bool HasChanged;
  BasicBlock *Header = L->getHeader();

  do {
    // The flag showing whether a grouping is happened in the iteration
    HasChanged = false;

    for (PHINode &Phi : Header->phis()) {
      if (!Phi.hasOneUse()) {
        continue;
      }

      BinaryOperator *BOp = dyn_cast<BinaryOperator>(*Phi.users().begin());

      if (!BOp) {
        continue;
      }

      Instruction::BinaryOps OpCode = BOp->getOpcode();

      PHINode *P0 = dyn_cast<PHINode>(BOp->getOperand(0));
      PHINode *P1 = dyn_cast<PHINode>(BOp->getOperand(1));

      if (!P0 || !P1) {
        continue;
      }

      bool IsSwappedOrder = P0 != &Phi;
      PHINode *Phi2 = IsSwappedOrder ? P0 : P1;

      if (Phi2->getParent() != Header) {
        continue;
      }

      Value *LatchVal1 = nullptr;
      Value *LatchVal2 = nullptr;
      Value *PreheaderValue1 = nullptr;
      Value *PreheaderValue2 = nullptr;

      if (Phi.getIncomingBlock(0) == LoopLatch) {
        LatchVal1 = Phi.getIncomingValue(0);
        PreheaderValue1 = Phi.getIncomingValue(1);
      } else {
        LatchVal1 = Phi.getIncomingValue(1);
        PreheaderValue1 = Phi.getIncomingValue(0);
      }

      if (Phi2->getIncomingBlock(0) == LoopLatch) {
        LatchVal2 = Phi2->getIncomingValue(0);
        PreheaderValue2 = Phi2->getIncomingValue(1);
      } else {
        LatchVal2 = Phi2->getIncomingValue(1);
        PreheaderValue2 = Phi2->getIncomingValue(0);
      }

      FPMathOperator *FPOp = dyn_cast<FPMathOperator>(*Phi.users().begin());

      User *MatchedLatchUser = findMatchedLatchUser(
          LatchVal1, LatchVal2, FPOp, OpCode, IsSwappedOrder, LoopLatch, DT);

      if (!MatchedLatchUser) {
        continue;
      }

      IRBuilder<> Builder(Preheader->getTerminator());
      Value *V = nullptr;

      if (!IsSwappedOrder) {
        V = Builder.CreateBinOp(OpCode, PreheaderValue1, PreheaderValue2);
      } else {
        V = Builder.CreateBinOp(OpCode, PreheaderValue2, PreheaderValue1);
      }

      IRBuilder<> PHIBuilder(&Phi);

      PHINode *NewPhi =
          PHIBuilder.CreatePHI(Phi.getType(), 2, Phi.getName() + ".lccse");
      NewPhi->addIncoming(V, Preheader);
      NewPhi->addIncoming(MatchedLatchUser, LoopLatch);

      // Check whether Phi2 has one use before we erase BOp below
      bool CanErasePhi2 = Phi2->hasOneUse();

      BOp->replaceAllUsesWith(NewPhi);
      BOp->eraseFromParent();

      Phi.dropAllReferences();
      Phi.eraseFromParent();

      if (CanErasePhi2) {
        Phi2->dropAllReferences();
        Phi2->eraseFromParent();
      }

      HasChanged = true;
      Modified = true;
      break;
    }
  } while (HasChanged);

  return Modified;
}

static bool runImpl(LoopInfo *LI, DominatorTree *DT) {
  bool Changed = false;

  auto Loops = LI->getLoopsInPreorder();

  for (Loop *Lp : Loops) {
    if (Lp->empty()) {
      Changed |= processLoop(Lp, DT);
    }
  }

  return Changed;
}

namespace {

/// The pass class.
class LoopCarriedCSELegacy : public FunctionPass {
public:
  static char ID;

  LoopCarriedCSELegacy() : FunctionPass(ID) {
    // The default is set by the caller.
    initializeLoopCarriedCSELegacyPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    if (skipFunction(F))
      return false;

    auto *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
    auto *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();

    return runImpl(LI, DT);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<LoopInfoWrapperPass>();
    AU.addPreserved<LoopInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addPreserved<DominatorTreeWrapperPass>();
    AU.setPreservesCFG();
  }
};

} // end anonymous namespace

PreservedAnalyses LoopCarriedCSEPass::run(Function &F,
                                          FunctionAnalysisManager &AM) {
  auto &LI = AM.getResult<LoopAnalysis>(F);
  auto &DT = AM.getResult<DominatorTreeAnalysis>(F);

  bool Changed = runImpl(&LI, &DT);
  if (!Changed)
    return PreservedAnalyses::all();
  PreservedAnalyses PA;
  PA.preserve<LoopAnalysis>();
  PA.preserve<DominatorTreeAnalysis>();
  PA.preserveSet<CFGAnalyses>();
  return PA;
}

char LoopCarriedCSELegacy::ID;

static const char ldist_name[] = "Loop Carried CSE";

INITIALIZE_PASS_BEGIN(LoopCarriedCSELegacy, LDIST_NAME, ldist_name, false,
                      false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_END(LoopCarriedCSELegacy, LDIST_NAME, ldist_name, false, false)

FunctionPass *llvm::createLoopCarriedCSEPass() {
  return new LoopCarriedCSELegacy();
}
