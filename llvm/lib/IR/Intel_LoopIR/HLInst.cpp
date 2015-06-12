//===-------- HLInst.cpp - Implements the HLInst class --------------------===//
//
// Copyright (C) 2015 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements the HLInst class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/Intel_LoopTransforms/Utils/HLNodeUtils.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/DDRefUtils.h"
#include "llvm/IR/Instructions.h"

using namespace llvm;
using namespace llvm::loopopt;

void HLInst::initialize() {
  /// This call is to get around calling virtual functions in the constructor.
  unsigned NumOp = getNumOperandsInternal();

  /// Number of operands stays the same over the lifetime of HLInst so make
  /// that the min size.
  RegDDRefs.resize(NumOp, nullptr);
}

HLInst::HLInst(Instruction *In)
    : HLDDNode(HLNode::HLInstVal), Inst(In), SafeRednSucc(nullptr) {
  assert(Inst && "LLVM Instruction for HLInst cannot be null!");
  initialize();
}

HLInst::HLInst(const HLInst &HLInstObj)
    : HLDDNode(HLInstObj), Inst(HLInstObj.Inst), SafeRednSucc(nullptr) {

  unsigned NumOp, Count = 0;

  initialize();
  NumOp = getNumOperandsInternal();

  /// Clone DDRefs
  for (auto I = HLInstObj.ddref_begin(), E = HLInstObj.ddref_end(); I != E;
       I++, Count++) {
    if (Count < NumOp) {
      setOperandDDRef((*I)->clone(), Count);
    } else {
      addFakeDDRef((*I)->clone());
    }
  }
}

HLInst *HLInst::cloneImpl(GotoContainerTy *GotoList,
                          LabelMapTy *LabelMap) const {
  // Call the Copy Constructor
  HLInst *NewHLInst = new HLInst(*this);

  return NewHLInst;
}

HLInst *HLInst::clone() const {

  // Call the clone implementation.
  return cloneImpl(nullptr, nullptr);
}

void HLInst::print(formatted_raw_ostream &OS, unsigned Depth) const {
  unsigned Count = 0;

  indent(OS, Depth);

  /// TODO: Add beautification logic based on instruction types.
  for (auto I = op_ddref_begin(), E = op_ddref_end(); I != E; I++, Count++) {
    if ((Count > 1) || (!hasLval() && (Count > 0))) {
      OS << " , ";
    }

    if (Count == 0) {
      if (hasLval()) {
        *I ? (*I)->print(OS) : (void)(OS << *I);

        OS << " = ";

        if (!isa<LoadInst>(Inst) && !isa<StoreInst>(Inst)) {
          OS << Inst->getOpcodeName() << " ";
        }
      } else {
        OS << Inst->getOpcodeName() << " ";

        *I ? (*I)->print(OS) : (void)(OS << *I);
      }
    } else {
      *I ? (*I)->print(OS) : (void)(OS << *I);
    }
  }

  OS << ";\n";
}

bool HLInst::hasLval() const {
  /// The following logic is copied from AssemblyWriter::printInstruction().
  /// TODO: Is there a better way to determine this, probably by checking
  /// non-zero uses?
  return (Inst->hasName() || !Inst->getType()->isVoidTy() ||
          isa<StoreInst>(Inst));
}

RegDDRef *HLInst::getOperandDDRef(unsigned OperandNum) {
  assert(OperandNum < getNumOperands() && "Operand is out of range!");
  return getOperandDDRefImpl(OperandNum);
}

const RegDDRef *HLInst::getOperandDDRef(unsigned OperandNum) const {
  return const_cast<HLInst *>(this)->getOperandDDRef(OperandNum);
}

void HLInst::setOperandDDRef(RegDDRef *Ref, unsigned OperandNum) {
  assert(OperandNum < getNumOperands() && "Operand is out of range!");
  setOperandDDRefImpl(Ref, OperandNum);
}

RegDDRef *HLInst::removeOperandDDRef(unsigned OperandNum) {
  auto TRef = getOperandDDRef(OperandNum);

  if (TRef) {
    setOperandDDRef(nullptr, OperandNum);
  }

  return TRef;
}

RegDDRef *HLInst::getLvalDDRef() {
  if (hasLval()) {
    return cast<RegDDRef>(getOperandDDRefImpl(0));
  }

  return nullptr;
}

const RegDDRef *HLInst::getLvalDDRef() const {
  return const_cast<HLInst *>(this)->getLvalDDRef();
}

void HLInst::setLvalDDRef(RegDDRef *RDDRef) {
  assert(hasLval() && "This instruction does not have an lval!");

  setOperandDDRefImpl(RDDRef, 0);
}

RegDDRef *HLInst::removeLvalDDRef() {
  auto TRef = getLvalDDRef();

  setLvalDDRef(nullptr);

  return TRef;
}

bool HLInst::hasRval() const {
  return (isa<StoreInst>(Inst) || (hasLval() && isa<UnaryInstruction>(Inst)));
}

RegDDRef *HLInst::getRvalDDRef() {
  if (hasRval()) {
    return getOperandDDRefImpl(1);
  }

  return nullptr;
}

const RegDDRef *HLInst::getRvalDDRef() const {
  return const_cast<HLInst *>(this)->getRvalDDRef();
}

void HLInst::setRvalDDRef(RegDDRef *Ref) {
  assert(hasRval() && "This instruction does not have a rval!");

  setOperandDDRefImpl(Ref, 1);
}

RegDDRef *HLInst::removeRvalDDRef() {
  auto TRef = getRvalDDRef();

  setRvalDDRef(nullptr);

  return TRef;
}

void HLInst::addFakeDDRef(RegDDRef *RDDRef) {
  assert(RDDRef && "Cannot add null fake DDRef!");

  RegDDRefs.push_back(RDDRef);
  setNode(RDDRef, this);
}

void HLInst::removeFakeDDRef(RegDDRef *RDDRef) {
  HLDDNode *Node;

  assert(RDDRef && "Cannot remove null fake DDRef!");
  assert(RDDRef->isFake() && "RDDRef is not a fake DDRef!");
  assert((Node = RDDRef->getHLDDNode()) && isa<HLInst>(Node) &&
         (cast<HLInst>(Node) == this) &&
         "RDDRef does not belong to this HLInst!");

  for (auto I = fake_ddref_begin(), E = fake_ddref_end(); I != E; I++) {
    if ((*I) == RDDRef) {
      setNode(RDDRef, nullptr);
      RegDDRefs.erase(I);
      return;
    }
  }

  llvm_unreachable("Unexpected condition!");
}

unsigned HLInst::getNumOperands() const { return getNumOperandsInternal(); }

unsigned HLInst::getNumOperandsInternal() const {
  unsigned NumOp = Inst->getNumOperands();

  if (hasLval() && !isa<StoreInst>(Inst)) {
    NumOp++;
  }

  return NumOp;
}

bool HLInst::isInPreheaderPostexitImpl(bool Preheader) const {
  auto HLoop = getParentLoop();

  if (!HLoop) {
    return false;
  }

  auto I = Preheader ? HLoop->pre_begin() : HLoop->post_begin();
  auto E = Preheader ? HLoop->pre_end() : HLoop->post_end();

  for (; I != E; I++) {
    if (cast<HLInst>(I) == this) {
      return true;
    }
  }

  return false;
}

bool HLInst::isInPreheader() const { return isInPreheaderPostexitImpl(true); }

bool HLInst::isInPostexit() const { return isInPreheaderPostexitImpl(false); }

bool HLInst::isInPreheaderOrPostexit() const {
  return (isInPreheader() || isInPostexit());
}
