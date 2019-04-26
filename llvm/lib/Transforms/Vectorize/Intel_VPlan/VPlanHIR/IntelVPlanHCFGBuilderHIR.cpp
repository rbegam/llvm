//===-- IntelVPlanHCFGBuilderHIR.cpp --------------------------------------===//
//
//   Copyright (C) 2017-2019 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file extends VPlanCFGBuilderBase with support to build a hierarchical
/// CFG from HIR.
///
/// The algorithm consist of a Visitor that traverses HLNode's (lexical links)
/// in topological order and builds a plain CFG out of them. It returns a region
/// (TopRegion) containing the plain CFG.
///
/// It is inspired by AVR-based VPOCFG algorithm and uses a non-recursive
/// visitor to explicitly handle visits of "compound" HLNode's (HLIfs, HLLoop,
/// HLSwitch) and trigger the creation-closure of VPBasicBlocks.
///
/// Creation/closure of VPBasicBlock's is triggered by:
///   1) HLLoop Pre-header
///   *) HLoop Header
///   *) End of HLLoop body
///   *) HLoop Exit (Postexit)
///   *) If-then branch
///   *) If-else branch
///   *) End of HLIf
///   *) HLLabel
///   *) HLGoto
///
/// The algorithm keeps an active VPBasicBlock (ActiveVPBB) that is populated
/// with "instructions". When one of the previous conditions is met, a new
/// active VPBasicBlock is created and connected to its predecessors. A list of
/// VPBasicBlock (Predecessors) holds the predecessors to be connected to the
/// new active VPBasicBlock when it is created HLGoto needs special treatment
/// since its VPBasicBlock is not reachable from an HLLabel. For that reason, a
/// VPBasicBlock ending with an HLGoto is connected to its successor when HLGoto
/// is visited.
///
/// TODO's:
///   - Outer loops.
///   - Expose ZTT for inner loops.
///   - HLSwitch
///   - Loops with multiple exits.
///
//===----------------------------------------------------------------------===//

#include "IntelVPlanHCFGBuilderHIR.h"
#include "IntelVPLoopRegionHIR.h"
#include "IntelVPlanBuilderHIR.h"
#include "IntelVPlanDecomposerHIR.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRDDAnalysis.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRSafeReductionAnalysis.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/HLNodeUtils.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/HLNodeVisitor.h"
#include "llvm/Pass.h"
#include "llvm/PassAnalysisSupport.h"

#define DEBUG_TYPE "VPlanHCFGBuilder"

using namespace llvm;
using namespace vpo;

// Build plain CFG from incomming IR using only VPBasicBlock's that contain
// VPInstructions. Return VPRegionBlock that encloses all the VPBasicBlock's of
// the plain CFG.
class PlainCFGBuilderHIR : public HLNodeVisitorBase {
  friend HLNodeVisitor<PlainCFGBuilderHIR, false /*Recursive*/>;

private:
  /// Outermost loop of the input loop nest.
  HLLoop *TheLoop;

  VPlan *Plan;

  /// Map between loop header VPBasicBlock's and their respective HLLoop's. It
  /// is populated in this phase to keep the information necessary to create
  /// VPLoopRegionHIR's later in the H-CFG construction process.
  SmallDenseMap<VPBasicBlock *, HLLoop *> &Header2HLLoop;

  /// Output TopRegion.
  VPRegionBlock *TopRegion = nullptr;
  /// Number of VPBasicBlocks in TopRegion.
  unsigned TopRegionSize = 0;

  /// Hold the set of dangling predecessors to be connected to the next active
  /// VPBasicBlock.
  std::deque<VPBasicBlock *> Predecessors;

  /// Hold a pointer to the current HLLoop being processed.
  HLLoop *CurrentHLp = nullptr;

  /// Hold the VPBasicBlock that is being populated with instructions. Null
  /// value indicates that a new active VPBasicBlock has to be created.
  VPBasicBlock *ActiveVPBB = nullptr;

  /// Hold the VPBasicBlock that will be used as a landing pad for loops with
  /// multiple exits. If the loop is a single-exit loop, no landing pad
  /// VPBasicBlock is created.
  VPBasicBlock *MultiExitLandingPad = nullptr;

  /// Map between HLNode's that open a VPBasicBlock and such VPBasicBlock's.
  DenseMap<HLNode *, VPBasicBlock *> HLN2VPBB;

