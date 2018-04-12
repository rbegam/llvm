//===---- HIRSafeReductionAnalysis.cpp - Identify Safe Reduction Chain ----===//
//
// Copyright (C) 2015-2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements Safe Reduction Identification
//
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"

#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/DDTests.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRSafeReductionAnalysis.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRLoopStatistics.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Framework/HIRFramework.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Passes.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/BlobUtils.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/CanonExprUtils.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/DDRefUtils.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/DDUtils.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/HLNodeUtils.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/HLNodeVisitor.h"

#include <algorithm>
#include <map>
#include <vector>

using namespace llvm;
using namespace llvm::loopopt;

#define DEBUG_TYPE "hir-safe-reduction-analysis"
static cl::opt<bool>
    ForceSRA("force-hir-safe-reduction-analysis", cl::init(false), cl::Hidden,
             cl::desc("forces safe reduction analysis by request"));

FunctionPass *llvm::createHIRSafeReductionAnalysisPass() {
  return new HIRSafeReductionAnalysis();
}

char HIRSafeReductionAnalysis::ID = 0;
INITIALIZE_PASS_BEGIN(HIRSafeReductionAnalysis, "hir-safe-reduction-analysis",
                      "HIR Safe Reduction Analysis", false, true)
INITIALIZE_PASS_DEPENDENCY(HIRFrameworkWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRLoopStatisticsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRDDAnalysis)
INITIALIZE_PASS_END(HIRSafeReductionAnalysis, "hir-safe-reduction-analysis",
                    "HIR Safe Reduction Analysis", false, true)

void HIRSafeReductionAnalysis::getAnalysisUsage(AnalysisUsage &AU) const {

  AU.setPreservesAll();
  AU.addRequiredTransitive<HIRFrameworkWrapperPass>();
  // Loop Statistics is not used by this pass directly but it used by
  // HLNodeUtils::dominates() utility. This is a workaround to keep the pass
  // manager from freeing it.
  AU.addRequiredTransitive<HIRLoopStatisticsWrapperPass>();
  AU.addRequiredTransitive<HIRDDAnalysis>();
}

//  Sample code for calling Safe Reduction.
//
//  SRA = &getAnalysis<HIRSafeReductionAnalysis>();
//  -> Compute by passing outerloops or innermost loops
//  SRA->computeSafeReductionChains(outerloops);
//  a. In LoopInterchange:
//  if (SRA->isSafeReduction(Inst))
//
//  b. In Vectorizer:
//	if (SRA->isSafeReduction(Inst))   ....
//  or this to walk the chains
//  const SafeRedChainList & SRCL = SRA->getSafeReductionChain(Loop);
//	if (!SRCL.empty()) {
//		for (auto SRC : SRCL) {
//			for (auto Inst : SRC) {
//			 	Inst->print(OS, 2, false);
//  TODO: Compute SafeReduction chains for non-innermost loops
//
bool HIRSafeReductionAnalysis::runOnFunction(Function &F) {

  auto HIRF = &getAnalysis<HIRFrameworkWrapperPass>().getHIR();
  DDA = &getAnalysis<HIRDDAnalysis>();

  if (!ForceSRA) {
    return false;
  }
  // For stress testing only
  formatted_raw_ostream OS(dbgs());

  // Gather the innermost loops as candidates.
  SmallVector<HLLoop *, 32> CandidateLoops;
  HIRF->getHLNodeUtils().gatherInnermostLoops(CandidateLoops);

  for (auto &Loop : CandidateLoops) {
    identifySafeReduction(Loop);
  }

  return false;
}

namespace {
void printAChain(formatted_raw_ostream &OS, unsigned Indented,
                 const loopopt::SafeRedChain &SRC) {
  for (auto Inst : SRC) {
    Inst->print(OS, Indented, false);
  }
}
} // namespace

//  Identify Safe Reduction chain for a loop
//  "Safe" implies Reduction recurrence can be ignored for both
//  parallelization and vectorization.
//  Handles temps only. For memory reference, some preprocessing
//  for load hoisting or store sinking is needed.
//  It selects child stmts directly under the loop and skips
//  if stmts and stmts inside inner loops.
//  Invoked mostly for innermost loops, but supports any loop level.
//
//  Refer to HIRLoopInterchange.cpp for sample code to invoke and get
//  Safe Reductions

