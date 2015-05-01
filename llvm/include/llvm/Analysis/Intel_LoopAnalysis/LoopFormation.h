//===----------- LoopFormation.h - Creates HIR loops ----------*-- C++ --*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This analysis is used to create HIR loops inside HIR Regions.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_INTEL_LOOPANALYSIS_LOOPFORMATION_H
#define LLVM_ANALYSIS_INTEL_LOOPANALYSIS_LOOPFORMATION_H

#include "llvm/Pass.h"
#include "llvm/ADT/SmallVector.h"

namespace llvm {

class Function;
class Loop;
class LoopInfo;
class ScalarEvolution;

namespace loopopt {

class HLRegion;
class HLLoop;
class HIRCreation;
class HIRCleanup;

/// \brief This analysis forms HIR loops within HIR regions created by the
/// HIRCreation pass.
class LoopFormation : public FunctionPass {
public:
  typedef std::pair<const Loop *, HLLoop *> LoopPairTy;

private:
  /// Func - The function we are analyzing.
  Function *Func;

  /// LI - The loop information for the function we are currently analyzing.
  LoopInfo *LI;

  /// SE - Scalar Evolution analysis for the function.
  ScalarEvolution *SE;

  /// HIR - Pointer to HIRCreation pass.
  HIRCreation *HIR;

  /// HIRC - Pointer to HIRCleanup pass.
  HIRCleanup *HIRC;

  /// Loops - Sorted vector of Loops to HLLoops.
  SmallVector<LoopPairTy, 32> Loops;

  /// \brief Forms loops in HIR.
  void formLoops();

  /// \brief Inserts (Lp, HLoop) pair in the map.
  void insertHLLoop(const Loop *Lp, HLLoop *HLoop);

  /// \brief Implements find()/insert() functionality.
  HLLoop *findOrInsertHLLoopImpl(const Loop *Lp, HLLoop *HLoop, bool Insert);

  /// \brief Traces back to the IV PHINode in the loop header starting from
  /// Inst.
  const PHINode *findIVDefInHeader(const Loop *Lp,
                                   const Instruction *Inst) const;

  /// \brief Sets the IV type for HLoop.
  void setIVType(HLLoop *HLoop) const;

public:
  static char ID; // Pass identification
  LoopFormation();

  bool runOnFunction(Function &F) override;
  void releaseMemory() override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
  void print(raw_ostream &OS, const Module * = nullptr) const override;
  void verifyAnalysis() const override;

  /// \brief Returns HLLoop corresponding to Lp.
  HLLoop *findHLLoop(const Loop *Lp);
};

} // End namespace loopopt

} // End namespace llvm

#endif
