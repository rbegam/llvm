//===--- ParSecTrans.cpp - Pre-pass Transformations of Parallel Sections --===//
//
// Copyright (C) 2016 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive property
// of Intel Corporation and may not be disclosed, examined or reproduced in
// whole or in part without explicit written authorization from the company.
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// \file
//
// This file implements VPO pre-pass transformations for parallel sections,
// which transforms OpenMP parallel sections to parallel do loop and OpenMP
// work-sharing sections to work-sharing do loop:
//
// #pragma omp parallel sections // or #pragma omp sections
// {
//   #pragma omp section
//     Xdirection();
//   #pragma omp section
//     Ydirection();
//   #pragma omp section
//     Zdirection();
// }
//
// is transformed to
//
// #pragma omp parallel sections   // or #pragma omp sections
//   for (int i = 0; i <= 2 ; i++) {
//     switch(i) {
//       case 0:
//         Xdirection();
//         break;
//       case 1:
//         Ydirection();
//         break;
//       case 2:
//         Zdirection();
//         break;
//       default:
//     }
//   }
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Function.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/ADT/Twine.h"
#include "llvm/IR/IRBuilder.h"
#include "llvm/IR/LLVMContext.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Transforms/Intel_VPO/Utils/VPOUtils.h"


using namespace llvm;
using namespace llvm::vpo;

#define DEBUG_TYPE "vpo-parsectrans"

/////////////// Transformation Description /////////////////////
//
// For the following user code,
//
// #pragma omp parallel sections   (or #pragma omp sections)
// {
//   #pragma omp section  (this directive can be omitted in the user code)
//     Xdirection();
//   #pragma omp section
//     Ydirection();
//   #pragma omp section
//     Zdirection();
// }
//
// the compiler will generate the following CFG:
//
//      DIR_OMP_PARALLEL_SECTIONS    (or DIR_OMP_SECTIONS)
//                |
//          DIR_OMP_SECTION          (this directive must present in CFG)
//                |
//            Xdirection()
//                |
//       DIR_OMP_END_SECTION         (this directive must present in CFG)
//                |
//          DIR_OMP_SECTION
//                |
//            Ydirection()
//                |
//       DIR_OMP_END_SECTION
//                |
//          DIR_OMP_SECTION
//                |
//            Zdirection()
//                |
//       DIR_OMP_END_SECTION
//                |
//    DIR_OMP_END_PARALLEL_SECTIONS (or DIR_END_OMP_SECTIONS)
//
// which is the input to this transformation. Note that:
//
// 1) Each directive must have an END directive to pair with;
//
// 2) Each directive is represented by a group of Intel directive intrinsics
// that must reside in a standalone basic block, e.g.,
//
// par.sections.begin:
//   call void @llvm.intel.directive(metadata !"DIR.OMP.PARALLEL.SECTIONS")
//   .... // directive qualifiers
//   ...
//   call void @llvm.intel.directive(metadata !"DIR.QUAL.LIST.END")
//   br label %par.sections.body
//
// ...
//
// par.sections.end:
//   call void @llvm.intel.directive(metadata !"DIR.OMP.END.PARALLEL.SECTIONS")
//   .... // directive qualifiers
//   ...
//   call void @llvm.intel.directive(metadata !"DIR.QUAL.LIST.END")
//   br label %after.par
//
// 3) The directive DIR_OMP_SECTION/DIR_OMP_END_SECTION for the first section
// must be presented in the CFG, although it can be omitted in the user code;
//
// 4) There can be data flow across sections inside a parallel
// section or work-sharing section; however, if that happens, the variable and
// its related operations will be guarded in a critical section which inforces
// the load and store of the variable from/to memory first (this is OpenMP
// shared-memory model). In other words, such variable will not be
// registerized, and we do not have to worry about the SSA form or update for
// it.
//
// 5) Each OMP_PARALLEL_SECTIONS, OMP_SECTIONS and OMP_SECTION must form a
// single-entry and single-exit region;
//
// 6) OMP_PARALLEL_SECTIONS or OMP_SECTIONS can be an empty region, e.g.,(in
// the CFG form)
//
//      DIR_OMP_PARALLEL_SECTIONS    (or DIR_OMP_SECTIONS)
//                |
//                |                  (no code between them)
//                |
//    DIR_OMP_END_PARALLEL_SECTIONS  (or DIR_OMP_END_SECTIONS)
//
// 7) OMP_PARALLEL_SECTIONS can be nested, for example (presented in user
// code):
//
// #pragma omp parallel sections                  (par1)
// {
//   #pragma omp section                          (sec1)
//     #pragma omp parallel sections              (par2)
//       #pragma omp section                      (sec2)
//       #pragma omp section                      (sec3)
//
//   #pragma omp section                          (sec4)
//     #pragma omp parallel sections              (par3)
//       #pragma omp section                      (sec5)
//       #pragma omp section                      (sec6)
//
//   #pragma omp section                          (sec7)
// }
// #pragma omp sections                           (secs1)
// {
//   //empty
// }
//
// To do the transformations, we build a Section Tree first (based on
// Dominator Tree) to represent this nested relationship. For example, for the
// above code, we will have:
//
//                 Root
//                 /  \
//             secs1   par1
//                   /   |  \
//                sec1  sec4 sec7
//                 |      |
//               par2    par3
//               / \     / \
//            sec2sec3 sec5sec6
//
// (Note that, the order of children of each node does not matter.)
//
// And then, we traverse the Section Tree in a post order, and do the
// transformation for each OMP_PARALLEL_SECTIONS or OMP_SECTIONS node in the
// tree if it has children (which must be OMP_SECTION nodes). We use the
// post-order traversal since we can delete/free all the children after we visit
// each node. By the time we finish all the transformations, only Root node is
// left in the tree, which we will delete at last.
//
// The transformation function returns TRUE if any transformation happens,
// otherwise returns FALSE.
//
bool VPOUtils::parSectTransformer(
  Function *F,
  DominatorTree *DT
)
{
  ParSectNode *Root = buildParSectTree(F, DT);

#ifdef DEBUG
  printParSectTree(Root);
#endif

  // Keep track of how many transformations take place; used for naming
  // purpose.
  int Counter = 0;

  parSectTransRecursive(F, Root, Counter, DT);

  delete Root;

  if (Counter)
    return true;
  else
    return false;
}

