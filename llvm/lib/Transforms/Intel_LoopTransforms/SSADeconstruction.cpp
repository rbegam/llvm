//===----- SSADeconstruction.cpp - Deconstructs SSA for HIR ---------------===//
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
// This file Deconstructs SSA for HIR. It inserts copies for livein/liveout
// values to SCC and non-SCC phis. Metadata nodes are attached to the
// livein/liveout copies, to the SCC nodes and non-SCC phi node. The livein
// copies are assigned the same metadata kind node as the root/phi node so that
// they can all be assigned the same symbase by ScalarSymbaseAssignment pass.
//
// Liveout copies require metadata to indicate to ScalarEvolution analysis to
// not trace through them.
//
// Non-phi SCC nodes also require metadata to indicate to ScalarEvolution
// analysis to not trace through them because that can cause live-range
// violations. A different live range metadata type is used for these nodes.
//
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/SmallString.h"

#include "llvm/IR/Dominators.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instruction.h"
#include "llvm/IR/Metadata.h"

#include "llvm/IR/Intel_LoopIR/IRRegion.h"

#include "llvm/Support/CommandLine.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"

#include "llvm/Analysis/Intel_LoopAnalysis/RegionIdentification.h"
#include "llvm/Analysis/Intel_LoopAnalysis/SCCFormation.h"

#include "llvm/Transforms/Utils/BasicBlockUtils.h"

#include "llvm/Transforms/Intel_LoopTransforms/Passes.h"

#define DEBUG_TYPE "hir-ssa-deconstruction"

using namespace llvm;
using namespace llvm::loopopt;

namespace {

class SSADeconstruction : public FunctionPass {
public:
  static char ID;

  SSADeconstruction() : FunctionPass(ID), ModifiedIR(false), NamingCounter(0) {
    initializeSSADeconstructionPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.addRequired<ScalarEvolutionWrapperPass>();
    AU.addRequired<RegionIdentification>();
    AU.addRequired<SCCFormation>();

    // We need to preserve all the analysis computed for HIR.
    AU.setPreservesCFG();
    AU.addPreserved<ScalarEvolutionWrapperPass>();
    AU.addPreserved<RegionIdentification>();
    AU.addPreserved<SCCFormation>();
  }

  void releaseMemory() override {
    ModifiedIR = false;
    NamingCounter = 0;
    ProcessedSCCs.clear();
  }

private:
  enum MetadataType {
    LiveInType,
    LiveOutType,
    LiveRangeType,
  };

  /// \brief Attaches a string metadata node to instruction. This will be used
  /// by ScalarSymbaseAssignment to assign symbases. The metadata kind used for
  /// livein/liveout values is different because livein copies need to be
  /// assigned the same aymbase as other values in SCC whereas liveout copies
  /// don't. Live range type is used to indicate live range violation and
  /// suppress traceback during SCEV creation.
  void attachMetadata(Instruction *Inst, StringRef Name,
                      MetadataType MType) const;

  /// \brief Returns a copy of Val.
  Instruction *createCopy(Value *Val, StringRef Name, bool IsLivein) const;

  /// \brief Inserts copy of Val at the end of BB.
  void insertCopyAsLastInst(Value *Val, BasicBlock *BB, StringRef Name);

  /// \brief Inserts copy of Inst at the first insertion point of BB.
  Instruction *insertCopyAsFirstInst(Instruction *Inst, BasicBlock *BB,
                                     StringRef Name);

  /// \brief Constructs name to be used for Val.
  StringRef constructName(const Value *Val, SmallString<32> &Name);

  /// \brief Returns the SCC this phi belongs to, if any, otherwise returns
  /// null.
  SCCFormation::SCCTy *getPhiSCC(const PHINode *Phi) const;

  /// \brief Inserts copies of Phi operands livein to the SCC. If SCCNodes is
  /// null, Phi is treated as a standalone phi and all operands are considered
  /// livein. Returns true if a livein copy was inserted.
  bool processPhiLiveins(PHINode *Phi, SCCFormation::SCCNodesTy *SCCNodes,
                         StringRef Name);

  /// \brief Inserts copies of Phi if it has uses live outside the SCC and
  /// replaces the liveout uses with the copy. If SCCNodes is null, Phi is
  /// treated as a standalone phi and this is needed to handle a special case
  /// described in the function definition.
  void processPhiLiveouts(PHINode *Phi, SCCFormation::SCCNodesTy *SCCNodes,
                          StringRef Name);

