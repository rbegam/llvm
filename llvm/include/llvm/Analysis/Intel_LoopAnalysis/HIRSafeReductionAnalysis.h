//===--------   HIRSafeReductionAnalysis.h    -----------------------------===//
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
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_INTEL_SAFEREDUCTION_H
#define LLVM_ANALYSIS_INTEL_SAFEREDUCTION_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/Intel_LoopAnalysis/HIRAnalysisPass.h"
#include "llvm/Analysis/Intel_LoopAnalysis/HIRDDAnalysis.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

#include <map>

namespace llvm {

class formatted_raw_ostream;

namespace loopopt {

class CanonExpr;
class HLInst;
class HLLoop;

typedef SmallVector<const HLInst *, 4> SafeRedChain;
typedef SmallVector<SafeRedChain, 2> SafeRedChainList;

class HIRSafeReductionAnalysis final : public HIRAnalysisPass {

private:
  HIRDDAnalysis *DDA;
  unsigned FirstRvalSB;
  const HLNode *FirstChild;
  // From Loop, look up all sets of Insts in a Safe Reduction chain
  SmallDenseMap<const HLLoop *, SafeRedChainList, 16> SafeReductionMap;
  // From Inst, Look up  Instruction Set.
  // There is no need to go through Loop,
  // because there are no many safe reductions in a function.
  SmallDenseMap<const HLInst *, SafeRedChain *, 16> SafeReductionInstMap;
  // Map of reduction DDRef symbases and the reduction opcode
  SmallDenseMap<unsigned, unsigned, 16> SafeReductionSymbaseMap;

  bool findFirstRedStmt(const HLLoop *Loop, const HLInst *Inst,
                        bool *SingleStmtReduction, unsigned *FirstSB,
                        unsigned *ReductionOpCode, DDGraph DDG);

  void setSafeRedChainList(SafeRedChain &RedInsts, const HLLoop *Loop,
                           unsigned RedSymbase, unsigned RedOpCode);

  void identifySingleStatementReduction(const HLLoop *Loop, DDGraph DDG);
  void identifySafeReductionChain(const HLLoop *Loop, DDGraph DDG);
  bool isValidSR(const RegDDRef *LRef, const HLLoop *Loop, HLInst **SinkInst,
                 DDRef **SinkDDRef, unsigned ReductionOpCode, DDGraph DDG);

  // Perform  SafeReducton Analysis
  void identifySafeReduction(const HLLoop *Loop);
  // Checks if a temp is legal to be used for Reduction
  //  e.g s =  10 * s + ..  is not legal
  bool isRedTemp(CanonExpr *CE, BlobTy Blob);

public:
  HIRSafeReductionAnalysis()
      : HIRAnalysisPass(ID, HIRAnalysisPass::HIRSafeReductionAnalysisVal) {}
  static char ID;

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;

  // \brief Compute SafeReduction for all innermost loops
  void computeSafeReductionChains(const HLLoop *Loop);

  // \brief Get SafeReduction of a Loop
  const SafeRedChainList &getSafeReductionChain(const HLLoop *Loop);

  // \brief Is Inst part of a Safe Reduction. Indicate of Single Stmt when
  // argument
  //  supplied
  bool isSafeReduction(const HLInst *Inst, bool *IsSingleStmt = nullptr) const;

  void print(formatted_raw_ostream &OS, const HLLoop *Loop);
  void print(formatted_raw_ostream &OS, const HLLoop *Loop,
             const SafeRedChainList *SR);
  // void print(formatted_raw_ostream &OS, unsigned Indented,
  //           const SafeRedChain *SRC);
  void markLoopBodyModified(const HLLoop *L) override;
  void releaseMemory() override;

  /// Return true if given Symbase corresponds to a reduction DDRef, return
  /// reduction opcode in OpCodeP if it is not null.
  bool isSafeReductionSymbase(unsigned Symbase, unsigned *OpCodeP = nullptr);

  /// \brief Method for supporting type inquiry through isa, cast, and dyn_cast.
  static bool classof(const HIRAnalysisPass *AP) {
    return AP->getHIRAnalysisID() ==
           HIRAnalysisPass::HIRSafeReductionAnalysisVal;
  }
};

} // End namespace loopopt

} // End namespace llvm

#endif
