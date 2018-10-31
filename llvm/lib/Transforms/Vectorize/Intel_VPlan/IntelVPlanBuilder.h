//===- VPlanBuilder.h - A VPlan utility for constructing VPInstructions ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file provides a VPlan-based builder utility analogous to IRBuilder.
/// It provides an instruction-level API for generating VPInstructions while
/// abstracting away the Recipe manipulation details.
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_INTELVPLANBUILDER_H
#define LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_INTELVPLANBUILDER_H

#include "IntelVPlan.h"

namespace llvm {
namespace vpo {    

class VPBuilder {
#if INTEL_CUSTOMIZATION
protected:
#else
private:
#endif
  VPBasicBlock *BB = nullptr;
  VPBasicBlock::iterator InsertPt = VPBasicBlock::iterator();

#if INTEL_CUSTOMIZATION
  VPInstruction *createInstruction(unsigned Opcode, Type *BaseTy,
                                   ArrayRef<VPValue *> Operands) {
    VPInstruction *Instr = new VPInstruction(Opcode, BaseTy, Operands);
    if (BB)
      BB->insert(Instr, InsertPt);
    return Instr;
  }

  VPInstruction *createInstruction(unsigned Opcode, Type *BaseTy,
                                   std::initializer_list<VPValue *> Operands) {
    return createInstruction(Opcode, BaseTy, ArrayRef<VPValue *>(Operands));
  }

  /// \brief Create VPCmpInst with its two operands.
  VPCmpInst *createCmpInst(CmpInst::Predicate Pred, VPValue *LeftOp,
                           VPValue *RightOp) {
    assert(LeftOp && RightOp && "VPCmpInst's operands can't be null!");
    VPCmpInst *Instr = new VPCmpInst(LeftOp, RightOp, Pred);
    if (BB)
      BB->insert(Instr, InsertPt);
    return Instr;
  }
#else
  VPInstruction *createInstruction(unsigned Opcode,
                                   std::initializer_list<VPValue *> Operands) {
    VPInstruction *Instr = new VPInstruction(Opcode, Operands);
    BB->insert(Instr, InsertPt);
    return Instr;
  }
#endif //INTEL_CUSTOMIZATION

public:
  VPBuilder() {}
#if INTEL_CUSTOMIZATION
  /// \brief Clear the insertion point: created instructions will not be
  /// inserted into a block.
  void clearInsertionPoint() {
    BB = nullptr;
    InsertPt = VPBasicBlock::iterator();
  }

  VPBasicBlock *getInsertBlock() const { return BB; }
  VPBasicBlock::iterator getInsertPoint() const { return InsertPt; }

  /// \brief Insert and return the specified instruction.
  VPInstruction *insert(VPInstruction *I) const {
    BB->insert(I, InsertPt);
    return I;
  }

  /// InsertPoint - A saved insertion point.
  class VPInsertPoint {
    VPBasicBlock *Block = nullptr;
    VPBasicBlock::iterator Point;

  public:
    /// \brief Creates a new insertion point which doesn't point to anything.
    VPInsertPoint() = default;

    /// \brief Creates a new insertion point at the given location.
    VPInsertPoint(VPBasicBlock *InsertBlock, VPBasicBlock::iterator InsertPoint)
        : Block(InsertBlock), Point(InsertPoint) {}

    /// \brief Returns true if this insert point is set.
    bool isSet() const { return (Block != nullptr); }

    VPBasicBlock *getBlock() const { return Block; }
    VPBasicBlock::iterator getPoint() const { return Point; }
  };

  /// \brief Sets the current insert point to a previously-saved location.
  void restoreIP(VPInsertPoint IP) {
    if (IP.isSet())
      setInsertPoint(IP.getBlock(), IP.getPoint());
    else
      clearInsertionPoint();
  }
#endif

  /// This specifies that created VPInstructions should be appended to the end
  /// of the specified block.
  void setInsertPoint(VPBasicBlock *TheBB) {
    assert(TheBB && "Attempting to set a null insert point");
    BB = TheBB;
    InsertPt = BB->end();
  }
#if INTEL_CUSTOMIZATION
  /// \brief This specifies that created instructions should be inserted
  /// before the specified instruction.
  void setInsertPoint(VPInstruction *I) {
    BB = I->getParent();
    InsertPt = I->getIterator();
  }

