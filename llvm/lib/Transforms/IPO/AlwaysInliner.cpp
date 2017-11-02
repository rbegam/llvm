//===- InlineAlways.cpp - Code to inline always_inline functions ----------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements a custom inliner that handles only functions that
// are marked as "always inline".
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/AssumptionCache.h"
#include "llvm/Analysis/CallGraph.h"
#include "llvm/Analysis/InlineCost.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/CallingConv.h"
#include "llvm/IR/DataLayout.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Type.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/Inliner.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils/ModuleUtils.h"

using namespace llvm;
using namespace InlineReportTypes; // INTEL

#define DEBUG_TYPE "inline"

#if INTEL_CUSTOMIZATION
// Inline if function has always_inline or inline_list attributes, don't inline
// if function has noinline-list attribute. InlineList/NoinlineList attributes
// are stronger than any other attributes of function.
static bool hasAppropriateInlineAttribute(Function &F) {
  if (F.hasFnAttribute(Attribute::NoinlineList))
    return false;
  if (F.hasFnAttribute(Attribute::AlwaysInline) ||
      F.hasFnAttribute(Attribute::InlineList))
    return true;
  return false;
}

// Inline if callsite has always_inline or inline_list attributes, don't inline
// if callsite has noinline-list attribute. InlineList/NoinlineList attributes
// are stronger than any other attributes of function.
static bool hasAppropriateInlineAttribute(CallSite &CS) {
  if (CS.hasFnAttr(Attribute::NoinlineList))
    return false;
  if (CS.hasFnAttr(Attribute::AlwaysInline) ||
      CS.hasFnAttr(Attribute::InlineList))
    return true;
  return false;
}
#endif // INTEL_CUSTOMIZATION

PreservedAnalyses AlwaysInlinerPass::run(Module &M, ModuleAnalysisManager &) {
  InlineFunctionInfo IFI;
  SmallSetVector<CallSite, 16> Calls;
  bool Changed = false;
  SmallVector<Function *, 16> InlinedFunctions;
  InlineReason Reason; // INTEL
  for (Function &F : M)
    if (!F.isDeclaration() && hasAppropriateInlineAttribute(F) && // INTEL
        isInlineViable(F, Reason)) { // INTEL
      Calls.clear();

      for (User *U : F.users())
        if (auto CS = CallSite(U))
          if (CS.getCalledFunction() == &F)
            Calls.insert(CS);

      for (CallSite CS : Calls)
        // FIXME: We really shouldn't be able to fail to inline at this point!
        // We should do something to log or check the inline failures here.
        Changed |= InlineFunction(CS, IFI);

      // Remember to try and delete this function afterward. This both avoids
      // re-walking the rest of the module and avoids dealing with any iterator
      // invalidation issues while deleting functions.
      InlinedFunctions.push_back(&F);
    }

  // Remove any live functions.
  erase_if(InlinedFunctions, [&](Function *F) {
    F->removeDeadConstantUsers();
    return !F->isDefTriviallyDead();
  });

  // Delete the non-comdat ones from the module and also from our vector.
  auto NonComdatBegin = partition(
      InlinedFunctions, [&](Function *F) { return F->hasComdat(); });
  for (Function *F : make_range(NonComdatBegin, InlinedFunctions.end()))
    M.getFunctionList().erase(F);
  InlinedFunctions.erase(NonComdatBegin, InlinedFunctions.end());

  if (!InlinedFunctions.empty()) {
    // Now we just have the comdat functions. Filter out the ones whose comdats
    // are not actually dead.
    filterDeadComdatFunctions(M, InlinedFunctions);
    // The remaining functions are actually dead.
    for (Function *F : InlinedFunctions)
      M.getFunctionList().erase(F);
  }

  return Changed ? PreservedAnalyses::none() : PreservedAnalyses::all();
}

namespace {

/// Inliner pass which only handles "always inline" functions.
///
/// Unlike the \c AlwaysInlinerPass, this uses the more heavyweight \c Inliner
/// base class to provide several facilities such as array alloca merging.
class AlwaysInlinerLegacyPass : public LegacyInlinerBase {

#if INTEL_SPECIFIC_IL0_BACKEND
  // This is used to enable/disable standard inliner pass for
  // AlwaysInline attribute and perform it only for inline functions
  // specifically marked with "INTEL_ALWAYS_INLINE".
  bool Il0BackendMode;
#endif // INTEL_SPECIFIC_IL0_BACKEND

public:
  AlwaysInlinerLegacyPass() : LegacyInlinerBase(ID, /*InsertLifetime*/ true) {
    initializeAlwaysInlinerLegacyPassPass(*PassRegistry::getPassRegistry());
#if INTEL_SPECIFIC_IL0_BACKEND
    Il0BackendMode = false;
#endif // INTEL_SPECIFIC_IL0_BACKEND
  }