  /// \brief Deconstructs phi by inserting copies.
  void deconstructPhi(PHINode *Phi);

  /// \brief Performs SSA deconstruction on the regions.
  void deconstructSSAForRegions();

private:
  ScalarEvolution *SE;
  RegionIdentification *RI;
  SCCFormation *SCCF;

  bool ModifiedIR;
  unsigned NamingCounter;
  RegionIdentification::iterator CurRegIt;
  SmallPtrSet<SCCFormation::SCCTy *, 32> ProcessedSCCs;
};
}

char SSADeconstruction::ID = 0;
INITIALIZE_PASS_BEGIN(SSADeconstruction, "hir-ssa-deconstruction",
                      "HIR SSA Deconstruction", false, false)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(RegionIdentification)
INITIALIZE_PASS_DEPENDENCY(SCCFormation)
INITIALIZE_PASS_END(SSADeconstruction, "hir-ssa-deconstruction",
                    "HIR SSA Deconstruction", false, false)

FunctionPass *llvm::createSSADeconstructionPass() {
  return new SSADeconstruction();
}

void SSADeconstruction::attachMetadata(Instruction *Inst, StringRef Name,
                                       MetadataType MType) const {
  Twine NewName(Name, ".de.ssa");

  Metadata *Args[] = {MDString::get(Inst->getContext(), NewName.str())};
  MDNode *Node = MDNode::get(Inst->getContext(), Args);

  if (MType == LiveInType) {
    Inst->setMetadata("in.de.ssa", Node);
  } else if (MType == LiveOutType) {
    Inst->setMetadata("out.de.ssa", Node);
  } else {
    Inst->setMetadata("live.range.de.ssa", Node);
  }
}

Instruction *SSADeconstruction::createCopy(Value *Val, StringRef Name,
                                           bool IsLivein) const {
  Twine NewName(Name, (IsLivein ? ".in" : ".out"));

  auto CInst =
      CastInst::Create(Instruction::BitCast, Val, Val->getType(), NewName);

  attachMetadata(CInst, Name, IsLivein ? LiveInType : LiveOutType);

  return CInst;
}

void SSADeconstruction::insertCopyAsLastInst(Value *Val, BasicBlock *BB,
                                             StringRef Name) {
  // Create a copy.
  auto CopyInst = createCopy(Val, Name, true);

  // Insert at the end of BB.
  CopyInst->insertBefore(BB->getTerminator());

  // Indicate that we have modified the IR by inserting a copy.
  ModifiedIR = true;
}

Instruction *SSADeconstruction::insertCopyAsFirstInst(Instruction *Inst,
                                                      BasicBlock *BB,
                                                      StringRef Name) {
  // Create a copy.
  auto CopyInst = createCopy(Inst, Name, false);

  // Insert at the first insertion point of BB.
  CopyInst->insertBefore(BB->getFirstInsertionPt());

  // Indicate that we have modified the IR by inserting a copy.
  ModifiedIR = true;

  return CopyInst;
}

SCCFormation::SCCTy *SSADeconstruction::getPhiSCC(const PHINode *Phi) const {
  for (auto SCCIt = SCCF->begin(CurRegIt), E = SCCF->end(CurRegIt); SCCIt != E;
       ++SCCIt) {

    // Present in this SCC.
    if (((*SCCIt)->Nodes).count(Phi)) {
      return *SCCIt;
    }
  }

  return nullptr;
}

