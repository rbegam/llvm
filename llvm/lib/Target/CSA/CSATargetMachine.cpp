//===-- CSATargetMachine.cpp - Define TargetMachine for CSA ---------------===//
//
// Copyright (C) 2017-2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// Top-level implementation for the CSA target.
//
//===----------------------------------------------------------------------===//

#include "CSATargetMachine.h"
#include "CSA.h"
#include "CSAFortranIntrinsics.h"
#include "CSAIROpt.h"
#include "CSAIntrinsicCleaner.h"
#include "CSALoopIntrinsicExpander.h"
#include "CSALowerAggrCopies.h"
#include "CSAUtils.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Bitcode/CSASaveRawBC.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/LegacyPassManager.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetOptions.h"
#include "llvm/Target/TargetRegisterInfo.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "llvm/Transforms/IPO/PassManagerBuilder.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/LoopSimplify.h"
#include "llvm/Transforms/Utils/Mem2Reg.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"

using namespace llvm;

static cl::opt<int> RunCSAStatistics(
  "csa-run-statistics", cl::Hidden,
  cl::desc("CSA Specific: collect statistics for DF instructions"),
  cl::init(0));

static cl::opt<int>
  CSAStructurizeCFG("csa-structurize-cfg", cl::Hidden,
                    cl::desc("CSA Specific: leverage llvm StructurizeCFG"),
                    cl::init(1));

// Helper function to build a DataLayout string
static std::string computeDataLayout() { return "e-m:e-i64:64-n32:64"; }

namespace llvm {
void initializeCSALowerAggrCopiesPass(PassRegistry &);
void initializeCSAFortranIntrinsicsPass(PassRegistry &);
void initializeCSAInnerLoopPrepPass(PassRegistry &);
} // namespace llvm

extern "C" void LLVMInitializeCSATarget() {
  // Register the target.
  RegisterTargetMachine<CSATargetMachine> X(getTheCSATarget());

  // The original comment in the CSA target says this optimization
  // is placed here because it is too target-specific.
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeCSAInnerLoopPrepPass(PR);
  initializeCSALowerAggrCopiesPass(PR);
  initializeCSAFortranIntrinsicsPass(PR);
}

static Reloc::Model getEffectiveRelocModel(Optional<Reloc::Model> RM) {
  if (!RM.hasValue())
    return Reloc::Static;
  return *RM;
}

CSATargetMachine::CSATargetMachine(const Target &T, const Triple &TT,
                                   StringRef CPU, StringRef FS,
                                   const TargetOptions &Options,
                                   Optional<Reloc::Model> RM,
                                   CodeModel::Model CM, CodeGenOpt::Level OL)
    : LLVMTargetMachine(T, computeDataLayout(), TT, CPU, FS, Options,
                        getEffectiveRelocModel(RM), CM, OL),
      TLOF(make_unique<TargetLoweringObjectFileELF>()),
      Subtarget(TT, CPU, FS, *this) {

  // Although it's still not clear from a performance point of view whether or
  // not we need 'setRequiresStructuredCFG', we're enabling it because it
  // disables certain machine-level transformations in MachineBlockPlacement.
  // At The problematic transformation which prompted us to enable this again
  // was tail merging, but this disables other transformations as well.
  setRequiresStructuredCFG(true);
  initAsmInfo();
  // setAsmVerbosityDefault(true);
}

CSATargetMachine::~CSATargetMachine() {}

namespace {
/// CSA Code Generator Pass Configuration Options.
class CSAPassConfig : public TargetPassConfig {
public:
  CSAPassConfig(CSATargetMachine &TM, legacy::PassManagerBase &PM)
      : TargetPassConfig(TM, PM) {
    disablePass(&PostRAMachineLICMID);
  }

  CSATargetMachine &getCSATargetMachine() const {
    return getTM<CSATargetMachine>();
  }

  bool addInstSelector() override {

    // Add the pass to lower memset/memmove/memcpy
    addPass(createLowerAggrCopies());

    // Install an instruction selector.
    addPass(createCSAISelDag(getCSATargetMachine(), getOptLevel()));

    // Add the pass to expand inline assembly.
    addPass(createCSAExpandInlineAsmPass(), false, true);

    return false;
  }

