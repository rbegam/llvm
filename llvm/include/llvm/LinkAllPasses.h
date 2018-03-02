//===- llvm/LinkAllPasses.h ------------ Reference All Passes ---*- C++ -*-===//
//
//                      The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This header file pulls in all transformation and analysis passes for tools
// like opt and bugpoint that need this functionality.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LINKALLPASSES_H
#define LLVM_LINKALLPASSES_H

#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/AliasAnalysisEvaluator.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/Analysis/BasicAliasAnalysis.h"
#include "llvm/Analysis/CFLAndersAliasAnalysis.h"
#include "llvm/Analysis/CFLSteensAliasAnalysis.h"
#include "llvm/Analysis/CallPrinter.h"
#include "llvm/Analysis/DomPrinter.h"
#include "llvm/Analysis/GlobalsModRef.h"
#include "llvm/Analysis/IntervalPartition.h"
#include "llvm/Analysis/Lint.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/RegionPass.h"
#include "llvm/Analysis/RegionPrinter.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionAliasAnalysis.h"
#include "llvm/Analysis/Intel_Andersens.h"  // INTEL
#include "llvm/Analysis/Intel_DTrans/DTransAnalysis.h" // INTEL
#include "llvm/Analysis/Intel_LoopAnalysis/Passes.h" // INTEL - HIR
#include "llvm/Analysis/Intel_StdContainerAA.h"  // INTEL
#include "llvm/Analysis/Intel_VPO/Vecopt/Passes.h"   // INTEL
#include "llvm/Analysis/Intel_VPO/WRegionInfo/WRegionPasses.h" // INTEL
#include "llvm/Analysis/Intel_XmainOptLevelPass.h" // INTEL
#include "llvm/Analysis/Intel_OptReport/OptReportOptionsPass.h" // INTEL
#include "llvm/Analysis/ScopedNoAliasAA.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/TypeBasedAliasAnalysis.h"
#include "llvm/CodeGen/Passes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/IRPrintingPasses.h"
#include "llvm/Support/Valgrind.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/IPO/AlwaysInliner.h"
#include "llvm/Transforms/IPO/FunctionAttrs.h"
#include "llvm/Transforms/IPO/Intel_InlineLists.h" // INTEL
#include "llvm/Transforms/Instrumentation.h"
#include "llvm/Transforms/Instrumentation/BoundsChecking.h"
#include "llvm/Transforms/ObjCARC.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Scalar/GVN.h"
#include "llvm/Transforms/Utils/SymbolRewriter.h"
#include "llvm/Transforms/Utils/UnifyFunctionExitNodes.h"
#include "llvm/Transforms/Vectorize.h"
#include "llvm/Transforms/Intel_DTrans/DTransOpt.h"              // INTEL
#include "llvm/Transforms/Intel_LoopTransforms/Passes.h"         // INTEL - HIR
#include "llvm/Transforms/Intel_MapIntrinToIml/MapIntrinToIml.h" // INTEL
#include "llvm/Transforms/Intel_VPO/VPOPasses.h"                 // INTEL
#include "llvm/Transforms/Intel_VPO/Vecopt/VecoptPasses.h"       // INTEL
#include "llvm/Transforms/Utils/Intel_VecClone.h"                // INTEL
#include <cstdlib>

