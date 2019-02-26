//===-- CSA.h - Top-level interface for CSA representation ------*- C++ -*-===//
//
// Copyright (C) 2017-2019 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file contains the entry points for global functions defined in
// the LLVM CSA back-end.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_CSA_CSA_H
#define LLVM_LIB_TARGET_CSA_CSA_H

#include "MCTargetDesc/CSAMCTargetDesc.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetMachine.h"

namespace llvm {
class CSATargetMachine;
class FunctionPass;
class MachineFunctionPass;

FunctionPass *createCSAISelDag(CSATargetMachine &TM,
                               llvm::CodeGenOpt::Level OptLevel);
MachineFunctionPass *createCSAConvertControlPass();
MachineFunctionPass *createControlDepenceGraph();
MachineFunctionPass *createCSACvtCFDFPass();
MachineFunctionPass *createCSAStatisticsPass();
MachineFunctionPass *createCSAProcCallsPass();
MachineFunctionPass *createCSAOptDFPass();
MachineFunctionPass *createCSAMultiSeqPass();
MachineFunctionPass *createCSADFParLoopPass();
MachineFunctionPass *createCSADeadInstructionElimPass();
MachineFunctionPass *createCSAAllocUnitPass();
MachineFunctionPass *createCSAPrologEpilogPass();
MachineFunctionPass *createCSAExpandInlineAsmPass();
MachineFunctionPass *createCSANormalizeDebugPass();
MachineFunctionPass *createCSADataflowCanonicalizationPass();
Pass *createCSAStreamingMemoryConversionPass();
MachineFunctionPass *createCSASeqotToSeqOptimizationPass();
MachineFunctionPass *createCSANameLICsPass();
MachineFunctionPass *createCSARASReplayableLoadsDetectionPass();
MachineFunctionPass *createCSADataflowVerifier();
// FunctionPass *createCSALowerStructArgsPass();
Pass *createCSAInnerLoopPrepPass();
Pass *createCSAReplaceAllocaWithMallocPass(CSATargetMachine &TM);
Pass *createCSAMemopOrderingPass(const CSATargetMachine &TM);
Pass *createCSAParseAnnotateAttributesPass();

void initializeCSAMemopOrderingPasses(PassRegistry &);
void initializeCSAStreamingMemoryPass(PassRegistry &);

bool shouldRunDataflowPass(const MachineFunction &MF);

} // namespace llvm

#endif