ParSectNode *VPOUtils::buildParSectTree(Function *F, DominatorTree *DT)
{
  // Used to find a pair of directives' basic blocks.
  std::stack<ParSectNode *> SectStack;
  std::stack<ParSectNode *> ImpSectStack;

  // The Root node does not correspond to any region represented by
  // OMP_PARALLEL_SECTIONS, OMP_SECTIONS or OMP_SECTION, but its children do.
  ParSectNode *Root = new ParSectNode();
  ParSectNode *ImpRoot = new ParSectNode();

  ImpRoot->EntryBB = nullptr;
  ImpRoot->ExitBB  = nullptr;
  ImpSectStack.push(ImpRoot);

  gatherImplicitSectionRecursive(&F->getEntryBlock(), ImpSectStack, DT);

#ifdef DEBUG
  printParSectTree(ImpRoot);
#endif

  int Cnt = 0;
  insertSectionRecursive(F, ImpRoot, Cnt, DT);

  delete ImpRoot;

  Root->EntryBB = nullptr;
  Root->ExitBB = nullptr;

  SectStack.push(Root);

  buildParSectTreeRecursive(&F->getEntryBlock(), SectStack, DT);

  return Root;
}


// Pre-order traversal on Dominator Tree with the use of a stack
// to build Section Tree.
void VPOUtils::gatherImplicitSectionRecursive(
  BasicBlock* BB,
  std::stack<ParSectNode *> &ImpSectStack,
  DominatorTree *DT
)
{
  auto DomNode = DT->getNode(BB);
  ParSectNode *Node = nullptr;

  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {

    if (dyn_cast<IntrinsicInst>(&*I)) {
      int DirID = VPOAnalysisUtils::getDirectiveID(&*I);
      if (DirID == DIR_OMP_SECTIONS ||
          DirID == DIR_OMP_PARALLEL_SECTIONS) {

        BasicBlock *SuccBB = BB->getUniqueSuccessor();
        BasicBlock::iterator SI = SuccBB->begin();
        bool IsDirSection = false;

        if (dyn_cast<TerminatorInst>(&*SI)) {
          SuccBB = SuccBB->getUniqueSuccessor();
          SI = SuccBB->begin();
        }

        if (dyn_cast<IntrinsicInst>(&*SI)) {
          int SuccDirID = VPOAnalysisUtils::getDirectiveID(&*SI);
          if (SuccDirID == DIR_OMP_SECTION) {
            IsDirSection = true;
          }
        }

        if (!IsDirSection) {
           Node = new ParSectNode();
           Node->EntryBB = BB;
           Node->ExitBB  = nullptr;
           Node->DirBeginID = DirID;

           ParSectNode *Parent = ImpSectStack.top();
           Parent->Children.push_back(Node);
           ImpSectStack.push(Node);
        }
      }

      if (DirID == DIR_OMP_SECTION ||
          (DirID == DIR_OMP_END_SECTIONS ||
           DirID == DIR_OMP_END_PARALLEL_SECTIONS)) {

        Node = ImpSectStack.top();
        if (Node && Node->ExitBB == nullptr &&
            (Node->DirBeginID == DIR_OMP_SECTIONS ||
             Node->DirBeginID == DIR_OMP_PARALLEL_SECTIONS)) {

          bool IsMatchedImpEnd = false;

          if (DirID == DIR_OMP_SECTION) {
            IsMatchedImpEnd = true;
          }
          else {
            BasicBlock *PredBB = BB->getUniquePredecessor();
            BasicBlock::iterator EI = PredBB->begin();

            if (dyn_cast<TerminatorInst>(&*EI)) {
              PredBB = PredBB->getUniquePredecessor();
              EI = PredBB->begin();
            }

            if (dyn_cast<IntrinsicInst>(&*EI)) {
              int PredDirID = VPOAnalysisUtils::getDirectiveID(&*EI);
              if (PredDirID != DIR_OMP_END_SECTION) {
                IsMatchedImpEnd = true;
              }
            }
            else {
              IsMatchedImpEnd = true;
            }
          }

          if (IsMatchedImpEnd) {
            Node->ExitBB = BB;
            ImpSectStack.pop();
          }
        }
      }
    }
  }

  /// Walk over dominator children.
  for (auto D = DomNode->begin(), E = DomNode->end(); D != E; ++D) {
    auto DomChildBB = (*D)->getBlock();
    gatherImplicitSectionRecursive(DomChildBB, ImpSectStack, DT);
  }
}