void HIRSafeReductionAnalysis::identifySafeReduction(const HLLoop *Loop) {

  // Safe reductions are of the follow forms:
  // a. Single statement
  //    t1 = t1 + ..
  // b. Reduction chain
  //
  //    t1 = t3 + ..
  //    t2 = t1 + .
  //    t3 = t2 + ..
  //    (In some cases,  we see copy stmt as the first stmt in
  //    the cycle,  t1 = t3)
  //
  //  Checking needed:
  //  - opcode:  max, min, add, sub, and, or, xor, mul, div
  //  - non-linear temps
  //  - flow edge (<) ; anti edge (=)
  //  - single use (single flow edge) in loop
  //  - not under if
  //  - stmt post-dom loop-entry

  if (!Loop->isDo() || !(FirstChild = Loop->getFirstChild())) {
    return;
  }

  DDGraph DDG = DDA->getGraph(Loop, false);

  identifySafeReductionChain(Loop, DDG);
}

void HIRSafeReductionAnalysis::computeSafeReductionChains(const HLLoop *Loop) {

  SmallVector<const HLLoop *, 32> CandidateLoops;
  Loop->getHLNodeUtils().gatherInnermostLoops(CandidateLoops, Loop);
  for (auto &Lp : CandidateLoops) {
    auto SRCL = SafeReductionMap.find(Lp);
    if (SRCL != SafeReductionMap.end()) {
      continue;
    }
    identifySafeReduction(Lp);
  }
}

const SafeRedChainList &
HIRSafeReductionAnalysis::getSafeReductionChain(const HLLoop *Loop) {

  assert(Loop->isInnermost() && "SafeReduction supports only innermost loop");
  SafeRedChainList &SRCL = SafeReductionMap[Loop];
  return SRCL;
}

// Safe reduction chain could be
// a.  t1 = t2 +
//     t3 = t1 +
//     t2 = t3 +
// b.
//     t1 = t2
//     t3 = t1 +
//     t2 = t3 +
bool HIRSafeReductionAnalysis::isValidSR(const RegDDRef *LRef,
                                         const HLLoop *Loop, HLInst **SinkInst,
                                         DDRef **SinkDDRef,
                                         unsigned ReductionOpCode,
                                         DDGraph DDG) {

  auto I = DDG.outgoing_edges_begin(LRef);
  auto E = DDG.outgoing_edges_end(LRef);

  // No outgoing edges
  if (I == E) {
    return false;
  }

  HLNode *UseNode = nullptr;

  for (; I != E; ++I) {
    const DDEdge *Edge = *I;
    if (!Edge->isFLOWdep()) {
      return false;
    }
    *SinkDDRef = Edge->getSink();
    HLNode *SinkNode = (*SinkDDRef)->getHLDDNode();
    if (!HLNodeUtils::postDominates(SinkNode, FirstChild)) {
      return false;
    }
    *SinkInst = dyn_cast<HLInst>(SinkNode);
    if (!(*SinkInst)) {
      return false;
    }
    BlobDDRef *Bref = dyn_cast<BlobDDRef>(*SinkDDRef);
    if (Bref) {
      // Avoids
      // %t = %t1 + 1
      // %t1 = A[%t]
      RegDDRef *ParentRef = Bref->getParentDDRef();
      if (!ParentRef->isTerminalRef()) {
        return false;
      }
      // Integer sum can occur as blobs
      // sum =  10 * sum + ..
      CanonExpr *CE = ParentRef->getSingleCanonExpr();
      if (!isRedTemp(CE, Bref->getBlobIndex())) {
        return false;
      }
    }
    // Is Sink a copy stmt?
    if ((*SinkInst)->isCopyInst()) {
      continue;
    }
    unsigned ReductionOpCodeSave = ReductionOpCode;
    if (!(*SinkInst)->isReductionOp(&ReductionOpCode) ||
        ReductionOpCode != ReductionOpCodeSave) {
      return false;
    }
    if (Bref && ReductionOpCode != Instruction::Add) {
      // If BlobDDRef
      // Bail out t1 = (t1 + t2) * A[i];
      // OK if it were t1 = (t1 + t2) + A[i];
      // We intentionally skip Sub for now because
      // being t1 = (t1 + -1* t2) - A[i] or (t1 + t2) - A[i]
      // a safe reduction depends on the client's interpretation.
      return false;
    }
    bool IsMinMax = (ReductionOpCode == Instruction::Select);
    // In case of min/max reduction, make sure both uses belong to the same
    // 'select' operation
    if (IsMinMax) {
      if (!UseNode) {
        UseNode = SinkNode;
      } else {
        return (UseNode == SinkNode);
      }
    }
    if (!DDUtils::maxUsesInLoop(LRef, Loop, DDG, IsMinMax ? 2 : 1)) {
      return false;
    }
  }
  return true;
}

