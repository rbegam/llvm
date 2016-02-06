//===----------------------------------------------------------------------===//
//
//   Copyright (C) 2015-2016 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation. and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//   Source file:
//   ------------
//   VPOVecoptAnalysis.cpp -- Vecopt Analysis Passes initializers.
//
//===----------------------------------------------------------------------===//

#include "llvm/InitializePasses.h"

using namespace llvm;

void llvm::initializeIntel_VPOVecoptAnalysis(PassRegistry &Registry) {
  initializeIdentifyVectorCandidatesPass(Registry);
  initializeAVRGeneratePass(Registry);
  initializeAVRGenerateHIRPass(Registry);
}
