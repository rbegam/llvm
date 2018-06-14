//===---- HIRPostVecCompleteUnroll.h -------------------------------------------===//
//
// Copyright (C) 2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_HIRPOSTVECCOMPLETEUNROLL_H
#define LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_HIRPOSTVECCOMPLETEUNROLL_H

#include "llvm/IR/PassManager.h"

namespace llvm {

namespace loopopt {

class HIRPostVecCompleteUnrollPass
    : public PassInfoMixin<HIRPostVecCompleteUnrollPass> {
  unsigned OptLevel;
public:
  HIRPostVecCompleteUnrollPass(bool OptLevel = 0) : OptLevel(OptLevel) {}
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

} // namespace loopopt

} // namespace llvm

#endif // LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_HIRPOSTVECCOMPLETEUNROLL_H

