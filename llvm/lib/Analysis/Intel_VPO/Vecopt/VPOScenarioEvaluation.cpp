//===-- VPOScenarioEvaluation.cpp -----------------------------------------===//
//
//   Copyright (C) 2015-2016 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements the VPO vectorizer engine that drives the exploration
// of different vectorization alternatives.
//
//===----------------------------------------------------------------------===//

#include <algorithm>
#include "llvm/Analysis/Intel_VPO/Vecopt/VPOScenarioEvaluation.h"
#include "llvm/Analysis/Intel_VPO/Vecopt/VPOPredicator.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRAnalysisPass.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRVLSClient.h"
#include "llvm/Analysis/Intel_VPO/Vecopt/VPOAvrVisitor.h"
#include "llvm/Analysis/Intel_VPO/Vecopt/VPOAvrStmt.h"

#define DEBUG_TYPE "VPOScenarioEvaluation"

static cl::opt<unsigned> DefaultVF("default-vpo-vf", cl::init(0),
                                   cl::desc("Default vector length"));
static cl::opt<unsigned> EnableVectVLS("enable-vect-vls", cl::init(0),
                             cl::desc("Enable VLS group analysis by default"));
static cl::opt<bool> EnableCastCost("enable-cast-cost", cl::init(false),
     cl::desc("Enable TTI cost analysis for cast instructions zext/sext"));

static cl::opt<float> TweakVPOCostFactor("tweak-vpo-cost-factor", cl::init(0.0),
    cl::Hidden, cl::desc("For VF > 1, multiply calculated cost by this factor"));

using namespace llvm;
using namespace llvm::vpo;
using namespace llvm::loopopt;

#undef USE_EXPERIMENTAL_CODE
//#define USE_EXPERIMENTAL_CODE 1

// Given a region that contains loops/loop-nest, decide which of the loops
// (or combinations of loops) to vectorize, and how.
// Currently, a limited version is implemented:
// Given a region that contains a single innermost loop, consider different
// Vectorization facotrs (VFs) for vectorizing it.
//
// Current initial flow:
//
// BestCost = scalarCost;
// BestCand = nullptr (scalar version);
//
// foreach Aloop in Wrn {                       //currently only one Aloop
//   |[DataDepInfo] = getDataDepInfo(Aloop);
//   |[memrefs] = gatherMemrefsInLoop(Aloop);
//   |VF-Candidates = getPossibleVFs(Aloop);
//   |
//   |foreach VF in VF-Candidates {
//   |  |  VectContextBase VC = setVecContext(Aloop,VF);
//   |  |
//   |  |  VLSInfo = getVLSInfoForCandidate([VC],[DataDepInfo],[memrefs]); (*)
//   |  |  MemAccessInfo = VLSInfo->analyzeVLSMemrefsInLoop();             (*)
//   |  |  Grps = VLSInfo->analyzeVLSGroupsInLoop(MemAccessInfo);          (*)
//   |  |
//   |  |  int Cost = CM.getCost(VC,Grps);
//   |  |  If (Cost < BestCost){
//   |  |    BestCost <-- Cost
//   |  |    BestCand <-- VC;
//   |  |  }
//   |  +}
//   +}
//
// (*) TODO: Compute VLS groups and Memory accesses conservatively in a VF
// agnostic way, and then refine per VF if needed (ideally most processing
// happens at loop level, and is insensitive to the actual VF).
//
// CHECKME: A different flow for different optimization levels?
//
// Implementation stages (for innermost loop vectorization):
// Step0 [done]: Incorporate the VLS Memref analysis and VLS Group analysis
//        into the vectorizer.
//        "evaluate" a single given/default VF candidate (dummy evaluation);
//        No AVR changes.
//        No Compile-time considerations.
//        No changes in the behavior of the vectorizer.
// Step1: [done] Really evaluate a single given/default VF candidate (via TTI costs);
//        Still no AVR changes.
//        Still no Compile-time considerations.
//        VLSGroups ignored in cost analysis.
//        Given the default VF, the Vectorizer may now decide not to vectorize.
// Step2: [current] Evaluate several VF candidates;
//        Still no Compile-time considerations.
//        Still no AVR changes.
// Step3: Fill remaining holes in the cost evaluation
// Step4: Take into account VLSGroups in cost evaluation.
// Step5: Prepare mechanism to allow changing the AVR.
// Step6: Incorporate passes that may change the AVR.
// Step7: Refine the skeleton: Optimize (minimize) processing across VF
//        candidates (for Compile time).
// Step7: ...
VPOVecContextBase VPOScenarioEvaluationBase::getBestCandidate(AVRWrn *AWrn) {

  // Set the default VF according to any directives or user compiler switches,
  // or otherwise set it to 1.
  ForceVF = AWrn->getSimdVectorLength();
  LLVM_DEBUG(errs() << "VF = " << ForceVF << " DefaultVF = " << DefaultVF
                    << "\n");
  if (ForceVF == 0) {
    ForceVF = DefaultVF;
  }
  int initialVF = ForceVF ? ForceVF : 1;
  // LLVM_DEBUG(errs() << "Set dummy vectCand with VF = " << initialVF << "\n");
  VPOVecContextBase VectCand(initialVF);

#if 1
  // FORNOW: An AVRWrn node is expected to have only one AVRLoop child
  AVRLoop *AvrLoop = AVRUtils::findAVRLoop(AWrn);

  // Check that we have an AVRLoop
  if (!AvrLoop)
    return VectCand;
#endif

  // Loop over search space of candidates within AWrn. In the future this will
  // examine all candidate ALoops (and combinations thereof) within the AWrn.
  // FORNOW: we expect to encounter only a single ALoop in AWrn.
  // So FORNOW: A region has a single innermost loop, and therefore a single
  // scenario: Scenario == a single AvrLoop considered for vectoriztion

  // TODO: CostOfBestScenario = 0;
  // TODO: foreach Scenario:
  {
    // TODO: ScenarioCost = 0;
    // TODO: foreach AVRLoop candidate in the sceanrio:
    {
      setALoop(AvrLoop);

      // VectCand represents the best way to vectorize this loop, including
      // compared to leaving it scalar (in which case VectCand in null);

      // Check if widening stage supports this loop. If ForceVF is zero, 
      // isLoopHandled will not check if a remainder loop is needed.
      // FORNOW: If this loop is not supported, return a dummy VC; In the 
      // future we should continue to next candidate loop in region.
      if (!loopIsHandled(ForceVF, AvrLoop))
        return VectCand;

      // Decomposition of AVRValueHIRs happens here
      prepareLoop(AvrLoop);

      uint64_t BestCostForALoop = 0;
      // Get the scalar cost for this loop.
      // (No need to compute the cost if ForceVF is set to a VF forced by the 
      // user).
      //
      // FORNOW: Calculate cost only for the candidate AvrLoop. We assume that
      // the rest of the code in the region will remain the same between the
      // scalar and vector versions.
      if (ForceVF == 0) {
        HIRSafeReductionAnalysis *SRA = nullptr;
        if (VPOScenarioEvaluationHIR *HIRSE =
            dyn_cast<VPOScenarioEvaluationHIR>(this)) {
          SRA = HIRSE->getSRA();
        }
        BestCostForALoop = getCM()->getCost(ALoop, 1, nullptr, SRA);
      }
      VectCand = processLoop(AvrLoop, &BestCostForALoop);

      // TODO: Cache the best result so far for this loop (best VF, and
      // corresponding best cost), add it to the overall cost for this scenario,
      // and move on to the next Loop in the region for this scenario.
      // ScenarioCost += BestCostForALoop;

    } // End iterating over all loops in the scenario
    // TODO: Keep track of best scenario so far.
    // CostOfBestScenario = min(CostOfBestScenario, ScenarioCost);

  } // End iterating over all scenarios for the region

  // Currently VectCands represents a single loop (the only loop in the
  // scenario). If best thing for this loop is to leave it scalar, VectCand is
  // nullptr. In the future this may be coded directly in the AVR, for all the
  // loops of this region, according to the best scenario.
  return VectCand;
}

