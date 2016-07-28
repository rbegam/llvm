//===-- VectorGraphInfo.h ---------------------------------------*- C++ -*-===//
//
//   Copyright (C) 2016 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//
///
/// \file
///
//===----------------------------------------------------------------------===//
#ifndef LLVM_ANALYSIS_VECTOR_GRAPH_INFO_H
#define LLVM_ANALYSIS_VECTOR_GRAPH_INFO_H

#include "llvm/Analysis/Intel_VPO/Vecopt/VectorGraph.h"
#include "llvm/Analysis/Intel_VPO/Vecopt/VectorGraphInfo.h"
#include "llvm/Pass.h"
#include "llvm/IR/Dominators.h"

namespace llvm { // LLVM Namespace

class LoopInfo;

namespace vpo { // VPO Vectorizer namespace

class VectorGraphInfo : public FunctionPass {

private:

  /// LI - Loop Info for this function.
  const LoopInfo *LI;

protected:
  /// VectorGraph
  VectorGraphTy VectorGraph;

  /// \brief Sets the Loop Info for this function.
  void setLoopInfo(const LoopInfo *LpIn) { LI = LpIn; }

public:
  /// Pass Identification
  static char ID;

  VectorGraphInfo();

  bool runOnFunction(Function &F);
  void create();
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module * = nullptr) const override;

  /// \brief Release the memory of AVRList container built by this pass.
  void releaseMemory();

  /// \brief Builds the vector graph for all inner loops identified by 
  /// LoopInfo for the current function.
  void constructVectorGraph();

  /// \brief Given a loop \p Lp. 
  void constructVectorGraphForLoop(Loop *Lp);

  /// Iterators to iterate over generated vector graph
  typedef VectorGraphTy::iterator iterator;
  typedef VectorGraphTy::const_iterator const_iterator;
  typedef VectorGraphTy::reverse_iterator reverse_iterator;
  typedef VectorGraphTy::const_reverse_iterator const_reverse_iterator;

  /// Iterator Methods
  iterator begin() { return VectorGraph.begin(); }
  const_iterator begin() const { return VectorGraph.begin(); }
  iterator end() { return VectorGraph.end(); }
  const_iterator end() const { return VectorGraph.end(); }

  reverse_iterator rbegin() { return VectorGraph.rbegin(); }
  const_reverse_iterator rbegin() const { return VectorGraph.rbegin(); }
  reverse_iterator rend() { return VectorGraph.rend(); }
  const_reverse_iterator rend() const { return VectorGraph.rend(); }

  /// \brief Returns the Loop Info for this function.
  const LoopInfo *getLoopInfo() { return LI; }

  void addInnerLoop(Loop &L, SmallVectorImpl<Loop *> &V);

  /// \brief Returns true is AvrGenerate Analysis pass list is empty
  bool isVectorGraphEmpty() const { return VectorGraph.empty(); }
};

} // End VPO Vectorizer Namespace
} // End LLVM Namespace

#endif // LLVM_ANALYSIS_VECTOR_GRAPH_INFO_H
