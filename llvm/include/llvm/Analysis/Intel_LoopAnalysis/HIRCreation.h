//===----------- HIRCreation.h - Creates HIR nodes ------------*-- C++ --*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This analysis is used to create HIR nodes out of identified HIR regions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_INTEL_LOOPANALYSIS_HIRCREATION_H
#define LLVM_ANALYSIS_INTEL_LOOPANALYSIS_HIRCREATION_H

#include "llvm/Pass.h"

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallVector.h"

#include "llvm/IR/Intel_LoopIR/HLNode.h"

#include "llvm/Analysis/Intel_LoopAnalysis/RegionIdentification.h"

namespace llvm {

class Function;
class DominatorTree;
struct PostDominatorTree;

namespace loopopt {

class HLRegion;
class HLLabel;
class HLGoto;
class HLIf;
class HLSwitch;

/// \brief This analysis creates and populates HIR regions with HLNodes using
/// the information provided by RegionIdentification pass. The overall sequence
/// of building the HIR is as follows-
///
/// 1) RegionIdentification - identifies regions in IR.
/// 2) HIRCreation - populates HIR regions with a sequence of HLNodes (without
///    HIR loops).
/// 3) LoopFormation - Forms HIR loops within HIR regions.
/// 4) HIRParser - Creates DDRefs and parses SCEVs into CanonExprs.
/// 5) TODO: Add more steps.
///
class HIRCreation : public FunctionPass {
public:
  /// Iterators to iterate over regions
  typedef HLContainerTy::iterator iterator;
  typedef HLContainerTy::const_iterator const_iterator;
  typedef HLContainerTy::reverse_iterator reverse_iterator;
  typedef HLContainerTy::const_reverse_iterator const_reverse_iterator;

private:
  /// Regions - HLRegions formed out of incoming LLVM IR.
  HLContainerTy Regions;

  /// Func - The function we are analyzing.
  Function *Func;

  /// DT - The dominator tree.
  DominatorTree *DT;

  /// PDT - The post-dominator tree.
  PostDominatorTree *PDT;

  /// LastRegionBB - Points to the (lexically) last bblock of the region.
  BasicBlock *LastRegionBB;

  /// Labels - HLLabel map to be used by later passes.
  SmallDenseMap<BasicBlock *, HLLabel *, 64> Labels;

  /// Gotos - HLGotos vector to be used by later passes.
  SmallVector<HLGoto *, 64> Gotos;

  /// Ifs - HLIfs map to be used by later passes.
  SmallDenseMap<HLIf *, BasicBlock *, 32> Ifs;

  /// Switches - HLSwitches map to be used by later passes.
  SmallDenseMap<HLSwitch *, BasicBlock *, 8> Switches;

  /// \brief Creates HLNodes corresponding to the terminator of the basic block.
  HLNode *populateTerminator(BasicBlock *BB, HLNode *InsertionPos);

  /// \brief Creates HLNodes for the instructions in the basic block.
  HLNode *populateInstSequence(BasicBlock *BB, HLNode *InsertionPos);

  /// \brief Performs lexical (preorder) walk of the dominator tree for the
  /// region.
  /// Returns the last HLNode for the current sub-tree.
  HLNode *
  doPreOrderRegionWalk(BasicBlock *BB, HLNode *InsertionPos,
                       RegionIdentification::RegionBBlocksTy &RegionBBlocks);

  /// \brief Creates HLRegions out of IRRegions.
  void create(RegionIdentification *RI);

public:
  static char ID; // Pass identification
  HIRCreation();

  bool runOnFunction(Function &F) override;
  void releaseMemory() override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module * = nullptr) const override;
  void verifyAnalysis() const override;

  /// Region iterator methods
  iterator begin() { return Regions.begin(); }
  const_iterator begin() const { return Regions.begin(); }
  iterator end() { return Regions.end(); }
  const_iterator end() const { return Regions.end(); }

  reverse_iterator rbegin() { return Regions.rbegin(); }
  const_reverse_iterator rbegin() const { return Regions.rbegin(); }
  reverse_iterator rend() { return Regions.rend(); }
  const_reverse_iterator rend() const { return Regions.rend(); }
};

} // End namespace loopopt

} // End namespace llvm

#endif