  bool addPreISel() override {
    // addPass(createUnifyFunctionExitNodesPass());
    addPass(createLowerSwitchPass());
    addPass(createLoopSimplifyPass());
    // Add a pass to generate more candidates for reduction operations
    addPass(createCSAIRReductionOptPass());

    if (CSAStructurizeCFG) {
      addPass(createStructurizeCFGPass(false));
      // remove the single input phi and constant branch created from
      // StructurizeCFG
      addPass(createInstructionCombiningPass());
    }

    // Add a pass to identify and prepare inner loops for pipelinling. This
    // only happens at O1+ so as to avoid requiring excessive additional
    // analyses at O0.
    if (getOptLevel() != CodeGenOpt::None) {
      addPass(createCSAInnerLoopPrepPass());
      // Add streaming memory reductions.
      addPass(createCSAStreamingMemoryPrepPass());
    }

    // Remove any remaining intrinsics which should not go through instruction
    // selection
    addPass(createCSAIntrinsicCleanerPass());

    return false;
  }

#define DEBUG_TYPE "csa-convert-control"
  void addPreRegAlloc() override {
    std::string Banner;
    Banner = std::string("Before Machine CDG Pass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createControlDepenceGraph(), false);
    Banner = std::string("After Machine CDG Pass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createCSAMemopOrderingPass());
    Banner = std::string("After CSAMemopOrderingPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createCSANameLICsPass(), false);

    addPass(createCSACvtCFDFPass(), false);
    Banner = std::string("After CSACvtCFDFPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    if (RunCSAStatistics) {
      addPass(createCSAStatisticsPass(), false);
    }

    addPass(createCSAOptDFPass(), false);
    Banner = std::string("After CSAOptDFPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createCSADataflowCanonicalizationPass(), false);
    Banner = std::string("After CSADataflowCanonicalizationPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createCSAStreamingMemoryConversionPass(), false);
    Banner = std::string("After CSAStreamingMemoryConversionPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createCSAMultiSeqPass(), false);
    Banner = std::string("After CSAMultiSeqPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    
    addPass(createCSARedundantMovElimPass(), false);
    Banner = std::string("After CSARedundantMovElim");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createCSADeadInstructionElimPass(), false);
    Banner = std::string("After CSADeadInstructionElim");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    if (csa_utils::isAlwaysDataFlowLinkageSet()) {
      addPass(createCSAProcCallsPass(), false);
      Banner = std::string("After CSAProcCallsPass");
      DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));
    }

    addPass(createCSAReassocReducPass(), false);
    Banner = std::string("After CSAReassocReducPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createCSANormalizeDebugPass(), false);
    Banner = std::string("After CSANormalizeDebug");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    // Register coalescing causes issues with our def-after-use nature of
    // dataflow.
    disablePass(&RegisterCoalescerID);
  }

  void addPostRegAlloc() override {
    addPass(createCSAAllocUnitPass(), false);

    // These functions don't like vregs.
    disablePass(&ShrinkWrapID);
    disablePass(&MachineCopyPropagationID);
    disablePass(&PostRASchedulerID);
    disablePass(&FuncletLayoutID);
    disablePass(&StackMapLivenessID);
    disablePass(&LiveDebugValuesID);
    disablePass(&PatchableFunctionID);
  }

  void addIRPasses() override {
    // Add the CSASaveRawBC pass which will preserve the initial IR
    // for a module. This must be added early so it gets IR that's
    // equivalent to the Bitcode emitted by the -flto option.
    addPass(createCSASaveRawBCPass());

    // Do any necessary atomic expansion according to Subtarget features.
    addPass(createAtomicExpandPass());

    // Pass call onto parent
    TargetPassConfig::addIRPasses();
  }

}; // class CSAPassConfig

} // namespace

TargetPassConfig *
CSATargetMachine::createPassConfig(legacy::PassManagerBase &PM) {
  CSAPassConfig *PassConfig = new CSAPassConfig(*this, PM);
  return PassConfig;
}

void CSATargetMachine::adjustPassManager(PassManagerBuilder &PMB) {
  PMB.addExtension(PassManagerBuilder::EP_EarlyAsPossible,
                   [](const PassManagerBuilder &, legacy::PassManagerBase &PM) {

                     // Add the pass to convert Fortran "builtin" calls
                     PM.add(createFortranIntrinsics());

                     // Add the pass to expand loop intrinsics
                     PM.add(createSROAPass());
                     PM.add(createLoopSimplifyPass());
                     PM.add(createLICMPass());
                     PM.add(createCSALoopIntrinsicExpanderPass());
                   });
}
