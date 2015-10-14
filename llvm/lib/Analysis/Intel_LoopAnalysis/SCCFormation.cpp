//===------ SCCFormation.cpp - Identifies SCC in IRRegions ----------------===//
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
// This file implements the SCC formation pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/Pass.h"

#include "llvm/IR/CFG.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Intel_LoopIR/IRRegion.h"

#include "llvm/Analysis/LoopInfo.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"

#include "llvm/Analysis/Intel_LoopAnalysis/SCCFormation.h"
#include "llvm/Analysis/Intel_LoopAnalysis/RegionIdentification.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Passes.h"

using namespace llvm;
using namespace llvm::loopopt;

#define DEBUG_TYPE "hir-scc-formation"

INITIALIZE_PASS_BEGIN(SCCFormation, "hir-scc-formation", "HIR SCC Formation",
                      false, true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(RegionIdentification)
INITIALIZE_PASS_END(SCCFormation, "hir-scc-formation", "HIR SCC Formation",
                    false, true)

char SCCFormation::ID = 0;

FunctionPass *llvm::createSCCFormationPass() { return new SCCFormation(); }

SCCFormation::SCCFormation() : FunctionPass(ID), GlobalNodeIndex(1) {
  initializeSCCFormationPass(*PassRegistry::getPassRegistry());
}

void SCCFormation::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<DominatorTreeWrapperPass>();
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.addRequiredTransitive<ScalarEvolutionWrapperPass>();
  AU.addRequiredTransitive<RegionIdentification>();
}

bool SCCFormation::isLinear(const NodeTy *Node) const {

  if (SE->isSCEVable(Node->getType())) {
    auto SC = SE->getSCEV(const_cast<NodeTy *>(Node));

    if (auto AddRecSCEV = dyn_cast<SCEVAddRecExpr>(SC)) {
      if (AddRecSCEV->isAffine()) {
        return true;
      }
    }
  }

  return false;
}

bool SCCFormation::isCandidateRootNode(const NodeTy *Node) const {
  assert(isa<PHINode>(Node) && "Instruction is not a phi!");

  // Already visited?
  if (VisitedNodes.find(Node) != VisitedNodes.end()) {
    return false;
  }

  return true;
}

bool SCCFormation::isCandidateNode(const NodeTy *Node) const {

  // Use is outside the loop bring processed.
  if (!CurLoop->contains(Node->getParent())) {
    return false;
  }

  // Phi SCCs do not have anything to do with control flow.
  if (isa<TerminatorInst>(Node)) {
    return false;
  }

  // Unary instruction types are alloca, cast, extractvalue, load and vaarg.
  if (isa<UnaryInstruction>(Node) && !isa<CastInst>(Node)) {
    return false;
  }

  // Phi SCCs do not have anything to do with memory.
  if (isa<StoreInst>(Node) || isa<AtomicCmpXchgInst>(Node) ||
      isa<AtomicRMWInst>(Node)) {
    return false;
  }

  // Phi SCCs do not have anything to do with exception handling.
  if (isa<LandingPadInst>(Node)) {
    return false;
  }

  // Phi SCCs do not have anything to do with calls.
  if (isa<CallInst>(Node)) {
    return false;
  }

  return true;
}

SCCFormation::NodeTy::const_user_iterator
SCCFormation::getNextSucc(const NodeTy *Node,
                          NodeTy::const_user_iterator PrevSucc) const {
  NodeTy::const_user_iterator I;

  // Called from getFirstSucc().
  if (PrevSucc == getLastSucc(Node)) {
    I = Node->user_begin();
  } else {
    I = std::next(PrevSucc);
  }

  for (auto E = getLastSucc(Node); I != E; ++I) {
    assert(isa<NodeTy>(*I) && "Use is not an instruction!");

    if (isCandidateNode(cast<NodeTy>(*I))) {
      break;
    }
  }

  return I;
}

SCCFormation::NodeTy::const_user_iterator
SCCFormation::getFirstSucc(const NodeTy *Node) const {
  return getNextSucc(Node, getLastSucc(Node));
}

SCCFormation::NodeTy::const_user_iterator
SCCFormation::getLastSucc(const NodeTy *Node) const {
  return Node->user_end();
}

void SCCFormation::removeIntermediateNodes(SCCNodesTy &CurSCC) {

  SmallVector<const NodeTy *, 4> IntermediateNodes;

  for (auto NodeIt = CurSCC.begin(), NodeEndIt = CurSCC.end();
       NodeIt != NodeEndIt; ++NodeIt) {
    if (isa<PHINode>(*NodeIt)) {
      continue;
    }

    bool IsUsedInPhi = false;
    // Check whether this non-phi instruction is used in any phi contained in
    // the SCC.
    for (auto UseIt = (*NodeIt)->user_begin(), UseEndIt = (*NodeIt)->user_end();
         UseIt != UseEndIt; ++UseIt) {
      auto Use = cast<NodeTy>(*UseIt);

      if (!isa<PHINode>(Use)) {
        continue;
      }

      if (!CurSCC.count(Use)) {
        continue;
      }

      IsUsedInPhi = true;
    }

    if (!IsUsedInPhi) {
      IntermediateNodes.push_back(*NodeIt);
    }
  }

  for (auto &I : IntermediateNodes) {
    CurSCC.erase(I);
  }
}

