//===-------- DDUtils.h - Utilities for DD  -------------------------------===//
//
// Copyright (C) 2015-2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file defines the utilities for DDUtils class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_UTILS_DDUTILS_H
#define LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_UTILS_DDUTILS_H
#include "llvm/ADT/SmallSet.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRDDAnalysis.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Framework/HIRFramework.h"

namespace llvm {
namespace loopopt {

typedef SmallSet<unsigned, 16> InterchangeIgnorableSymbasesTy;

/// \brief Defines utilities for DDUtils Class
///
/// It contains a functions which analyze and manipulate DD
/// It does not store any state.
///
class HIRFramework;
class DDGraph;

class DDUtils {
private:
  /// \brief Do not allow instantiation
  DDUtils() = delete;
  friend class HIRParser;
  friend class HLNodeUtils;

public:
  /// \brief Any incoming/outgoing edge into Loop?
  static bool anyEdgeToLoop(DDGraph DDG, const DDRef *Ref, HLLoop *Loop);

  ///  \brief Update the linearity of DDRef when it becomes part of the
  ///  innermost loop
  ///  (as a result of ld/st movement or complete unrolling)
  ///  Current code only work on stmts inside the innermost loop
  static void updateDDRefsLinearity(SmallVectorImpl<HLInst *> &HLInsts,
                                    DDGraph DDG);

  /// \brief  Enables Perfect Loop Nests
  /// Takes care of simple cases that are needed for Interchange
  static bool
  enablePerfectLoopNest(HLLoop *InnermostLoop, DDGraph DDG,
                        SmallSet<unsigned, 16> &SinkedTempDDRefSymbases);
  /// \brief  Checks if a LvalRef has 'Threshold' uses in a loop
  static bool maxUsesInLoop(const RegDDRef *LvalRef, const HLLoop *Loop,
                            DDGraph DDG, const unsigned Threshold);
  /// \brief  Checks if a LvalRef has 1 single use in a loop
  static bool singleUseInLoop(const RegDDRef *LvalRef, const HLLoop *Loop,
                              DDGraph DDG);
  /// \brief  Checks if a DDRef is a valid reduction. It needs to match
  /// the symbase as well
  static bool isValidReductionDDRef(RegDDRef *RRef, HLLoop *Loop,
                                    unsigned FirstSymbase,
                                    bool *LastReductionInst, DDGraph DDG);
};
} // End namespace loopopt
} // End namespace llvm

#endif

// TODO:
// 1.Passing a DDGraph object is very expensive, try to use DDGraph& instead;
// 2.
// 3.
// 4.
// 5.
