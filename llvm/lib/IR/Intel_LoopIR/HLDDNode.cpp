//===--- HLDDNode.cpp - Implements the HLDDNode class ---------------------===//
//
// Copyright (C) 2015-2016 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements the HLDDNode class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Debug.h"

#include "llvm/Transforms/Intel_LoopTransforms/Utils/HLNodeUtils.h"
#include "llvm/IR/Intel_LoopIR/DDRef.h"
#include "llvm/IR/Intel_LoopIR/RegDDRef.h"
#include "llvm/IR/Intel_LoopIR/BlobDDRef.h"

using namespace llvm;
using namespace llvm::loopopt;

static cl::opt<bool>
    PrintConstDDRefs("hir-details-constants", cl::init(false), cl::Hidden,
                     cl::desc("Print constant DDRefs in detailed print"));

/// DDRefs are taken care of in the derived classes.
HLDDNode::HLDDNode(unsigned SCID) : HLNode(SCID) {}

/// DDRefs are taken care of in the derived classes.
HLDDNode::HLDDNode(const HLDDNode &HLDDNodeObj) : HLNode(HLDDNodeObj) {}

void HLDDNode::setNode(RegDDRef *Ref, HLDDNode *HNode) {
  Ref->setHLDDNode(HNode);
}

HLDDNode::ddref_iterator HLDDNode::ddref_begin() {
  HLLoop *HLoop;

  /// Skip null DDRefs for unknown loops
  if ((HLoop = dyn_cast<HLLoop>(this)) && HLoop->isUnknown()) {
    return RegDDRefs.end();
  }
  return RegDDRefs.begin();
}

HLDDNode::const_ddref_iterator HLDDNode::ddref_begin() const {
  return const_cast<HLDDNode *>(this)->ddref_begin();
}

HLDDNode::ddref_iterator HLDDNode::ddref_end() { return RegDDRefs.end(); }

HLDDNode::const_ddref_iterator HLDDNode::ddref_end() const {
  return const_cast<HLDDNode *>(this)->ddref_end();
}

HLDDNode::reverse_ddref_iterator HLDDNode::ddref_rbegin() {
  HLLoop *HLoop;

  /// Skip null DDRefs for unknown loops
  if ((HLoop = dyn_cast<HLLoop>(this)) && HLoop->isUnknown()) {
    return RegDDRefs.rend();
  }
  return RegDDRefs.rbegin();
}

HLDDNode::const_reverse_ddref_iterator HLDDNode::ddref_rbegin() const {
  return const_cast<HLDDNode *>(this)->ddref_rbegin();
}

HLDDNode::reverse_ddref_iterator HLDDNode::ddref_rend() {
  return RegDDRefs.rend();
}

HLDDNode::const_reverse_ddref_iterator HLDDNode::ddref_rend() const {
  return const_cast<HLDDNode *>(this)->ddref_rend();
}

RegDDRef *HLDDNode::getOperandDDRefImpl(unsigned OperandNum) const {
  return RegDDRefs[OperandNum];
}

void HLDDNode::setOperandDDRefImpl(RegDDRef *Ref, unsigned OperandNum) {

#ifndef NDEBUG
  /// Reset HLDDNode of a previous DDRef, if any. We can catch more errors
  /// this way.
  if (auto TRef = RegDDRefs[OperandNum]) {
    setNode(TRef, nullptr);
  }
#endif

  if (Ref) {
    assert(!Ref->getHLDDNode() && "DDRef attached to some other node, please "
                                  "remove it first!");
    setNode(Ref, this);
  }

  RegDDRefs[OperandNum] = Ref;
}

void HLDDNode::print(formatted_raw_ostream &OS, unsigned Depth,
                     bool Detailed) const {
  if (Detailed) {
    printDDRefs(OS, Depth);
  }
}

void HLDDNode::printDDRefs(formatted_raw_ostream &OS, unsigned Depth) const {
  bool printed = false;
  bool IsLoop = false;

  // DD refs attached to Loop nodes require additional
  // "|" symbol to make listing nice
  if (isa<HLLoop>(this)) {
    IsLoop = true;
  }

  for (auto I = ddref_begin(), E = ddref_end(); I != E; ++I) {
    // Simply checking for isConstant() also filters out lval DDRefs whose
    // canonical represenation is a constant. We should print out lval DDRefs
    // regardless.
    if (!PrintConstDDRefs && ((*I)->getSymbase() == CONSTANT_SYMBASE)) {
      continue;
    }

    bool IsZttDDRef = false;
    indent(OS, Depth);

    if (IsLoop) {
      OS << "| ";

      IsZttDDRef = cast<HLLoop>(this)->isZttOperandDDRef(*I);
    }

    IsZttDDRef ? (void)(OS << "<ZTT-REG> ") : isLval(*I)
                                                  ? (void)(OS << "<LVAL-REG> ")
                                                  : (void)(OS << "<RVAL-REG> ");

    (*I)->print(OS, true);

    OS << "\n";

    for (auto B = (*I)->blob_cbegin(), BE = (*I)->blob_cend(); B != BE; ++B) {
      indent(OS, Depth);
      if (IsLoop) {
        OS << "| ";
      }

      // Add extra indentation for blob ddrefs.
      OS.indent(IndentWidth);

      OS << "<BLOB> ";
      (*B)->print(OS, true);
      OS << "\n";
    }

    printed = true;
  }

  if (printed) {
    indent(OS, Depth);
    if (IsLoop) {
      OS << "| ";
    }
    OS << "\n";
  }
}

void HLDDNode::verify() const {
  for (auto I = ddref_begin(), E = ddref_end(); I != E; ++I) {
    assert((*I) != nullptr && "null ddref found in the list");
    assert((*I)->getHLDDNode() == this &&
           "DDRef is attached to a different node");
    (*I)->verify();
  }

  HLNode::verify();
}

bool HLDDNode::isLval(const RegDDRef *Ref) const {
  assert((this == Ref->getHLDDNode()) && "Ref does not belong to this node!");

  const HLInst *HInst = dyn_cast<HLInst>(this);

  if (!HInst) {
    return false;
  }

  return (HInst->getLvalDDRef() == Ref);
}

bool HLDDNode::isRval(const RegDDRef *Ref) const { return !isLval(Ref); }

bool HLDDNode::isFake(const RegDDRef *Ref) const {
  assert((this == Ref->getHLDDNode()) && "Ref does not belong to this node!");

  const HLInst *HInst = dyn_cast<HLInst>(this);

  if (!HInst) {
    return false;
  }

  for (auto I = HInst->fake_ddref_begin(), E = HInst->fake_ddref_end(); I != E;
       ++I) {

    if ((*I) == Ref) {
      return true;
    }
  }

  return false;
}