unsigned
SCCFormation::getRegionIndex(RegionIdentification::const_iterator RegIt) const {
  return RegIt - RI->begin();
}

void SCCFormation::setRegionSCCBegin() {
  if (isNewRegion) {
    // Set the index of the last RegionSCC element as the current region's first
    // SCC.
    RegionSCCBegin[getRegionIndex(CurRegIt)] = RegionSCCs.size() - 1;
    isNewRegion = false;
  }
}

void SCCFormation::setRegion(RegionIdentification::const_iterator RegIt) {
  CurRegIt = RegIt;
  isNewRegion = true;
}

bool SCCFormation::isValidSCC(SCCTy *NewSCC) {
  SmallPtrSet<const BasicBlock *, 12> BBlocks;

  for (auto InstIt = NewSCC->Nodes.begin(), IEndIt = NewSCC->Nodes.end();
       InstIt != IEndIt; ++InstIt) {
    if (isa<PHINode>(*InstIt)) {

      auto ParentBB = (*InstIt)->getParent();

      if (BBlocks.count(ParentBB)) {
        // If any two phis in the SCC have the same bblock parent then we
        // cannot assign the same symbase to them because they are live inside
        // the bblock at the same time, hence we invalidate the SCC. This can
        // happen in circular wrap cases. The following example generates a
        // single SCC out of a, b and c.
        //
        // for(i=0; i<n; i++) {
        //   A[i] = a;
        //   t = a;
        //   a = b;
        //   b = c;
        //   c = t;
        // }
        //
        // IR-
        //
        // for.body:
        //   %a.addr.010 = phi i32 [ %b.addr.07, %for.body ], [ %a, %entry ]
        //   %c.addr.08 = phi i32 [ %a.addr.010, %for.body ], [ %c, %entry ]
        //   %b.addr.07 = phi i32 [ %c.addr.08, %for.body ], [ %b, %entry ]
        // ...
        //
        return false;
      }

      BBlocks.insert(ParentBB);
    }
  }

  return true;
}

unsigned SCCFormation::findSCC(const NodeTy *Node) {
  unsigned Index = GlobalNodeIndex++;
  unsigned LowLink = Index;

  // Push onto stack.
  NodeStack.push_back(Node);

  // Mark it as visited.
  auto Ret = VisitedNodes.insert(std::make_pair(Node, Index));
  (void)Ret;
  assert((Ret.second == true) && "Node has already been visited!");

  for (auto SuccIter = getFirstSucc(Node); SuccIter != getLastSucc(Node);
       SuccIter = getNextSucc(Node, SuccIter)) {
    assert(isa<NodeTy>(*SuccIter) && "Successor is not an instruction!");

    auto SuccNode = cast<NodeTy>(*SuccIter);
    auto Iter = VisitedNodes.find(SuccNode);

    // Successor hasn't been visited yet.
    if (Iter == VisitedNodes.end()) {
      // Recurse on the successor.
      auto SuccLowlink = findSCC(SuccNode);

      LowLink = std::min(LowLink, SuccLowlink);
    }
    // If this node has been visited already and has non-zero index, it belongs
    // to the current SCC.
    else if (Iter->second) {
      LowLink = std::min(LowLink, Iter->second);
    }
  }

  // This is the root of a new SCC.
  if (LowLink == Index) {

    // Ignore trivial single node SCC.
    if (Node == NodeStack.back()) {
      NodeStack.pop_back();

      // Invalidate index so node is ignored in subsequent traversals.
      VisitedNodes[Node] = 0;
    } else {
      // Create new SCC.
      SCCTy *NewSCC = new SCCTy(Node);
      auto &NewSCCNodes = NewSCC->Nodes;
      const NodeTy *SCCNode;

      // Insert Nodes in new SCC.
      do {
        SCCNode = NodeStack.pop_back_val();
        NewSCCNodes.insert(SCCNode);

        // Invalidate index so node is ignored in subsequent traverals.
        VisitedNodes[SCCNode] = 0;
      } while (SCCNode != Node);

      if (isValidSCC(NewSCC)) {
        // Add new SCC to the list.
        RegionSCCs.push_back(NewSCC);

        // Set pointer to first SCC of region, if applicable.
        setRegionSCCBegin();

        // Remove nodes not directly associated with the phi nodes.
        removeIntermediateNodes(NewSCCNodes);

      } else {
        // Not a valid SCC.
        delete NewSCC;
      }
    }
  }

  return LowLink;
}