void SSADeconstruction::processPhiLiveouts(PHINode *Phi,
                                           SCCFormation::SCCNodesTy *SCCNodes,
                                           StringRef Name) {

  Instruction *CopyInst = nullptr;

  for (auto UserIt = Phi->user_begin(), EndIt = Phi->user_end();
       UserIt != EndIt;) {
    assert(isa<Instruction>(*UserIt) && "Use is not an instruction!");
    auto UserInst = cast<Instruction>(*UserIt);

    Use &Us = UserIt.getUse();
    // Increment it before it gets invalidated later in the iteration.
    ++UserIt;

    // Handle a SCC phi.
    if (SCCNodes) {
      // Ignore if this value is region live-out.
      if (!(*CurRegIt)->containsBBlock(UserInst->getParent())) {
        continue;
      }

      // Check if the use is outside SCC.
      if (SCCNodes->count(UserInst)) {
        continue;
      }
    } else {
      // If this phi is used in another phi in the same basic block, then we can
      // potentially have ordering issues with the insertion of livein copies
      // for the phis. This is because the use of phi operands is deemed to
      // occur on the edge of the basic block which means that the 'use' takes
      // the value from the previous execution of the bblock, not the merged
      // value in the current basic block.
      //
      // Let us consider the example below.
      //
      // for.body:              ; preds = %entry, %for.body
      //   %b.addr.08 = phi i32 [ %c.addr.09, %for.body ], [ %b, %entry ]
      //   %a.addr.07 = phi i32 [ %b.addr.08, %for.body ], [ %a, %entry ]
      //   ...
      //   br i1 %exitcond, label %for.end, label %for.body
      //
      // After inserting livein copies for the two phis, the basic block would
      // look like this-
      //
      // for.body:              ; preds = %entry, %for.body
      //   %b.addr.08 = phi i32 [ %c.addr.09, %for.body ], [ %b, %entry ]
      //   !in.de.ssa
      //   %a.addr.07 = phi i32 [ %b.addr.08, %for.body ], [ %a, %entry ]
      //   !in.de.ssa
      //   ...
      //   %b.addr.08.in = %c.addr.09 !in.de.ssa
      //   %a.addr.07.in = %b.addr.08 !in.de.ssa
      //   br i1 %exitcond, label %for.end, label %for.body
      //
      // The livein copies %a.addr.07.in and %b.addr.08.in would be
      // assigned the same symbase as %a.addr.07 and %b.addr.08, repectively.
      // This means that the value of %a.addr.07 after the execution of the
      // basic block would be the updated value of %b.addr.08 through the copy,
      // which is wrong.
      //
      // To fix this problem, we create a liveout copy of %b.addr.08 so it
      // looks like this-
      //
      // for.body:              ; preds = %entry, %for.body
      //   %b.addr.08 = phi i32 [ %c.addr.09, %for.body ], [ %b, %entry ]
      //   !in.de.ssa
      //   %a.addr.07 = phi i32 [ %b.addr.08, %for.body ], [ %a, %entry ]
      //   !in.de.ssa
      //   %b.addr.08.out = %b.addr.08 !out.de.ssa
      //   ...
      //   %b.addr.08.in = %c.addr.09 !in.de.ssa
      //   %a.addr.07.in = %b.addr.08.out !in.de.ssa
      //   br i1 %exitcond, label %for.end, label %for.body
      //
      // Note that reordering the livein copies which produces a cleaner HIR
      // works in some cases but cannot resolve phi cycles. In comparison,
      // adding a liveout copy always works. Looking for a cycle would take
      // more compile time so this seems like an acceptable solution.
      //
      // Here's an example of a phi cycle-
      //
      // for(i=0; i<n; i++) {
      //   A[i] = a;
      //   t = a;
      //   a = b;
      //   b = c;
      //   c = t;
      // }

      if (!isa<PHINode>(UserInst) ||
          (Phi->getParent() != UserInst->getParent())) {
        continue;
      }

      bool CopyRequired = true;
      // If the 'user' phi occurs before definition phi the copies are inserted
      // in the correct order (on the assumption that we traverse the bblock
      // instructions in order).
      for (auto InstIt = Phi->getParent()->begin(),
                EndIt = BasicBlock::iterator(Phi);
           InstIt != EndIt; ++InstIt) {
        if (&(*InstIt) == UserInst) {
          CopyRequired = false;
        }
      }

      if (!CopyRequired) {
        continue;
      }
    }

    // Insert copy, if it doesn't exist.
    if (!CopyInst) {
      CopyInst = insertCopyAsFirstInst(Phi, Phi->getParent(), Name);
    }

    // Replace liveout use by copy.
    Us.set(CopyInst);

    // Invalidate cached SCEV of the user, if any.
    SE->forgetValue(UserInst);
  }
}

bool SSADeconstruction::processPhiLiveins(PHINode *Phi,
                                          SCCFormation::SCCNodesTy *SCCNodes,
                                          StringRef Name) {
  bool Ret = false;

  // Insert a copy in the predecessor bblock for each phi operand which
  // lies outside the SCC(livein values).
  for (unsigned I = 0, E = Phi->getNumIncomingValues(); I != E; ++I) {
    auto PhiOp = Phi->getIncomingValue(I);

    // This check is only for SCC phis.
    // Constant operand is assumed to lie outside SCC.
    if (SCCNodes && !isa<Constant>(PhiOp) && isa<Instruction>(PhiOp) &&
        SCCNodes->count(cast<Instruction>(PhiOp))) {
      continue;
    }

    // Ignore if this value is region live-in.
    if (!(*CurRegIt)->containsBBlock(Phi->getIncomingBlock(I))) {
      continue;
    }

    // Insert copy.
    insertCopyAsLastInst(PhiOp, Phi->getIncomingBlock(I), Name);
    Ret = true;
  }

  return Ret;
}