  /// Utility to create VPInstructions out of a HLNode.
  VPDecomposerHIR Decomposer;

  VPBasicBlock *getOrCreateVPBB(HLNode *HNode = nullptr);
  void connectVPBBtoPreds(VPBasicBlock *VPBB);
  void updateActiveVPBB(HLNode *HNode = nullptr, bool IsPredecessor = true);

  // Visitor methods
  void visit(const HLNode *Node) {}
  void postVisit(const HLNode *Node) {}

  void visit(HLLoop *HLp);
  void visit(HLIf *HIf);
  void visit(HLSwitch *HSw) {
    llvm_unreachable("Switches are not supported yet.");
  };
  void visit(HLInst *HInst);
  void visit(HLGoto *HGoto);
  void visit(HLLabel *HLabel);

public:
  PlainCFGBuilderHIR(HLLoop *Lp, const DDGraph &DDG, VPlan *Plan,
                     SmallDenseMap<VPBasicBlock *, HLLoop *> &H2HLLp)
      : TheLoop(Lp), Plan(Plan), Header2HLLoop(H2HLLp),
        Decomposer(Plan, Lp, DDG) {}

  /// Build a plain CFG for an HLLoop loop nest. Return the TopRegion containing
  /// the plain CFG.
  VPRegionBlock *buildPlainCFG();

  /// Convert incoming loop entities to the VPlan format.
  void
  convertEntityDescriptors(llvm::loopopt::HIRSafeReductionAnalysis *SRA,
                           VPlanHCFGBuilder::VPLoopEntityConverterList &CvtVec);
};

/// Retrieve an existing VPBasicBlock for \p HNode. It there is no existing
/// VPBasicBlock, a new VPBasicBlock is created and mapped to \p HNode. If \p
/// HNode is null, the new VPBasicBlock is not mapped to any HLNode.
VPBasicBlock *PlainCFGBuilderHIR::getOrCreateVPBB(HLNode *HNode) {

  // Auxiliary function that creates an empty VPBasicBlock, set its parent to
  // TopRegion and increases TopRegion's size.
  auto createVPBB = [&]() -> VPBasicBlock * {
    VPBasicBlock *NewVPBB =
        new VPBasicBlock(VPlanUtils::createUniqueName("BB"));
    NewVPBB->setParent(TopRegion);
    ++TopRegionSize;

    return NewVPBB;
  };

  if (!HNode) {
    // No HLNode associated to this VPBB.
    return createVPBB();
  } else {
    // Try to retrieve existing VPBB for this HLNode. Otherwise, create a new
    // VPBB and add it to the map.
    auto BlockIt = HLN2VPBB.find(HNode);

    if (BlockIt == HLN2VPBB.end()) {
      // New VPBB
      // TODO: Print something more useful.
      LLVM_DEBUG(dbgs() << "Creating VPBasicBlock for " << HNode->getNumber()
                        << "\n");
      VPBasicBlock *VPBB = createVPBB();
      HLN2VPBB[HNode] = VPBB;
      // NewVPBB->setOriginalBB(BB);
      return VPBB;
    } else {
      // Retrieve existing VPBB
      return BlockIt->second;
    }
  }
}

/// Connect \p VPBB to all the predecessors in Predecessors and clear
/// Predecessors.
void PlainCFGBuilderHIR::connectVPBBtoPreds(VPBasicBlock *VPBB) {

  for (VPBasicBlock *Pred : Predecessors) {
    Pred->appendSuccessor(VPBB);
    VPBB->appendPredecessor(Pred);
  }

  Predecessors.clear();
}

// Update active VPBasicBlock only when this is null. It creates a new active
// VPBasicBlock, connect it to existing predecessors, set it as new insertion
// point in VPHIRBUilder and, if \p ISPredecessor is true, add it as predecessor
// of the (future) subsequent active VPBasicBlock's.
void PlainCFGBuilderHIR::updateActiveVPBB(HLNode *HNode, bool IsPredecessor) {
  if (!ActiveVPBB) {
    ActiveVPBB = getOrCreateVPBB(HNode);
    connectVPBBtoPreds(ActiveVPBB);

    if (IsPredecessor)
      Predecessors.push_back(ActiveVPBB);
  }
}

