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

#include "llvm/Analysis/Intel_VPO/Vecopt/VPOScenarioEvaluation.h"
#include "llvm/Analysis/Intel_VPO/Vecopt/VPOPredicator.h"
#include "llvm/Analysis/Intel_LoopAnalysis/HIRAnalysisPass.h"
#include "llvm/Analysis/Intel_LoopAnalysis/HIRVLSClient.h"
#include "llvm/Analysis/Intel_VPO/Vecopt/VPOAvrVisitor.h"

#define DEBUG_TYPE "VPOScenarioEvaluation"

static cl::opt<unsigned> DefaultVF("default-vpo-vf", cl::init(0),
                                   cl::desc("Default vector length"));
static cl::opt<unsigned> EnableVectVLS("enable-vect-vls", cl::init(1),
                             cl::desc("Enable VLS group analysis by default"));

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
  DEBUG(errs() << "VF = " << ForceVF << " DefaultVF = " << DefaultVF << "\n");
  if (ForceVF == 0) {
    ForceVF = DefaultVF;
  }
  int initialVF = ForceVF ? ForceVF : 1;
  //DEBUG(errs() << "Set dummy vectCand with VF = " << initialVF << "\n");
  VPOVecContextBase VectCand(initialVF); 

#if 1
  // FORNOW: An AVRWrn node is expected to have only one AVRLoop child
  AVRLoop *AvrLoop = nullptr;
  for (auto Itr = AWrn->child_begin(), End = AWrn->child_end(); Itr != End;
       ++Itr) {
    if (AVRLoop *TempALoop = dyn_cast<AVRLoop>(Itr)) {
      if (AvrLoop)
        return VectCand;

      AvrLoop = TempALoop;
    }
  }

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
#ifndef USE_EXPERIMENTAL_CODE
      if (!loopIsHandled(ForceVF))
        return VectCand;
#endif

      // Decomposition of AVRValueHIRs happens here
      prepareLoop(AvrLoop);

      int BestCostForALoop = 0;
      // Get the scalar cost for this loop.
      // (No need to compute the cost if ForceVF is set to a VF forced by the 
      // user).
      //
      // FORNOW: Calculate cost only for the candidate AvrLoop. We assume that
      // the rest of the code in the region will remain the same between the
      // scalar and vector versions.
      if (ForceVF == 0) {
        BestCostForALoop = getCM()->getCost(ALoop, 1, nullptr);
      }
      DEBUG(errs() << "Scalar Cost for candidate Loop = " << BestCostForALoop
                   << "\n");
      DEBUG(ALoop->dump(PrintDataType));

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

// FORNOW: A trivial implementation.
// TODO: Should really be based on the data-types used in the loop and the
// vector regsiters supported by the target.
void VPOScenarioEvaluationBase::findVFCandidates(
    VFsVector &VFCandidates) const {
  unsigned int MinVF, MaxVF;
  if (ForceVF == 0) {
    MinVF = 4;  // FIXME
    MaxVF = 4;  // FIXME
  } else {
    MinVF = ForceVF;
    MaxVF = ForceVF;
  }

  for (unsigned int VF = MinVF; VF <= MaxVF; VF *= 2) {
    VFCandidates.push_back(VF);
  }
}

