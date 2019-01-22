//===------------------------------------------------------------*- C++ -*-===//
//
//   Copyright (C) 2017-2019 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_INTELVPINSTRUCTION_H
#define LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_INTELVPINSTRUCTION_H

#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"

namespace llvm {
namespace vpo {  // VPO Vectorizer Namespace

class VPValue {

private:
  const unsigned char VVID; // Subclass identifier (for isa/dyn_cast)

public:
  typedef enum {
    VPConstantSC,
    VPInstructionSC,
  } VPValueTy;

  VPValue(VPValueTy SC) : VVID(SC) {}
  unsigned char getVVID() const { return VVID; }
};

class VPConstant : public VPValue {

private:
  /// Pointer to LLVM IR Constant.
  Constant *ConstValue;

public:
  VPConstant(Constant *C) : VPValue(VPConstantSC), ConstValue(C) {}
  Constant *getConstant() const { return ConstValue; }
};

class VPInstruction : public VPValue,
                      public ilist_node<VPInstruction> { // Abstract class!
private:
  //  SmallVector<VPValue *, 3> Operands;
  //  unsigned OpCode;

  const unsigned char VIID; // Subclass identifier (for isa/dyn_cast)

public:
  typedef enum {
    VPInstructionIRSC,
  } VPInstructionTy;

  VPInstruction(VPInstructionTy SC) : VPValue(VPInstructionSC), VIID(SC) {}
  unsigned char getVIID() const { return VIID; }
};

class VPInstructionIR : public VPInstruction {

private:
  // Underlying IR instruction.
  Instruction *Inst;

public:
  VPInstructionIR(Instruction *Inst)
      : VPInstruction(VPInstructionIRSC), Inst(Inst) {}

  Instruction *getInstruction() const { return Inst; }

  // Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPInstruction *V) {
    return V->getVIID() == VPInstructionIRSC;
  }
};

typedef iplist<VPInstruction> VPInstructionContainerTy;
} // End VPO Vectorizer Namespace
} // end llvm namespace

#if 0
using namespace vpo;

// \brief Traits for iplist<VPInstruction>
// See ilist_traits<Instruction> in BasicBlock.h for details
template <>
struct ilist_traits<VPInstruction>
    : public ilist_default_traits<VPInstruction> {

  static VPInstruction *createNode(const VPInstruction &) {
    llvm_unreachable("VPInstruction should be explicitly created via Utils"
                     "class");

    return nullptr;
  }
  static void deleteNode(VPInstruction *) {}
};
#endif

#endif // LLVM_TRANSFORMS_VECTORIZE_INTEL_VPLAN_INTELVPINSTRUCTION_H