// Pre-order traversal on Dominator Tree with the use of a stack
// to build Section Tree.
void VPOUtils::buildParSectTreeRecursive(
  BasicBlock* BB,
  std::stack<ParSectNode *> &SectStack,
  DominatorTree *DT
)
{
  auto DomNode = DT->getNode(BB);
  ParSectNode *Node = nullptr;

  for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) {

    if (dyn_cast<IntrinsicInst>(&*I)) {
      int DirID = VPOAnalysisUtils::getDirectiveID(&*I);
      if (DirID == DIR_OMP_SECTION ||
          DirID == DIR_OMP_SECTIONS ||
          DirID == DIR_OMP_PARALLEL_SECTIONS) {

        Node = new ParSectNode();
        Node->EntryBB = BB;
        Node->DirBeginID = DirID;

        ParSectNode *Parent = SectStack.top();
        Parent->Children.push_back(Node);
        SectStack.push(Node);
      }

      if (DirID == DIR_OMP_END_SECTION ||
          DirID == DIR_OMP_END_SECTIONS ||
          DirID == DIR_OMP_END_PARALLEL_SECTIONS) {

        Node = SectStack.top();
        Node->ExitBB = BB;

        SectStack.pop();
      }
    }
  }

  /// Walk over dominator children.
  for (auto D = DomNode->begin(), E = DomNode->end(); D != E; ++D) {
    auto DomChildBB = (*D)->getBlock();
    buildParSectTreeRecursive(DomChildBB, SectStack, DT);
  }
}