  /// \brief This specifies that created instructions should be inserted at the
  /// specified point.
  void setInsertPoint(VPBasicBlock *TheBB, VPBasicBlock::iterator IP) {
    BB = TheBB;
    InsertPt = IP;
  }

  // Create an N-ary operation with \p Opcode, \p Operands and set \p Inst as
  // its underlying Instruction.
  VPValue *createNaryOp(unsigned Opcode, Type *BaseTy,
                        ArrayRef<VPValue *> Operands,
                        Instruction *Inst = nullptr) {
    VPInstruction *NewVPInst = createInstruction(Opcode, BaseTy, Operands);
    NewVPInst->setUnderlyingValue(Inst);
    return NewVPInst;
  }
  VPValue *createNaryOp(unsigned Opcode, Type *BaseTy,
                        std::initializer_list<VPValue *> Operands,
                        Instruction *Inst = nullptr) {
    return createNaryOp(Opcode, BaseTy, ArrayRef<VPValue *>(Operands), Inst);
  }

  // Create a VPInstruction with \p LHS and \p RHS as operands and Add opcode.
  // For now, no no-wrap flags are used since they cannot be modeled in VPlan.
  VPValue *createAdd(VPValue *LHS, VPValue *RHS) {
    return createInstruction(Instruction::BinaryOps::Add, LHS->getBaseType(),
                             {LHS, RHS});
  }

  VPValue *createAnd(VPValue *LHS, VPValue *RHS) {
    return createInstruction(Instruction::BinaryOps::And, LHS->getBaseType(),
                             {LHS, RHS});
  }

#else

  VPValue *createNot(VPValue *Operand) {
    return createInstruction(VPInstruction::Not, {Operand});
  }

  VPValue *createAnd(VPValue *LHS, VPValue *RHS) {
    return createInstruction(Instruction::BinaryOps::And, {LHS, RHS});
  }

  VPValue *createOr(VPValue *LHS, VPValue *RHS) {
    return createInstruction(Instruction::BinaryOps::Or, {LHS, RHS});
  }
#endif //INTEL_CUSTOMIZATION

#if INTEL_CUSTOMIZATION
  // Create a VPCmpInst with \p LeftOp and \p RightOp as operands, and \p CI's
  // predicate as predicate. \p CI is also set as underlying Instruction.
  VPCmpInst *createCmpInst(VPValue *LeftOp, VPValue *RightOp, CmpInst *CI) {
    // TODO: If a null CI is needed, please create a new interface.
    assert(CI && "CI can't be null.");
    VPCmpInst *VPCI =
        createCmpInst(CI->getPredicate(), LeftOp, RightOp);
    VPCI->setUnderlyingValue(CI);
    return VPCI;
  }

  // Create dummy VPBranchInst instruction.
  VPBranchInst *createBr(Type *BaseTy) {
    VPBranchInst *Instr = new VPBranchInst(BaseTy);
    if (BB)
      BB->insert(Instr, InsertPt);
    return Instr;
  }

  VPInstruction *createPhiInstruction(Instruction *Inst) {
    assert(Inst != nullptr && "Instruction cannot be a nullptr");
    VPInstruction *NewVPInst = createPhiInstruction(Inst->getType());
    NewVPInst->setUnderlyingValue(Inst);
    if (BB)
      BB->insert(NewVPInst, InsertPt);
    return NewVPInst;
  }

  VPInstruction *createPhiInstruction(Type *BaseTy) {
    VPInstruction *NewVPInst = new VPPHINode(BaseTy);
    return NewVPInst;
  }
  //===--------------------------------------------------------------------===//
  // RAII helpers.
  //===--------------------------------------------------------------------===//

  // \brief RAII object that stores the current insertion point and restores it
  // when the object is destroyed.
  class InsertPointGuard {
    VPBuilder &Builder;
    // TODO: AssertingVH<VPBasicBlock> Block;
    VPBasicBlock* Block;
    VPBasicBlock::iterator Point;

  public:
    InsertPointGuard(VPBuilder &B)
        : Builder(B), Block(B.getInsertBlock()), Point(B.getInsertPoint()) {}

    InsertPointGuard(const InsertPointGuard &) = delete;
    InsertPointGuard &operator=(const InsertPointGuard &) = delete;

    ~InsertPointGuard() {
      Builder.restoreIP(VPInsertPoint(Block, Point));
    }
  };
#endif // INTEL_CUSTOMIZATION
};

} // namespace vpo
} // namespace llvm

#endif // LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_INTELVPLANBUILDER_H
