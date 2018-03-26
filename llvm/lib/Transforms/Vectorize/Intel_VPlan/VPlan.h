//===------------------------------------------------------------*- C++ -*-===//
//
//   Copyright (C) 2017 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_PLAN_H
#define LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_PLAN_H

#include "../Intel_VPlan.h"
#include "VPLoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/GenericDomTreeConstruction.h"

namespace llvm {

// Forward declarations
class VPNewInstructionRangeRecipe;

namespace loopopt {
class HLLoop;
}

using namespace loopopt;

namespace vpo {

class VPOCodeGenHIR;

class VPMaskGenerationRecipe : public VPRecipeBase {
  friend class VPlanUtilsLoopVectorizer;

private:
  const Value *IncomingPred;
  const Value *LoopBackedge;

public:
  VPMaskGenerationRecipe(const Value *Pred, const Value *Backedge)
      : VPRecipeBase(VPMaskGenerationRecipeSC), IncomingPred(Pred),
        LoopBackedge(Backedge) {}

  ~VPMaskGenerationRecipe() {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPMaskGenerationRecipeSC;
  }

  /// Print the recipe.
  void print(raw_ostream &OS, const Twine &Indent) const override {
    OS << " +\n" << Indent << "\"MaskGeneration";
    OS << " " << *LoopBackedge << "\\l\"";
  }

#if INTEL_CUSTOMIZATION
  void dump(raw_ostream &OS) const override {
    OS << *LoopBackedge << "\n";
  }
  void dump() const override {
    dump(errs());
  }
#endif /* INTEL_CUSTOMIZATION */

  void execute(struct VPTransformState &State) override {
    // TODO: vectorizing this recipe should involve generating a mask for the
    // instructions in the loop body. i.e., a phi instruction that has incoming
    // values using IncomingPred and LoopBackedge.
  }
  void executeHIR(VPOCodeGenHIR *CG) override {}
};


class VPLoopRegion : public VPRegionBlock {
  friend class IntelVPlanUtils;

private:
  // Pointer to VPLoopInfo analysis information for this loop region
  VPLoop *VPLp;

protected:
  VPLoopRegion(const unsigned char SC, const std::string &Name, VPLoop *Lp)
      : VPRegionBlock(SC, Name), VPLp(Lp) {}

public:
  VPLoopRegion(const std::string &Name, VPLoop *Lp)
      : VPRegionBlock(VPLoopRegionSC, Name), VPLp(Lp) {}

  const VPLoop *getVPLoop() const { return VPLp; }
  VPLoop *getVPLoop() { return VPLp; }

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPBlockBase *B) {
    return B->getVPBlockID() == VPBlockBase::VPLoopRegionSC ||
           B->getVPBlockID() == VPBlockBase::VPLoopRegionHIRSC;
  }
};

/// \brief Specialization of VPLoopRegion that holds the HIR-specific loop
/// representation (HLLoop).
///
/// Design Principle: access to underlying IR is forbidden by default. Adding
/// new friends to this class to have access to it must be very well justified.
class VPLoopRegionHIR : public VPLoopRegion {
  friend class IntelVPlanUtils;
  friend class VPlanVerifierHIR;
  friend class VPDecomposerHIR;

private:
  // Pointer to the underlying HLLoop.
  HLLoop *HLLp;

protected:
  VPLoopRegionHIR(const std::string &Name, VPLoop *VPLp, HLLoop *HLLp)
      : VPLoopRegion(VPLoopRegionHIRSC, Name, VPLp), HLLp(HLLp) {}

  const HLLoop *getHLLoop() const { return HLLp; }
  HLLoop *getHLLoop() { return HLLp; }

public:
  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPBlockBase *B) {
    return B->getVPBlockID() == VPBlockBase::VPLoopRegionHIRSC;
  }
};

class IntelVPlan : public VPlan {

private:
  VPLoopInfo *VPLInfo;

public:
  IntelVPlan() : VPlan(IntelVPlanSC), VPLInfo(nullptr) {}

  ~IntelVPlan() {
    if (VPLInfo)
      delete (VPLInfo);
  }

  void execute(struct VPTransformState *State) override;
  void executeHIR(VPOCodeGenHIR *CG) override;