// Pre-order traversal on Section Tree to print debug messages.
void VPOUtils::printParSectTree(ParSectNode *Node)
{

  if (!Node->EntryBB && !Node->ExitBB) {
    DEBUG(dbgs() << "\nSectionTree: Root:\n");
  }
  else if (Node->EntryBB && Node->ExitBB) {
    DEBUG(dbgs() << "\n\n\nSectionTreeNode: EntryBB:\n"
                 << *(Node->EntryBB)
                 << "\nExitBB:\n"
                 << *(Node->ExitBB));
  }

  if (Node->Children.size() == 0) {
    DEBUG(dbgs() << "\nNo Children:\n");
    return;
  }

  DEBUG(dbgs() << "\nStarting Chidren Printing:\n");

  for (auto *Child: Node->Children)
    printParSectTree(Child);

  DEBUG(dbgs() << "\nEnding Chidren Printing:\n");
}

// Post-order traversal
void VPOUtils::insertSectionRecursive(
  Function *F,
  ParSectNode *Node,
  int &Counter,
  DominatorTree *DT
)
{
  if (Node->Children.size() != 0) {
    // Insert from inner to outer
    for (auto *Child: Node->Children)
      insertSectionRecursive(F, Child, Counter, DT);

    // Free children
    for (auto *Child: Node->Children)
      delete Child;
  }

  // We only need to insert SECTION to OMP_PARALLEL_SECTIONS and
  // OMP_SECTIONS nodes, not OMP_SECTION nodes or the tree Root.
  //
  if (Node->EntryBB && Node->ExitBB &&
      (Node->DirBeginID == DIR_OMP_SECTIONS ||
       Node->DirBeginID == DIR_OMP_PARALLEL_SECTIONS)) {

    Module *M = F->getParent();
    Counter++;

    SmallVector<llvm::Value*, 1> BundleValue;
    llvm::OperandBundleDef EntryOpB("DIR.OMP.SECTION", BundleValue);

    SmallVector<llvm::OperandBundleDef, 1> EntryOpBundle;
    EntryOpBundle.push_back(EntryOpB);

    Function *DirEntry =
      Intrinsic::getDeclaration(M, Intrinsic::directive_region_entry);

    SmallVector<llvm::Value*, 1> Arg;

    Instruction *I = &Node->EntryBB->front();

    CallInst *DirEntryCI = CallInst::Create(DirEntry, Arg, EntryOpBundle, "");

    DirEntryCI->insertAfter(I);

    BasicBlock *SecEntry = SplitBlock(Node->EntryBB, DirEntryCI, DT, nullptr);
    SecEntry->setName("implicit.section.entry." + Twine(Counter));

    SmallVector<llvm::Value*, 1> ExitBundleValue;
    llvm::OperandBundleDef ExitOpB("DIR.OMP.END.SECTION", ExitBundleValue);

    SmallVector<llvm::OperandBundleDef, 1> ExitOpBundle;
    ExitOpBundle.push_back(ExitOpB);

    Function *DirExit =
      Intrinsic::getDeclaration(M, Intrinsic::directive_region_exit);

    SmallVector<llvm::Value*, 1> ArgIn;
    ArgIn.push_back(DirEntryCI);

    I = &Node->ExitBB->front();

    CallInst *DirExitCI = CallInst::Create(DirExit, ArgIn, ExitOpBundle, "");
    DirExitCI->insertBefore(I);

    BasicBlock *SecExitSucc = SplitBlock(Node->ExitBB, I, DT, nullptr);
    SecExitSucc->setName("implicit.section.exit.succ." + Twine(Counter));
  }
  return;
}


