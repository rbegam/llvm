//===----- HIRSCCFormation.h - Identifies SCC in IRRegions -----*- C++ --*-===//
//
// Copyright (C) 2015-2016 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This analysis is used to identify Phi SCCs in the IRRegions created by
// HIRRegionIdentification pass.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_INTEL_LOOPANALYSIS_SCCFORMATION_H
#define LLVM_ANALYSIS_INTEL_LOOPANALYSIS_SCCFORMATION_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Pass.h"

#include "llvm/IR/Instruction.h"

#include "llvm/Analysis/Intel_LoopAnalysis/HIRRegionIdentification.h"

namespace llvm {

class Function;
class Loop;
class LoopInfo;
class DominatorTree;
class ScalarEvolution;

namespace loopopt {

/// \brief This analysis identifies SCCs for non-linear loop header phis in the
/// regions which are then used by SSA deconstruction pass to map different
/// values to the same symbase.
/// It looks for phis(nodes) in the loop headers and traverses the def-use
/// chain(edges) to identify cycles(SCCs) using Tarjan's algorithm.
class HIRSCCFormation : public FunctionPass {
public:
  typedef Instruction NodeTy;
  typedef SmallPtrSet<NodeTy *, 8> SCCNodesTy;

  struct SCC {
    // Outermost loop's header phi is set as the root.
    NodeTy *Root;
    SCCNodesTy Nodes;

    SCC(NodeTy *R) : Root(R) {}
  };

  typedef struct SCC SCCTy;

  typedef SmallVector<SCCTy, 32> RegionSCCTy;
  /// Iterators to iterate over regions
  typedef RegionSCCTy::const_iterator const_iterator;

  /// Vector of pair of begin/end indices into RegionSCCs vector.
  typedef SmallVector<std::pair<int, int>, 16> RegionSCCBeginTy;

private:
  /// LI - The loop information for the function we are currently analyzing.
  LoopInfo *LI;

  /// DT - The dominator tree.
  DominatorTree *DT;

  /// SE - Scalar Evolution analysis for the function.
  ScalarEvolution *SE;

  /// RI - The region identification pass.
  const HIRRegionIdentification *RI;

  /// RegionSCCs - Vector of SCCs identified by this pass.
  RegionSCCTy RegionSCCs;

  /// RegionSCCBegin - Vector of indices pointing to first SCC of regions in
  /// RegionSCCs. If there are no SCCs for the region, index is set to NO_SCC.
  RegionSCCBeginTy RegionSCCBegin;

  /// VisitedNodes - Maps visited instructions to indices. This is a per-region
  /// data structure.
  SmallDenseMap<const NodeTy *, unsigned, 64> VisitedNodes;

  /// NodeStack - Running stack of nodes visited during a call to findSCC().
  SmallVector<NodeTy *, 32> NodeStack;

  /// CurRegIt - Points to the region being processed.
  HIRRegionIdentification::const_iterator CurRegIt;

  /// LastSCCRegIt - Points to the last region for which we created SCCs.
  HIRRegionIdentification::const_iterator LastSCCRegIt;

  /// CurLoop - Points to the loop being processed.
  Loop *CurLoop;

  /// GlobalNodeIndex - Used to assign index to nodes.
  unsigned GlobalNodeIndex;

  /// isNewRegion - Indicates that we have started processing a new region.
  bool isNewRegion;

  /// NO_SCC - Used in RegionSCCBegin to indicate that there are no sccs
  /// associated with the region.
  const int NO_SCC = -1;

private:
  /// \brief Returns true if this is a potential root of a new SCC.
  bool isCandidateRootNode(const NodeTy *Node) const;

  /// Returns true if \p Phi is used in a header phi contained in CurLoop.
  bool usedInHeaderPhi(const PHINode *Phi) const;

  /// Returns true if \p Inst is used outside the loop it is defined in.
  bool isLoopLiveOut(const Instruction *Inst) const;

  /// Returns true if any of \p Phi's operands depend directly or indirectly on
  /// another phi defined in the same bblock as itself.
  bool dependsOnSameBasicBlockPhi(const PHINode *Phi) const;

  /// Returns true if this is a single trip loop.
  bool isSingleTripLoop(Loop *Lp) const;

  /// \brief Returns true if this is a node of the graph.
  bool isCandidateNode(const NodeTy *Node) const;

  /// \brief Returns the next successor of Node in the graph.
  NodeTy::user_iterator getNextSucc(NodeTy *Node,
                                    NodeTy::user_iterator PrevSucc) const;

  /// \brief Returns the first successor of Node in the graph.
  NodeTy::user_iterator getFirstSucc(NodeTy *Node) const;

  /// \brief Returns the last successor of Node in the graph.
  NodeTy::user_iterator getLastSucc(NodeTy *Node) const;

  /// \brief Removes non-phi nodes which are not the same type as the root of
  /// the phi. These are encountered when tracing through casts. Only single-use
  /// nodes are allowed here so we know that they only appear in the SCC and
  /// thus cannot cause live-range issues.
  void removeIntermediateNodes(SCCTy &CurSCC) const;

  /// \brief Sets the RegionSCCBegin iterator for a new region.
  void setRegionSCCBegin();

  /// \brief Returns the index/offset of this region relative to RI->begin().
  unsigned getRegionIndex(HIRRegionIdentification::const_iterator RegIt) const;

  /// \brief Sets RegIt as the current region being processed.
  void setRegion(HIRRegionIdentification::const_iterator RegIt);

  /// \brief Returns true if forming this SCC results in a cleaner HIR.
  bool isProfitableSCC(const SCCNodesTy &Nodes) const;

  /// \brief Checks the validity of an SCC w.r.t assigning the same symbase to
  /// all its nodes.
  bool isValidSCC(const SCCTy &CurSCC) const;

  /// \brief Checks that Phi is used in another phi in the SCC.
  static bool isUsedInSCCPhi(PHINode *Phi, const SCCNodesTy &NewSCC);

  /// Used to set the outermost loop header phi amongst the nodes as the root
  /// node.
  void updateRoot(SCCTy &SCC, NodeTy *NewRoot) const;

  /// \brief Runs Tarjan's algorithm on this node. Returns the lowlink for this
  /// node.
  unsigned findSCC(NodeTy *Node);

  /// \brief Forms SCCs for non-linear loop header phis in the regions.
  void formRegionSCCs();

public:
  static char ID; // Pass identification
  HIRSCCFormation();

  bool runOnFunction(Function &F) override;
  void releaseMemory() override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  /// \brief Prints SCCs for all regions.
  void print(raw_ostream &OS, const Module * = nullptr) const override;
  /// \brief Prints SCCs for a region.
  void print(raw_ostream &OS,
             HIRRegionIdentification::const_iterator RegIt) const;
  void verifyAnalysis() const override;

  /// \brief Returns true if this node is considered linear by parsing.
  bool isConsideredLinear(const NodeTy *Node) const;

  /// \brief Returns true if Inst has a user outside region pointed to by RegIt.
  static bool isRegionLiveOut(HIRRegionIdentification::const_iterator RegIt,
                              const Instruction *Inst);

  /// SCC iterator methods
  const_iterator begin(HIRRegionIdentification::const_iterator RegIt) const;
  const_iterator end(HIRRegionIdentification::const_iterator RegIt) const;
};

} // End namespace loopopt

} // End namespace llvm

#endif