//  Check for  valid temps
//  These are not safe reductions:
//  s = 2 * s  +  ..
//  s = n * s  +  ..
//  s = 2 * s * i  +  ..
bool HIRSafeReductionAnalysis::isRedTemp(CanonExpr *CE, unsigned BlobIndex) {

  if (CE->getDenominator() != 1) {
    return false;
  }

  auto &BU = CE->getBlobUtils();
  auto TempBlob = BU.getBlob(BlobIndex);

  for (auto I = CE->iv_begin(), E = CE->iv_end(); I != E; ++I) {
    unsigned BlobIdx = CE->getIVBlobCoeff(I);
    if (BlobIdx == InvalidBlobIndex) {
      continue;
    }
    auto Blob = BU.getBlob(BlobIdx);
    if (BU.contains(Blob, TempBlob)) {
      return false;
    }
  }

  bool Found = false;
  for (auto I = CE->blob_begin(), E = CE->blob_end(); I != E; ++I) {
    auto Blob = BU.getBlob(CE->getBlobIndex(I));
    if (BU.contains(Blob, TempBlob)) {
      if (Found || (Blob != TempBlob) || (CE->getBlobCoeff(I) != 1)) {
        return false;
      }
      Found = true;
    }
  }
  assert(Found && "Blob not found!");
  return true;
}

void HIRSafeReductionAnalysis::identifySafeReductionChain(const HLLoop *Loop,
                                                          DDGraph DDG) {

  DEBUG(dbgs() << "\nIn Sum Reduction Chain\n");

  for (auto It = Loop->child_begin(), E = Loop->child_end(); It != E; ++It) {
    FirstRvalSB = 0;
    unsigned ReductionOpCode = 0;
    SafeRedChain RedInsts;
    bool SingleStmtReduction;
    const HLNode *NodeI = &(*It);

    const HLInst *Inst = dyn_cast<HLInst>(NodeI);
    if (!Inst) {
      continue;
    }

    // By checking for PostDomination, it allows goto and label
    if (!HLNodeUtils::postDominates(Inst, FirstChild)) {
      continue;
    }

    if (isSafeReduction(Inst)) {
      continue;
    }

    if (!findFirstRedStmt(Loop, Inst, &SingleStmtReduction, &FirstRvalSB,
                          &ReductionOpCode, DDG)) {
      continue;
    }

    RedInsts.push_back(Inst);

    HLInst *SinkInst = nullptr;
    DDRef *SinkDDRef = nullptr;

    // Loop thru all flow edges to sink stmt
    //      t1 = t2 +
    //      t3 = t1 +
    //      t2 = t3 +
    //       - sink stmt postdom FirstChild
    //       - reduction Op matches
    //       - single use

    while (true) {

      const RegDDRef *LRef = Inst->getLvalDDRef();
      if (!isValidSR(LRef, Loop, &SinkInst, &SinkDDRef, ReductionOpCode, DDG)) {
        break;
      }
      if (FirstRvalSB == SinkDDRef->getSymbase()) {
        if (SingleStmtReduction) {
          DEBUG(dbgs() << "\nSelf-reduction found\n");
        } else {
          DEBUG(dbgs() << "\nSafe Reduction chain found\n");
        }
        setSafeRedChainList(RedInsts, Loop, FirstRvalSB, ReductionOpCode);
        DEBUG(formatted_raw_ostream FOS(dbgs()); printAChain(FOS, 1, RedInsts));
        break;
      }

      if (Inst == SinkInst) {
        break;
      }
      // if SinkInst (s3) strictly dominates Inst(s4),
      // then s2 is no longer a valid 1st stmt of the cycle.
      // e.g.    s2:   x = y
      //         s3:   z = w
      //         s4:   w = x + z
      if (HLNodeUtils::strictlyDominates(SinkInst, Inst)) {
        break;
      }
      RedInsts.push_back(SinkInst);
      Inst = SinkInst;
    }
  }
}