// Post-order traversal
void VPOUtils::parSectTransRecursive(
  Function *F,
  ParSectNode *Node,
  int &Counter,
  DominatorTree *DT
)
{
  // This is a leaf node. Nothing to do with it.
  if (Node->Children.size() == 0)
    return;

  for (auto *Child: Node->Children)
    parSectTransRecursive(F, Child, Counter, DT);

  // We only need to do transformations at OMP_PARALLEL_SECTIONS and
  // OMP_SECTIONS nodes, not OMP_SECTION nodes or the tree Root.
  //
  if (Node->EntryBB && Node->ExitBB)
    if (Node->DirBeginID == DIR_OMP_SECTIONS ||
        Node->DirBeginID == DIR_OMP_PARALLEL_SECTIONS) {

      // Just a check
      for (auto *Child: Node->Children) {
        if (!Child->EntryBB->getSinglePredecessor())
          DEBUG(dbgs() << "Not a single-entry OMP Section\n");

        if (!Child->ExitBB->getSingleSuccessor())
          DEBUG(dbgs() << "Not a single-exit OMP Section\n");
      }

      Counter++;
      doParSectTrans(F, Node, Counter, DT);
    }

  // Free children
  for (auto *Child: Node->Children)
    delete Child;
}

// This function does the real transformation work. For the following CFG:
//
//      OMP_PARALLEL_SECTIONS  (or OMP_SECTIONS)
//                |
//          OMP_SECTION
//                |
//               X()
//                |
//          OMP_END_SECTION
//                |
//          OMP_SECTION
//                |
//               Y()
//                |
//         OMP_END_SECTION
//                |
//              .....
//                |
//    OMP_END_PARALLEL_SECTIONS
//
// the function transforms it to:
//
//      OMP_PARALLEL_SECTIONS  (or OMP_SECTIONS)
//                |
//          Loop PreheaderBB
//                |
//            Loop HeaderBB: <-------------|
//             i = phi(0, i')              |
//             switch (i)                  |
//             /  |  ...  \                |
//            /   |   ...  \               |
//  OMP_SECTION OMP_SECTION..DefaultBB     |
//       |        |             /          |
//      X()      Y()           /           |
//       |        |           /            |
//  END_SECTION END_SECTION  /             |
//       \         |        /              |
//        \        |       /               |
//         \       | ...  /                |
//          SwitchEpilogBB                 |
//                 |                       |
//             SwitchSuccBB:               |
//               i' = i + 1                |
//          if (i' <= (NumSections-1))     |
//                | |                      |
//                | |----------------------|
//                |
//            Loop ExitBB
//                |
//    OMP_END_PARALLEL_SECTIONS
//
// Note that, the directives OMP_SECTION and OMP_END_SECTION will be deleted
// although we show them here for illustration purpose.
//
void VPOUtils::doParSectTrans(
  Function *F,
  ParSectNode *Node,
  int Counter,
  DominatorTree *DT
)
{
  assert(Node->Children.size() != 0 && "No section nodes to be transformed");

  BasicBlock *SectionsEntryBB = Node->EntryBB;
  BasicBlock *SectionsExitBB = Node->ExitBB;

  // 1) Take all sections out first, so OMP_PARALLEL_SECTIONS(or OMP_SECTIONS)
  // is directly connected to OMP_END_PARALLEL_SECTIONS(or OMP_END_SECTIONS)
  //
  //      OMP_PARALLEL_SECTIONS  (or OMP_SECTIONS)
  //                |
  //    OMP_END_PARALLEL_SECTIONS
  //
  IRBuilder<> Builder(SectionsEntryBB);
  SectionsEntryBB->getTerminator()->eraseFromParent();
  Builder.CreateBr(SectionsExitBB);

  // 2) Insert an empty loop between the pair of directives:
  //
  //      OMP_PARALLEL_SECTIONS  (or OMP_SECTIONS)
  //                |
  //          Loop PreheaderBB
  //                |
  //            Loop HeaderBB: <-------------|
  //             i = phi(0, i')              |
  //             i' = i + 1                  |
  //          if (i' <= (NumSections-1))     |
  //                | |----------------------|
  //                |
  //            Loop ExitBB
  //                |
  //    OMP_END_PARALLEL_SECTIONS
  //
  // Generating "if (i' < = NumSections -1)" is more efficient than
  // "if (i' < NumSections)" for the loop.
  //
  // generateLoop() will return the first non-PHI instruction as the loop
  // insertion point, for the following code to insert the loop body.
  //
  unsigned NumSections = Node->Children.size();
  IntegerType *IntTy = Type::getInt32Ty(F->getContext());
  Constant *LB = ConstantInt::get(IntTy, 0);
  Constant *UB = ConstantInt::get(IntTy, (NumSections - 1));
  Constant *Stride = ConstantInt::get(IntTy, 1);
  Value *NormalizedUB = nullptr;

  Value *IV = genNewLoop(LB, UB, Stride, Builder, Counter, NormalizedUB, DT);

  // 3) Insert a switch statement right before the first non-PHI instruction
  // in the loop. The code (e.g., sec1, sec2) for each OMP_SECTION will be
  // inserted at the point for each case of the switch.
  //
  //      OMP_PARALLEL_SECTIONS  (or OMP_SECTIONS)
  //                |
  //          Loop PreheaderBB
  //                |
  //            Loop HeaderBB: <-------------|
  //             i = phi(0, i')              |
  //             switch (i)                  |
  //             /  |  ...  \                |
  //        sec1  sec2 ... DefualtCase       |
  //          \      | ...  /                |
  //          SwitchEpilogBB                 |
  //                 |                       |
  //             SwitchSuccBB:               |
  //               i' = i + 1                |
  //          if (i' <= (NumSections-1))     |
  //                | |                      |
  //                | |----------------------|
  //                |
  //            Loop ExitBB
  //                |
  //      OMP_END_PARALLEL_SECTIONS
  //
  // We always generate an empty basic block for the default case, which does
  // not correspond to any OMP_SECTION since CreateSwitch needs it.
  //
  // generateSwitch() will return the switch instruction created.
  //
  genParSectSwitch(IV, Node, Builder, Counter, DT);

  Instruction *Inst = Node->EntryBB->getFirstNonPHI();
  CallInst *CI = dyn_cast<CallInst>(Inst);
  SmallVector<Value *, 8> Args;
  for (auto AI = CI->arg_begin(), AE = CI->arg_end(); AI != AE; AI++) {
    Args.insert(Args.end(), *AI);
  }
  SmallVector<OperandBundleDef, 1> OpBundles;
  CI->getOperandBundlesAsDefs(OpBundles);
  OperandBundleDef B1("QUAL.OMP.NORMALIZED.IV", IV);
  OpBundles.push_back(B1);
  OperandBundleDef B2("QUAL.OMP.NORMALIZED.UB", NormalizedUB);
  OpBundles.push_back(B2);
  auto NewI = CallInst::Create(CI->getCalledValue(), Args, OpBundles, "", CI);
  NewI->takeName(CI);
  NewI->setCallingConv(CI->getCallingConv());
  NewI->setAttributes(CI->getAttributes());
  NewI->setDebugLoc(CI->getDebugLoc());
  CI->replaceAllUsesWith(NewI);
  CI->eraseFromParent();

  return;
}

