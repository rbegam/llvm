//===---------------- WeakAlign.cpp - DTransWeakAlignPass -----------------===//
//
// Copyright (C) 2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive property
// of Intel Corporation and may not be disclosed, examined or reproduced in
// whole or in part without explicit written authorization from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements the DTrans Weak Align pass
//
//===----------------------------------------------------------------------===//

#include "Intel_DTrans/Transforms/WeakAlign.h"
#include "Intel_DTrans/Analysis/DTransAnnotator.h"
#include "Intel_DTrans/DTransCommon.h"
#include "llvm/Analysis/Intel_WP.h"
#include "llvm/Analysis/MemoryBuiltins.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/Pass.h"

using namespace llvm;

#define DEBUG_TYPE "dtrans-weakalign"

// This option controls whether the weak align transformation heuristics are
// used for enabling the transformation. Setting this to true will allow the
// transformation and safety analysis to be run without identifying a routine
// marked by the SAO-to-AOS transformation.
static cl::opt<bool> HeurOverride("dtrans-weakalign-heur-override",
                                  cl::init(false), cl::ReallyHidden);

namespace {
class DTransWeakAlignWrapper : public ModulePass {
private:
  dtrans::WeakAlignPass Impl;

public:
  static char ID;

  DTransWeakAlignWrapper() : ModulePass(ID) {
    initializeDTransWeakAlignWrapperPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override {
    if (skipModule(M))
      return false;

    auto &TLI = getAnalysis<TargetLibraryInfoWrapperPass>().getTLI();
    auto &WPInfo = getAnalysis<WholeProgramWrapperPass>().getResult();
    return Impl.runImpl(M, TLI, WPInfo);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<TargetLibraryInfoWrapperPass>();
    AU.addRequired<WholeProgramWrapperPass>();
    AU.addPreserved<WholeProgramWrapperPass>();
  }
};
} //  end anonymous namespace

PreservedAnalyses dtrans::WeakAlignPass::run(Module &M,
                                             ModuleAnalysisManager &AM) {
  auto &TLI = AM.getResult<TargetLibraryAnalysis>(M);
  auto &WPInfo = AM.getResult<WholeProgramAnalysis>(M);
  if (!runImpl(M, TLI, WPInfo))
    return PreservedAnalyses::all();

  PreservedAnalyses PA;
  PA.preserve<WholeProgramAnalysis>();
  return PA;
}

namespace llvm {
namespace dtrans {

class WeakAlignImpl {
public:
  // Analyze and perform the transform, if possible. Return 'true'
  // if IR changes are made.
  bool run(Module &M, const TargetLibraryInfo &TLI);

private:
  bool analyzeModule(Module &M, const TargetLibraryInfo &TLI);
  bool analyzeFunction(Function &F);
  bool isSupportedIntrinsicInst(IntrinsicInst *II);