  VPLoopInfo *getVPLoopInfo() { return VPLInfo; }
  const VPLoopInfo *getVPLoopInfo() const { return VPLInfo; }

  void setVPLoopInfo(VPLoopInfo *VPLI) { VPLInfo = VPLI; }

  static inline bool classof(const VPlan *V) {
    return V->getVPlanID() == VPlan::IntelVPlanSC;
  }
};


/// A VPConstantRecipe is a recipe which represents a constant in VPlan.
/// This recipe represents a scalar integer w/o any relation to the source IR.
/// The usage of this recipe is mainly beneficial when we need to argue about
/// new recipes altering the original structure of the code and introducing new
/// commands. e.g. consider the single-exit loop massaging, we need to
/// represent a new \phi with respect to new constant values and compares to 
/// those same values.
class VPConstantRecipe : public VPRecipeBase {
public:
  VPConstantRecipe(int val) : VPRecipeBase(VPConstantSC), val(val) {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPConstantSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void execute(VPTransformState &State) override {}
  void executeHIR(VPOCodeGenHIR *CG) override {}

  Value *getValue(void) const {
    // TODO after vectorize.
    return nullptr;
  }

  /// Print the recipe.
  void print(raw_ostream &OS, const Twine &Indent) const override {
    OS << " +\n" << Indent << "\"Const " << val << "\\l\"";
  }

#if INTEL_CUSTOMIZATION
  void dump(raw_ostream &OS) const override {
    OS << val << "\n";;
  }
  void dump() const override {
    dump(errs());
  }
#endif /* INTEL_CUSTOMIZATION */

  StringRef getName() const { return "Constant: " + val; };

private:
  int val;
};

/// A VPPhiValueRecipe is a recipe which represents a new Phi in VPlan to 
/// facilitate the alteration of VPlan from its original source coded form.
/// Currently the elements of the phi are constants in-order to generate the
/// needed \phi for the single-exit loop massaging. However, this phi can be
/// further enhanced to handle any type of value.
class VPPhiValueRecipe : public VPRecipeBase {
public:
  VPPhiValueRecipe() : VPRecipeBase(VPPhiValueSC), Incoming(), Phi(nullptr) {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPLiveInBranchSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void execute(VPTransformState &State) override {}
  void executeHIR(VPOCodeGenHIR *CG) override {}

  /// Return the phi value after vectorization.
  Value *getValue(void) const {
    return Phi;
  }

  /// Adds a new element to the resulting \phi.
  void addIncomingValue(VPConstantRecipe IncomingValue,
    VPBlockBase* IncomingBlock) {
    Incoming.push_back(IncomingPair(IncomingValue, IncomingBlock));
  }

  /// Print the recipe.
  void print(raw_ostream &OS, const Twine &Indent) const override {
    OS << " +\n" << Indent << "\"Phi ";

    for (auto item : Incoming) {
      OS << "[";
      item.first.print(OS, Indent);
      OS << ", " << item.second->getName() << "] ";
    }

    OS << "\\l\"";
  }

#if INTEL_CUSTOMIZATION
  void dump(raw_ostream &OS) const override {
    OS << "Phi ";
    for (auto Item : Incoming) {
      Item.first.dump(OS);
      OS << ", " << Item.second->getName() << " ";
    }
    OS << "\n";
  }
  void dump() const override {
    dump(errs());
  }
#endif /* INTEL_CUSTOMIZATION */

  StringRef getName() const { return "Phi Recipe"; };

  ~VPPhiValueRecipe() {
    Phi->deleteValue();
  }

private:
  typedef std::pair<VPConstantRecipe , VPBlockBase *> IncomingPair;
  SmallVector<IncomingPair, 4> Incoming;
  Value* Phi;
};

/// ; IntelVPlanUtils class provides interfaces for the construction and
/// manipulation of a VPlan.
class IntelVPlanUtils : public VPlanUtils {

public:
  IntelVPlanUtils(IntelVPlan *Plan) : VPlanUtils(Plan) {}

  IntelVPlan *getVPlan() { return cast<IntelVPlan>(Plan); }