void SCCFormation::formRegionSCCs() {

  // Iterate through the regions.
  for (auto RegIt = RI->begin(), RegionEndIt = RI->end(); RegIt != RegionEndIt;
       ++RegIt) {

    setRegion(RegIt);
    VisitedNodes.clear();

    auto Root = DT->getNode((*RegIt)->getEntryBBlock());

    // Iterate the dominator tree of the region.
    for (df_iterator<DomTreeNode *> DomIt = df_begin(Root),
                                    DomEndIt = df_end(Root);
         DomIt != DomEndIt; ++DomIt) {
      auto BB = (*DomIt)->getBlock();

      // Skip this basic block as it isn't part of the region.
      if (!(*RegIt)->containsBBlock(BB)) {
        continue;
      }

      // We only care about loop headers as the phi cycle starts there.
      if (!LI->isLoopHeader(BB)) {
        continue;
      }

      CurLoop = LI->getLoopFor(BB);

      // Iterate through the phi nodes in the header.
      for (auto I = BB->begin(); isa<PHINode>(I); ++I) {

        if (isCandidateRootNode(I)) {
          findSCC(I);
          assert(NodeStack.empty() && "NodeStack isn't empty!");
        }
      }
    }
  }
}

bool SCCFormation::runOnFunction(Function &F) {

  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
  RI = &getAnalysis<RegionIdentification>();

  // Initialize to NO_SCC.
  RegionSCCBegin.resize(RI->getNumRegions(), NO_SCC);

  formRegionSCCs();

  return false;
}

void SCCFormation::releaseMemory() {
  GlobalNodeIndex = 1;
  isNewRegion = false;

  for (auto &I : RegionSCCs) {
    delete I;
  }

  RegionSCCs.clear();
  RegionSCCBegin.clear();
  VisitedNodes.clear();
  NodeStack.clear();
}

SCCFormation::const_iterator
SCCFormation::begin(RegionIdentification::const_iterator RegIt) const {
  unsigned Index = getRegionIndex(RegIt);
  int BeginOffset = RegionSCCBegin[Index];

  // No SCCs associated with this region, return end().
  if (BeginOffset == NO_SCC) {
    return RegionSCCs.end();
  }

  return RegionSCCs.begin() + BeginOffset;
}

SCCFormation::const_iterator
SCCFormation::end(RegionIdentification::const_iterator RegIt) const {

  // RegionSCCBegin vector contains an offset indicating the first SCC of the
  // region in RegionSCCs vector. Index set to NO_SCC means the region has no
  // SCCs so we can simply return RegionSCCs end() iterator. Otherwise, to find
  // the last SCC associated with the region, we need to traverse the
  // RegionSCCBegin vector and find the next non - NO_SCC element. For exmaple,
  // consider the following RegionSCCBegin vector-
  //
  // [NO_SCC, 0, NO_SCC, 4]
  //
  // The above vector indicates that:
  // - First region does not contain any SCCs.
  // - Second region contains SCCs 0 to 3(4 is the end() element).
  // - Third region does not contain any SCCs.
  // - Fourth region contains all the remaining SCCs starting from 4.
  //
  unsigned Index = getRegionIndex(RegIt);
  int BeginOffset = RegionSCCBegin[Index];

  // No SCCs associated with this region, return end().
  if (BeginOffset == NO_SCC) {
    return RegionSCCs.end();
  }

  // Look for the end() for this region by looking at the next non-null index in
  // the array.
  for (++Index; Index < RegionSCCBegin.size(); ++Index) {
    int EndOffset = RegionSCCBegin[Index];

    if (EndOffset != NO_SCC) {
      assert(EndOffset > BeginOffset && "Region SCC offsets are wrong!");
      return RegionSCCs.begin() + EndOffset;
    }
  }

  // Couldn't find a non-null index, return end().
  return RegionSCCs.end();
}

void SCCFormation::print(raw_ostream &OS,
                         RegionIdentification::const_iterator RegIt) const {
  unsigned Count = 1;
  bool FirstSCC = true;
  auto RegBegin = RI->begin();

  for (auto SCCIt = begin(RegIt), SCCEndIt = end(RegIt); SCCIt != SCCEndIt;
       ++SCCIt, ++Count) {
    if (FirstSCC) {
      OS << "\nRegion " << RegIt - RegBegin + 1;
      FirstSCC = false;
    }

    OS << "\n   SCC" << Count << ": ";
    for (auto InstI = (*SCCIt)->Nodes.begin(), InstE = (*SCCIt)->Nodes.end();
         InstI != InstE; ++InstI) {
      if (InstI != (*SCCIt)->Nodes.begin()) {
        OS << " -> ";
      }
      (*InstI)->printAsOperand(OS, false);
    }
  }
  // Add a newline only if we printed anything.
  if (!FirstSCC) {
    OS << "\n";
  }
}

void SCCFormation::print(raw_ostream &OS, const Module *M) const {

  for (auto RegIt = RI->begin(), RegEndIt = RI->end(); RegIt != RegEndIt;
       ++RegIt) {
    print(OS, RegIt);
  }
}

void SCCFormation::verifyAnalysis() const {
  // TODO: implement later
}
