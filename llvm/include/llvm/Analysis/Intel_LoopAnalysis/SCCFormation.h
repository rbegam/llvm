//===------- SCCFormation.h - Identifies SCC in IRRegions ------*- C++ --*-===//
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
// This analysis is used to identify Phi SCCs in the IRRegions created by
// RegionIdentification pass.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_INTEL_LOOPANALYSIS_SCCFORMATION_H
#define LLVM_ANALYSIS_INTEL_LOOPANALYSIS_SCCFORMATION_H

#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Pass.h"

#include "llvm/IR/Instruction.h"

#include "llvm/Analysis/Intel_LoopAnalysis/RegionIdentification.h"

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
class SCCFormation : public FunctionPass {
public:
  typedef Instruction NodeTy;
  typedef SmallPtrSet<const NodeTy *, 12> SCCNodesTy;

  struct SCC {
    const NodeTy *Root;
    SCCNodesTy Nodes;

    SCC(const NodeTy *R) : Root(R) {}
  };

  typedef struct SCC SCCTy;

  typedef SmallVector<SCCTy *, 32> RegionSCCTy;
  /// Iterators to iterate over regions
  typedef RegionSCCTy::const_iterator const_iterator;

  typedef SmallVector<const_iterator, 16> RegionSCCBeginTy;

private:
  /// LI - The loop information for the function we are currently analyzing.
  LoopInfo *LI;

  /// DT - The dominator tree.
  DominatorTree *DT;

  /// SE - Scalar Evolution analysis for the function.
  ScalarEvolution *SE;

  /// RI - The region identification pass.
  const RegionIdentification *RI;

  /// RegionSCCs - Vector of SCCs identified by this pass.
  RegionSCCTy RegionSCCs;

  /// RegionSCCBegin - Vector of iterators pointing to first SCC of regions.
  RegionSCCBeginTy RegionSCCBegin;

  /// VisitedNodes - Maps visited instructions to index. This is a per-region
  /// data structure.
  SmallDenseMap<const NodeTy *, unsigned, 64> VisitedNodes;

  /// NodeStack - Running stack of nodes visited during a call to findSCC().
  SmallVector<const NodeTy *, 32> NodeStack;

  /// CurRegIt - Points to the region being processed.
  RegionIdentification::const_iterator CurRegIt;

  /// CurLoop - Points to the loop being processed.
  Loop *CurLoop;

  /// GlobalNodeIndex - Used to assign index to nodes.
  unsigned GlobalNodeIndex;

  /// isNewRegion - Indicates that we have started processing a new region.
  bool isNewRegion;

  /// \brief Returns true if this is a potential root of a new SCC.
  bool isCandidateRootNode(const NodeTy *Node) const;

  /// \brief Returns true if this is a node of the graph.
  bool isCandidateNode(const NodeTy *Node) const;

  /// \brief Returns the next successor of Node in the graph.
  NodeTy::const_user_iterator
  getNextSucc(const NodeTy *Node, NodeTy::const_user_iterator PrevSucc) const;

  /// \brief Returns the first successor of Node in the graph.
  NodeTy::const_user_iterator getFirstSucc(const NodeTy *Node) const;

  /// \brief Returns the last successor of Node in the graph.
  NodeTy::const_user_iterator getLastSucc(const NodeTy *Node) const;

  /// \brief Removes intermediate nodes of the SCC. Intermediate nodes are the
  /// ones which do not appear in any phi contained in the SCC. Although they
  /// are part of the SCC they are not strongly associated with the phis. They
  /// should not be assigned the same symbase as they can be live(used) at the
  /// same time as other nodes in the SCC.
  void removeIntermediateNodes(SCCNodesTy &CurSCC);

  /// \brief Sets the RegionSCCBegin iterator for a new region.
  void setRegionSCCBegin();

  /// \brief Sets RegIt as the current region being processed.
  void setRegion(RegionIdentification::const_iterator RegIt);

  /// \brief Checks the validity of an SCC w.r.t assigning the same symbase to
  /// all its nodes.
  bool isValidSCC(SCCTy *NewSCC);

  /// \brief Runs Tarjan's algorithm on this node. Returns the lowlink for this
  /// node.
  unsigned findSCC(const NodeTy *Node);

  /// \brief Forms SCCs for non-linear loop header phis in the regions.
  void formRegionSCCs();

public:
  static char ID; // Pass identification
  SCCFormation();

  bool runOnFunction(Function &F) override;
  void releaseMemory() override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module * = nullptr) const override;
  void verifyAnalysis() const override;

  /// \brief Returns true is the Node has linear SCEV.
  bool isLinear(const NodeTy *Node) const;

  /// SCC iterator methods
  const_iterator begin(RegionIdentification::const_iterator RegIt) const;
  const_iterator end(RegionIdentification::const_iterator RegIt) const;
};

} // End namespace loopopt

} // End namespace llvm

#endif