  /// Creates a new recipe that represents generation of an i1 vector to be used
  /// as a mask.
  VPMaskGenerationRecipe *createMaskGenerationRecipe(
    const Value *Pred, const Value *Backedge);

  /// Create a new VPIfTruePredicateRecipe.
  VPIfTruePredicateRecipe *
  createIfTruePredicateRecipe(VPValue *CV,
                              VPPredicateRecipeBase *PredecessorPredicate,
                              BasicBlock *From, BasicBlock *To) {
    VPIfTruePredicateRecipe *newRecipe =
        new VPIfTruePredicateRecipe(CV, PredecessorPredicate, From, To);
    newRecipe->setName(createUniqueName("IfT"));
    return newRecipe;
  }

  /// Create a new VPIfFalsePredicateRecipe.
  VPIfFalsePredicateRecipe *
  createIfFalsePredicateRecipe(VPValue *CV,
                               VPPredicateRecipeBase *PredecessorPredicate,
                               BasicBlock *From, BasicBlock *To) {
    VPIfFalsePredicateRecipe *newRecipe =
        new VPIfFalsePredicateRecipe(CV, PredecessorPredicate, From, To);
    newRecipe->setName(createUniqueName("IfF"));
    return newRecipe;
  }

  VPEdgePredicateRecipe *
  createEdgePredicateRecipe(VPPredicateRecipeBase *PredecessorPredicate,
                            BasicBlock *From, BasicBlock *To) {
    VPEdgePredicateRecipe *newRecipe =
        new VPEdgePredicateRecipe(PredecessorPredicate, From, To);
    newRecipe->setName(createUniqueName("AuxEdgeForMaskSetting"));
    return newRecipe;
  }
  /// Create a new VPBlockPredicateRecipe.
  VPBlockPredicateRecipe *createBlockPredicateRecipe(void) {
    VPBlockPredicateRecipe *newRecipe = new VPBlockPredicateRecipe();
    newRecipe->setName(createUniqueName("BP"));
    return newRecipe;
  }

  /// Returns true if the edge FromBlock->ToBlock is a back-edge.
  bool isBackEdge(const VPBlockBase *FromBlock, const VPBlockBase *ToBlock,
                  const VPLoopInfo *VPLI) {
    assert(FromBlock->getParent() == ToBlock->getParent() &&
           FromBlock->getParent() != nullptr && "Must be in same region");
    const VPLoop *FromLoop = VPLI->getLoopFor(FromBlock);
    const VPLoop *ToLoop = VPLI->getLoopFor(ToBlock);
    if (FromLoop == nullptr || ToLoop == nullptr || FromLoop != ToLoop) {
      return false;
    }
    // A back-edge is latch->header
    return (ToBlock == ToLoop->getHeader() && ToLoop->isLoopLatch(FromBlock));
  }

  /// Create a new and empty VPLoopRegion.
  VPLoopRegion *createLoopRegion(VPLoop *VPL) {
    assert (VPL && "Expected a valid VPLoop.");
    VPLoopRegion *Loop = new VPLoopRegion(createUniqueName("loop"), VPL);
    setReplicator(Loop, false /*IsReplicator*/);
    return Loop;
  }

  /// Create a new and empty VPLoopRegionHIR.
  VPLoopRegion *createLoopRegionHIR(VPLoop *VPL, HLLoop *HLLp) {
    assert (VPL && HLLp && "Expected a valid VPLoop and HLLoop.");
    VPLoopRegion *Loop =
        new VPLoopRegionHIR(createUniqueName("loop"), VPL, HLLp);
    setReplicator(Loop, false /*IsReplicator*/);
    return Loop;
  }

  /// Returns true if Block is a loop latch
  bool blockIsLoopLatch(const VPBlockBase *Block,
                        const VPLoopInfo *VPLInfo) const {

    if (const VPLoop *ParentVPL = VPLInfo->getLoopFor(Block)) {
      return ParentVPL->isLoopLatch(Block);
    }

    return false;
  }

  VPBasicBlock *splitBlock(VPBlockBase *Block, VPLoopInfo *VPLInfo,
                           VPDominatorTree &DomTree,
                           VPPostDominatorTree &PostDomTree);
};

} // namespace vpo
} // namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_PLAN_H 