VPOVecContextBase
VPOScenarioEvaluationBase::processLoop(AVRLoop *ALoop, int *BestCostForALoop) {
  DEBUG(errs() << "Process Loop\n");

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
  DEBUG(formatted_raw_ostream FOS(dbgs()); FOS << "After SLEV analysis:\n";
        AWrn->print(FOS, 1, PrintNumber));

  VPOPredicator Predicator;
  Predicator.runOnAvr(VPOScenarioEvaluationBase::ALoop);
#endif

  // Identify VF candidates
  //
  VFsVector VFCandidates;
  findVFCandidates(VFCandidates);

  // Evaluate each VF candidate
  //
  VPOVecContextBase BestCand(1);
  for (auto &VF : VFCandidates) {

    DEBUG(errs() << "Evaluate candidate with VF = " << VF << "\n");
    // Currently VecContext is used to hold underlying-IR level information
    // required for some of the analyses in processCandidates (namely, for VLS
    // grouping).
    VPOVecContextBase VC = setVecContext(VF);
    int Cost = processCandidate(ALoop, VF, VC);

    // FIXME: Change condition back to <; FORNOW using <= simply to avoid
    // change of behavior resulting in not vectorizing things that our current
    // tests expect will be vectorized.
    if (Cost <= *BestCostForALoop) {
      *BestCostForALoop = Cost;
      BestCand = VC;
      DEBUG(errs() << "New Best Candidate Cost = " << *BestCostForALoop
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
int VPOScenarioEvaluationBase::processCandidate(AVRLoop *ALoop, unsigned int VF,
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
    DEBUG(errs() << "Cost for Group = " << Cost << "\n");
  }
#endif

  // Calculate the cost of the current candidate
  // (No need to calculate cost if the user forced a specific VF).
  //
  int Cost = 0; 
  if (ForceVF == 0) { 
    Cost = getCM()->getCost(ALoop, VF, VLSInfo);
  } 
  DEBUG(errs() << "Vector Cost for candidate Loop = " << Cost << "\n");

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

/// A helper function for converting Scalar types to vector types.
/// If the incoming type is void, we return void. If the VF is 1, we return
/// the scalar type (copied from LoopVectorize.cpp).
static Type *ToVectorTy(Type *Scalar, unsigned VF) {
  if (Scalar->isVoidTy() || VF == 1)
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
int64_t getConsecutiveStride(AVRValue *PtrOp) {
  assert(PtrOp->getType()->isPointerTy() && "Unexpected non-ptr");
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
//  DEBUG(errs() << "visiting loop!\n");
}

// TODO: If this is an inner-loop inside the ALoop being vectorized: multiply
// by the iteration-count
void VPOCostGathererBase::postVisit(AVRLoop *Loop) {
  //DEBUG(errs() << "visited loop!\n");
}

void VPOCostGathererBase::visit(AVRAssign *Assign) {
  //DEBUG(errs() << "visiting assign!\n");
}

// TODO: Take blend cost into account if masked.
void VPOCostGathererBase::postVisit(AVRAssign *Assign) {
  //DEBUG(errs() << "visited assign!\n");
}

// Following will soon move under handling of Expr
#if 1
void VPOCostGathererBase::visit(AVRLabel *Label) {
  //DEBUG(errs() << "visit Label!\n");
}

void VPOCostGathererBase::visit(AVRCall *Call) {
  DEBUG(errs() << "TODO: visit Call!\n");
}

// CHECKME: Account for reduction cost here?
void VPOCostGathererBase::visit(AVRPhi *Phi) {
  DEBUG(errs() << "TODO: visit Phi!\n");
}

void VPOCostGathererBase::visit(AVRBranch *Branch) {
  DEBUG(errs() << "TODO: visit Branch!\n");
}

void VPOCostGathererBase::visit(AVRCompare *Compare) {
  DEBUG(errs() << "TODO: visit Compare!\n");
}

void VPOCostGathererBase::visit(AVRIf *If) {
  DEBUG(errs() << "TODO: visit If!\n");
}

void VPOCostGathererBase::visit(AVRSelect *Select) {
  DEBUG(errs() << "TODO: visit Select!\n");
}

// Of the following, only (uniform) AVRIf should survive after perdication.
bool VPOCostGathererBase::skipRecursion (AVR *ANode) { 
  if (isa<AVRIf>(ANode) || isa<AVRSelect>(ANode) || isa<AVRSwitch>(ANode))
    return true;
  return false;
}

void VPOCostGathererBase::visit(AVR *ANode) {
  DEBUG(errs() << "VPOCostModel: Unsupported AVR kind\n");
  DEBUG(ANode->dump(PrintBase));
  //llvm_unreachable("unsupported AVR kind");
}
#endif

void VPOCostGathererBase::visit(AVRExpression *Expr) {
  //DEBUG(errs() << "visiting expr!\n");
  if (Expr->isLHSExpr()) {
    // We assume that there is no operation here, just a Value
    return;
  }
}

void VPOCostGathererBase::postVisit(AVRExpression *Expr) {
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

  //DEBUG(errs() << "visit expr: \n");
  //DEBUG(Expr->dump(PrintDataType));
  if (Expr->isLHSExpr()) {
    // FORNOW: Not contributing any cost
    // TODO: What about costly address computations on HIR side?
    // DEBUG(errs() << "visited expr: LHS: no cost contributed!\n");
    return;
  }

  switch (Expr->getOperation()) {
  case Instruction::GetElementPtr: {
    // DEBUG(errs() << "Query cost of getElementPtr\n");
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
    DEBUG(errs() << "Query cost of branch\n");
    Cost = TTI.getCFInstrCost(Expr->getOperation());
    break;
  }

  // CHECKME: account for reduction cost here?
  case Instruction::PHI: {
    DEBUG(errs() << "Query cost of phi\n");
    Cost = 0;
    break;
  }

  case Instruction::Call: {
    DEBUG(errs() << "TODO: Query cost of call instruction\n");
    Cost = 10;
    break;
  }

  // TODO. Not yet supported by Codegen; Does not yet exist as an Expr.
  case Instruction::ICmp:
  case Instruction::FCmp: {
    DEBUG(errs() << "TODO: Query cost of compare instruction\n");
    Cost = 10;
#if 0
    Type *ValTy = Expr->getOperand(0)->getType();
    VectorTy = ToVectorTy(ValTy, VF);
    Cost = TTI.getCmpSelInstrCost(Expr->getOeration(), VectorTy);
#endif
    break;
  }

  // TODO. Not yet supported by Codegen; Does not yet exist as an Expr.
  case Instruction::Select: {
    DEBUG(errs() << "TODO: Query cost of select instruction\n");
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
    DEBUG(errs() << "Query cost of arithmetic instruction\n");
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
    Cost = TTI.getArithmeticInstrCost(Expr->getOperation(), VectorTy, Op1VK,
                                      Op2VK, Op1VP, Op2VP);
    break;
  }

  case Instruction::SExt:
  case Instruction::ZExt:
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
    DEBUG(errs() << "TODO: Query cost of cast instruction\n");
    Cost = 10;
#if 0 // TODO
    // TODO: Look at special cases
    AVR *Op0 = Expr->getOperand(0);
    Type *SrcScalarTy = Op0->getType();
    Type *SrcVecTy = ToVectorTy(SrcScalarTy, VF);
    Cost =  TTI.getCastInstrCost(Expr->getOperation(), VectorTy, SrcVecTy);
#endif
    break;
  }

  case Instruction::Load:
  case Instruction::Store: {
    //DEBUG(errs() << "Query cost of load/store instruction\n");

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
    assert(isa<AVRValue>(Op) && "Not an AVRValue?");
    AVRValue *PtrOp = cast<AVRValue>(Op);
    Type *PtrType = PtrOp->getType();
    assert(PtrOp->getType()->isPointerTy() && "Unexpected non-ptr");

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
      DEBUG(errs() << "Case1: Scalar Load/Store\n");
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

    // TODO: consider masking
    bool isMaskRequired = false; // FIXME

    // Classify the access (stride)
    //
    int64_t ConsecutiveStride = 0;
    int64_t StrideInElements = 0;

    VPOVLSInfoBase *VLSInfo = getVLSInfo();
    assert(VLSInfo && "VLSInfo not available"); 
    OVLSMemref *Mrf = VLSInfo->getVLSMemrefInfoForAccess(PtrOp);
    Type *DataTy = (cast<PointerType>(PtrType))->getElementType();
     
#ifdef USE_EXPERIMENTAL_CODE 
    ConsecutiveStride = getConsecutiveStride(PtrOp);
    if (PtrOp->getSLEV().isStrided()) {
      StrideInElements =
          PtrOp->getSLEV().getStride().getInteger().getSExtValue();
    } 
#else
    // Temporary work around until SLEV is operational
    int64_t StrideInBytes;
    if (Mrf && Mrf->hasAConstStride(&StrideInBytes)) {
      unsigned ElemSize = DataTy->getPrimitiveSizeInBits() / 8;
      if (DataTy->isPointerTy()) {
        // Avoid div-by-zero for pulldown purpose.
        // TODO: Load can be a pointer. What do we do?
        Cost = 0;
        break; 
      }
      StrideInElements = StrideInBytes / ElemSize;
      if ((StrideInElements == 1) || (StrideInElements == -1))
        ConsecutiveStride = StrideInElements;
    } 
    else {
      ConsecutiveStride = getConsecutiveStride(PtrOp);
    }
#endif
    DEBUG(errs() << "Consecutive Stride = " << ConsecutiveStride << "\n");
    DEBUG(errs() << "Stride = " << StrideInElements << "\n");
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
      DEBUG(errs() << "Found a VLS group for the access!\n");
      // The group cost is accounted in its entirety to the first Memref of Grp 
      if (Mrf == Grp->getFirstMemref()) {
        OVLSTTICostModel *TTICM = getVLSCostModel(); 
        int64_t GrpCost = OptVLSInterface::getGroupCost(*Grp, *TTICM); 
        Cost = GrpCost;
        DEBUG(errs() << "Group Cost = " << Cost << "\n");
        break;
      }
      // If this memref is in a VLS group but is not the first Memref of the 
      // group -- no cost is added (as the entire group cost is accounted to 
      // the first Memref of the group).
      DEBUG(errs() << "skip -- not first access of the group!\n");
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
      DEBUG(errs() << "Case 2: Non-consecutive access Scalarization Cost.\n");
      bool IsComplexComputation = isLikelyComplexAddressComputation(PtrOp);
      Cost = 0;
      // The cost of extracting from the value vector and pointer vector.
      Type *PtrsVecTy = ToVectorTy(PtrOp->getType(), VF);
      for (unsigned i = 0; i < VF; ++i) {
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
      Cost +=
          VF * TTI.getAddressComputationCost(PtrsVecTy, IsComplexComputation);
      Cost += VF *
              TTI.getMemoryOpCost(Expr->getOperation(), ValTy->getScalarType(),
                                  Alignment, AS);
      break;
    }

    Cost = TTI.getAddressComputationCost(VectorTy);

    // Case 4: Non unit-stride access, using Gather/Scatter
    //
#ifdef USE_EXPERIMENTAL_CODE 
    if (PtrOp->getSLEV().isStrided()) {
      int64_t StrideInElements =
          PtrOp->getSLEV().getStride().getInteger().getSExtValue();
      DEBUG(errs() << "Stride = " << StrideInElements << "\n");
    } else
      DEBUG(errs() << "Stride Unknown\n");
#endif

    if (UseGatherOrScatter) {
      DEBUG(errs() << "Case 3: GatherScatterCost.\n");
      assert(ConsecutiveStride == 0 &&
             "Gather/Scatter are not used for consecutive stride");
      Cost += getGatherScatterOpCost(Expr->getOperation(), VectorTy, PtrOp,
                                     isMaskRequired, Alignment);
      break;
    }

    // Case 5: Wide load/stores.
    //
    // TODO: Take masking into account.
    DEBUG(errs() << "Case 4: Wide Load/Store Cost.\n");
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
    DEBUG(errs() << "Unsupported expression kind.\n");
    DEBUG(Expr->dump(PrintDataType));
    //llvm_unreachable("unsupported expression kind");
  }

  // Costs related to creating the operands were already counted when
  // operands were visited. Costs related to the operation itself:
  //DEBUG(errs() << "visited expr: add cost of operation!\n");
  //DEBUG(Expr->dump(PrintDataType));
  DEBUG(errs() << "added a cost of " << Cost << "  to LoopBodyCost\n");
  LoopBodyCost += Cost;
}

// TODO: Contribute the cost of producing this value if not already
// available, according to the SLEV property.
void VPOCostGathererBase::visit(AVRValue *AValue) {
  //DEBUG(errs() << "visiting value!\n");
}

void VPOCostGathererBase::postVisit(AVRValue *AValue) {
  //DEBUG(errs() << "Post-visiting value!\n");
}

// CHECKME: getCost() operates on a single AvrLoop. In the future will be
// called several times per scenario, if the region contains several candidate
// AvrLoops.
// TODO: What additional information will the costModel need?:
// - a Map of Memrefs to the VLS Group they belong to (if any).
// - ?
int VPOCostModelBase::getCost(AVRLoop *ALoop, unsigned int VF, 
                              VPOVLSInfoBase *VLSInfo) {
  DEBUG(errs() << "getCost: VF = " << VF << "\n");
  int Cost;

  // Calculate LoopBody Cost 
  VPOCostGathererBase *CostGatherer = getCostGatherer(VF, ALoop, VLSInfo);
  assert(CostGatherer && "Invalid CostGatherer");
  AVRVisitor<VPOCostGathererBase> AVisitor(*CostGatherer);
  // Enabling RecursiveInsideValues to visit AVRValueHIR's sub-tree decomposition
  AVisitor.visit(ALoop, true, true, false /*RecursiveInsideValues*/, true);
  unsigned int LoopBodyCost = CostGatherer->getLoopBodyCost();

  // Calculate OutOfLoop Costs. 
  unsigned int LoopCount;
  unsigned int RemainderLoopCost = getRemainderLoopCost(ALoop, VF, LoopCount);
  DEBUG(errs() << "RemainderLoopCost = " << RemainderLoopCost
               << " LoopCount = " << LoopCount << "\n");
  CostGatherer->addOutOfLoopCost(RemainderLoopCost);
  unsigned int OutOfLoopCost = CostGatherer->getOutOfLoopCost();
  DEBUG(errs() << "LoopBodyCost = " << LoopBodyCost
               << " OutOfLoopCost = " << OutOfLoopCost << "\n");
  if (LoopCount <= 0) LoopCount = 100;
  Cost = (LoopBodyCost * LoopCount / VF) + OutOfLoopCost;

  delete CostGatherer;
  return Cost;
}