// Insert an empty loop right after BeforeBB
//
//             BeforeBB
//                |
//             HeaderBB:
//            i = phi(LB, i')
//                |
//                |
//              BodyBB: <-----------+
//               ...                |
//               ...                |
//             i' = i + Stride      | latch
//             if (i' <= UB)        |
//                |   |             |
//                |   +-------------+
//              ExitBB
//
Value *VPOUtils::genNewLoop(Value *LB, Value *UB, Value *Stride,
                            IRBuilder<> &Builder, int Counter,
                            Value *&NormalizedUB, DominatorTree *DT) {
  assert(LB->getType() == UB->getType() && "Loop bound types do not match");

  IntegerType *LoopIVType = dyn_cast<IntegerType>(UB->getType());
  assert(LoopIVType && "UB is not integer?");

  Function *F = Builder.GetInsertBlock()->getParent();
  LLVMContext &Context = F->getContext();
  StringRef FName = F->getName();

  BasicBlock *BeforeBB = Builder.GetInsertBlock();

  BasicBlock *PreHeaderBB =
      BasicBlock::Create(Context, ".sloop.preheader." + Twine(Counter), F);

  BasicBlock *HeaderBB =
      BasicBlock::Create(Context, ".sloop.header." + Twine(Counter), F);

  BasicBlock *BodyBB =
      BasicBlock::Create(Context, ".sloop.body." + Twine(Counter), F);

  // The default insertion point is InsertBlock->end(), we need to move it one
  // step back to point to the terminator instruction in InsertBlock, for
  // SplitBlock to use to create ExitBB
  //
  Builder.SetInsertPoint(&*--Builder.GetInsertPoint());
  BasicBlock *ExitBB =
      SplitBlock(BeforeBB, &*Builder.GetInsertPoint(), DT, nullptr);
  ExitBB->setName(FName + ".sloop.latch." + Twine(Counter));

  // BeforeBB
  //BeforeBB->getTerminator()->setSuccessor(0, HeaderBB);
  BeforeBB->getTerminator()->setSuccessor(0, PreHeaderBB);

  Builder.SetInsertPoint(PreHeaderBB);
  Builder.CreateBr(HeaderBB);

  Value *UpperBnd = UB;

  if (ConstantInt* CI = cast<ConstantInt>(UB)) {
    if (CI->getBitWidth() <= 32) {
      // PreHeaderBB
      IntegerType *IntTy = Type::getInt32Ty(F->getContext());
      const DataLayout &DL = F->getParent()->getDataLayout();
      Instruction *InsertPt = F->getEntryBlock().getTerminator();
      AllocaInst *TmpUB =
          new AllocaInst(IntTy, DL.getAllocaAddrSpace(), "num.sects", InsertPt);
      TmpUB->setAlignment(4);
      NormalizedUB = TmpUB;

      StoreInst *SI = new StoreInst(UB, TmpUB, false, InsertPt);
      SI->setAlignment(4);

      InsertPt = PreHeaderBB->getTerminator();

      UpperBnd = new LoadInst(TmpUB, "sloop.ub", false, InsertPt);
    }
  }

  Builder.SetInsertPoint(F->getEntryBlock().getTerminator());
  AllocaInst *IV =
      Builder.CreateAlloca(LoopIVType, nullptr, ".sloop.iv." + Twine(Counter));

  // HeaderBB and Loop ZTT
  Builder.SetInsertPoint(PreHeaderBB->getTerminator());
  Builder.CreateStore(LB, IV);

  Builder.SetInsertPoint(HeaderBB);

  // Value *LoopZTT = Builder.CreateICmp(ICmpInst::ICMP_SLE, IV, UB);
  // LoopZTT->setName(FName + ".sloop.ztt." + Twine(Counter));
  // Builder.CreateCondBr(LoopZTT, BodyBB, ExitBB);
  Builder.CreateBr(BodyBB);

  // Loop BodyBB
  Builder.SetInsertPoint(BodyBB);
  Value *IncIV = Builder.CreateAdd(Builder.CreateLoad(IV, true), Stride,
                                   ".sloop.inc." + Twine(Counter),
                                   true /*HasNUW*/, true /*HasNSW*/);
  Builder.CreateStore(IncIV, IV, true);
  Value *LoopCond = Builder.CreateICmp(ICmpInst::ICMP_SLE,
                                       Builder.CreateLoad(IV, true), UpperBnd);
  LoopCond->setName(FName + ".sloop.cond." + Twine(Counter));

  // Loop latch
  Builder.CreateCondBr(LoopCond, HeaderBB, ExitBB);

  // Now move the newly created loop blocks from the end of basic block list
  // to the proper place, which is right before loop ExitBB. This will not
  // affect CFG, but CFG printing and readability.
  F->getBasicBlockList().splice(ExitBB->getIterator(),
                                F->getBasicBlockList(),
                                HeaderBB->getIterator(),
                                F->end());

  if (DT) {
    DT->addNewBlock(PreHeaderBB, BeforeBB);
    DT->addNewBlock(HeaderBB, PreHeaderBB);
    DT->addNewBlock(BodyBB, HeaderBB);
    DT->changeImmediateDominator(ExitBB, PreHeaderBB);
  }

  // The loop body should be added here.
  Builder.SetInsertPoint(BodyBB->getFirstNonPHI());

  // Return the loop PHI instruction
  return IV;
}