void PlainCFGBuilderHIR::visit(HLLoop *HLp) {
  assert((HLp->isDo() || HLp->isDoMultiExit()) && HLp->isNormalized() &&
         "Unsupported HLLoop type.");
  // Set HLp as current loop before we visit its children.
  HLLoop *PrevCurrentHLp = CurrentHLp;
  CurrentHLp = HLp;

  // TODO: Print something more useful.
  LLVM_DEBUG(dbgs() << "Visiting HLLoop: " << HLp->getNumber() << "\n");

  // - ZTT for inner loops -
  // TODO: isInnerMost(), ztt_pred_begin/end

  // - Loop PH -
  // Force creation of a new VPBB for PH.
  ActiveVPBB = nullptr;

  // Visit loop PH only if the loop is not the outermost loop we are
  // vectorizing. DDGraph doesn't include outermost loop PH and Exit at this
  // point so we push them outside of the region represented in VPlan.
  if (HLp != TheLoop && HLp->hasPreheader()) {
    HLNodeUtils::visitRange<false /*Recursive*/>(
        *this /*visitor*/, HLp->pre_begin(), HLp->pre_end());

    assert(ActiveVPBB == HLN2VPBB[&*HLp->pre_begin()] &&
           "Loop PH generates more than one VPBB?");
  } else
    // There is no PH in HLLoop. Create dummy VPBB as PH. We could introduce
    // this dummy VPBB in simplifyPlainCFG, but according to the design for
    // LLVM-IR, we expect to have a loop with a PH as input. It's then better to
    // introduce the dummy PH here.
    updateActiveVPBB();

  VPBasicBlock *Preheader = ActiveVPBB;

  // - Loop Body -
  if (HLp->isMultiExit()) {
    // FIXME: In outer loop vectorization scenarios, more than one loop can be a
    // multi-exit loop. We need to use a stack to store the landing pad of each
    // multi-exit loop in the loop nest.
    assert(!MultiExitLandingPad && "Only one multi-exit loops is supported!");
    // Create a new landing pad for all the multiple exits.
    MultiExitLandingPad = getOrCreateVPBB();
  }

  // Force creation of a new VPBB for loop H.
  ActiveVPBB = nullptr;
  updateActiveVPBB();
  VPBasicBlock *Header = ActiveVPBB;
  assert(Header && "Expected VPBasicBlock for loop header.");

  // Map loop header VPBasicBlock with HLLoop for later loop region detection.
  Header2HLLoop[Header] = HLp;

  // Materialize the Loop IV and IV Start.
  Decomposer.createLoopIVAndIVStart(HLp, Preheader);

  // Visit loop body
  HLNodeUtils::visitRange<false /*Recursive*/>(
      *this /*visitor*/, HLp->child_begin(), HLp->child_end());

  // Loop latch: an HLoop will always have a single latch that will also be an
  // exiting block. Keep track of it. If there is no active VPBB, we have to
  // create a new one.
  updateActiveVPBB();
  VPBasicBlock *Latch = ActiveVPBB;

  // Materialize IV Next and bottom test in the loop latch. Connect Latch to
  // Header and set Latch condition bit.
  VPValue *LatchCondBit =
      Decomposer.createLoopIVNextAndBottomTest(HLp, Preheader, Latch);
  VPBlockUtils::connectBlocks(Latch, Header);
  Latch->setCondBit(LatchCondBit, Plan);

  // - Loop Exits -
  // Force creation of a new VPBB for Exit.
  ActiveVPBB = nullptr;

  // Visit loop Exit only if the loop is not the outermost loop we are
  // vectorizing. DDGraph doesn't include outermost loop PH and Exit at this
  // point so we push them outside of the region represented in VPlan.
  if (HLp != TheLoop && HLp->hasPostexit()) {
    HLNodeUtils::visitRange<false /*Recursive*/>(
        *this /*visitor*/, HLp->post_begin(), HLp->post_end());

    assert(ActiveVPBB == HLN2VPBB[&*HLp->post_begin()] &&
           "Loop Exit generates more than one VPBB?");
  } else
    // There is no Exit in HLLoop. Create dummy VPBB as Exit (see comment for
    //  dummy PH).
    updateActiveVPBB();

  if (HLp->isMultiExit()) {
    // Connect loop's regular exit to multi-exit landing pad and set landing pad
    // as new predecessor for subsequent VPBBs.
    connectVPBBtoPreds(MultiExitLandingPad);
    Predecessors.push_back(MultiExitLandingPad);
    ActiveVPBB = MultiExitLandingPad;
  }

  // Restore previous current HLLoop.
  CurrentHLp = PrevCurrentHLp;
}

