//===------ HIRPostVecCompleteUnroll.cpp - post vec complete unroll -------===//
//
// Copyright (C) 2015-2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
// Wrapper over HIRCompleteUnroll pass. This is executed after vectorizer. The
// idea is to unroll loops which have not been vectorized. The profitability
// threshold is therefore smaller than prevec complete unroll.
//===----------------------------------------------------------------------===//
//

#include "HIRCompleteUnroll.h"
#include "llvm/Support/CommandLine.h"

using namespace llvm;
using namespace llvm::loopopt;

cl::opt<bool>
    DisablePostVecUnroll("disable-hir-post-vec-complete-unroll",
                         cl::desc("Disables post vec complete unroll"),
                         cl::Hidden, cl::init(false));

namespace {

class HIRPostVecCompleteUnroll : public HIRCompleteUnroll {

public:
  static char ID;

  HIRPostVecCompleteUnroll(unsigned OptLevel = 0)
      : HIRCompleteUnroll(ID, OptLevel, false) {
    initializeHIRPostVecCompleteUnrollPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override {
    if (DisablePostVecUnroll) {
      return false;
    }
    return HIRCompleteUnroll::runOnFunction(F);
  }
};
}

char HIRPostVecCompleteUnroll::ID = 0;
INITIALIZE_PASS_BEGIN(HIRPostVecCompleteUnroll, "hir-post-vec-complete-unroll",
                      "HIR PostVec Complete Unroll", false, false)
INITIALIZE_PASS_DEPENDENCY(OptReportOptionsPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetTransformInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRFrameworkWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRLoopStatisticsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRDDAnalysis)
INITIALIZE_PASS_DEPENDENCY(HIRSafeReductionAnalysis)
INITIALIZE_PASS_END(HIRPostVecCompleteUnroll, "hir-post-vec-complete-unroll",
                    "HIR PostVec Complete Unroll", false, false)

FunctionPass *llvm::createHIRPostVecCompleteUnrollPass(unsigned OptLevel) {
  return new HIRPostVecCompleteUnroll(OptLevel);
}