// Insert a switch statement at the SwitchInsertPoint in SwitchBB:
//
// Given the following basic block:
//
//             --------------------
//             |SwitchBB:         |
//             |......            |
//             |SwitchInsertPoint |
//             |......            |
//             --------------------
//
// the function transforms it to:
//
//             --------------------
//             |SwitchBB:         |
//             |......            |
//             |switch(i)         |
//             --------------------
//             /    |    ...  \
//        case1  case2    ... DefualtCase
//           \      |     ...  /
//             SwitchEpilogBB
//                  |
//             --------------------
//             |SwitchsuccBB:     |
//             |SwitchInsertPoint |
//             |......            |
//             --------------------
//
//SwitchInst *VPOUtils::genParSectSwitch(
void VPOUtils::genParSectSwitch(
  Value *SwitchCond,
  ParSectNode *Node,
  IRBuilder<> &Builder,
  int Counter,
  DominatorTree *DT
)
{
  BasicBlock *SwitchBB = Builder.GetInsertBlock();
  BasicBlock::iterator InsertPoint = Builder.GetInsertPoint();

  Function *F = SwitchBB->getParent();
  LLVMContext &Context = F->getContext();
  StringRef FName = F->getName();

  unsigned NumCases = Node->Children.size();

  // Split SwitchBB at the SwitchInsertPoint
  Instruction *I = &*InsertPoint;
  BasicBlock *SwitchSuccBB = SplitBlock(SwitchBB, I, DT, nullptr);

  SwitchSuccBB->setName(FName + ".sw.succBB." + Twine(Counter));

  // Insert the Switch right before the original terminator
  Builder.SetInsertPoint(SwitchBB->getTerminator());

  BasicBlock *Default = Node->Children[0]->EntryBB;
  SwitchInst *SwitchInstruction = Builder.CreateSwitch(
      Builder.CreateLoad(SwitchCond, true), Default, NumCases - 1);

  BasicBlock *Epilog = BasicBlock::Create(
          Context, FName + ".sw.epilog." + Twine(Counter), F);
  Builder.SetInsertPoint(Epilog);
  Builder.CreateBr(SwitchSuccBB);

  for (unsigned i = 0, e = NumCases; i != e; ++i) {
    ConstantInt *CaseValue =
        ConstantInt::get(Type::getInt32Ty(Context), i);

    BasicBlock *SectionEntryBB = Node->Children[i]->EntryBB;
    BasicBlock *SectionExitBB = Node->Children[i]->ExitBB;

    SectionEntryBB->setName(
            FName + ".sw.case" + Twine(i) + "." + Twine(Counter));
    if (i != 0)
      SwitchInstruction->addCase(CaseValue, SectionEntryBB);

    SectionExitBB->getTerminator()->eraseFromParent();
    Builder.SetInsertPoint(SectionExitBB);
    Builder.CreateBr(Epilog);

    if (DT) {
      DT->changeImmediateDominator(SectionEntryBB, SwitchBB);
    }

    // Delete DIR_OMP_END_SECTION directive, which has the following form:
    //
    // sec.end:
    // call void @llvm.intel.directive(metadata !"DIR.OMP.END.SECTION");
    // call void @llvm.intel.directive(metadata !"DIR.QUAL.LIST.END");
    // br label %after.sec
    //
    SectionExitBB->getInstList().pop_front();

    // Delete DIR_OMP_SECTION directive, which has the following form:
    //
    // sec.begin:
    // call void @llvm.intel.directive(metadata !"DIR.OMP.SECTION");
    // call void @llvm.intel.directive(metadata !"DIR.QUAL.LIST.END");
    // br label %sec.body
    //
    SectionEntryBB->getInstList().pop_front();

    auto I = SectionEntryBB->begin();
    if (VPOAnalysisUtils::isIntelDirective(&*I)) {
      SectionExitBB->getInstList().pop_front();
      SectionEntryBB->getInstList().pop_front();
    }
  }

  SwitchBB->getTerminator()->eraseFromParent();

  if (DT) {
    DT->addNewBlock(Epilog, SwitchBB);
    DT->changeImmediateDominator(SwitchSuccBB, Epilog);
  }

  return;
  //return SwitchInstruction;
}
