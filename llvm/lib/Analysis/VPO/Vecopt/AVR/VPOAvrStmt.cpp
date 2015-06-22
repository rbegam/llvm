//===--------- VPOAvrStmt.cpp - Implements AVRAssign class ------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the the AVR STMT classes.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ErrorHandling.h"
#include "llvm/IR/CFG.h"
#include "llvm/Analysis/VPO/Vecopt/AVR/VPOAvrStmt.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"

using namespace llvm;
using namespace intel;

#define DEBUG_TYPE "avr"

// TODO: Properly define print routines.


//----------AVR Assign Implementation----------//
AVRAssign::AVRAssign(Instruction * Inst)
  : AVR(AVR::AVRAssignNode), Instruct(Inst) {}

AVRAssign *AVRAssign::clone() const {
  return nullptr;
}

void AVRAssign::print() const {
  DEBUG(dbgs() <<"AVR_Assign\n");
}

void AVRAssign::dump() const {
  print();
}

void AVRAssign::CodeGen() {
  Instruction *inst;

  DEBUG(Instruct->dump());
  inst = Instruct->clone();

  if (!inst->getType()->isVoidTy())
    inst->setName(Instruct->getName() + 
                  ".VPOClone");

  ReplaceInstWithInst(Instruct, inst);
  DEBUG(inst->dump());
}

//----------AVR Label Implementation----------//
AVRLabel::AVRLabel(BasicBlock *SourceB)
  : AVR(AVR::AVRLabelNode), SourceBlock(SourceB) {}

AVRLabel *AVRLabel::clone() const {
  return nullptr;
}

void AVRLabel::print() const {
  DEBUG(dbgs() <<"\nAVR_Label\n");
}

void AVRLabel::dump() const {
  print();
}

void AVRLabel::CodeGen() {
}

//----------AVR Phi Implementation----------//
AVRPhi::AVRPhi(Instruction * Inst)
  : AVR(AVR::AVRPhiNode), Instruct(Inst) {}

AVRPhi *AVRPhi::clone() const {
  return nullptr;
}

void AVRPhi::print() const {
  DEBUG(dbgs() <<"AVR_Phi\n");
}

void AVRPhi::dump() const {
  print();
}

void AVRPhi::CodeGen() {
  Instruction *inst;

  DEBUG(Instruct->dump());
  inst = Instruct->clone();

  if (!inst->getType()->isVoidTy())
    inst->setName(Instruct->getName() + 
                  ".VPOClone");

  ReplaceInstWithInst(Instruct, inst);
  DEBUG(inst->dump());
}

//----------AVR Call Implementation----------//
AVRCall::AVRCall(Instruction * Inst)
  : AVR(AVR::AVRCallNode), Instruct(Inst) {}

AVRCall *AVRCall::clone() const {
  return nullptr;
}

void AVRCall::print() const {
  DEBUG(dbgs() <<"AVR_Call\n");
}

void AVRCall::dump() const {
  print();
}

void AVRCall::CodeGen() {
  Instruction *inst;

  DEBUG(Instruct->dump());
  inst = Instruct->clone();

  if (!inst->getType()->isVoidTy())
    inst->setName(Instruct->getName() + 
                  ".VPOClone");

  ReplaceInstWithInst(Instruct, inst);
  DEBUG(inst->dump());
}

//----------AVR FBranch Implementation----------//
AVRFBranch::AVRFBranch(Instruction * Inst)
  : AVR(AVR::AVRFBranchNode), Instruct(Inst) {}

AVRFBranch *AVRFBranch::clone() const {
  return nullptr;
}

void AVRFBranch::print() const {
  DEBUG(dbgs() <<"AVR_ForwardBranch\n");
}

void AVRFBranch::dump() const {
  print();
}

void AVRFBranch::CodeGen() {
  Instruction *inst;

  DEBUG(Instruct->dump());
  inst = Instruct->clone();

  if (!inst->getType()->isVoidTy())
    inst->setName(Instruct->getName() + 
                  ".VPOClone");

  ReplaceInstWithInst(Instruct, inst);
  DEBUG(inst->dump());
}

//----------AVR BackEdge Implementation----------//
AVRBackEdge::AVRBackEdge(Instruction * Inst)
  : AVR(AVR::AVRFBranchNode), Instruct(Inst) {}

AVRBackEdge *AVRBackEdge::clone() const {
  return nullptr;
}

void AVRBackEdge::print() const {
  DEBUG(dbgs() <<"AVR_BackEdge\n");
}

void AVRBackEdge::dump() const {
  print();
}

void AVRBackEdge::CodeGen() {
  Instruction *inst;

  DEBUG(Instruct->dump());
  inst = Instruct->clone();

  if (!inst->getType()->isVoidTy())
    inst->setName(Instruct->getName() + 
                  ".VPOClone");

  ReplaceInstWithInst(Instruct, inst);
  DEBUG(inst->dump());
}

//----------AVR Entry Implementation----------//
AVREntry::AVREntry(Instruction * Inst)
  : AVR(AVR::AVREntryNode), Instruct(Inst) {}

AVREntry *AVREntry::clone() const {
  return nullptr;
}

void AVREntry::print() const {
  DEBUG(dbgs() <<"AVR_Entry\n");
}

void AVREntry::dump() const {
  print();
}

void AVREntry::CodeGen() {
  Instruction *inst;

  DEBUG(Instruct->dump());
  inst = Instruct->clone();

  if (!inst->getType()->isVoidTy())
    inst->setName(Instruct->getName() + 
                  ".VPOClone");

  ReplaceInstWithInst(Instruct, inst);
  DEBUG(inst->dump());
}


//----------AVR Return Implementation----------//
AVRReturn::AVRReturn(Instruction * Inst)
  : AVR(AVR::AVRReturnNode), Instruct(Inst) {}

AVRReturn *AVRReturn::clone() const {
  return nullptr;
}

void AVRReturn::print() const {
  DEBUG(dbgs() <<"AVR_Return\n");
}

void AVRReturn::dump() const {
  print();
}

void AVRReturn::CodeGen() {
  Instruction *inst;

  DEBUG(Instruct->dump());
  inst = Instruct->clone();

  if (!inst->getType()->isVoidTy())
    inst->setName(Instruct->getName() + 
                  ".VPOClone");

  ReplaceInstWithInst(Instruct, inst);
  DEBUG(inst->dump());
}