void PlainCFGBuilderHIR::visit(HLIf *HIf) {

  // - Condition -
  // We do not create a new active  VPBasicBlock for HLIf predicates
  // (condition). We reuse the previous one (if possible).
  updateActiveVPBB(HIf);
  VPBasicBlock *ConditionVPBB = ActiveVPBB;

  // Create (single, not decomposed) VPInstruction for HLIf's predicate and set
  // it as condition bit of the active VPBasicBlock.
  // TODO: Remove "not decomposed" when decomposing HLIfs.
  VPInstruction *CondBit =
      Decomposer.createVPInstructionsForNode(HIf, ActiveVPBB);
  ConditionVPBB->setCondBit(CondBit, Plan);

  // - Then branch -
  // Force creation of a new VPBB for Then branch even if the Then branch has no
  // children.
  ActiveVPBB = nullptr;
  updateActiveVPBB();
  HLNodeUtils::visitRange<false /*Recursive*/>(
      *this /*visitor*/, HIf->then_begin(), HIf->then_end());

  // - Else branch -
  if (HIf->hasElseChildren()) {
    // Hold predecessors from Then branch to be used after HLIf visit and before
    // visiting else branch.
    SmallVector<VPBasicBlock *, 2> ThenOutputPreds(Predecessors.begin(),
                                                   Predecessors.end());
    // Clear Predecessors before Else branch visit (we don't want to connect
    // Then branch VPBasicBlock's with Else branch VPBasicBlock's) and add HLIf
    // condition as new predecessor for Else branch.
    Predecessors.clear();
    Predecessors.push_back(ConditionVPBB);

    // Force creation of a new VPBB for Else branch.
    ActiveVPBB = nullptr;
    HLNodeUtils::visitRange<false /*Recursive*/>(
        *this /*visitor*/, HIf->else_begin(), HIf->else_end());

    // Prepend predecessors generated by Then branch to those in Predecessors
    // from Else branch.
    // to be used after HLIf visit.
    Predecessors.insert(Predecessors.begin(), ThenOutputPreds.begin(),
                        ThenOutputPreds.end());
  } else {
    // No Else branch

    // Add ConditionVPBB to Predecessors for HLIf successor. Predecessors
    // contains predecessors from Then branch.
    // TODO: In this order? back or front?
    Predecessors.push_back(ConditionVPBB);
  }

  // Force the creation of a new VPBB for the next HLNode.
  ActiveVPBB = nullptr;
}

void PlainCFGBuilderHIR::visit(HLInst *HInst) {
  // Create new VPBasicBlock if there isn't a reusable one.
  updateActiveVPBB(HInst);

  // Create VPInstructions for HInst.
  Decomposer.createVPInstructionsForNode(HInst, ActiveVPBB);
}

void PlainCFGBuilderHIR::visit(HLGoto *HGoto) {

  // If there is an ActiveVPBB we have to remove it from Predecessors. HLGoto's
  // VPBB and HLLabel's VPBB are connected explicitly in this visit function
  // because they "break" the expected topological order traversal and,
  // therefore, need special treatment.
  if (ActiveVPBB) {
    // If this assert is raised, we would have to remove ActiveVPBB using
    // find/erase (more expensive).
    assert(Predecessors.back() == ActiveVPBB &&
           "Expected ActiveVPBB at the end of Predecessors.");
    Predecessors.pop_back();
  }

  // Create new VPBasicBlock if there isn't a reusable one. If a new ActiveVPBB
  // is created, do not add it to Predecessors (see previous comment).
  updateActiveVPBB(HGoto, false /*IsPredecessor*/);

  HLLabel *Label = HGoto->getTargetLabel();
  VPBasicBlock *LabelVPBB;
  if (HGoto->isExternal() || !HLNodeUtils::contains(CurrentHLp, Label)) {
    // Exiting goto in multi-exit loop. Use multi-exit landing pad as successor
    // of the goto VPBB.
    // TODO: When dealing with multi-loop H-CFGs, landing pad needs to properly
    // dispatch exiting gotos when labels have representation in VPlan. That
    // massaging should happen as a separate simplification step. Currently, all
    // the exiting gotos would go to the landing pad.
    assert(CurrentHLp->isDoMultiExit() && "Expected multi-exit loop!");
    assert(MultiExitLandingPad && "Expected landing pad for multi-exit loop!");

    Decomposer.createVPInstructionsForNode(HGoto, ActiveVPBB);
    LabelVPBB = MultiExitLandingPad;
  } else {
    assert(Label && "Label can't be null!");
    // Goto inside the loop. Create (or get) a new VPBB for HLLabel
    LabelVPBB = getOrCreateVPBB(Label);
  }

  // Connect to HLGoto's VPBB to HLLabel's VPBB.
  VPBlockUtils::connectBlocks(ActiveVPBB, LabelVPBB);

  // Force the creation of a new VPBasicBlock for the next HLNode.
  ActiveVPBB = nullptr;
}