void VPOScenarioEvaluationBase::findVFCandidates(VFsVector &VFCandidates) {

  unsigned int MinVF, MaxVF;
  unsigned VecRegWidth;

  if (ForceVF == 0) {

    // The user has not explicitly requested a specific vector length, so use
    // the maximum vector register width of the target and the data types used
    // in the loop to select an appropriate vector length. The VF candidate
    // ranges will be between the VFs based on the smallest and largest types
    // found in the loop. Type frequency information can be used later to weight
    // the VF candidates.

    unsigned SmallestTySize = 0;
    unsigned LargestTySize = 0;
    for (int I = 0; I < NUM_TYPE_SIZE; I++) {
      LoopTypeSizes[I] = 0;
    }

    VecRegWidth = TTI.getRegisterBitWidth(true);
    VPOScenarioEvaluationBase *VSE =
      const_cast<VPOScenarioEvaluationBase*>(this);
    AVRVisitor<VPOScenarioEvaluationBase> AVisitor(*VSE);
    // No need to recurse inside values. See comments in the visit function.
    AVisitor.visit(ALoop, true, true, false /*RecursiveInsideValues*/, true);

    int Idx = I8_TYPE_SIZE;
    while (Idx < NUM_TYPE_SIZE) {
      if (LoopTypeSizes[Idx] > 0) {
        SmallestTySize = (0x1 << Idx) & 0xFF;
        break;
      }
      Idx++;
    }

    Idx = NUM_TYPE_SIZE - 1;
    while (Idx >= I8_TYPE_SIZE) {
      if (LoopTypeSizes[Idx] > 0) {
        LargestTySize = (0x1 << Idx) & 0xFF;
        break;
      }
      Idx--;
    }
    MinVF = VecRegWidth / LargestTySize;
    MaxVF = VecRegWidth / SmallestTySize;

    LLVM_DEBUG(errs() << "Type Frequencies: \n");
    LLVM_DEBUG(errs() << " i1 Types: " << LoopTypeSizes[I1_TYPE_SIZE] << "\n");
    LLVM_DEBUG(errs() << " i8 Types: " << LoopTypeSizes[I8_TYPE_SIZE] << "\n");
    LLVM_DEBUG(errs() << "i16 Types: " << LoopTypeSizes[I16_TYPE_SIZE] << "\n");
    LLVM_DEBUG(errs() << "i32 Types: " << LoopTypeSizes[I32_TYPE_SIZE] << "\n");
    LLVM_DEBUG(errs() << "i64 Types: " << LoopTypeSizes[I64_TYPE_SIZE] << "\n");
    LLVM_DEBUG(errs() << "Target register width: " << VecRegWidth << "\n");
  } else {
    MinVF = ForceVF;
    MaxVF = ForceVF;
  }

  std::string MinVFStr = APInt(32, MinVF).toString(10, false);
  std::string MaxVFStr = APInt(32, MaxVF).toString(10, false);
  StringRef VFRange = MinVF == MaxVF ? MinVFStr :
                                       MinVFStr + " - " + MaxVFStr;
  LLVM_DEBUG(errs() << "VF Candidates are: " << VFRange << "\n");
  (void) VFRange;
  assert(MinVF && MaxVF && "Unexpected zero min/max VF");

  for (unsigned int VF = MinVF; VF <= MaxVF; VF *= 2) {
    VFCandidates.push_back(VF);
  }
}

VPOVecContextBase
VPOScenarioEvaluationBase::processLoop(AVRLoop *ALoop,
                                       uint64_t *BestCostForALoop) {
  LLVM_DEBUG(errs() << "Process Loop\n");

  // Place holder for loop-level, VF-agnostic passes.
  //
  setLoop(ALoop);

  // Obtain data-dependence information and gather memory references.
  //
  // CHECKME: Currently the results of these analyses are kept under the covers
  // at the level of the derived implementation. We may prefer passing them
  // here explicitly. However this will require introducing base-level
  // abstractions to be passed around instead of holding on to the derived-level
  // data-structured already at hand. To be revisited.
  getDataDepInfoForLoop();
  gatherMemrefsInLoop();

#ifdef USE_EXPERIMENTAL_CODE
  // TODO: adapt to being called on a loop.
  getSLEVUtil().runOnAvr(AWrn->child_begin(), AWrn->child_end());
  LLVM_DEBUG(formatted_raw_ostream FOS(dbgs()); FOS << "After SLEV analysis:\n";
             AWrn->print(FOS, 1, PrintNumber));

#endif
  VPOPredicator Predicator;
  Predicator.runOnAvr(VPOScenarioEvaluationBase::ALoop);

  // Identify VF candidates
  //
  VFsVector VFCandidates;
  findVFCandidates(VFCandidates);

  // Evaluate each VF candidate
  //
  VPOVecContextBase BestCand(1);
  for (auto &VF : VFCandidates) {

    LLVM_DEBUG(errs() << "Evaluate candidate with VF = " << VF << "\n");

    // Currently VecContext is used to hold underlying-IR level information
    // required for some of the analyses in processCandidates (namely, for VLS
    // grouping).
    VPOVecContextBase VC = setVecContext(VF);
    uint64_t Cost = processCandidate(ALoop, VF, VC);

    if (Cost < *BestCostForALoop || VF == ForceVF) {
      *BestCostForALoop = Cost;
      BestCand = VC;
      LLVM_DEBUG(errs() << "New Best Candidate Cost = " << *BestCostForALoop
                        << " for VF = " << VF << " \n");
    }
  }

  // Clear data-structures for this loop
  resetLoopInfo();

  // If best candidate is the original scalar one, nullptr is returned.
  return BestCand;
}

