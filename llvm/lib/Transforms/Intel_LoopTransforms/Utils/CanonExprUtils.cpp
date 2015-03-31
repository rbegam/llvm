//===--- CanonExprUtils.cpp - Implements CanonExprUtils class ----- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements CanonExprUtils class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Intel_LoopTransforms/Utils/CanonExprUtils.h"
#include "llvm/IR/Intel_LoopIR/CanonExpr.h"

using namespace llvm;
using namespace loopopt;

CanonExpr *CanonExprUtils::createCanonExpr(Type *Typ, int Level, int64_t Const,
                                           int64_t Denom) {

  return new CanonExpr(Typ, Level, Const, Denom);
}

void CanonExprUtils::destroy(CanonExpr *CE) { CE->destroy(); }

void CanonExprUtils::destroyAll() {
  CanonExpr::destroyAll();
  CanonExpr::BlobTable.clear();
}
