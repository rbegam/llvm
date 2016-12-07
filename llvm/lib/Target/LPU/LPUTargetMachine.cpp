//===-- LPUTargetMachine.cpp - Define TargetMachine for LPU ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Top-level implementation for the LPU target.
//
//===----------------------------------------------------------------------===//

#include "LPUTargetMachine.h"
#include "LPUTargetTransformInfo.h"
#include "LPULowerAggrCopies.h"
#include "LPU.h"
#include "CSASetIntrinsicFunctionAttributes.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/CodeGen/TargetLoweringObjectFileImpl.h"
#include "llvm/CodeGen/TargetPassConfig.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/IR/Verifier.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInstrInfo.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSubtargetInfo.h"
#include "llvm/IR/LegacyPassManager.h"
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
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"


using namespace llvm;

static cl::opt<int>
RunLPUStatistics("lpu-run-statistics", cl::Hidden,
              cl::desc("LPU Specific: collect statistics for DF instructions"),
              cl::init(0));

// Helper function to build a DataLayout string
static std::string computeDataLayout() {
  return "e-m:e-i64:64-n32:64";
}


namespace llvm {
  void initializeLPULowerAggrCopiesPass(PassRegistry &);
}

extern "C" void LLVMInitializeLPUTarget() {
  // Register the target.
  RegisterTargetMachine<LPUTargetMachine> X(TheLPUTarget);

  // The original comment in the LPU target says this optimization
  // is placed here because it is too target-specific.
  PassRegistry &PR = *PassRegistry::getPassRegistry();
  initializeLPULowerAggrCopiesPass(PR);
}

static Reloc::Model getEffectiveRelocModel(Optional<Reloc::Model> RM) {
  if (!RM.hasValue())
    return Reloc::Static;
  return *RM;
}

LPUTargetMachine::LPUTargetMachine(const Target &T, const Triple &TT,
                                         StringRef CPU, StringRef FS,
                                         const TargetOptions &Options,
                                         Optional<Reloc::Model> RM,
                                         CodeModel::Model CM,
                                         CodeGenOpt::Level OL)
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
  //setAsmVerbosityDefault(true);
}


TargetIRAnalysis LPUTargetMachine::getTargetIRAnalysis() {
  return TargetIRAnalysis([this](const Function &F) {
    return TargetTransformInfo(LPUTTIImpl(this, F));
  });
}



LPUTargetMachine::~LPUTargetMachine() {}

namespace {
/// LPU Code Generator Pass Configuration Options.
class LPUPassConfig : public TargetPassConfig {
public:
  LPUPassConfig(LPUTargetMachine *TM, legacy::PassManagerBase &PM)
    : TargetPassConfig(TM, PM) {}

  LPUTargetMachine &getLPUTargetMachine() const {
    return getTM<LPUTargetMachine>();
  }

  bool addInstSelector() override {

    // Add the pass to lower memset/memmove/memcpy
    addPass(createLowerAggrCopies());
    
    // Install an instruction selector.
    addPass(createLPUISelDag(getLPUTargetMachine(), getOptLevel()));
    return false;
  }



  bool addPreISel() override {
    //addPass(createUnifyFunctionExitNodesPass());
    addPass(createLowerSwitchPass());
    return false;
  }


#define DEBUG_TYPE "lpu-convert-control"
  void addPreRegAlloc() override {
    std::string Banner;
#if 1
    Banner = std::string("Before Machine CDG Pass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createControlDepenceGraph(), false);
    Banner = std::string("After Machine CDG Pass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createLPUCvtCFDFPass(), false);
    Banner = std::string("After LPUCvtCFDFPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createLPUOptDFPass(), false);
    Banner = std::string("After LPUOptDFPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createLPURedundantMovElimPass(), false);
    Banner = std::string("After LPURedundantMovElim");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));
    
    addPass(createLPUDeadInstructionElimPass(), false);
    Banner = std::string("After LPUDeadInstructionElim");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    if (RunLPUStatistics) {
      addPass(createLPUStatisticsPass(), false);
    }
#else
    Banner = std::string("Before LPUConvertControlPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));
    addPass(createLPUConvertControlPass(), false);
    Banner = std::string("After LPUConvertControlPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    Banner = std::string("Before LPUOptDFPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));

    addPass(createLPUOptDFPass(), false);
    Banner = std::string("After LPUOptDFPass");
    DEBUG(addPass(createMachineFunctionPrinterPass(errs(), Banner), false));
#endif

  }

  void addPostRegAlloc() override {
    addPass(createLPUAllocUnitPass(), false);
  }

  void addIRPasses() override {

    // Add pass to set readnone attribute for intrinsic library functions
    // so they will be converted to instructions when the calls are lowered
    addPass(createCSASetIntrinsicFunctionAttributesPass(), false);

    // Pass call onto parent
    TargetPassConfig::addIRPasses();
  }

}; // class LPUPassConfig

} // namespace

TargetPassConfig *LPUTargetMachine::createPassConfig(legacy::PassManagerBase &PM) {
  LPUPassConfig *PassConfig = new LPUPassConfig(this, PM);
  return PassConfig;
}
