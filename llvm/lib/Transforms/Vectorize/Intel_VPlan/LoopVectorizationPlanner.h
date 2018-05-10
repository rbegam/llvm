//===-- LoopVectorizationPlanner.h ------------------------------*- C++ -*-===//
//
//   Copyright (C) 2016-2018 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation. and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines LoopVectorizationPlannerBase and LoopVectorizationPlanner.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_LOOPVECTORIZATIONPLANNER_H
#define LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_LOOPVECTORIZATIONPLANNER_H

#include "VPLoopAnalysis.h"
#include "VPlan.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/Support/CommandLine.h"

#if INTEL_CUSTOMIZATION
extern cl::opt<uint64_t> VPlanDefaultEstTrip;
#else
extern cl::opt<unsigned> VPlanDefaultEstTrip;
#endif // INTEL_CUSTOMIZATION

namespace llvm {
class Loop;
class ScalarEvolution;
class TargetLibraryInfo;
class TargetTransformInfo;

namespace loopopt {
class HLLoop;
class DDGraph;
}

using namespace llvm::loopopt;

namespace vpo {
class VPOCodeGen;
class VPOVectorizationLegality;
class WRNVecLoopNode;
class VPlanHCFGBuilder;
class VPlanCostModel;

/// LoopVectorizationPlanner - builds and optimizes the Vectorization Plans
/// which record the decisions how to vectorize the given loop.
/// In particular, represent the control-flow of the vectorized version,
/// the replication of instructions that are to be scalarized, and interleave
/// access groups.
class LoopVectorizationPlannerBase {
public:
  /// Build initial VPlans according to the information gathered by Legal
  /// when it checked if it is legal to vectorize this loop.
  /// Returns the number of VPlans built, zero if failed.
  unsigned buildInitialVPlans(void);

  virtual void collectDeadInstructions() {}

  /// On VPlan construction, each instruction marked for predication by Legal
  /// gets its own basic block guarded by an if-then. This initial planning
  /// is legal, but is not optimal. This function attempts to leverage the
  /// necessary conditional execution of the predicated instruction in favor
  /// of other related instructions. The function applies these optimizations
  /// to all VPlans.
  // void optimizePredicatedInstructions();

  /// Select the best plan and dispose all other VPlans.
  /// \Returns the selected vectorization factor.
  template <typename CostModelTy = VPlanCostModel>
  unsigned selectBestPlan(void);

  /// \brief Predicate all unique non-scalar VPlans
  void predicate(void);

  /// Generate the IR code for the body of the vectorized loop according to the
  /// best selected VPlan.
  // void executeBestPlan(InnerLoopVectorizer &LB);

  IntelVPlan *getVPlanForVF(unsigned VF) const {
    auto It = VPlans.find(VF);
    return It != VPlans.end() ? It->second.get() : nullptr;
  }

  bool hasVPlanForVF(const unsigned VF) const { return VPlans.count(VF) != 0; }

protected:
  LoopVectorizationPlannerBase(WRNVecLoopNode *WRL,
                               const TargetLibraryInfo *TLI,
                               const TargetTransformInfo *TTI,
                               VPOVectorizationLegality *Legal)
      : WRLp(WRL), TLI(TLI), TTI(TTI), Legal(Legal) {}
  virtual ~LoopVectorizationPlannerBase() {}

  /// Build an initial VPlan according to the information gathered by Legal
  /// when it checked if it is legal to vectorize this loop. \return a VPlan
  /// that corresponds to vectorization factors starting from the given
  /// \p StartRangeVF and up to \p EndRangeVF, exclusive, possibly decreasing
  /// the given \p EndRangeVF.
  // TODO: If this function becomes more complicated, move common code to base
  // class.
  virtual std::shared_ptr<IntelVPlan>
  buildInitialVPlan(unsigned StartRangeVF, unsigned &EndRangeVF) = 0;

  /// \Returns a pair of the <min, max> types' width used in the underlying loop.
  /// Doesn't take into account i1 type.
  virtual std::pair<unsigned, unsigned> getTypesWidthRangeInBits() const = 0;

  /// WRegion info of the loop we evaluate. It can be null.
  WRNVecLoopNode *WRLp;

  /// Target Library Info.
  const TargetLibraryInfo *TLI;

  /// Target Transform Info.
  const TargetTransformInfo *TTI;

  /// The legality analysis.
  // TODO: Turn into a reference when supported for HIR.
  LoopVectorizationLegality *Legal;

  /// This class is copied from open-source LoopVectorize.cpp and it's supposed
  /// to be temporal. VPO doesn't need it but we have it to minimize divergency
  /// with TransformState.
  struct VPCallbackILV : public VPCallback {

    ~VPCallbackILV() override {}

    Value *getOrCreateVectorValues(Value *V, unsigned Part) override {
      llvm_unreachable("Not implemented");
      return nullptr;
    }
  };

private:
  /// Determine whether \p I will be scalarized in a given range of VFs.
  /// The returned value reflects the result for a prefix of the range, with \p
  /// EndRangeVF modified accordingly.
  // bool willBeScalarized(Instruction *I, unsigned StartRangeVF,
  //                      unsigned &EndRangeVF);