StringRef SSADeconstruction::constructName(const Value *Val,
                                           SmallString<32> &Name) {
  raw_svector_ostream VOS(Name);

  if (Val->hasName()) {
    VOS << Val->getName();
  } else {
    // This string has to be unique across SCCs as it is used to assign
    // symbases. A longer name is used to avoid risk of name conflict with any
    // other instructions.
    VOS << "hir.de.ssa.copy" << NamingCounter++;
  }

  return VOS.str();
}

void SSADeconstruction::deconstructPhi(PHINode *Phi) {
  SmallString<32> Name;

  // Phi is part of SCC
  if (auto PhiSCC = getPhiSCC(Phi)) {

    // Return if this SCC has been processed already.
    if (ProcessedSCCs.count(PhiSCC)) {
      return;
    }

    ProcessedSCCs.insert(PhiSCC);

    bool IsLinear = SCCF->isLinear(Phi);

    bool LiveinCopyInserted = false;
    constructName(PhiSCC->Root, Name);

    for (auto const &SCCInst : PhiSCC->Nodes) {

      if (auto SCCPhiInst = dyn_cast<PHINode>(SCCInst)) {
        LiveinCopyInserted =
            processPhiLiveins(const_cast<PHINode *>(SCCPhiInst), &PhiSCC->Nodes,
                              Name.str()) ||
            LiveinCopyInserted;

        // Liveout copies are not needed for linear SCCs as they cannot cause
        // live range violation.
        if (!IsLinear) {
          processPhiLiveouts(const_cast<PHINode *>(SCCPhiInst), &PhiSCC->Nodes,
                             Name.str());
        }

      }
      // Linear SCCs cannot cause live range violation.
      else if (!IsLinear) {

        // Attach live range type metadata to suppress SCEV traceback.
        attachMetadata(const_cast<Instruction *>(SCCInst), Name.str(),
                       LiveRangeType);
        // Tell SCEV to reparse the instruction.
        SE->forgetValue(const_cast<Instruction *>(SCCInst));
      }
    }

    if (LiveinCopyInserted) {
      // Attach metadata to the root node to connect the SCC to its livein
      // copies.
      attachMetadata(const_cast<Instruction *>(PhiSCC->Root), Name.str(),
                     LiveInType);
    }

  } else {
    // This is a standalone phi such as the one which occurs at an if-else join.
    // Deconstruct all the operands.
    //
    // Shown below is an example of a standalone phi case where output will have
    // a phi at the if-else join.
    //
    // if (cond) {
    //   output = a;
    // }
    // else {
    //   output = b;
    // }
    //
    // In some cases the standalone phi can occur in loop headers as well.
    // Example test case-
    //
    // for(i=0; i<n; i++) {
    //   A[i] = a;
    //   a = b;
    //   b = c;
    //   c += i;
    // }
    //

    constructName(Phi, Name);

    // Attach metadata to Phi to connect it to its copies.
    attachMetadata(Phi, Name.str(), LiveInType);

    processPhiLiveins(Phi, nullptr, Name.str());

    if (!SCCF->isLinear(Phi)) {
      processPhiLiveouts(Phi, nullptr, Name.str());
    }
  }
}

void SSADeconstruction::deconstructSSAForRegions() {
  // Traverse regions.
  for (auto RegIt = RI->begin(), EndRegIt = RI->end(); RegIt != EndRegIt;
       ++RegIt) {

    // Set current region
    CurRegIt = RegIt;

    // Traverse region basic blocks.
    for (auto BBIt = (*RegIt)->bb_begin(), EndBBIt = (*RegIt)->bb_end();
         BBIt != EndBBIt; ++BBIt) {

      // Process instructions inside the basic blocks.
      for (auto Inst = (*BBIt)->begin(), EndI = (*BBIt)->end(); Inst != EndI;
           ++Inst) {
        if (auto Phi = dyn_cast<PHINode>(Inst)) {
          deconstructPhi(const_cast<PHINode *>(Phi));
        }
      }
    }
  }
}

bool SSADeconstruction::runOnFunction(Function &F) {
  SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  RI = &getAnalysis<RegionIdentification>();
  SCCF = &getAnalysis<SCCFormation>();

  deconstructSSAForRegions();

  return ModifiedIR;
}