  AlwaysInlinerLegacyPass(bool InsertLifetime)
      : LegacyInlinerBase(ID, InsertLifetime) {
    initializeAlwaysInlinerLegacyPassPass(*PassRegistry::getPassRegistry());
#if INTEL_SPECIFIC_IL0_BACKEND
    Il0BackendMode = false;
#endif // INTEL_SPECIFIC_IL0_BACKEND
  }

#if INTEL_SPECIFIC_IL0_BACKEND
  AlwaysInlinerLegacyPass(bool InsertLifetime, bool Il0BackendMode)
      : Inliner(ID, InsertLifetime) {
    initializeAlwaysInlinerPass(*PassRegistry::getPassRegistry());
    this->Il0BackendMode = Il0BackendMode;
  }
#endif // INTEL_SPECIFIC_IL0_BACKEND

  /// Main run interface method.  We override here to avoid calling skipSCC().
  bool runOnSCC(CallGraphSCC &SCC) override { return inlineCalls(SCC); }

  static char ID; // Pass identification, replacement for typeid

  InlineCost getInlineCost(CallSite CS) override;

  using llvm::Pass::doFinalization;
  bool doFinalization(CallGraph &CG) override {
#if INTEL_CUSTOMIZATION
    bool ReturnValue = removeDeadFunctions(CG, /*AlwaysInlineOnly=*/true);
    getReport().print();
    return ReturnValue;
#endif // INTEL_CUSTOMIZATION
  }
};
}

char AlwaysInlinerLegacyPass::ID = 0;
INITIALIZE_PASS_BEGIN(AlwaysInlinerLegacyPass, "always-inline",
                      "Inliner for always_inline functions", false, false)
INITIALIZE_PASS_DEPENDENCY(AssumptionCacheTracker)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(AlwaysInlinerLegacyPass, "always-inline",
                    "Inliner for always_inline functions", false, false)

Pass *llvm::createAlwaysInlinerLegacyPass(bool InsertLifetime) {
  return new AlwaysInlinerLegacyPass(InsertLifetime);
}

#if INTEL_SPECIFIC_IL0_BACKEND
Pass *llvm::createAlwaysInlinerLegacyPass(bool InsertLifetime,
                                          bool Il0BackendMode) {
  return new AlwaysInliner(InsertLifetime, Il0BackendMode);
}
#endif // INTEL_SPECIFIC_IL0_BACKEND

/// \brief Get the inline cost for the always-inliner.
///
/// The always inliner *only* handles functions which are marked with the
/// attribute to force inlining. As such, it is dramatically simpler and avoids
/// using the powerful (but expensive) inline cost analysis. Instead it uses
/// a very simple and boring direct walk of the instructions looking for
/// impossible-to-inline constructs.
///
/// Note, it would be possible to go to some lengths to cache the information
/// computed here, but as we only expect to do this for relatively few and
/// small functions which have the explicit attribute to force inlining, it is
/// likely not worth it in practice.
InlineCost AlwaysInlinerLegacyPass::getInlineCost(CallSite CS) {
  Function *Callee = CS.getCalledFunction();

#if INTEL_SPECIFIC_IL0_BACKEND
  // Only specially marked functions are inlined here.
  // The rest always_inline functions are processed by the IL0 backend.
  // This is necessary due to current CilkPlus implementation, where front-end
  // emits some code outlined, but it has to be inlined to have valid
  // debug info in IL0 and also IL0 backend does not inline back functions
  // with call to Cilk's setjmp.
  if (Il0BackendMode) {
    InlineReason Reason;
    if (Callee && !Callee->isDeclaration() &&
        Callee->hasFnAttribute("INTEL_ALWAYS_INLINE") &&
        isInlineViable(*Callee, Reason))
      return InlineCost::getAlways(InlrAlwaysInline);
    return InlineCost::getNever(NinlrNotAlwaysInline);
  }
#endif // INTEL_SPECIFIC_IL0_BACKEND

  // Only inline direct calls to functions with always-inline attributes
  // that are viable for inlining. FIXME: We shouldn't even get here for
  // declarations.
#if INTEL_CUSTOMIZATION
  InlineReason Reason;
  if (Callee && !Callee->isDeclaration() &&
      hasAppropriateInlineAttribute(CS) &&
      isInlineViable(*Callee, Reason)) {
    if (CS.hasFnAttr(Attribute::InlineList))
      return InlineCost::getAlways(InlrInlineList);
    return InlineCost::getAlways(InlrAlwaysInline);
  }

  if (CS.hasFnAttr(Attribute::NoinlineList))
    return InlineCost::getNever(NinlrNoinlineList);

  return InlineCost::getNever(NinlrNotAlwaysInline);
#endif // INTEL_CUSTOMIZATION
}
