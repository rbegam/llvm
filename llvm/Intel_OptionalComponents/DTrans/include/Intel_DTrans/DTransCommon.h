//===------------------ DTransCommon.h - Shared DTrans code ---------------===//
//
// Copyright (C) 2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive property
// of Intel Corporation and may not be disclosed, examined or reproduced in
// whole or in part without explicit written authorization from the company.
//
//===----------------------------------------------------------------------===//
//
// This file declares functions that are common to all DTrans passes.
//
//===----------------------------------------------------------------------===//

#if !INTEL_INCLUDE_DTRANS
#error DTransCommon.h include in an non-INTEL_INCLUDE_DTRANS build.
#endif

#ifndef INTEL_DTRANS_DTRANS_COMMON_H
#define INTEL_DTRANS_DTRANS_COMMON_H

// The new pass manager's PassRegistry.def needs to see the declarations
// for each pass.
#include "Intel_DTrans/Analysis/DTransAnalysis.h"
#include "Intel_DTrans/Transforms/AOSToSOA.h"
#include "Intel_DTrans/Transforms/DTransPaddedMalloc.h"
#include "Intel_DTrans/Transforms/DeleteField.h"
#include "Intel_DTrans/Transforms/DynClone.h"
#include "Intel_DTrans/Transforms/EliminateROFieldAccess.h"
#include "Intel_DTrans/Transforms/PaddedPointerPropagation.h"
#include "Intel_DTrans/Transforms/ReorderFields.h"

#if !INTEL_PRODUCT_RELEASE
#include "Intel_DTrans/Transforms/DTransOptBaseTest.h"
#endif // !INTEL_PRODUCT_RELEASE

namespace llvm {

namespace legacy {
class PassManagerBase;
} // namespace legacy

void initializeDTransPasses(PassRegistry&);

void initializeDTransAnalysisWrapperPass(PassRegistry&);
void initializeDTransAOSToSOAWrapperPass(PassRegistry&);
void initializeDTransDeleteFieldWrapperPass(PassRegistry&);
void initializeDTransPaddedMallocWrapperPass(PassRegistry&);
void initializeDTransReorderFieldsWrapperPass(PassRegistry&);
// Pass for elimination of unreacheble access to field which is only read.
void initializeDTransEliminateROFieldAccessWrapperPass(PassRegistry&);
void initializePaddedPtrPropWrapperPass(PassRegistry&);
void initializeDTransDynCloneWrapperPass(PassRegistry&);

#if !INTEL_PRODUCT_RELEASE
void initializeDTransOptBaseTestWrapperPass(PassRegistry&);
#endif // !INTEL_PRODUCT_RELEASE

// This is used by ForcePassLinking.
void createDTransPasses();

void addDTransPasses(ModulePassManager &MPM);
void addDTransLegacyPasses(legacy::PassManagerBase &PM);
void addLateDTransPasses(ModulePassManager &MPM);
void addLateDTransLegacyPasses(legacy::PassManagerBase &PM);

} // namespace llvm


#endif // INTEL_DTRANS_DTRANS_COMMON_H