// TODO: Ideally we have very few VF sensitive adjusments to make.
// ProcessCandidate will be as much as possible just a getCost call.
uint64_t VPOScenarioEvaluationBase::processCandidate(AVRLoop *ALoop,
                                                     unsigned int VF,
                                                     VPOVecContextBase &VC) {

  // Place holder for VF-specific passes.
  //

  // Memrefs Analysis:
  //
  // Analyze the access patterns of the Loop memory-references.
  // Returns an interface that can answer questions about memrefs, such as
  // their stride, and distance from one another, relative to a given context
  // (that includes the loop, DataDepsInfo, and the VF; these are
  // provided under the covers when we set up the VLSInfo below).
  //
  // TODO: VLS grouping is currently the only user of the results of
  // Memrefs analysis. Idiom recognition, CostModel, and CodeGen should also
  // use thes results of Memref Analysis.
  //
  // TODO: Memrefs analysis is largely VF-independent. In some cases SLEV
  // analysis can refine memref information for specific VFs. Move this to be
  // processed once per loop and refine per VF only if necessary.
  // In any case, no need to invalidate and recompute all memory-access
  // information from cratch for each candidate.
  //
  // TODO: An AVRLoop may contain very many memrefs, many of which cannot be
  // grouped together. In order to avoid redundant queries, better break the
  // memrefs into subsets of memrefs that can safely be grouped together.
  //
  // FIXME?: Under the covers this is dependent on the memrefs having been
  // gathered (at the underlying IR level); May want to expose this explicitly
  // here. However this will require introducing a base-level abstraction for
  // the memrefs. Revisit this based on how we want to handle memrefs in VPO in
  // general.
  VPOVLSInfoBase *VLSInfo = getVLSInfoForCandidate();
  // Analyze the Loop Memrefs; Produce a mapping from each memory access to 
  // the respective OVLSMemref object that contains the information and 
  // utilities required for VLS group analysis. The resulting map resides in 
  // the VLSInfo object.
  OVLSMemrefVector VLSMrfs;
  VLSInfo->analyzeVLSMemrefsInLoop(VLSMrfs);

  // VLS-Groups Analysis:
  //
  // Find groups of neighboring memory-references to be used by the cost model.
  //
  // FIXME?: under the convers VLS is dependent on the DDG (and the Vector
  // Context). May want to expose this explicitly here (passing around
  // base-class objects) rather than keeping the derived-class objects
  // internally.
  //
  // Produce a mapping from each memory access to the respective VLS Group
  // it belongs to. The resulting map from memrefs to groups resides in 
  // the VLSInfo object.
  OVLSGroupVector VLSGrps;
  VLSInfo->analyzeVLSGroupsInLoop(VLSMrfs, VLSGrps);
#if 0 // Testing
  // Do something with the grps.
  OVLSTTICostModelHIR VLSCM(TTI, LLVMCntxt); 
  for (OVLSGroup *Grp : VLSGrps) {
    int64_t Cost = OptVLSInterface::getGroupCost(*Grp, VLSCM); 
    LLVM_DEBUG(errs() << "Cost for Group = " << Cost << "\n");
  }
#endif

  // Calculate the cost of the current candidate
  // (No need to calculate cost if the user forced a specific VF).
  //
  uint64_t Cost = 0; 
  if (ForceVF == 0) { 
    HIRSafeReductionAnalysis *SRA = nullptr;
    if (VPOScenarioEvaluationHIR *HIRSE =
        dyn_cast<VPOScenarioEvaluationHIR>(this)) {
      SRA = HIRSE->getSRA();
    }
    Cost = getCM()->getCost(ALoop, VF, VLSInfo, SRA);
  } 

  // Cleaups.
  // FORNOW: Release the groups and memrefs.  TODO (save compile time): Keep
  // around the Mrfs and Grps of the best candidate. Also can keep the Mrfs
  // and Grps across different Candidates, as they are usually/largely not
  // invalidated by the changing VF.
  for (OVLSMemref *Memref : VLSMrfs) {
    delete Memref;
  }
  VLSMrfs.clear();
  for (OVLSGroup *Grp : VLSGrps) {
    delete Grp;
  }
  VLSGrps.clear();
  delete VLSInfo;

  return Cost;
}

/// A helper function for converting Scalar types to vector types. If the
/// incoming type is void or metadata, we return the same. If the VF is 1,
/// we return the scalar type (copied from LoopVectorize.cpp).
static Type *ToVectorTy(Type *Scalar, unsigned VF) {
  if (Scalar->isVoidTy() || Scalar->isMetadataTy() || VF == 1)
    return Scalar;
  return VectorType::get(Scalar, VF);
}

/// Check if this  pointer is consecutive (under the current scenario) and
/// hence can be vectorized into a wide load/store).
/// Returns:
/// 0 - Stride is unknown or non-consecutive.
/// 1 - Address is consecutive.
/// -1 - Address is consecutive, and decreasing.
/// (Same behavior as LoopVectorize::isConsecutivePtr).
int64_t getConsecutiveStride(AVR *PtrOp) {
  assert((isa<AVRValue>(PtrOp) || isa<AVRExpression>(PtrOp)) &&
         "Unexpected AVR node");

  // TODO: Move type to AVR. This pattern is very common
  if (AVRValue *ValOp = dyn_cast<AVRValue>(PtrOp)) {
    (void)ValOp;
    assert(ValOp->getType()->isPointerTy() && "Unexpected non-ptr");
  } else if (AVRExpression *ExprOp = dyn_cast<AVRExpression>(PtrOp)) {
    (void)ExprOp;
    assert(ExprOp->getType()->isPointerTy() && "Unexpected non-ptr");
  }

#ifdef USE_EXPERIMENTAL_CODE
  // TODO: Use instead IsPointerConsecutive, once available
  // TODO: SLEV's isConsecutive is currently true only for stride=1; Consider
  // covering also the stride == -1 case in SLEV's isConsecutive.
  if (PtrOp->getSLEV().isConsecutive())
    return 1;
  if (!PtrOp->getSLEV().isStrided())
    return 0;
  // At this point we know the pointer is Strided, and non Consecutive
  // (i.e. Stride != 1); Remains to check if Stride == -1
  int64_t StrideInElements =
      PtrOp->getSLEV().getStride().getInteger().getSExtValue();
  if (StrideInElements == -1)
    return -1;
  return 0;
#else
  // Optimistic... (just so we don't break current LLVMIR tests)
  return 1;
#endif
}

// CostModel Visit routine.
// FORNOW: Only Assigns are supported.
// TODO: Support Reductions, Inductions, Calls, Select, Compare, Branch, Phi. 
// TODO: Move to a different module.
void VPOCostGathererBase::visit(AVRLoop *Loop) {
  //  LLVM_DEBUG(errs() << "visiting loop!\n");
}

// TODO: If this is an inner-loop inside the ALoop being vectorized: multiply
// by the iteration-count
void VPOCostGathererBase::postVisit(AVRLoop *Loop) {
  // LLVM_DEBUG(errs() << "visited loop!\n");
}

void VPOCostGathererBase::visit(AVRAssign *Assign) {
  // LLVM_DEBUG(errs() << "visiting assign!\n");
}

// TODO: Take blend cost into account if masked.
void VPOCostGathererBase::postVisit(AVRAssign *Assign) {
  // LLVM_DEBUG(errs() << "visited assign!\n");
}