void PlainCFGBuilderHIR::visit(HLLabel *HLabel) {
  // Force the creation of a new VPBasicBlock for an HLLabel.
  ActiveVPBB = nullptr;
  updateActiveVPBB(HLabel);
}

VPRegionBlock *PlainCFGBuilderHIR::buildPlainCFG() {
  // Create new TopRegion.
  TopRegion = new VPRegionBlock(VPBlockBase::VPRegionBlockSC,
                                VPlanUtils::createUniqueName("region"));

  // Create a dummy VPBB as TopRegion's Entry.
  assert(!ActiveVPBB && "ActiveVPBB must be null.");
  updateActiveVPBB();
  TopRegion->setEntry(ActiveVPBB);

  // Trigger the visit of the loop nest.
  visit(TheLoop);

  // Create a dummy VPBB as TopRegion's Exit.
  ActiveVPBB = nullptr;
  updateActiveVPBB();

  // At this point, all the VPBasicBlocks have been built and all the
  // VPInstructions have been created for the loop nest. It's time to fix
  // VPInstructions representing a PHI operation.
  Decomposer.fixPhiNodes();

  TopRegion->setExit(ActiveVPBB);
  TopRegion->setSize(TopRegionSize);

  return TopRegion;
}

VPlanHCFGBuilderHIR::VPlanHCFGBuilderHIR(const WRNVecLoopNode *WRL, HLLoop *Lp,
                                         VPlan *Plan,
                                         HIRSafeReductionAnalysis *SRA,
                                         const DDGraph &DDG)
    : VPlanHCFGBuilder(nullptr, nullptr, nullptr,
                       Lp->getHLNodeUtils().getDataLayout(), WRL, Plan,
                       nullptr),
      TheLoop(Lp), DDG(DDG), SRA(SRA) {
  Verifier = new VPlanVerifierHIR(Lp);
  assert((!WRLp || WRLp->getTheLoop<HLLoop>() == TheLoop) &&
         "Inconsistent Loop information");
}

/// Class implements input iterator for reductions. The input is done
/// from HIRSafeReductionAnalysis object.
class ReductionInputIteratorHIR {
  using RecurrenceKind = VPReduction::RecurrenceKind;
  using MinMaxRecurrenceKind = VPReduction::MinMaxRecurrenceKind;

public:
  class ReductionDescriptorHIR {
    using DataType = SafeRedChain::value_type;
    friend class ReductionInputIteratorHIR;

  public:
    ReductionDescriptorHIR() { clear(); }

    DataType getHLInst() const { return HLInst; }
    RecurrenceKind getKind() const { return RKind; }
    MinMaxRecurrenceKind getMinMaxKind() const { return MK; }
    Type *getRedType() const { return RedType; }
    bool getSigned() const { return Signed; }

  private:
    void clear() {
      HLInst = nullptr;
      RKind = RecurrenceKind::RK_NoRecurrence;
      MK = MinMaxRecurrenceKind::MRK_Invalid;
      RedType = nullptr;
      Signed = false;
      ;
    }

    DataType HLInst;
    RecurrenceKind RKind;
    MinMaxRecurrenceKind MK;
    Type *RedType;
    bool Signed;
  };

  using iterator_category = std::input_iterator_tag;
  using value_type = ReductionDescriptorHIR;
  using const_pointer = const ReductionDescriptorHIR *;
  using const_reference = const ReductionDescriptorHIR &;

