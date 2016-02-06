//===----------------------------------------------------------------------===//
//
//   Copyright (C) 2015 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation. and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//   Source file:
//   ------------
//   VPOAvrUtilsHIR.cpp -- Implements the Abstract Vector Representation (AVR)
//   utilities specific to HIR.
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/Intel_VPO/Vecopt/AVR/VPOAvrUtilsHIR.h"

#define DEBUG_TYPE "avr-utilities"

using namespace llvm;
using namespace llvm::vpo;
using namespace llvm::loopopt;

// AVR Creation Utilities
AVRAssignHIR *AVRUtilsHIR::createAVRAssignHIR(HLInst *Inst) {
  return new AVRAssignHIR(Inst);
}

AVRLabelHIR *AVRUtilsHIR::createAVRLabelHIR(HLNode *Inst) {
  return new AVRLabelHIR(Inst);
}

AVRBranchHIR *AVRUtilsHIR::createAVRBranchHIR(HLNode *Inst) {
  return new AVRBranchHIR(Inst);
}

AVRIfHIR *AVRUtilsHIR::createAVRIfHIR(HLIf *If) {
  return new AVRIfHIR(If);
}

AVRExpressionHIR *AVRUtilsHIR::createAVRExpressionHIR(AVRAssignHIR *HLAssign,
                                                      AssignOperand AOp) {
  return new AVRExpressionHIR(HLAssign, AOp);
}

AVRValueHIR *AVRUtilsHIR::createAVRValueHIR(RegDDRef *DDRef,
                                            HLInst *HLInstruct) {
  return new AVRValueHIR(DDRef, HLInstruct);
}