  Constant *getMalloptFunction(Module &M, const TargetLibraryInfo &TLI);
};

bool WeakAlignImpl::run(Module &M, const TargetLibraryInfo &TLI) {
  // Make sure the mallopt function is available before analyzing the IR.
  Constant *MalloptFunc = getMalloptFunction(M, TLI);
  if (!MalloptFunc)
    return false;

  // Check for safety issues that prevent the transform.
  if (!analyzeModule(M, TLI))
    return false;

  LLVM_DEBUG(
      dbgs()
      << "DTRANS Weak Align: enabled -- Heuristics and safety tests passed\n");

  bool Changed = false;
  // TODO: Add insertion of mallopt call at the start of main.
  return Changed;
}

// Get a handle the mallopt() function, if it is available. Otherwise,
// return nullptr.
Constant *WeakAlignImpl::getMalloptFunction(Module &M,
                                            const TargetLibraryInfo &TLI) {
  LibFunc MalloptLF;
  bool Found = TLI.getLibFunc("mallopt", MalloptLF);
  if (!Found) {
    LLVM_DEBUG(
        dbgs() << "DTRANS Weak Align: inhibited -- mallopt() not available\n");
    return nullptr;
  }

  // Verify the function is available
  if (!TLI.has(MalloptLF)) {
    LLVM_DEBUG(
        dbgs() << "DTRANS Weak Align: inhibited -- mallopt() not available\n");
    return nullptr;
  }

  LLVMContext &Ctx = M.getContext();
  llvm::Type *Int32Ty = IntegerType::getInt32Ty(Ctx);
  auto *MalloptFunc =
      M.getOrInsertFunction("mallopt", Int32Ty, Int32Ty, Int32Ty);
  if (!MalloptFunc) {
    LLVM_DEBUG(
        dbgs()
        << "DTRANS Weak Align: inhibited -- mallopt() mismatched signature\n");
    return nullptr;
  }

  return MalloptFunc;
}

// Check if there are issues within the module that should inhibit setting
// qkmalloc allocator to use the weak memory allocation mode. Return 'true'
// if the function is safe, 'false' otherwise.
bool WeakAlignImpl::analyzeModule(Module &M, const TargetLibraryInfo &TLI) {

  // List of allocation functions that are allowed to be seen in the program.
  // Any reference to an allocation function (as identified in the
  // AllocationFnData table within Analysis/MemoryBuiltins.cpp) will inhibit the
  // transformation.
  static const LibFunc SupportedAllocFns[] = {
      LibFunc_malloc,
      LibFunc_Znwj,
      LibFunc_ZnwjRKSt9nothrow_t,
      LibFunc_Znwm,
      LibFunc_ZnwmRKSt9nothrow_t,
      LibFunc_Znaj,
      LibFunc_ZnajRKSt9nothrow_t,
      LibFunc_Znam,
      LibFunc_ZnamRKSt9nothrow_t,
      LibFunc_calloc,
      LibFunc_realloc,
  };

  auto IsSupportedAllocationFn = [](LibFunc LF) {
    auto Fns = makeArrayRef(SupportedAllocFns);
    return std::any_of(Fns.begin(), Fns.end(),
                       [&LF](LibFunc Elem) { return Elem == LF; });
  };

  // Check for functions that allocate memory to make sure there are only calls
  // to specific routines. This is to ensure there are no uses of a function
  // which may take an alignment argument. It is sufficient to just see if a
  // declaration exists, because that is enough to know that it may be called
  // directly or indirectly without checking each call site since we know we
  // have the whole program. In other words, if it's not seen, there are no
  // calls to it.
  //
  // Also, search for functions marked by the SOA-to-AOS transformation to know
  // whether this transformation should be applied. This is an ugly hack, but
  // the only time we want this transform to run is on cases that are also
  // transformed by the SOA-to-AOS transformation, and there's not another
  // cheap mechanism to determine that.
  LibFunc TheLibFunc;
  bool SawSOAToAOS = false;
  for (auto &F : M) {
    if (TLI.getLibFunc(F.getName(), TheLibFunc) && TLI.has(TheLibFunc) &&
        llvm::isAllocationLibFunc(TheLibFunc) &&
        !IsSupportedAllocationFn(TheLibFunc)) {
      LLVM_DEBUG(dbgs() << "DTRANS Weak Align: inhibited -- May allocate "
                           "alignment memory:\n  "
                        << F.getName() << "\n");
      return false;
    }

    if (DTransAnnotator::lookupDTransSOAToAOSTypeAnnotation(F))
      SawSOAToAOS = true;
  }

  if (!HeurOverride && !SawSOAToAOS) {
    LLVM_DEBUG(dbgs() << "DTRANS Weak Align: inhibited -- Did not find "
      "SOA-to-AOS transformed routine:\n");
    return false;
  }

  for (auto &F : M)
    if (!analyzeFunction(F))
      return false;

  return true;
}

// Check if there are issues within the function that should inhibit setting
// qkmalloc allocator to use the weak memory allocation mode. Return 'true'
// if the function is safe, 'false' otherwise.
bool WeakAlignImpl::analyzeFunction(Function &F) {
  // Check if a load instruction is supported. Currently, this just checks
  // whether a vector type is loaded because a vector load instruction could
  // require a specific alignment, so we will disable the transform if any
  // are seen.
  auto IsSupportedLoad = [](LoadInst &LI) {
    return !LI.getType()->isVectorTy();
  };

  // Check if a store instruction is supported. Currently, this just checks
  // whether a vector type is stored because a vector store instruction could
  // require a specific alignment.
  auto IsSupportedStore = [](StoreInst &SI) {
    return !SI.getValueOperand()->getType()->isVectorTy();
  };

  LLVM_DEBUG(dbgs() << "DTRANS Weak Align: Analyzing " << F.getName() << "\n");
  for (auto &I : instructions(&F)) {
    switch (I.getOpcode()) {
    case Instruction::Load:
      if (!IsSupportedLoad(*cast<LoadInst>(&I))) {
        LLVM_DEBUG(
            dbgs()
            << "DTRANS Weak Align: inhibited -- Unsupported LoadInst:\n  " << I
            << "\n");
        return false;
      }
      break;

    case Instruction::Store:
      if (!IsSupportedStore(*cast<StoreInst>(&I))) {
        LLVM_DEBUG(
            dbgs()
            << "DTRANS Weak Align: inhibited -- Unsupported StoreInst:\n  " << I
            << "\n");
        return false;
      }
      break;

    case Instruction::Call: {
      if (auto *II = dyn_cast<IntrinsicInst>(&I)) {
        if (!isSupportedIntrinsicInst(II)) {
          LLVM_DEBUG(dbgs() << "DTRANS Weak Align: inhibited -- Contains "
                               "unsupported intrinsic:\n  "
                            << I << "\n");
          return false;
        }
        break;
      }

      auto *CI = cast<CallInst>(&I);
      if (CI->isInlineAsm()) {
        LLVM_DEBUG(
            dbgs() << "DTRANS Weak Align: inhibited -- Contains inline asm:\n  "
                   << I << "\n");
        return false;
      }
      // All other calls are allowed.
      break;
    }

    case Instruction::ExtractElement:
    case Instruction::InsertElement:
    case Instruction::ShuffleVector:
      // Disallow vector instructions.
      LLVM_DEBUG(dbgs() << "DTRANS Weak Align: inhibited -- Unsupported vector "
                           "instruction:\n  "
                        << I << "\n");
      return false;

    default:
      // All other instructions are allowed.
      break;
    }
  }

  return true;
}

// Check whether the intrinsic should be allowed for the transformation.
//
// This is an opt-in approach that covers the expected intrinsics for the case
// of interest, and rejects everything else. i.e., there may be other intrinsics
// that can be safely added to this list. However, there are some that we
// definitely want to exclude, such as:
//
// Intrinsic::assume:
//   The __assume_aligned expression turns into an assume intrinsic
//   call in the IR, so inhibit this transform for any case involving
//   an assume intrinsic. This is more conservative than strictly
//   necessary.
//
// Intrinsic::x86_mmx_palignr_b
//   This is using 16-byte aligned memory, and since we are excluding
//   aligned access, we will exclude this (and all other mmx intrinsics)
//
bool WeakAlignImpl::isSupportedIntrinsicInst(IntrinsicInst *II) {
  auto IntrinID = II->getIntrinsicID();
  switch (IntrinID) {
  default:
    // Conservatively disallow any other intrinsics.
    return false;

    // The following intrinsics will be allowed. This captures the basic
    // set required for the case being targeted, and may be expanded over time.
  case Intrinsic::lifetime_end:
  case Intrinsic::lifetime_start:
  case Intrinsic::icall_branch_funnel:
  case Intrinsic::dbg_addr:
  case Intrinsic::dbg_declare:
  case Intrinsic::dbg_label:
  case Intrinsic::dbg_value:
  case Intrinsic::annotation:
  case Intrinsic::ptr_annotation:
  case Intrinsic::var_annotation:
  case Intrinsic::eh_typeid_for:
  case Intrinsic::trap:
  case Intrinsic::vastart:
  case Intrinsic::vaend:
  case Intrinsic::vacopy:
  case Intrinsic::memcpy:
  case Intrinsic::memmove:
  case Intrinsic::memset:
  case Intrinsic::sqrt:
  case Intrinsic::pow:
  case Intrinsic::powi:
  case Intrinsic::sin:
  case Intrinsic::cos:
  case Intrinsic::exp:
  case Intrinsic::exp2:
  case Intrinsic::log:
  case Intrinsic::log10:
  case Intrinsic::fma:
  case Intrinsic::fabs:
  case Intrinsic::maxnum:
  case Intrinsic::minimum:
  case Intrinsic::maximum:
  case Intrinsic::copysign:
  case Intrinsic::floor:
  case Intrinsic::ceil:
  case Intrinsic::trunc:
  case Intrinsic::rint:
  case Intrinsic::nearbyint:
  case Intrinsic::round:
  case Intrinsic::bitreverse:
  case Intrinsic::bswap:
  case Intrinsic::ctpop:
  case Intrinsic::ctlz:
  case Intrinsic::cttz:
  case Intrinsic::fshl:
  case Intrinsic::fshr:
  case Intrinsic::sadd_with_overflow:
  case Intrinsic::uadd_with_overflow:
  case Intrinsic::ssub_with_overflow:
  case Intrinsic::usub_with_overflow:
  case Intrinsic::smul_with_overflow:
  case Intrinsic::umul_with_overflow:
    break;
  }

  return true;
}

bool WeakAlignPass::runImpl(Module &M, const TargetLibraryInfo &TLI,
                            WholeProgramInfo &WPInfo) {
  if (!WPInfo.isWholeProgramSafe()) {
    LLVM_DEBUG(
        dbgs() << "DTRANS Weak Align: inhibited -- not whole program safe");
    return false;
  }

  WeakAlignImpl Impl;
  return Impl.run(M, TLI);
}

} // end namespace dtrans
} // end namespace llvm

char DTransWeakAlignWrapper::ID = 0;
INITIALIZE_PASS_BEGIN(DTransWeakAlignWrapper, "dtrans-weakalign",
                      "DTrans weak align", false, false)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(WholeProgramWrapperPass)
INITIALIZE_PASS_END(DTransWeakAlignWrapper, "dtrans-weakalign",
                    "DTrans weak align", false, false)

ModulePass *llvm::createDTransWeakAlignWrapperPass() {
  return new DTransWeakAlignWrapper();
}