  /// Constructor. The \p begin defines for which point the iterator is created,
  /// either for the beginning of the sequence or for the end.
  ReductionInputIteratorHIR(bool Begin, const SafeRedInfoList &SRCL) {
    /// Obtain iterators of the three lists mentioned above.
    ChainCurrent = Begin ? SRCL.begin() : SRCL.end();
    ChainEnd = SRCL.end();
    resetRedIterators();
    fillData();
  }

  inline bool operator==(const ReductionInputIteratorHIR &R) const {
    return ChainCurrent == R.ChainCurrent && ChainEnd == R.ChainEnd &&
           RedCurrent == R.RedCurrent && RedEnd == R.RedEnd;
  }
  inline bool operator!=(const ReductionInputIteratorHIR &R) const {
    return !operator==(R);
  }
  inline const_reference operator*() { return Descriptor; }
  inline const_pointer operator->()  { return &operator*(); }

  ReductionInputIteratorHIR &operator++() {
    advance();
    return *this;
  }

private:
  /// Move the iterator forward.
  void advance() {
    if (RedCurrent != RedEnd)
      RedCurrent++;
    if (RedCurrent == RedEnd) {
      if (ChainCurrent != ChainEnd) {
        ChainCurrent++;
        resetRedIterators();
      } else
        llvm_unreachable("Can't advance iterator");
    }
    fillData();
  }

  void resetRedIterators() {
    RedCurrent = RedEnd = nullptr;
    while (ChainCurrent != ChainEnd) {
      RedCurrent = ChainCurrent->Chain.begin();
      RedEnd = ChainCurrent->Chain.end();
      if (RedCurrent != RedEnd) {
        // TODO: the only last statement in reduction chain is decomposed
        // as reduction, i.e. has a PHI instruction. Probably, it's ok but
        // need to investigate whether we need other statements as reductions.
        RedCurrent = RedEnd;
        RedCurrent--;
        fillReductionKinds();
        break;
      }
      ChainCurrent++;
    }
  }

  void fillData() {
    if (RedCurrent != RedEnd) {
      Descriptor.HLInst = *RedCurrent;
    }
  }

  void fillReductionKinds() {
    Descriptor.MK = MinMaxRecurrenceKind::MRK_Invalid;
    Descriptor.RedType = (*RedCurrent)->getLvalDDRef()->getDestType();
    Descriptor.Signed = false;
    switch (ChainCurrent->OpCode) {
    case Instruction::FAdd:
    case Instruction::FSub:
      Descriptor.RKind = RecurrenceKind::RK_FloatAdd;
      break;
    case Instruction::Add:
    case Instruction::Sub:
      Descriptor.RKind = RecurrenceKind::RK_IntegerAdd;
      break;
    case Instruction::FMul:
      Descriptor.RKind = RecurrenceKind::RK_FloatMult;
      break;
    case Instruction::Mul:
      Descriptor.RKind = RecurrenceKind::RK_IntegerMult;
      break;
    case Instruction::And:
      Descriptor.RKind = RecurrenceKind::RK_IntegerAnd;
      break;
    case Instruction::Or:
      Descriptor.RKind = RecurrenceKind::RK_IntegerOr;
      break;
    case Instruction::Xor:
      Descriptor.RKind = RecurrenceKind::RK_IntegerXor;
      break;
    case Instruction::Select: {
      if (Descriptor.RedType->isIntegerTy()) {
        Descriptor.RKind = RecurrenceKind::RK_IntegerMinMax;
      } else {
        assert(Descriptor.RedType->isFloatingPointTy() &&
               "Floating point type expected at this point!");
        Descriptor.RKind = RecurrenceKind::RK_FloatMinMax;
      }
      PredicateTy Pred = (*RedCurrent)->getPredicate();
      bool isMax = (*RedCurrent)->isMax();
      switch (Pred) {
      case PredicateTy::ICMP_SGE:
      case PredicateTy::ICMP_SGT:
      case PredicateTy::ICMP_SLE:
      case PredicateTy::ICMP_SLT:
        Descriptor.MK = isMax ? MinMaxRecurrenceKind::MRK_SIntMax
                              : MinMaxRecurrenceKind::MRK_SIntMin;
        Descriptor.Signed = true;
        break;
      case PredicateTy::ICMP_UGE:
      case PredicateTy::ICMP_UGT:
      case PredicateTy::ICMP_ULE:
      case PredicateTy::ICMP_ULT:
        Descriptor.MK = isMax ? MinMaxRecurrenceKind::MRK_UIntMax
                              : MinMaxRecurrenceKind::MRK_UIntMin;
        break;
      default:
        Descriptor.MK = isMax ? MinMaxRecurrenceKind::MRK_FloatMax
                              : MinMaxRecurrenceKind::MRK_FloatMin;
        break;
      }
    }
    default:
      llvm_unreachable("Unexpected reduction opcode");
      break;
    }
  }

private:
  ReductionDescriptorHIR Descriptor;
  SafeRedInfoList::const_iterator ChainCurrent;
  SafeRedInfoList::const_iterator ChainEnd;
  SafeRedChain::const_iterator RedCurrent;
  SafeRedChain::const_iterator RedEnd;
};