// Following will soon move under handling of Expr
#if 1
void VPOCostGathererBase::visit(AVRLabel *Label) {
  // LLVM_DEBUG(errs() << "visit Label!\n");
}

void VPOCostGathererBase::visit(AVRCall *Call) {
  LLVM_DEBUG(errs() << "TODO: visit Call!\n");
}

// CHECKME: Account for reduction cost here?
void VPOCostGathererBase::visit(AVRPhi *Phi) {
  LLVM_DEBUG(errs() << "TODO: visit Phi!\n");
}

void VPOCostGathererBase::visit(AVRBranch *Branch) {
  LLVM_DEBUG(errs() << "TODO: visit Branch!\n");
}

void VPOCostGathererBase::visit(AVRCompare *Compare) {
  LLVM_DEBUG(errs() << "TODO: visit Compare!\n");
}

void VPOCostGathererBase::visit(AVRIf *If) {
  // LLVM_DEBUG(errs() << "TODO: visit If!\n");
}

void VPOCostGathererBase::visit(AVRSelect *Select) {
  LLVM_DEBUG(errs() << "TODO: visit Select!\n");
}

void VPOCostGathererBase::visit(AVRPredicate *Predicate) {

  // (1) LOOP( IV )
  // {
  //   (24) PREDICATE {P24 := }
  //   (2) if /P24/ ((3)EXPR{i1 (4)VALUE{float* (%b)[i1]} fcmp/oeq
  //                            (5)VALUE{float 1.000000e+00}})   {
  //   }
  //
  //   (25) PREDICATE {P25 :=
  //       (24) && (29)EXPR{i1 (27)VALUE{i1 &(3)} icmp/eq (28)VALUE{i1 true}}}
  //
  //   (6) ASSIGN{/P25/ (9)EXPR{float (10)VALUE{float %conv}} =
  //       (11)EXPR{float uitofp (12)VALUE{i32 i1}}}
  //
  //   (7) ASSIGN{/P25/ (13)EXPR{float (14)VALUE{float %call}} =
  //       (15)EXPR{float call (16)VALUE{float %conv}}}
  //
  //   (8) ASSIGN{/P25/ (17)EXPR{float (18)VALUE{float* (%varray)[i1]}} =
  //       (19)EXPR{float store (20)VALUE{float %call}}}
  //
  // }
  //
  // This function handles AVRPredicate AVRs (designated above as "PREDICATE").
  // The cost of each node corresponds to computing a new mask based on the
  // conjunction of all incoming predicates and this node. E.g., for predicate
  // P25, the cost is the "and" operation of itself and P24. The AVRExpression
  // visitor will separately analyze the cost for computing the initial mask
  // that is part of the AVRIf on STMT(2).

  const SmallVectorImpl<AVRPredicate::IncomingTy> &IncomingPreds =
    Predicate->getIncoming();
  // IncomingPreds.size() is expected to be > 0 due to the existence of a
  // "VOID" predicate (see P24 above). i.e., it is assumed that the form
  // of a predicate node will be something like: Pn := (Pn-1) && cond. It
  // is possible in the future that we will need to deal with a predicate
  // node which is just an assignment. In that case, we'll have to adjust
  // with how cost is computed for predicate nodes where IncomingPreds.size()
  // is 0.
  unsigned Cost = IncomingPreds.size();
  LoopBodyCost += Cost;
  Predicate->setCost(Cost);
}

// Of the following, only (uniform) AVRIf should survive after predication.
bool VPOCostGathererBase::skipRecursion (AVR *ANode) { 
  if (isa<AVRSelect>(ANode) || isa<AVRSwitch>(ANode) ||
      isa<AVRPredicate>(ANode))
    return true;
  return false;
}

void VPOCostGathererBase::visit(AVR *ANode) {
  LLVM_DEBUG(errs() << "VPOCostModel: Unsupported AVR kind\n");
  LLVM_DEBUG(ANode->dump(PrintBase));
  //llvm_unreachable("unsupported AVR kind");
}
#endif

void VPOCostGathererBase::postVisit(AVRExpression *Expr) {
  // LLVM_DEBUG(errs() << "visiting expr!\n");
  if (Expr->isLHSExpr()) {
    // We assume that there is no operation here, just a Value
    return;
  }
}

void VPOCostGathererBase::calculateReductionCost(
  AVRExpression *Expr,
  TargetTransformInfo::OperandValueKind Op1VK,
  TargetTransformInfo::OperandValueKind Op2VK,
  TargetTransformInfo::OperandValueProperties Op1VP,
  TargetTransformInfo::OperandValueProperties Op2VP) {

  // Account for additional shuffle overhead introduced outside of loop for
  // reduction live-out values. Namely, we incur two additional shuffles and
  // one reduction operation for each half of VF down to 1.

  AVRLoopHIR *ALoopHIR = dyn_cast<AVRLoopHIR>(ALoop);

  // Reduction cost modeling assumes simple 3 address code format, w/o
  // nested expressions from decomposition. i.e., the parent of this
  // incoming expression Expr is an assignment and not another expression.
  HLInst *ParentInst = nullptr;
  if (Expr->getParent()) {
    AVRAssignHIR *AssignStmt = dyn_cast<AVRAssignHIR>(Expr->getParent());
    if (AssignStmt) {
      ParentInst = AssignStmt->getHIRInstruction();
    }
  }

  if (VF > 1 && ALoopHIR && ParentInst) {
    HLLoop *HIRLoop = ALoopHIR->getLoop();
    for (unsigned i = 0; i < Expr->getNumOperands(); i++) {
      if (AVRValueHIR *HIRVal = dyn_cast<AVRValueHIR>(Expr->getOperand(i))) {
        if (getSRA() && HIRVal->getValue()) {
          if (getSRA()->isSafeReduction(ParentInst) &&
              HIRLoop->isLiveOut(HIRVal->getValue()->getSymbase())) {

            for (unsigned RedVF = VF / 2; RedVF > 1; RedVF /= 2) {
              VectorType *ShuffleTy = VectorType::get(HIRVal->getType(),
                                                      RedVF);
              ReductionCost +=
                TTI.getShuffleCost(TargetTransformInfo::SK_ExtractSubvector,
                                   ShuffleTy, 0);
              ReductionCost +=
                TTI.getShuffleCost(TargetTransformInfo::SK_ExtractSubvector,
                                   ShuffleTy, RedVF);
              ReductionCost +=
                TTI.getArithmeticInstrCost(Expr->getOperation(), ShuffleTy,
                                           Op1VK, Op2VK, Op1VP, Op2VP);
            }
          }
        }
      }
    }
  }
}

