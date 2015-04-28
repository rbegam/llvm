//===-------- DDRefUtils.cpp - Implements DDRefUtils class ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements DDRefUtils class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Intel_LoopTransforms/Utils/DDRefUtils.h"

using namespace llvm;
using namespace loopopt;

RegDDRef *DDRefUtils::createRegDDRef(int SB) { return new RegDDRef(SB); }

BlobDDRef *DDRefUtils::createBlobDDRef(int SB, const CanonExpr *CE,
                                       RegDDRef *Parent) {

  return new BlobDDRef(SB, CE, Parent);
}

void DDRefUtils::destroy(DDRef *Ref) { Ref->destroy(); }

void DDRefUtils::destroyAll() { DDRef::destroyAll(); }