  /// Iteratively sink the scalarized operands of a predicated instruction into
  /// the block that was created for it.
  // void sinkScalarOperands(Instruction *PredInst, VPlan *Plan);

  /// Determine whether a newly-created recipe adds a second user to one of the
  /// variants the values its ingredients use. This may cause the defining
  /// recipe to generate that variant itself to serve all such users.
  // void assignScalarVectorConversions(Instruction *PredInst, VPlan *Plan);

  /// The profitablity analysis.
  // LoopVectorizationCostModel *CM;

  // InnerLoopVectorizer *ILV = nullptr;

  // Holds instructions from the original loop that we predicated. Such
  // instructions reside in their own conditioned VPBasicBlock and represent
  // an optimization opportunity for sinking their scalarized operands thus
  // reducing their cost by the predicate's probability.
  // SmallPtrSet<Instruction *, 4> PredicatedInstructions;

  /// VPlans are shared between VFs, use smart pointers.
  DenseMap<unsigned, std::shared_ptr<IntelVPlan>> VPlans;

protected:
  unsigned BestVF = 0;
  unsigned BestUF = 0;
};

class LoopVectorizationPlanner : public LoopVectorizationPlannerBase {
public:
  LoopVectorizationPlanner(WRNVecLoopNode *WRL, Loop *Lp, LoopInfo *LI,
                           ScalarEvolution *SE, const TargetLibraryInfo *TLI,
                           const TargetTransformInfo *TTI,
                           class DominatorTree *DT,
                           VPOVectorizationLegality *Legal)
      : LoopVectorizationPlannerBase(WRL, TLI, TTI, Legal), TheLoop(Lp), LI(LI),
        SE(SE), DT(DT) {
    VPLA = std::make_shared<VPLoopAnalysis>(SE, VPlanDefaultEstTrip);
  }

  /// On VPlan construction, each instruction marked for predication by Legal
  /// gets its own basic block guarded by an if-then. This initial planning
  /// is legal, but is not optimal. This function attempts to leverage the
  /// necessary conditional execution of the predicated instruction in favor
  /// of other related instructions. The function applies these optimizations
  /// to all VPlans.
  // void optimizePredicatedInstructions();

  /// Record CM's decision and dispose of all other VPlans.
  // void setBestPlan(unsigned VF, unsigned UF);

  /// Generate the IR code for the body of the vectorized loop according to the
  /// best selected VPlan.
  void executeBestPlan(VPOCodeGen &LB);

  void collectDeadInstructions() override;

  /// Feed information from explicit clauses to the loop Legality.
  /// This information is necessary for initial loop analysis in the CodeGen.
  static void EnterExplicitData(WRNVecLoopNode *WRLp,
                                VPOVectorizationLegality &Legality);

private:
  std::shared_ptr<IntelVPlan> buildInitialVPlan(unsigned StartRangeVF,
                                                unsigned &EndRangeVF) override;

  std::pair<unsigned, unsigned> getTypesWidthRangeInBits(void) const final;

  /// Determine whether \p I will be scalarized in a given range of VFs.
  /// The returned value reflects the result for a prefix of the range, with
  /// \p
  /// EndRangeVF modified accordingly.
  // bool willBeScalarized(Instruction *I, unsigned StartRangeVF,
  //                      unsigned &EndRangeVF);

  /// Iteratively sink the scalarized operands of a predicated instruction
  /// into
  /// the block that was created for it.
  // void sinkScalarOperands(Instruction *PredInst, VPlan *Plan);

  /// Determine whether a newly-created recipe adds a second user to one of
  /// the
  /// variants the values its ingredients use. This may cause the defining
  /// recipe to generate that variant itself to serve all such users.
  // void assignScalarVectorConversions(Instruction *PredInst, VPlan *Plan);

  /// The loop that we evaluate.
  Loop *TheLoop;

  /// Loop Info analysis.
  LoopInfo *LI;

  /// Scalar Evolution analysis.
  ScalarEvolution *SE;

  /// The dominators tree.
  class DominatorTree *DT;

  /// VPLoop Analysis
  std::shared_ptr<VPLoopAnalysisBase> VPLA;

  /// The profitablity analysis.
  // LoopVectorizationCostModel *CM;

  // TODO: Move to base class
  VPOCodeGen *ILV = nullptr;

  // Holds instructions from the original loop that we predicated. Such
  // instructions reside in their own conditioned VPBasicBlock and represent
  // an optimization opportunity for sinking their scalarized operands thus
  // reducing their cost by the predicate's probability.
  // SmallPtrSet<Instruction *, 4> PredicatedInstructions;

  // Holds instructions from the original loop whose counterparts in the
  // vectorized loop would be trivially dead if generated. For example,
  // original induction update instructions can become dead because we
  // separately emit induction "steps" when generating code for the new loop.
  // Similarly, we create a new latch condition when setting up the structure
  // of the new loop, so the old one can become dead.
  SmallPtrSet<Instruction *, 4> DeadInstructions;
};

} // namespace vpo
} // namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_LOOPVECTORIZATIONPLANNER_H
