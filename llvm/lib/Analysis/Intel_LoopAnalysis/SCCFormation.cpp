//===------ SCCFormation.cpp - Identifies SCC in IRRegions *- C++ -*-------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
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

#define DEBUG_TYPE "hir-sccs"

INITIALIZE_PASS_BEGIN(SCCFormation, "hir-sccs", "HIR SCC Formation", false,
                      true)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_DEPENDENCY(RegionIdentification)
INITIALIZE_PASS_END(SCCFormation, "hir-sccs", "HIR SCC Formation", false, true)

char SCCFormation::ID = 0;

FunctionPass *llvm::createSCCFormationPass() { return new SCCFormation(); }

SCCFormation::SCCFormation() : FunctionPass(ID), GlobalNodeIndex(1) {
  initializeSCCFormationPass(*PassRegistry::getPassRegistry());
}

void SCCFormation::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<DominatorTreeWrapperPass>();
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.addRequiredTransitive<ScalarEvolution>();
  AU.addRequiredTransitive<RegionIdentification>();
}

bool SCCFormation::isLinear(const NodeTy *Node) const {
  auto SC = SE->getSCEV(const_cast<NodeTy *>(Node));

  if (auto AddRecSCEV = dyn_cast<SCEVAddRecExpr>(SC)) {
    if (AddRecSCEV->isAffine()) {
      return true;
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

  // Is linear?
  if (isLinear(Node)) {
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

  // Ignore linear uses.
  if (isLinear(Node)) {
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

void SCCFormation::removeIntermediateNodes(SCCTy &CurSCC) {

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

void SCCFormation::setRegionSCCBegin() {
  if ((CurRegIt != RI->begin()) && isNewRegion) {

    // Make the last RegionSCC iterator, current region's first SCC.
    RegionSCCBegin[CurRegIt - RI->begin() - 1] = std::prev(RegionSCCs.end());
    isNewRegion = false;
  }
}

void SCCFormation::setRegion(RegionIdentification::const_iterator RegIt) {
  CurRegIt = RegIt;
  isNewRegion = true;
}

unsigned SCCFormation::findSCC(const NodeTy *Node) {
  unsigned Index = GlobalNodeIndex++;
  unsigned LowLink = Index;

  // Push onto stack.
  NodeStack.push_back(Node);

  // Mark it as visited.
  auto Ret = VisitedNodes.insert(std::make_pair(Node, Index));

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
    } else {
      // Create new SCC.
      RegionSCCs.push_back(new SCCTy());
      auto NewSCC = RegionSCCs.back();
      const NodeTy *SCCNode;

      // Set pointer to first SCC of region, if applicable.
      setRegionSCCBegin();

      // Insert Nodes in new SCC.
      do {
        SCCNode = NodeStack.back();
        NewSCC->insert(SCCNode);
        NodeStack.pop_back();

        // Invalidate index so it isn't used in another SCC.
        VisitedNodes[SCCNode] = 0;
      } while (SCCNode != Node);

      removeIntermediateNodes(*NewSCC);
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
  SE = &getAnalysis<ScalarEvolution>();
  RI = &getAnalysis<RegionIdentification>();

  formRegionSCCs();

  return false;
}

void SCCFormation::releaseMemory() {
  GlobalNodeIndex = 1;

  for (auto &I : RegionSCCs) {
    delete I;
  }

  RegionSCCs.clear();
}

SCCFormation::const_iterator
SCCFormation::begin(RegionIdentification::const_iterator RegIt) const {
  if (RegIt == RI->begin()) {
    return RegionSCCs.begin();
  }

  return RegionSCCBegin[RegIt - RI->begin() - 1];
}

SCCFormation::const_iterator
SCCFormation::end(RegionIdentification::const_iterator RegIt) const {
  if (RegIt == std::prev(RI->end())) {
    return RegionSCCs.end();
  }

  return RegionSCCBegin[RegIt - RI->begin()];
}

void SCCFormation::print(raw_ostream &OS, const Module *M) const {

  auto RegBegin = RI->begin();
  bool FirstSCC = true;
  unsigned Count;

  for (auto RegIt = RI->begin(), RegEndIt = RI->end(); RegIt != RegEndIt;
       ++RegIt) {
    FirstSCC = true;

    for (auto SCCIt = begin(RegIt), SCCEndIt = end(RegIt); SCCIt != SCCEndIt;
         ++SCCIt, ++Count) {
      if (FirstSCC) {
        OS << "\nRegion " << RegIt - RegBegin + 1;
        Count = 1;
        FirstSCC = false;
      }

      OS << "\n   SCC" << Count << ": ";
      for (auto InstI = (*SCCIt)->begin(), InstE = (*SCCIt)->end();
           InstI != InstE; ++InstI) {
        if (InstI != (*SCCIt)->begin()) {
          OS << " -> ";
        }
        OS << (*InstI)->getName();
      }
    }
    OS << "\n";
  }
}

void SCCFormation::verifyAnalysis() const {
  // TODO: implement later
}