bool HIRSafeReductionAnalysis::findFirstRedStmt(
    const HLLoop *Loop, const HLInst *Inst, bool *SingleStmtReduction,
    unsigned *FirstRvalSB, unsigned *ReductionOpCode, DDGraph DDG) {

  // Start by guessing the start of reduction chain
  // S1  is Inst
  // Look for these 3 patterns:
  // (1)
  //  S1: t1 = t1 + ..
  // (2)
  //  S1: t1 = t3 +
  //      ..
  //  S3: t3 = t2 + a[i];
  // (3)
  //  S1: t1 = t3
  //   ..
  //  S3: t3 = t2 + a[i];
  //  Look for incoming flow edge (<) into S1.
  //  S3 needs to be a reduction stmt
  //
  //  The code below loops thru the RHS ddref, and checks if  there is only
  //  1 incoming edge from stmts below. So stmt like this (integer)
  //  s1 =  (n * 4 +  s0) + tx
  //  The ddref encountered is only tx.
  //  Later,  isValidSR will check if the reduction temp on LHS has
  //  only single use through DD edge

  unsigned ReductionOpCodeSave = 0;
  *SingleStmtReduction = false;

  if (!Inst->isCopyInst()) {
    if (!Inst->isReductionOp(ReductionOpCode)) {
      return false;
    }
    ReductionOpCodeSave = *ReductionOpCode;
  }

  unsigned OperandNum = 0;
  for (auto I = Inst->rval_op_ddref_begin(), E2 = Inst->rval_op_ddref_end();
       I != E2; ++I, ++OperandNum) {
    const RegDDRef *RRef = *I;

    if (!RRef->isTerminalRef()) {
      continue;
    }

    // sum = a[i] - sum   is not a reduction
    if ((OperandNum == 1) && (ReductionOpCodeSave == Instruction::FSub ||
                              ReductionOpCodeSave == Instruction::Sub)) {
      return false;
    }

    enum Answer { NO_REDUCTION, SKIPTONEXT, POTENTIAL_REDUCTION };
    auto Finder = [&](const DDRef *Ref) {
      for (auto I = DDG.incoming_edges_begin(Ref),
                E = DDG.incoming_edges_end(Ref);
           I != E; ++I) {
        if (!(*I)->isFLOWdep()) {
          continue;
        }

        DDRef *DDRefSrc = (*I)->getSrc();
        HLInst *SrcInst = dyn_cast<HLInst>(DDRefSrc->getHLDDNode());
        assert(SrcInst && "Source of flow edge is not an instruction!");

        if (!SrcInst->isReductionOp(ReductionOpCode)) {
          return SKIPTONEXT;
        }

        // First stmt could be   a.  t1 = t2
        //          or           b.  t1 = t2 + ..

        if (!Inst->isCopyInst() && ReductionOpCodeSave != *ReductionOpCode) {
          return SKIPTONEXT;
        }

        if (Inst == SrcInst) {
          const RegDDRef *LRef = Inst->getLvalDDRef();
          if (DDUtils::maxUsesInLoop(LRef, Loop, DDG,
                                     Inst->isMinOrMax() ? 2 : 1)) {
            *SingleStmtReduction = true;
            *FirstRvalSB = DDRefSrc->getSymbase();
            return POTENTIAL_REDUCTION;
          } else {
            return NO_REDUCTION;
          }
        }

        // The caller has already checked that Inst post-dominates the first
        // child of the loop. So, SrcInst postDominating Inst implies that- a)
        // SrcInst also postdominates first child of the loop. b) This is a
        // cross-iteration dependency.
        if (!HLNodeUtils::postDominates(SrcInst, Inst)) {
          return SKIPTONEXT;
        }

        *FirstRvalSB = DDRefSrc->getSymbase();
        return POTENTIAL_REDUCTION;
      }
      return SKIPTONEXT;
    };

    auto Found = Finder(RRef);
    if (Found == POTENTIAL_REDUCTION) {
      return true;
    } else if (Found == NO_REDUCTION) {
      return false;
    }

    // Blob dd refs of rval dd refs scanned as well because
    // rval sinks of incoming edges can be a blob ddref.
    for (auto BI = RRef->blob_cbegin(), BE = RRef->blob_cend(); BI != BE;
         ++BI) {
      auto Found = Finder(*BI);
      if (Found == POTENTIAL_REDUCTION) {
        return true;
      } else if (Found == NO_REDUCTION) {
        return false;
      }
    }
  }

  return false;
}