void VPOCostGathererBase::visit(AVRExpression *Expr) {
  unsigned Cost = 0;
  Type *VectorTy;
#ifdef USE_EXPERIMENTAL_CODE
  if (Expr->getSLEV().isUniform())
    VectorTy = Expr->getType();
  else
    VectorTy = ToVectorTy(Expr->getType(), VF);
#else
  VectorTy = ToVectorTy(Expr->getType(), VF);
#endif

  // LLVM_DEBUG(errs() << "visit expr: \n");
  // LLVM_DEBUG(Expr->dump(PrintDataType));
  // LLVM_DEBUG(errs() << "\n");

  if (Expr->isLHSExpr()) {
    // FORNOW: Not contributing any cost
    // TODO: What about costly address computations on HIR side?
    // LLVM_DEBUG(errs() << "visited expr: LHS: no cost contributed!\n");
    return;
  }

  switch (Expr->getOperation()) {
  case Instruction::GetElementPtr: {
    // LLVM_DEBUG(errs() << "Query cost of getElementPtr\n");
    // "We mark this instruction as zero-cost because the cost of GEPs in
    // vectorized code depends on whether the corresponding memory instruction
    // is scalarized or not. Therefore, we handle GEPs with the memory
    // instruction cost."
    Cost = 0;
    break;
  }

// Currently these are separate AVRNodes, not yet an Expr under AVRAssign,
// so following code does not yet get exercised.
#if 1
  case Instruction::Br: {
    LLVM_DEBUG(errs() << "Query cost of branch\n");
    Cost = TTI.getCFInstrCost(Expr->getOperation());
    break;
  }

  // CHECKME: account for reduction cost here?
  case Instruction::PHI: {
    LLVM_DEBUG(errs() << "Query cost of phi\n");
    Cost = 0;
    break;
  }

  case Instruction::Call: {
    Instruction *Inst = nullptr;

    if (dyn_cast<AVRAssignHIR>(Expr->getParent())) {
      AVRAssignHIR *Parent = cast<AVRAssignHIR>(Expr->getParent());
      Inst = const_cast<Instruction*>(
        Parent->getHIRInstruction()->getLLVMInstruction()
      );
    } else if (dyn_cast<AVRAssignIR>(Expr->getParent())) {
      AVRAssignIR *Parent = cast<AVRAssignIR>(Expr->getParent());
      Inst = const_cast<Instruction*>(Parent->getLLVMInstruction());
    }
    assert(Inst && "Call parent expected to be an AVRAssign node");

    CallInst *Call = cast<CallInst>(Inst);
    Function *F = Call->getCalledFunction();
    if (!F) {
      // Indirect call.
      Cost = 20;
    } else if (TLI.isFunctionVectorizable(F->getName(), VF)) {
      // SVML call
      LLVM_DEBUG(errs() << "SVML call cost = 2\n");
      Cost = 2;
    } else {
      // LLVM cost model evaluates cost = 10 for calls to functions
      // using either scalar or vector call arguments. It does not
      // account for potential packing/unpacking of arguments.
      Type *RetTy = ToVectorTy(F->getReturnType(), VF);
      SmallVector<Type*, 1> ArgTys;
      for (auto &ArgOp : Call->arg_operands()) {
        Type *ArgTy = ToVectorTy(ArgOp->getType(), VF);
        ArgTys.push_back(ArgTy);
      }
      Cost = TTI.getCallInstrCost(F, RetTy, ArgTys);
    }
    break;
  }

  // AVRPredicate nodes are treated separately. See the visit function for
  // them for details.
  case Instruction::ICmp:
  case Instruction::FCmp: {
    AVRValue *AvrVal = cast<AVRValue>(Expr->getOperand(0));
    Type *ValTy = AvrVal->getType();
    VectorTy = ToVectorTy(ValTy, VF);
    Cost = TTI.getCmpSelInstrCost(Expr->getOperation(), VectorTy);
    break;
  }

  // TODO. Not yet supported by Codegen; Does not yet exist as an Expr.
  case Instruction::Select: {
    LLVM_DEBUG(errs() << "TODO: Query cost of select instruction\n");
    Cost = 10;
#if 0
    Type *CondTy = Expr->getOperans(/*CHECKME*/)->getType();
    CondTy = VectorType::get(CondTy, VF);
    Cost = TTI.getCmpSelInstrCost(Expr->getOperation(), VectorTy, CondTy);
#endif
    break;
  }
#endif

  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction::FSub:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor: {
    LLVM_DEBUG(errs() << "Query cost of arithmetic instruction\n");
    // "Certain instructions can be cheaper to vectorize if they have a constant
    // second vector operand. One example of this are shifts on x86."
    TargetTransformInfo::OperandValueKind Op1VK =
        TargetTransformInfo::OK_AnyValue;
    TargetTransformInfo::OperandValueKind Op2VK =
        TargetTransformInfo::OK_AnyValue;
    TargetTransformInfo::OperandValueProperties Op1VP =
        TargetTransformInfo::OP_None;
    TargetTransformInfo::OperandValueProperties Op2VP =
        TargetTransformInfo::OP_None;
#if 0 // TODO
    // Look at the SLEV value of the second operand.
    // CHECKME: operand ordering in AVR?
    AVR *Op1 = Expr->getOperand(1);
    // "Check for a splat of a constant or for a non uniform vector of constants."
    // TODO: Check if it is a OK_NonUniformConstantValue; 
    if (Op1->getSLEV().isConstant()) {
      // TODO: get the actual constant value and check if it isPowerOf2.
      // in case we can set Op2VP = TargetTransformInfo::OP_PowerOf2;
      Op2VK = TargetTransformInfo::OK_UniformConstantValue;
    }
#endif
    calculateReductionCost(Expr, Op1VK, Op2VK, Op1VP, Op2VP);

    Cost = TTI.getArithmeticInstrCost(Expr->getOperation(), VectorTy, Op1VK,
                                      Op2VK, Op1VP, Op2VP);

    // TODO: CodeGen is scalarizing srem and introducing a lot of overhead using
    // pshufd and punpck instructions. This is causing a 6-7% perf regression
    // for denbench/aes where several small loops with srem were being
    // "vectorized" inefficiently. For now, make srem costly, but we need a more
    // accurate assessment of what is going on. Previous srem cost was found to
    // be 1, which doesn't accurately represent what code is actually being
    // generated. Possible TTI problem.
    if (Expr->getOperation() == Instruction::SRem && VF > 1) {
      Cost += VF * 10;
    }
    break;
  }

  case Instruction::SExt:
  case Instruction::ZExt: {
    auto Op0 = dyn_cast<AVRValueHIR>(Expr->getOperand(0));
    
    // How do we account for "double pumping" in this cost model? In icc, we
    // introduced double pumped instructions, but in LLVM we use logical vector
    // lengths and let CodeGen decide what to do. Take for example the case of
    // networking/tcpjumbo when we have the following code within a loop in
    // function tcp_checksum. Target machine is xmm, VF=8. To reproduce, use:
    //
    // tc -t networking/tcpjumbo -l opt_speed -r none 
    //
    // %t18. = load <8 x i16>, <8 x i16>* %t18
    // %41 = zext <8 x i16> %t18. to <8 x i32>
    // %t19. = load <8 x i32>, <8 x i32>* %t19 (this is zero initialized)
    // %42 = add <8 x i32> %41, %t19.
    //
    // CodeGen generates the following:
    //
    // pxor  %xmm0
    // pxor  %xmm1
    // ...
    // punpckhwd  %xmm2, %xmm4
    // punpcklwd  %xmm2, %xmm3
    // paddd   %xmm0, %xmm3
    // paddd   %xmm1, %xmm4
    //
    // In this case, choosing VF=4 results in a significant performance
    // degradation (~16%) because then only a single interleave/add will result.
    //
    // For now, a flag has been introduced to turn off TTI querying of cast
    // cost. We ultimately need to figure out a long term solution to this
    // problem, as well as taking unrolling into account (this test also
    // gets unrolled by 4).
    if (Op0 && !Op0->getConstant()) {
      Type *SrcScalarTy = Op0->getType();
      Type *SrcVecTy = ToVectorTy(SrcScalarTy, VF);
      if (EnableCastCost) {
        Cost =  TTI.getCastInstrCost(Expr->getOperation(), VectorTy, SrcVecTy);
      } else {
        Cost = 1;
      }
    } else {
      LLVM_DEBUG(errs() << "TODO: Query cost of cast instruction\n");
      Cost = 10;
    }
    break;
  }

  case Instruction::FPExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::UIToFP:
  case Instruction::SIToFP:
  case Instruction::Trunc:
  case Instruction::FPTrunc:
  case Instruction::BitCast: {
    LLVM_DEBUG(errs() << "TODO: Query cost of cast instruction\n");
    Cost = 10;
#if 0 // TODO
    // TODO: Look at special cases
    auto Op0 = dyn_cast<AVRValueHIR>(Expr->getOperand(0));
    if (Op0 && !Op0->getConstant()) {
      Type *SrcScalarTy = Op0->getType();
      Type *SrcVecTy = ToVectorTy(SrcScalarTy, VF);
      Cost =  TTI.getCastInstrCost(Expr->getOperation(), VectorTy, SrcVecTy);
    }
#endif
    break;
  }

  case Instruction::Load:
  case Instruction::Store: {
    // LLVM_DEBUG(errs() << "Query cost of load/store instruction\n");

    // 0. Get the pointer and value operands
    bool isStore = (Expr->getOperation() == Instruction::Store ? true : false);
    bool isLoad = (Expr->getOperation() == Instruction::Load ? true : false);
    Type *ValTy;
    if (isLoad) {
      ValTy = Expr->getType();
    } else {
      assert(isa<AVRValue>(Expr->getOperand(0)) && "Not a Value?");
      ValTy = cast<AVRValue>(Expr->getOperand(0))->getType();
    }

    AVR *Op;
    if (isLoad) {
      Op = Expr->getOperand(0);
    } else {
      AVR *Assign = Expr->getParent();
      assert(isa<AVRAssign>(Assign) && "Not an Assign?");
      AVR *LHS = cast<AVRAssign>(Assign)->getLHS();
      assert(isa<AVRExpression>(LHS) && "Not an Expression?");
      Op = cast<AVRExpression>(LHS)->getOperand(0);
    }

    // Implicit loads introduced by HIR Temp Cleanup Pass need special
    // treatment. If decomposition is not enabled, they are hidden under an
    // AVRValueHIR and won't hit here. If decomposition analysis is enabled, a
    // new load (AVRExpression) is introduced. However, there are two important
    // differences between this new load and an original explicit load (built by
    // AVRGenerate):
    //     1. In a explicit load, RegDDRef is in the operand (AVRValue) that
    //     contains the load address. In a load generated in decomposition
    //     analysis for an implicit load, RegDDRef is in the AVRValueHIR that
    //     represents (hide) the whole load.
    //     2. The operand of an explicit load is an AVRValueHIR that represents
    //     the address. The operand of a load generated in decomposition
    //     analysis for an implicit load is an AVRExpression (sub-expression tree)
    //     with an explicit GEP.

    bool IsImplicitLoad;
    Type *PtrType;
    //TODO: Move type to AVR. This pattern is very common
    if (AVRValue *ValOp = dyn_cast<AVRValue>(Op)) {
      PtrType = ValOp->getType();
      IsImplicitLoad = false;
    } else if (AVRExpression *ExprOp = dyn_cast<AVRExpression>(Op)) {
      PtrType = ExprOp->getType();
      IsImplicitLoad = true;
    }
    else {
      llvm_unreachable("Op should be AVRValue or AVRExpression");
    }

    assert(PtrType->isPointerTy() && "Unexpected non-ptr");

    // 1. Get the Alignment (TODO)
    // CHECKME: get it from underlying IR? (LI->getAlignemnt())
    unsigned Alignment = 0; // CHECKME: means aligned or unknown?

    // 2. Get the Address Space
    unsigned AS = PtrType->getPointerAddressSpace();

    // Case 1: A scalar ld/st will be generated
    //
#ifdef USE_EXPERIMENTAL_CODE
    if (VF == 1 || Expr->getSLEV().isUniform()) {
#else
    if (VF == 1) {
#endif
      LLVM_DEBUG(errs() << "Case1: Scalar Load/Store\n");
      Cost = TTI.getAddressComputationCost(VectorTy) +
             TTI.getMemoryOpCost(Expr->getOperation(), VectorTy, Alignment, AS);
      break;
    }

    // TODO: Account for brodacst cost (for loads).
    // Only if the loaded value is used in an operation that will be
    // widened do we need a brodcast. So we defer this to processing of the
    // loaded value upon its use.
    // FIXME: This means we may be taking this cost multiple times at each use.
    // CHECKME: Do we want to account for this cost here or when we process the
    // used Value?
#if 0
    if (LI && Legal->isUniform(Ptr)) {
      // Scalar load + broadcast
      unsigned Cost = TTI.getAddressComputationCost(ValTy->getScalarType());
      Cost += TTI.getMemoryOpCost(I->getOpcode(), ValTy->getScalarType(),
                                  Alignment, AS);
      return Cost + TTI.getShuffleCost(TargetTransformInfo::SK_Broadcast,
                                       ValTy);
      break;
    }
#endif

    bool isMaskRequired = false;
    // Search up in the decomposed tree to get to the actual statement level
    // parent of all subexpressions.
    AVR *Parent = Expr->getParent();
    assert(Parent && "Expr should always have a parent");
    while (isa<AVRExpression>(Parent)) {
      Parent = Parent->getParent(); 
    }
    AVRPredicate *Pred = Parent->getPredicate();
    if (Pred) {
      isMaskRequired = true;
    }

    // Classify the access (stride)
    //
    int64_t ConsecutiveStride = 0;
    int64_t StrideInElements = 0;

    VPOVLSInfoBase *VLSInfo = getVLSInfo();
    assert(VLSInfo && "VLSInfo not available"); 
    OVLSMemref *Mrf;
    if (!IsImplicitLoad) {
      // Explicit load. The RegDDRef is in the load operand (pointer)
      Mrf = VLSInfo->getVLSMemrefInfoForAccess(cast<AVRValue>(Op));
    } else {
      // Implicit load. The RegDDRef is in the AVRValue hidding the whole load
      assert(Expr->getParent() && isa<AVRValueHIR>(Expr->getParent()) &&
             "Unexpectd parent in an implicit load");
      Mrf = VLSInfo->getVLSMemrefInfoForAccess(
          cast<AVRValueHIR>(Expr->getParent()));
    }

    Type *DataTy = Expr->getType();

#ifdef USE_EXPERIMENTAL_CODE 
    ConsecutiveStride = getConsecutiveStride(Op);
    if (Op->getSLEV().isStrided()) {
      StrideInElements =
          Op->getSLEV().getStride().getInteger().getSExtValue();
    } 
#else
    // Temporary work around until SLEV is operational
    int64_t StrideInBytes;
    if (Mrf && Mrf->hasAConstStride(&StrideInBytes)) {
      unsigned ElemSize = DataTy->getPrimitiveSizeInBits() / 8;
      if (ElemSize == 0) {
        // Avoid div-by-zero - proper fix is being checked into vpo branch
        Cost = 0;
        break; 
      }
      StrideInElements = StrideInBytes / ElemSize;
      if ((StrideInElements == 1) || (StrideInElements == -1))
        ConsecutiveStride = StrideInElements;
    } 
    else {
      ConsecutiveStride = getConsecutiveStride(Op);
    }
#endif
    LLVM_DEBUG(errs() << "Consecutive Stride = " << ConsecutiveStride << "\n");
    LLVM_DEBUG(errs() << "Stride = " << StrideInElements << "\n");
    bool Reverse = ConsecutiveStride < 0;
    bool isGatherOrScatterLegal = (isLoad && TTI.isLegalMaskedGather(DataTy)) ||
                                  (isStore && TTI.isLegalMaskedScatter(DataTy));
    bool UseGatherOrScatter =
        (ConsecutiveStride == 0) && isGatherOrScatterLegal;

    // Case 2: Strided access, part of VLS group
    //
    OVLSGroup *Grp = nullptr;
    if (Mrf) 
      Grp = VLSInfo->getVLSGroupInfoForVLSMemref(Mrf);
    if (Grp && !ConsecutiveStride && EnableVectVLS) {
      LLVM_DEBUG(errs() << "Found a VLS group for the access!\n");
      // The group cost is accounted in its entirety to the first Memref of Grp 
      if (Mrf == Grp->getFirstMemref()) {
        OVLSTTICostModel *TTICM = getVLSCostModel(); 
        int64_t GrpCost = OptVLSInterface::getGroupCost(*Grp, *TTICM); 
        Cost = GrpCost;
        LLVM_DEBUG(errs() << "Group Cost = " << Cost << "\n");
        break;
      }
      // If this memref is in a VLS group but is not the first Memref of the 
      // group -- no cost is added (as the entire group cost is accounted to 
      // the first Memref of the group).
      LLVM_DEBUG(errs() << "skip -- not first access of the group!\n");
      Cost = 0;
      break; 
    }

    // Case 3: Scalarized loads/stores
    // (for non unit-stride access without gather/scatter support)
    //
#if 0
    // TODO:
    const DataLayout &DL = I->getModule()->getDataLayout();
    unsigned ScalarAllocatedSize = DL.getTypeAllocSize(ValTy);
    unsigned VectorElementSize = DL.getTypeStoreSize(VectorTy) / VF;
    bool GapInElemSize = (ScalarAllocatedSize != VectorElementSize)
#endif
    bool GapInElemSize = false; // FIXME
    if ((!ConsecutiveStride && !UseGatherOrScatter) || GapInElemSize) {
      LLVM_DEBUG(
          errs() << "Case 2: Non-consecutive access Scalarization Cost.\n");
      Cost = 0;
      // The cost of extracting from the value vector and pointer vector.
      Type *PtrsVecTy = ToVectorTy(PtrType, VF);
      for (unsigned i = 0; i < VF; ++i) {
        // Add the cost of extracting the mask bit, icmp to check mask bit, and
        // br instruction.
        if (!UseGatherOrScatter && isMaskRequired) {
          for (unsigned i = 0; i < VF; ++i) {
            Type *CmpTy = Type::getInt1Ty(Expr->getType()->getContext());
            VectorType *VecCmpTy = VectorType::get(CmpTy, VF);
            Cost += TTI.getVectorInstrCost(Instruction::ExtractElement,
                                           VecCmpTy, i);
            Cost += TTI.getCmpSelInstrCost(Instruction::ICmp, CmpTy);
            Cost += TTI.getCFInstrCost(Instruction::Br);
          }
        }

        //  The cost of extracting the pointer operand.
        Cost +=
            TTI.getVectorInstrCost(Instruction::ExtractElement, PtrsVecTy, i);
        // In case of STORE, the cost of ExtractElement from the vector.
        // In case of LOAD, the cost of InsertElement into the returned vector.
        Cost += TTI.getVectorInstrCost(isStore ? Instruction::ExtractElement
                                               : Instruction::InsertElement,
                                       VectorTy, i);
      }

      // The cost of the scalar loads/stores.
      // TODO - see if we need to account for complex address computation.
      Cost +=
          VF * TTI.getAddressComputationCost(PtrsVecTy);
      Cost += VF *
              TTI.getMemoryOpCost(Expr->getOperation(), ValTy->getScalarType(),
                                  Alignment, AS);
      break;
    }

    Cost = TTI.getAddressComputationCost(VectorTy);

    // Case 4: Non unit-stride access, using Gather/Scatter
    //
#ifdef USE_EXPERIMENTAL_CODE 
    if (Op->getSLEV().isStrided()) {
      int64_t StrideInElements =
          Op->getSLEV().getStride().getInteger().getSExtValue();
      LLVM_DEBUG(errs() << "Stride = " << StrideInElements << "\n");
    } else
      LLVM_DEBUG(errs() << "Stride Unknown\n");
#endif

    if (UseGatherOrScatter) {
      LLVM_DEBUG(errs() << "Case 3: GatherScatterCost.\n");
      assert(ConsecutiveStride == 0 &&
             "Gather/Scatter are not used for consecutive stride");
      Cost += getGatherScatterOpCost(Expr->getOperation(), VectorTy, Op,
                                     isMaskRequired, Alignment);
      break;
    }

    // Case 5: Wide load/stores.
    LLVM_DEBUG(errs() << "Case 4: Wide Load/Store Cost.\n");
    if (isMaskRequired)
      Cost += TTI.getMaskedMemoryOpCost(Expr->getOperation(), VectorTy,
                                        Alignment, AS);
    else
      Cost +=
          TTI.getMemoryOpCost(Expr->getOperation(), VectorTy, Alignment, AS);
    if (Reverse)
      Cost += TTI.getShuffleCost(TargetTransformInfo::SK_Reverse, VectorTy, 0);

    break;
  }

  default:
    LLVM_DEBUG(errs() << "Unsupported expression kind.\n");
    LLVM_DEBUG(Expr->dump(PrintDataType));
    //llvm_unreachable("unsupported expression kind");
  }

  // Costs related to creating the operands were already counted when
  // operands were visited. Costs related to the operation itself:
  // LLVM_DEBUG(errs() << "visited expr: add cost of operation!\n");
  // LLVM_DEBUG(Expr->dump(PrintDataType));
  // LLVM_DEBUG(errs() << "added a cost of " << Cost << " to LoopBodyCost\n");
  LoopBodyCost += Cost;
  Expr->setCost(Cost);
}

// TODO: Contribute the cost of producing this value if not already
// available, according to the SLEV property.
void VPOCostGathererBase::visit(AVRValue *AValue) {
  // LLVM_DEBUG(errs() << "visiting value!\n");
}

void VPOCostGathererBase::postVisit(AVRValue *AValue) {
  // LLVM_DEBUG(errs() << "Post-visiting value!\n");
}

// This function visits all AVRValues and obtains the type information from each
// one so that this information can later be used to select an appropriate VF.
void VPOScenarioEvaluationBase::visit(AVRValue *AValue) {

  Type *Ty;

  LLVM_DEBUG(errs() << "AValue: "; AValue->dump(); errs() << "\n");
  LLVM_DEBUG(errs() << "AValue Parent: "; AValue->getParent()->dump();
             errs() << "\n");
  LLVM_DEBUG(errs() << "AValue Parent Parent: ";
             AValue->getParent()->getParent()->dump(); errs() << "\n");

  if (isa<AVRPredicate>(AValue->getParent()->getParent())) {
    // Just skip gathering type information from predicate nodes.
    return;
  }

  if (AVRValueHIR *Val = dyn_cast<AVRValueHIR>(AValue)) {
    assert(isa<RegDDRef>(Val->getValue()) && "Expected RegDDRef");
    const RegDDRef *Ref = cast<RegDDRef>(Val->getValue());
    if (Ref->hasGEPInfo() && Ref->isAddressOf()) {
      // address of computations should be pointer types.
      Type *BaseTy = Ref->getBitCastDestType();
  
      if (!BaseTy) {
        BaseTy = Ref->getBaseType();
      }
      Ty = cast<PointerType>(BaseTy);
    } else {
      // otherwise, things like "a[i] = ..." and "... = a[i]" should be the
      // element type and not the base type since they imply load/store of
      // an element.
      Ty = Ref->getDestType();
      if (Ref->hasGEPInfo()) {
        LLVM_DEBUG(errs() << " (implicit load/store)");
      }
      LLVM_DEBUG(errs() << "\n");
    }
  } else if (isa<AVRValueIR>(AValue)) {
    Ty = AValue->getType();
  }

  // Ignore types such as 'metadata', etc.
  if (Ty->isSized()) {
    uint64_t Size = DL.getTypeSizeInBits(Ty);
    TypeSizes TySizeIdx = I32_TYPE_SIZE;
    switch (Size) {
      case 1:
        TySizeIdx = I1_TYPE_SIZE; break;
      case 8:
        TySizeIdx = I8_TYPE_SIZE; break;
      case 16:
        TySizeIdx = I16_TYPE_SIZE; break;
      case 32:
        TySizeIdx = I32_TYPE_SIZE; break;
      case 64:
        TySizeIdx = I64_TYPE_SIZE; break;
    }
    LoopTypeSizes[TySizeIdx]++;
  }
}

// CHECKME: getCost() operates on a single AvrLoop. In the future will be
// called several times per scenario, if the region contains several candidate
// AvrLoops.
// TODO: What additional information will the costModel need?:
// - a Map of Memrefs to the VLS Group they belong to (if any).
// - ?
// TODO: A fix was made to return uint64_t here because this function was
// previously computing cost based on uint64_t and returning int. This
// caused a signed int overflow, but the remaining question that needs to
// be answered is why such a high cost was being computed in the first place.
uint64_t VPOCostModelBase::getCost(AVRLoop *ALoop, unsigned int VF,
                                   VPOVLSInfoBase *VLSInfo,
                                   HIRSafeReductionAnalysis *SRA) {
  LLVM_DEBUG(errs() << "\nEvaluating Loop Cost for VF = " << VF << "\n");
  uint64_t Cost;

  // Calculate LoopBody Cost
  if (AVRLoopHIR *ALoopHIR = dyn_cast<AVRLoopHIR>(ALoop)) {
    assert(SRA && "Expected existence of HIRSafeReductionAnalysis");
    HLLoop *HIRLoop = ALoopHIR->getLoop();
    SRA->computeSafeReductionChains(HIRLoop);
  }

  VPOCostGathererBase *CostGatherer = getCostGatherer(VF, ALoop, VLSInfo, SRA);
  assert(CostGatherer && "Invalid CostGatherer");
  AVRVisitor<VPOCostGathererBase> AVisitor(*CostGatherer);
  // Enabling RecursiveInsideValues to visit AVRValueHIR's sub-tree
  // decomposition.
  AVisitor.visit(ALoop, true, true, true /*RecursiveInsideValues*/, true);
  unsigned int LoopBodyCost = CostGatherer->getLoopBodyCost();

  // Used to play around with calculated cost to favor/disallow vectorization
  if (VF > 1 && TweakVPOCostFactor != 0.0)
    LoopBodyCost *= TweakVPOCostFactor;

  // Calculate OutOfLoop Costs. 
  uint64_t LoopCount;
  unsigned int RemainderLoopCost = getRemainderLoopCost(VF, LoopCount);
  LLVM_DEBUG(errs() << "RemainderLoopCost = " << RemainderLoopCost
                    << " LoopCount = " << LoopCount << "\n");
  CostGatherer->addOutOfLoopCost(RemainderLoopCost);
  unsigned int OutOfLoopCost = CostGatherer->getOutOfLoopCost();
  unsigned int ReductionCost = CostGatherer->getReductionCost();
  LLVM_DEBUG(errs() << "LoopBodyCost = " << LoopBodyCost
                    << " OutOfLoopCost = " << OutOfLoopCost
                    << " Reduction Cost = " << ReductionCost << "\n");

  if (LoopCount <= 0) {
    // Use max trip count estimate if available
    LoopCount = ALoop->getMaxTripCountEstimate();
  }

  if (LoopCount <= 0) LoopCount = 100;
  Cost = (LoopBodyCost * LoopCount / VF) + OutOfLoopCost + ReductionCost;

  LLVM_DEBUG(ALoop->dump(PrintCost));
  if (VF == 1) {
    LLVM_DEBUG(errs() << "Scalar ");
    ScalarIterCost = Cost / LoopCount;
  }
  LLVM_DEBUG(errs() << "Cost for candidate Loop = " << Cost << "\n");
  LLVM_DEBUG(errs() << "(" << LoopBodyCost << "(Loop Body) * " << LoopCount
                    << "(Loop Count) / " << VF << "(VF)) + " << OutOfLoopCost
                    << "(Remainder Loop Cost) + " << ReductionCost
                    << "(Reduction Cost)\n");

  delete CostGatherer;
  return Cost;
}