// Base class for VPLoopEntity conversion functors.
class VPEntityConverterBase {
public:
  using InductionList = VPDecomposerHIR::VPInductionHIRList;
  using ReductionList = VPOVectorizationLegality::ReductionList;

  VPEntityConverterBase(VPDecomposerHIR &Decomp) : Decomposer(Decomp) {}

protected:
  VPDecomposerHIR &Decomposer;
};

/// Convert the data from auto-recognized induction list.
class InductionListCvt : public VPEntityConverterBase {
public:
  InductionListCvt(VPDecomposerHIR &Decomp) : VPEntityConverterBase(Decomp) {}

  void operator()(InductionDescr &Descriptor,
                  const InductionList::value_type &CurValue) {
    VPDecomposerHIR::VPInductionHIR *ID = CurValue.get();
    Descriptor.setInductionBinOp(ID->getUpdateInstr());
    Descriptor.setBinOpcode(Instruction::BinaryOpsEnd);
    Type *IndTy = Descriptor.getInductionBinOp()->getType();
    if (IndTy->isIntegerTy())
      Descriptor.setKind(VPInduction::InductionKind::IK_IntInduction);
    else if (IndTy->isPointerTy())
      Descriptor.setKind(VPInduction::InductionKind::IK_PtrInduction);
    else if (IndTy->isFloatingPointTy())
      Descriptor.setKind(VPInduction::InductionKind::IK_FpInduction);
    else
      llvm_unreachable("Unsupported induction data type.");
    Descriptor.setStartPhi(nullptr);
    Descriptor.setStart(ID->getStart());
    Descriptor.setStep(ID->getStep());
    Descriptor.setAllocaInst(nullptr);
  }
};

/// Convert data from auto-recognized reductions list.
class ReductionListCvt : public VPEntityConverterBase {
public:
  ReductionListCvt(VPDecomposerHIR &Decomp) : VPEntityConverterBase(Decomp) {}

  void operator()(ReductionDescr &Descriptor,
                  const ReductionInputIteratorHIR::value_type &CurValue) {
    Descriptor.setExit(dyn_cast<VPInstruction>(
        Decomposer.getVPValueForNode(CurValue.getHLInst())));
    Descriptor.setStartPhi(nullptr);
    Descriptor.setStart(nullptr);
    Descriptor.setKind(CurValue.getKind());
    Descriptor.setMinMaxKind(CurValue.getMinMaxKind());
    Descriptor.setRecType(CurValue.getRedType());
    Descriptor.setSigned(CurValue.getSigned());
    Descriptor.setAllocaInst(nullptr);
    Descriptor.setLinkPhi(nullptr);
  }
};

class HLLoop2VPLoopMapper {
public:
  HLLoop2VPLoopMapper() = delete;
  explicit HLLoop2VPLoopMapper(
      const VPlan *Plan,
      SmallDenseMap<VPBasicBlock *, HLLoop *> Header2HLLoop) {

    std::function<void(const VPLoop *)> mapLoop2VPLoop =
        [&](const VPLoop *VPL) {
          const HLLoop *L = Header2HLLoop[cast<VPBasicBlock>(VPL->getHeader())];
          assert(L != nullptr && "Can't find Loop");
          LoopMap[L] = VPL;
          for (auto VLoop : *VPL)
            mapLoop2VPLoop(VLoop);
        };

    VPLoop *TopLoop = *(Plan->getVPLoopInfo()->begin());
    mapLoop2VPLoop(TopLoop);
  }

  const VPLoop *operator[](const HLLoop *L) const {
    auto Iter = LoopMap.find(L);
    return Iter == LoopMap.end() ? nullptr : Iter->second;
  }
protected:
  DenseMap<const HLLoop *, const VPLoop *> LoopMap;
};

