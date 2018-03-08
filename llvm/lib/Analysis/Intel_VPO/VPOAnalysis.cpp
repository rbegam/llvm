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
//   VPOAnalysis.cpp -- VPO Analysis Passes initializers.
//
//===----------------------------------------------------------------------===//

#include "llvm/InitializePasses.h"

using namespace llvm;

void llvm::initializeIntel_VPOAnalysis(PassRegistry &Registry) {
  initializeWRegionCollectionWrapperPassPass(Registry);
  initializeWRegionInfoWrapperPassPass(Registry);

  initializeAVRGeneratePass(Registry);
  initializeAVRGenerateHIRPass(Registry);
  initializeAVRDecomposeHIRPass(Registry);
  initializeVPOPredicatorPass(Registry);
  initializeVPOPredicatorHIRPass(Registry);

  initializeAvrDefUsePass(Registry);
  initializeAvrDefUseHIRPass(Registry);
  initializeAvrCFGPass(Registry);
  initializeAvrCFGHIRPass(Registry);
  initializeSIMDLaneEvolutionPass(Registry);
  initializeSIMDLaneEvolutionHIRPass(Registry);
  initializeVectorGraphInfoPass(Registry);
  initializeVectorGraphPredicatorPass(Registry); 
}