namespace {
  struct ForcePassLinking {
    ForcePassLinking() {
      // We must reference the passes in such a way that compilers will not
      // delete it all as dead code, even with whole program optimization,
      // yet is effectively a NO-OP. As the compiler isn't smart enough
      // to know that getenv() never returns -1, this will do the job.
      if (std::getenv("bar") != (char*) -1)
        return;

      (void) llvm::createAAEvalPass();
      (void) llvm::createAggInlAALegacyPass();  // INTEL
      (void) llvm::createAggressiveDCEPass();
      (void) llvm::createBitTrackingDCEPass();
      (void) llvm::createArgumentPromotionPass();
      (void) llvm::createAlignmentFromAssumptionsPass();
#if INTEL_CUSTOMIZATION
      (void) llvm::createAndersensAAWrapperPass();
      (void) llvm::createDTransOptWrapperPass();
      (void) llvm::createDTransAnalysisWrapperPass();
      (void) llvm::createNonLTOGlobalOptimizerPass();
      (void) llvm::createTbaaMDPropagationPass();
      (void) llvm::createCleanupFakeLoadsPass();
      (void) llvm::createStdContainerOptPass();
      (void) llvm::createStdContainerAAWrapperPass();
      (void) llvm::createInlineListsPass();
      (void) llvm::createXmainOptLevelWrapperPass();
      (void) llvm::createOptReportOptionsPass();
      (void) llvm::createLoopOptReportEmitterLegacyPass();
#endif // INTEL_CUSTOMIZATION
      (void) llvm::createBasicAAWrapperPass();
      (void) llvm::createSCEVAAWrapperPass();
      (void) llvm::createTypeBasedAAWrapperPass();
      (void) llvm::createScopedNoAliasAAWrapperPass();
      (void) llvm::createBoundsCheckingLegacyPass();
      (void) llvm::createBreakCriticalEdgesPass();
      (void) llvm::createCallGraphDOTPrinterPass();
      (void) llvm::createCallGraphViewerPass();
      (void) llvm::createCFGSimplificationPass();
      (void) llvm::createCFLAndersAAWrapperPass();
      (void) llvm::createCFLSteensAAWrapperPass();
      (void) llvm::createStructurizeCFGPass();
      (void) llvm::createLibCallsShrinkWrapPass();
      (void) llvm::createCalledValuePropagationPass();
      (void) llvm::createConstantMergePass();
      (void) llvm::createConstantPropagationPass();
      (void) llvm::createCostModelAnalysisPass();
      (void) llvm::createDeadArgEliminationPass();
      (void) llvm::createDeadCodeEliminationPass();
      (void) llvm::createDeadInstEliminationPass();
      (void) llvm::createDeadStoreEliminationPass();
      (void) llvm::createDependenceAnalysisWrapperPass();
      (void) llvm::createDivergenceAnalysisPass();
      (void) llvm::createDomOnlyPrinterPass();
      (void) llvm::createDomPrinterPass();
      (void) llvm::createDomOnlyViewerPass();
      (void) llvm::createDomViewerPass();
      (void) llvm::createGCOVProfilerPass();
      (void) llvm::createPGOInstrumentationGenLegacyPass();
      (void) llvm::createPGOInstrumentationUseLegacyPass();
      (void) llvm::createPGOIndirectCallPromotionLegacyPass();
      (void) llvm::createPGOMemOPSizeOptLegacyPass();
      (void) llvm::createInstrProfilingLegacyPass();
      (void) llvm::createFunctionImportPass();
      (void) llvm::createFunctionInliningPass();
      (void) llvm::createAlwaysInlinerLegacyPass();
      (void) llvm::createGlobalDCEPass();
      (void) llvm::createGlobalOptimizerPass();
      (void) llvm::createGlobalsAAWrapperPass();
      (void) llvm::createGuardWideningPass();
      (void) llvm::createIPConstantPropagationPass();
      (void) llvm::createIPSCCPPass();
      (void) llvm::createIndirectCallConvPass(); // INTEL
      (void) llvm::createInductiveRangeCheckEliminationPass();
      (void) llvm::createIndVarSimplifyPass();
      (void) llvm::createInstructionCombiningPass();
      (void) llvm::createInternalizePass();
      (void) llvm::createLCSSAPass();
      (void) llvm::createLICMPass();
      (void) llvm::createLoopSinkPass();
      (void) llvm::createLazyValueInfoPass();
      (void) llvm::createLoopExtractorPass();
      (void) llvm::createLoopInterchangePass();
      (void) llvm::createLoopPredicationPass();
      (void) llvm::createLoopSimplifyPass();
      (void) llvm::createLoopSimplifyCFGPass();
      (void) llvm::createLoopStrengthReducePass();
      (void) llvm::createLoopRerollPass();
      (void) llvm::createLoopUnrollPass();
      (void) llvm::createLoopUnswitchPass();
      (void) llvm::createLoopVersioningLICMPass();
      (void) llvm::createLoopIdiomPass();
      (void) llvm::createLoopRotatePass();
      (void) llvm::createLowerExpectIntrinsicPass();
      (void) llvm::createLowerInvokePass();
      (void) llvm::createLowerSwitchPass();
      (void) llvm::createNaryReassociatePass();
      (void) llvm::createObjCARCAAWrapperPass();
      (void) llvm::createObjCARCAPElimPass();
      (void) llvm::createObjCARCExpandPass();
      (void) llvm::createObjCARCContractPass();
      (void) llvm::createObjCARCOptPass();
      (void) llvm::createPAEvalPass();
      (void) llvm::createPromoteMemoryToRegisterPass();
      (void) llvm::createDemoteRegisterToMemoryPass();
      (void) llvm::createPruneEHPass();
      (void) llvm::createPostDomOnlyPrinterPass();
      (void) llvm::createPostDomPrinterPass();
      (void) llvm::createPostDomOnlyViewerPass();
      (void) llvm::createPostDomViewerPass();
      (void) llvm::createReassociatePass();
      (void) llvm::createRegionInfoPass();
      (void) llvm::createRegionOnlyPrinterPass();
      (void) llvm::createRegionOnlyViewerPass();
      (void) llvm::createRegionPrinterPass();
      (void) llvm::createRegionViewerPass();
      (void) llvm::createSCCPPass();
      (void) llvm::createSafeStackPass();
      (void) llvm::createSROAPass();
      (void) llvm::createSingleLoopExtractorPass();
      (void) llvm::createStripSymbolsPass();
      (void) llvm::createStripNonDebugSymbolsPass();
      (void) llvm::createStripDeadDebugInfoPass();
      (void) llvm::createStripDeadPrototypesPass();
      (void) llvm::createTailCallEliminationPass();
      (void) llvm::createJumpThreadingPass();
      (void) llvm::createUnifyFunctionExitNodesPass();
      (void) llvm::createInstCountPass();
      (void) llvm::createConstantHoistingPass();
      (void) llvm::createCodeGenPreparePass();
      (void) llvm::createEntryExitInstrumenterPass();
      (void) llvm::createPostInlineEntryExitInstrumenterPass();
      (void) llvm::createEarlyCSEPass();
      (void) llvm::createGVNHoistPass();
      (void) llvm::createMergedLoadStoreMotionPass();
      (void) llvm::createGVNPass();
      (void) llvm::createNewGVNPass();
      (void) llvm::createMemCpyOptPass();
      (void) llvm::createLoopDeletionPass();
      (void) llvm::createPostDomTree();
      (void) llvm::createInstructionNamerPass();
      (void) llvm::createMetaRenamerPass();
      (void) llvm::createPostOrderFunctionAttrsLegacyPass();
      (void) llvm::createReversePostOrderFunctionAttrsPass();
      (void) llvm::createMergeFunctionsPass();
      (void) llvm::createMergeICmpsPass();
      (void) llvm::createExpandMemCmpPass();
      std::string buf;
      llvm::raw_string_ostream os(buf);
      (void) llvm::createPrintModulePass(os);
      (void) llvm::createPrintFunctionPass(os);
      (void) llvm::createPrintBasicBlockPass(os);
      (void) llvm::createModuleDebugInfoPrinterPass();
      (void) llvm::createPartialInliningPass();
      (void) llvm::createLintPass();
      (void) llvm::createSinkingPass();
      (void) llvm::createLowerAtomicPass();
      (void) llvm::createCorrelatedValuePropagationPass();
      (void) llvm::createMemDepPrinter();
      (void) llvm::createInstructionSimplifierPass();
      (void) llvm::createLoopVectorizePass();
      (void) llvm::createSLPVectorizerPass();
      (void) llvm::createLoadStoreVectorizerPass();
#if INTEL_CUSTOMIZATION
      (void) llvm::createVPlanDriverPass();
      (void) llvm::createVPlanDriverHIRPass();
#endif
      (void) llvm::createPartiallyInlineLibCallsPass();
      (void) llvm::createScalarizerPass();
      (void) llvm::createSeparateConstOffsetFromGEPPass();
      (void) llvm::createSpeculativeExecutionPass();
      (void) llvm::createSpeculativeExecutionIfHasBranchDivergencePass();
      (void) llvm::createRewriteSymbolsPass();
      (void) llvm::createStraightLineStrengthReducePass();
      (void) llvm::createMemDerefPrinter();
      (void) llvm::createFloat2IntPass();
      (void) llvm::createEliminateAvailableExternallyPass();
      (void) llvm::createScalarizeMaskedMemIntrinPass();

      (void)new llvm::IntervalPartition();
      (void)new llvm::ScalarEvolutionWrapperPass();
      llvm::Function::Create(nullptr, llvm::GlobalValue::ExternalLinkage)->viewCFGOnly();
      llvm::RGPassManager RGM;
      llvm::TargetLibraryInfoImpl TLII;
      llvm::TargetLibraryInfo TLI(TLII);
      llvm::AliasAnalysis AA(TLI);
      llvm::AliasSetTracker X(AA);
      X.add(nullptr, 0, llvm::AAMDNodes()); // for -print-alias-sets
      (void) llvm::AreStatisticsEnabled();
      (void) llvm::sys::RunningOnValgrind();

  #if INTEL_CUSTOMIZATION
      (void) llvm::createSNodeAnalysisPass();
      (void) llvm::createLoopOptMarkerLegacyPass();
      // HIR passes
      (void) llvm::createHIRRegionIdentificationWrapperPass();
      (void) llvm::createHIRSCCFormationWrapperPass();
      (void) llvm::createHIRFrameworkWrapperPass();
      (void) llvm::createHIROptReportEmitterWrapperPass();
      (void) llvm::createHIRDDAnalysisPass();
      (void) llvm::createHIRLocalityAnalysisPass();
      (void) llvm::createHIRLoopResourcePass();
      (void) llvm::createHIRLoopStatisticsPass();
      (void) llvm::createHIRParVecAnalysisPass();
      (void) llvm::createHIRVectVLSAnalysisPass();
      (void) llvm::createHIRSafeReductionAnalysisPass();
      (void) llvm::createHIRSSADeconstructionLegacyPass();
      (void) llvm::createHIRTempCleanupPass();
      (void) llvm::createHIRLoopInterchangePass();
      (void) llvm::createHIROptPredicatePass();
      (void) llvm::createHIROptVarPredicatePass();
      (void) llvm::createHIRGeneralUnrollPass();
      (void) llvm::createHIRUnrollAndJamPass();
      (void) llvm::createHIRPreVecCompleteUnrollPass();
      (void) llvm::createHIRPostVecCompleteUnrollPass();
      (void) llvm::createHIRParDirInsertPass();
      (void) llvm::createHIRVecDirInsertPass();
      (void) llvm::createHIRLoopDistributionForMemRecPass();
      (void) llvm::createHIRLoopDistributionForLoopNestPass();
      (void) llvm::createHIRLoopReversalPass();
      (void) llvm::createHIRLMMPass();
      (void) llvm::createHIRSymbolicTripCountCompleteUnrollPass();
      (void) llvm::createHIRScalarReplArrayPass();
      (void) llvm::createHIRIdiomRecognitionPass();
      (void) llvm::createHIRMVForConstUBPass();
      (void) llvm::createHIRLoopConcatenationPass();
      (void) llvm::createHIRArrayTransposePass();
      (void) llvm::createHIRLoopFusionPass();
      (void) llvm::createHIRDummyTransformationPass();
      (void) llvm::createHIRCodeGenWrapperPass();

      // Optimize math calls
      (void) llvm::createMapIntrinToImlPass();

      // VPO WRegion Passes
      (void) llvm::createWRegionCollectionPass();
      (void) llvm::createWRegionInfoPass();

      // VPO Vectorizer Passes
      (void) llvm::createAVRGeneratePass();
      (void) llvm::createAVRGenerateHIRPass();
      (void) llvm::createVPOPredicatorPass();
      (void) llvm::createVPOPredicatorHIRPass();
      (void) llvm::createVPODriverPass();
      (void) llvm::createVPODriverHIRPass();
      (void) llvm::createVPODirectiveCleanupPass();
      (void) llvm::createVecClonePass();
      (void) llvm::createAvrDefUsePass();
      (void) llvm::createAvrDefUseHIRPass();
      (void) llvm::createAvrCFGPass();
      (void) llvm::createAvrCFGHIRPass();
      (void) llvm::createSIMDLaneEvolutionPass();
      (void) llvm::createSIMDLaneEvolutionHIRPass();
      (void) llvm::createVectorGraphInfoPass();
      (void) llvm::createVectorGraphPredicatorPass();
      (void) llvm::createAVRDecomposeHIRPass();

      // VPO Paropt Prepare Passes
      (void) llvm::createVPOParoptPreparePass();

      // VPO Parallelizer Passes
      (void) llvm::createVPOParoptPass();

      // VPO Thread Private Transformation
      (void) llvm::createVPOParoptTpvPass();
  #endif // INTEL_CUSTOMIZATION
    }
  } ForcePassLinking; // Force link by creating a global definition.
}

#endif