typedef VPLoopEntitiesConverter<ReductionDescr, HLLoop,
                                HLLoop2VPLoopMapper> ReductionConverter;
typedef VPLoopEntitiesConverter<InductionDescr, HLLoop,
                                HLLoop2VPLoopMapper> InductionConverter;

void PlainCFGBuilderHIR::convertEntityDescriptors(
    llvm::loopopt::HIRSafeReductionAnalysis *SRA,
    VPlanHCFGBuilder::VPLoopEntityConverterList &CvtVec) {

  using InductionList = VPDecomposerHIR::VPInductionHIRList;

  ReductionConverter *RedCvt = new ReductionConverter(Plan);
  InductionConverter *IndCvt = new InductionConverter(Plan);

  for (auto LoopDescr = Header2HLLoop.begin(); LoopDescr != Header2HLLoop.end();
       LoopDescr++) {
    HLLoop *HL = LoopDescr->second;
    SRA->computeSafeReductionChains(HL);
    const SafeRedInfoList &SRCL = SRA->getSafeRedInfoList(HL);

    LLVM_DEBUG(
        dbgs() << "Found the following auto-recognized reductions in the loop "
                  "with header ";
        dbgs() << LoopDescr->first->getName() << "\n";
        for (auto &SafeRedInfo : SRCL)
          for (auto &HlInst : SafeRedInfo.Chain) {
            const VPInstruction *Inst =
              dyn_cast<VPInstruction>(Decomposer.getVPValueForNode(HlInst));
            Inst->dump();
          }
        );

    const InductionList &IL = Decomposer.getInductions(HL);
    iterator_range<InductionList::const_iterator> InducRange(IL.begin(), IL.end());
    InductionListCvt InducListCvt(Decomposer);
    auto InducPair = std::make_pair(InducRange, InducListCvt);
#if 0
    LinearList *LL = Legal->getLinears();
    iterator_range<LinearList::iterator> LinearRange(LL->begin(), LL->end());
    LinearListCvt LinListCvt(Decomposer);
#endif

    iterator_range<ReductionInputIteratorHIR> ReducRange(
                   ReductionInputIteratorHIR(true, SRCL), ReductionInputIteratorHIR(false, SRCL));
    ReductionListCvt RedListCvt(Decomposer);
    auto ReducPair = std::make_pair(ReducRange, RedListCvt);

    RedCvt->createDescrList(HL, ReducPair);
    IndCvt->createDescrList(HL, InducPair);
  }
  CvtVec.push_back(std::unique_ptr<VPLoopEntitiesConverterBase>(RedCvt));
  CvtVec.push_back(std::unique_ptr<VPLoopEntitiesConverterBase>(IndCvt));
}

VPRegionBlock *
VPlanHCFGBuilderHIR::buildPlainCFG(VPLoopEntityConverterList &CvtVec) {
  PlainCFGBuilderHIR PCFGBuilder(TheLoop, DDG, Plan, Header2HLLoop);
  VPRegionBlock *TopRegion = PCFGBuilder.buildPlainCFG();
  PCFGBuilder.convertEntityDescriptors(SRA, CvtVec);
  return TopRegion;
}

void VPlanHCFGBuilderHIR::passEntitiesToVPlan(VPLoopEntityConverterList &Cvts) {
  typedef VPLoopEntitiesConverterTempl<HLLoop2VPLoopMapper> BaseConverter;

  HLLoop2VPLoopMapper Mapper(Plan, Header2HLLoop);
  for (auto &Cvt : Cvts) {
    BaseConverter *Converter = dyn_cast<BaseConverter>(Cvt.get());
    Converter->passToVPlan(Plan, Mapper);
  }
}

VPLoopRegion *VPlanHCFGBuilderHIR::createLoopRegion(VPLoop *VPLp) {
  assert(isa<VPBasicBlock>(VPLp->getHeader()) &&
         "Expected VPBasicBlock as Loop header.");
  HLLoop *HLLp = Header2HLLoop[cast<VPBasicBlock>(VPLp->getHeader())];
  assert(HLLp && "Expected HLLoop");
  VPLoopRegion *Loop =
      new VPLoopRegionHIR(VPlanUtils::createUniqueName("loop"), VPLp, HLLp);
  Loop->setReplicator(false /*IsReplicator*/);
  return Loop;
}
