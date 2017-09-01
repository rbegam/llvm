//===------------------------------------------------------------*- C++ -*-===//
//
//   Copyright (C) 2015-2016 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation. and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
///  \file
///  VPOAvrLoopHIR.h -- Defines the Abstract Vector Representation (AVR) loop
///  node for HIR.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_VPO_AVR_LOOP_HIR_H
#define LLVM_ANALYSIS_VPO_AVR_LOOP_HIR_H

#include "llvm/Analysis/Intel_VPO/Vecopt/VPOAvrLoop.h"
#include "llvm/Analysis/Intel_LoopAnalysis/IR/HLLoop.h"

using namespace llvm::loopopt;

namespace llvm { // LLVM Namespace
namespace vpo {  // VPO Vectorizer Namespace

/// \brief Loop node abstract vector representation
///
/// An AVRLoopIR node represents a loop found in LLVM IR.
class AVRLoopHIR : public AVRLoop {

private:
  /// Pointer to HIR loop node.
  HLLoop *HIRLoop;

protected:
  // Interface to create AVRLoop from LLVM Loop.
  AVRLoopHIR(HLLoop *Lp);

  // AvrLoop copy constructor.
  AVRLoopHIR(AVRLoopHIR &AVROrigLoop);

  virtual ~AVRLoopHIR() override {}

  /// \brief Set HIR Loop.
  void setLoop(HLLoop *Lp) { HIRLoop = Lp; }

  /// Only this utility class should be use to modify/delete AVR nodes.
  friend class AVRUtilsHIR;

public:
  AVRLoopHIR *clone() const override;

  /// \brief Method for supporting type inquiry through isa, cast, and dyn_cast.
  static bool classof(const AVR *Node) {
    return Node->getAVRID() == AVR::AVRLoopHIRNode;
  }

  /// \brief Returns HLLoop node.
  HLLoop *getLoop() const { return HIRLoop; }

  /// \brief Returns a constant StringRef for the type name of this node.
  virtual StringRef getAvrTypeName() const override;

  /// \brief Returns the value name of this node.
  virtual std::string getAvrValueName() const override;

  /// \brief Code generation for AVR loop.
  void codeGen() override;

  /// \brief Get maximum trip count estimate
  virtual uint64_t getMaxTripCountEstimate() const override {
    return HIRLoop->getMaxTripCountEstimate();
  }
};

} // End VPO Vectorizer Namespace
} // End LLVM Namespace

#endif // LLVM_ANALYSIS_VPO_AVR_LOOP_HIR_H
