//===- IntelVPlanBuilderHIR.h - A utility for constructing VPInstructions -===//
//
//   Copyright (C) 2017 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation. and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file extends VPlanBuilder utility to create VPInstruction from HIR.
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_VPLANHIR_INTELVPLANBUILDER_HIR_H
#define LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_VPLANHIR_INTELVPLANBUILDER_HIR_H

#include "../IntelVPlanBuilder.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/HLNodeUtils.h"

namespace llvm {
namespace vpo {

class VPBuilderHIR : public VPBuilder {
public:
  /// Create an N-ary operation with \p Opcode and \p Operands and set \p HInst
  /// as its VPInstructionData.
  VPValue *createNaryOp(unsigned Opcode, ArrayRef<VPValue *> Operands,
                        Type *BaseTy, loopopt::HLDDNode *DDNode = nullptr) {
    VPInstruction *NewVPInst =
        cast<VPInstruction>(VPBuilder::createNaryOp(Opcode, BaseTy, Operands));
    if (DDNode)
      NewVPInst->HIR.setUnderlyingNode(DDNode);
    return NewVPInst;
  }
  VPValue *createNaryOp(unsigned Opcode,
                        std::initializer_list<VPValue *> Operands, Type *BaseTy,
                        loopopt::HLDDNode *DDNode = nullptr) {
    return createNaryOp(Opcode, ArrayRef<VPValue *>(Operands), BaseTy, DDNode);
  }

  /// Create a VPInstruction with 'Add' opcode, \p LHS and \p RHS as operands
  /// and \p DDNode as its VPInstructionData.
  VPValue *createAdd(VPValue *LHS, VPValue *RHS, loopopt::HLDDNode *DDNode) {
    assert(DDNode && "DDNode can't be null.");
    auto *NewAdd = cast<VPInstruction>(VPBuilder::createAdd(LHS, RHS));
    NewAdd->HIR.setUnderlyingNode(DDNode);
    return NewAdd;
  }

  /// Create a VPCmpInst with \p LHS and \p RHS as operands, \p Pred as
  /// predicate and set \p DDNode as its VPInstructionData.
  VPCmpInst *createCmpInst(CmpInst::Predicate Pred, VPValue *LHS, VPValue *RHS,
                           loopopt::HLDDNode *DDNode) {
    assert(DDNode && "DDNode can't be null.");
    VPCmpInst *NewVPCmp = VPBuilder::createCmpInst(Pred, LHS, RHS);
    NewVPCmp->HIR.setUnderlyingNode(DDNode);
    return NewVPCmp;
  }

  /// Create a VPCmpInst with \p LHS and \p RHS as operands, \p Pred as
  /// predicate. This function provides a public wrapper interface to
  /// VPBuilder::createCmpInst in the derived class.
  /// NOTE: The base class versions of createCmpInst are hidden here so
  /// function overloading cannot be used.
  /// Reference:
  /// https://isocpp.org/wiki/faq/strange-inheritance#overload-derived
  VPCmpInst *createCmpInst(CmpInst::Predicate Pred, VPValue *LHS,
                           VPValue *RHS) {
    return VPBuilder::createCmpInst(Pred, LHS, RHS);
  }

  /// Create a semi-phi operation with \p Operands as reaching definitions.
  VPValue *createSemiPhiOp(Type *BaseTy, ArrayRef<VPValue *> Operands,
                           loopopt::HLDDNode *DDNode = nullptr) {
    // TODO: Enable assert, remove 'if' and invoke createPhi in super class for
    // semi-phis without underlying HIR when VPPhi representation is introduced.
    // assert(DDNode && "DDNode can't be null.");
    VPInstruction *NewSemiPhi =
        createInstruction(VPInstruction::SemiPhi, BaseTy, Operands);
    if (DDNode)
      NewSemiPhi->HIR.setUnderlyingNode(DDNode);
    return NewSemiPhi;
  }

  /// Create a semi-phi operation with \p Operands as reaching definitions.
  VPValue *createSemiPhiOp(Type *BaseTy,
                           std::initializer_list<VPValue *> Operands,
                           loopopt::HLDDNode *DDNode) {
    return createSemiPhiOp(BaseTy, ArrayRef<VPValue *>(Operands), DDNode);
  }

  // Construct VPBranchInst instruction from a \p Goto.
  VPBranchInst *createBr(Type *BaseTy, loopopt::HLGoto *Goto) {
    assert(Goto && "HLGoto must be passed to construct VPBranchInst.");
    VPBranchInst *BranchInst = VPBuilder::createBr(BaseTy);
    BranchInst->HIR.setUnderlyingNode(Goto);
    BranchInst->HIR.setValid();
    return BranchInst;
  }

  /// In the HIR-path we restrict creation of a VPGEPInstruction by making sure
  /// that GEP instructions can be created via the builder only with the base
  /// pointer operand. The index operands must be added subsequently by the
  /// client. This is needed to track the information about a given index
  /// operand being a trailing struct offset or not.

  /// Construct a GEP VPInstruction with type \p BaseTy and base pointer \p Ptr.
  VPInstruction *createGEP(Type *BaseTy, VPValue *Ptr) {
    VPInstruction *NewVPInst = new VPGEPInstruction(BaseTy, Ptr, {});
    if (BB)
      BB->insert(NewVPInst, InsertPt);
    return NewVPInst;
  }

  /// Construct an inbounds GEP VPInstruction with type \p BaseTy and base
  /// pointer \p Ptr.
  VPInstruction *createInBoundsGEP(Type *BaseTy, VPValue *Ptr) {
    VPInstruction *NewVPInst = createGEP(BaseTy, Ptr);
    cast<VPGEPInstruction>(NewVPInst)->setIsInBounds(true);
    return NewVPInst;
  }
};

} // namespace vpo
} // namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_VPLANHIR_INTELVPLANBUILDER_HIR_H