void HIRSafeReductionAnalysis::setSafeRedChainList(SafeRedChain &RedInsts,
                                                   const HLLoop *Loop,
                                                   unsigned RedSymbase,
                                                   unsigned RedOpCode) {

  SafeRedChainList &SRCL = SafeReductionMap[Loop];
  SRCL.emplace_back(RedInsts, RedSymbase, RedOpCode);
  unsigned SRIIndex = SRCL.size() - 1;

  // We should use []operator instead of insert() to overwrite the previous
  // entry for the instruction. SafeReductionMap and SafeReductionInstMap can go
  // out of sync due to deleted loops. Refer to comment in getSafeRedInfo().
  for (auto &Inst : RedInsts) {
    SafeReductionInstMap[Inst] = SRIIndex;
  }
}

bool HIRSafeReductionAnalysis::isSafeReduction(const HLInst *Inst,
                                               bool *IsSingleStmt) const {

  const SafeRedInfo *SRI = getSafeRedInfo(Inst);
  if (!SRI) {
    return false;
  }

  if (IsSingleStmt) {
    *IsSingleStmt = (SRI->Chain.size() == 1 ? true : false);
  }

  return true;
}

void HIRSafeReductionAnalysis::print(formatted_raw_ostream &OS,
                                     const HLLoop *Loop,
                                     const SafeRedChainList *SRCL) {

  unsigned Depth = Loop->getNestingLevel() + 1;

  if (SRCL->empty()) {
    Loop->indent(OS, Depth);
    OS << "No Safe Reduction\n";
    return;
  }

  for (auto &SRI : *SRCL) {
    Loop->indent(OS, Depth);
    OS << "Safe Reduction:\n";
    printAChain(OS, Depth, SRI.Chain);
  }
}

void HIRSafeReductionAnalysis::print(formatted_raw_ostream &OS,
                                     const HLLoop *Loop) {

  auto &SRCL = SafeReductionMap[Loop];
  print(OS, Loop, &SRCL);
}

void HIRSafeReductionAnalysis::releaseMemory() {
  SafeReductionMap.clear();
  SafeReductionInstMap.clear();
}

void HIRSafeReductionAnalysis::markLoopBodyModified(const HLLoop *Loop) {

  assert(Loop && " Loop parameter is null.");
  // No need to clean up info in parent loop
  auto Iter = SafeReductionMap.find(Loop);
  if (Iter != SafeReductionMap.end()) {
    for (auto &SRI : Iter->second) {
      for (auto Inst : SRI.Chain) {
        SafeReductionInstMap.erase(Inst);
      }
    }
    SafeReductionMap.erase(Loop);
  }
}

const SafeRedInfo *
HIRSafeReductionAnalysis::getSafeRedInfo(const HLInst *Inst) const {

  auto Iter = SafeReductionInstMap.find(Inst);
  if (Iter == SafeReductionInstMap.end()) {
    return nullptr;
  }

  // Get index of SafeRedInfo via Inst
  auto &SRIIndex = Iter->second;
  const HLLoop *Loop = Inst->getLexicalParentLoop();
  // Get SafeRedChainList via Loop
  auto Iter2 = SafeReductionMap.find(Loop);

  assert(Iter2 != SafeReductionMap.end() &&
         "safe reduction analysis is in an inconsistent state!");

  auto &SRCL = Iter2->second;

  // Return SafeRedInfo via obtained Index and SRCL
  return &SRCL[SRIIndex];
}

bool HIRSafeReductionAnalysis::isReductionRef(const RegDDRef *Ref,
                                              unsigned &RedOpCode) {
  auto Node = Ref->getHLDDNode();

  assert(Node && "RegDDRef with null HLDDNode?");
  auto Inst = dyn_cast<HLInst>(Node);

  if (!Inst) {
    return false;
  }

  const SafeRedInfo *SRI = getSafeRedInfo(Inst);

  if (!SRI) {
    return false;
  }

  if (SRI->Symbase == Ref->getSymbase()) {
    RedOpCode = SRI->OpCode;
    return true;
  } else {
    return false;
  }
}
