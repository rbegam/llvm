//===-- Intel_LoopTransforms.cpp-------------------------------------------===//
//
// Copyright (C) 2015 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//

#include "llvm/InitializePasses.h"
#include "llvm/PassRegistry.h"

using namespace llvm;

void llvm::initializeIntel_LoopTransforms(PassRegistry &Registry) {
  initializeSSADeconstructionPass(Registry);
  initializeHIROptPredicatePass(Registry);
  initializeHIRCompleteUnrollPass(Registry);
  initializeHIRGeneralUnrollPass(Registry);
  initializeHIRDummyTransformationPass(Registry);
  initializeHIRLoopInterchangePass(Registry);

  // TODO - LLVMBuild.txt needs to be updated - pending on separate
  // VPO vectorization driver pass for HIR.
  initializeVPODriverHIRPass(Registry);
  initializeHIRCodeGenPass(Registry);
  initializeParDirectiveInsertionPass(Registry);
  initializeVecDirectiveInsertionPass(Registry);
}
