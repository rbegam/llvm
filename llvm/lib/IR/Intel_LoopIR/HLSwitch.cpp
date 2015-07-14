//===---- HLSwitch.cpp - Implements the HLSwitch class --------------------===//
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
// This file implements the HLSwitch class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Debug.h"

#include "llvm/IR/Intel_LoopIR/HLSwitch.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/DDRefUtils.h"

using namespace llvm;
using namespace llvm::loopopt;

HLSwitch::HLSwitch(RegDDRef *ConditionRef) : HLDDNode(HLNode::HLSwitchVal) {
  unsigned NumOp;

  /// This call is to get around calling virtual functions in the constructor.
  NumOp = getNumOperandsInternal();

  RegDDRefs.resize(NumOp, nullptr);

  setConditionDDRef(ConditionRef);
}

HLSwitch::HLSwitch(const HLSwitch &HLSwitchObj, GotoContainerTy *GotoList,
                   LabelMapTy *LabelMap)
    : HLDDNode(HLSwitchObj) {
  const RegDDRef *TRef;
  RegDDRef *Ref;

  CaseBegin.resize(HLSwitchObj.getNumCases(), Children.end());
  RegDDRefs.resize(getNumOperandsInternal(), nullptr);

  /// Clone switch condition DDRef
  setConditionDDRef((TRef = HLSwitchObj.getConditionDDRef()) ? TRef->clone()
                                                             : nullptr);

  /// Clone case value RegDDRefs
  for (unsigned I = 1, E = getNumCases(); I <= E; I++) {
    Ref = (TRef = HLSwitchObj.getCaseValueDDRef(I)) ? TRef->clone() : nullptr;
    setCaseValueDDRef(Ref, I);
  }

  /// Clone default case children
  for (auto It = HLSwitchObj.default_case_child_begin(),
            EndIt = HLSwitchObj.default_case_child_end();
       It != EndIt; It++) {
    HLNode *NewHLNode = cloneBaseImpl(It, GotoList, LabelMap);
    HLNodeUtils::insertAsLastDefaultChild(this, NewHLNode);
  }

  /// Clone case children
  for (unsigned I = 1, E = HLSwitchObj.getNumCases(); I <= E; I++) {
    for (auto It = HLSwitchObj.case_child_begin(I),
              EndIt = HLSwitchObj.case_child_end(I);
         It != EndIt; It++) {
      HLNode *NewHLNode = cloneBaseImpl(It, GotoList, LabelMap);
      HLNodeUtils::insertAsLastChild(this, NewHLNode, I);
    }
  }
}

HLSwitch *HLSwitch::cloneImpl(GotoContainerTy *GotoList,
                              LabelMapTy *LabelMap) const {
  // Call the Copy Constructor
  HLSwitch *NewHLSwitch = new HLSwitch(*this, GotoList, LabelMap);

  return NewHLSwitch;
}

HLSwitch *HLSwitch::clone() const {
  HLContainerTy NContainer;
  HLNodeUtils::cloneSequence(&NContainer, this);
  HLSwitch *NewSwitch = cast<HLSwitch>(NContainer.remove(NContainer.begin()));
  return NewSwitch;
}

void HLSwitch::print_break(formatted_raw_ostream &OS, unsigned Depth,
                           unsigned CaseNum) const {

  auto LastChild = getLastCaseChildInternal(CaseNum);

  if (!LastChild || !isa<HLGoto>(LastChild)) {
    indent(OS, Depth);
    OS.indent(IndentWidth);
    OS << "break;\n";
  }
}

void HLSwitch::print(formatted_raw_ostream &OS, unsigned Depth,
                     bool Detailed) const {

  indent(OS, Depth);

  OS << "switch(";

  auto Ref = getConditionDDRef();
  Ref ? Ref->print(OS, Detailed) : (void)(OS << Ref);

  OS << ")\n";
  indent(OS, Depth);
  OS << "{\n";

  /// Print cases
  for (unsigned I = 1, E = getNumCases(); I <= E; I++) {
    indent(OS, Depth);

    OS << "case ";
    auto Ref = getCaseValueDDRef(I);
    Ref ? Ref->print(OS, Detailed) : (void)(OS << Ref);
    OS << ":\n";

    for (auto It = case_child_begin(I), EndIt = case_child_end(I); It != EndIt;
         It++) {
      It->print(OS, Depth + 1, Detailed);
    }

    print_break(OS, Depth, I);
  }

  /// Print default case
  indent(OS, Depth);
  OS << "default:\n";

  for (auto It = default_case_child_begin(), EndIt = default_case_child_end();
       It != EndIt; It++) {
    It->print(OS, Depth + 1, Detailed);
  }

  print_break(OS, Depth, 0);

  indent(OS, Depth);
  OS << "}\n";
}

HLSwitch::case_child_iterator
HLSwitch::case_child_begin_internal(unsigned CaseNum) {
  if (CaseNum == 0) {
    return Children.begin();
  } else {
    return CaseBegin[CaseNum - 1];
  }
}

HLSwitch::const_case_child_iterator
HLSwitch::case_child_begin_internal(unsigned CaseNum) const {
  return const_cast<HLSwitch *>(this)->case_child_begin_internal(CaseNum);
}

HLSwitch::case_child_iterator
HLSwitch::case_child_end_internal(unsigned CaseNum) {
  if (CaseNum == getNumCases()) {
    return Children.end();
  } else {
    return CaseBegin[CaseNum];
  }
}

HLSwitch::const_case_child_iterator
HLSwitch::case_child_end_internal(unsigned CaseNum) const {
  return const_cast<HLSwitch *>(this)->case_child_end_internal(CaseNum);
}

HLSwitch::reverse_case_child_iterator
HLSwitch::case_child_rbegin_internal(unsigned CaseNum) {
  if (CaseNum == getNumCases()) {
    return Children.rbegin();
  } else {
    return reverse_case_child_iterator(CaseBegin[CaseNum]);
  }
}

HLSwitch::const_reverse_case_child_iterator
HLSwitch::case_child_rbegin_internal(unsigned CaseNum) const {
  return const_cast<HLSwitch *>(this)->case_child_rbegin_internal(CaseNum);
}

HLSwitch::reverse_case_child_iterator
HLSwitch::case_child_rend_internal(unsigned CaseNum) {
  if (CaseNum == 0) {
    return Children.rend();
  } else {
    return reverse_case_child_iterator(CaseBegin[CaseNum - 1]);
  }
}

HLSwitch::const_reverse_case_child_iterator
HLSwitch::case_child_rend_internal(unsigned CaseNum) const {
  return const_cast<HLSwitch *>(this)->case_child_rend_internal(CaseNum);
}

HLNode *HLSwitch::getFirstCaseChildInternal(unsigned CaseNum) {
  if (hasCaseChildrenInternal(CaseNum)) {
    return case_child_begin_internal(CaseNum);
  }

  return nullptr;
}

HLNode *HLSwitch::getLastCaseChildInternal(unsigned CaseNum) {
  if (hasCaseChildrenInternal(CaseNum)) {
    return std::prev(case_child_end_internal(CaseNum));
  }

  return nullptr;
}

RegDDRef *HLSwitch::getConditionDDRef() { return getOperandDDRefImpl(0); }

const RegDDRef *HLSwitch::getConditionDDRef() const {
  return const_cast<HLSwitch *>(this)->getConditionDDRef();
}

void HLSwitch::setConditionDDRef(RegDDRef *ConditionRef) {
  setOperandDDRefImpl(ConditionRef, 0);
}

RegDDRef *HLSwitch::removeConditionDDRef() {
  auto TRef = getConditionDDRef();

  if (TRef) {
    setConditionDDRef(nullptr);
  }

  return TRef;
}

RegDDRef *HLSwitch::getCaseValueDDRef(unsigned CaseNum) {
  assert((CaseNum != 0) && "Default case does not contain DDRef!");
  assert((CaseNum <= getNumCases()) && "CaseNum is out of range!");

  return getOperandDDRefImpl(CaseNum);
}

const RegDDRef *HLSwitch::getCaseValueDDRef(unsigned CaseNum) const {
  return const_cast<HLSwitch *>(this)->getCaseValueDDRef(CaseNum);
}

void HLSwitch::setCaseValueDDRef(RegDDRef *ValueRef, unsigned CaseNum) {
  assert((CaseNum != 0) && "Default case does not contain DDRef!");
  assert((CaseNum <= getNumCases()) && "CaseNum is out of range!");

  setOperandDDRefImpl(ValueRef, CaseNum);
}

RegDDRef *HLSwitch::removeCaseValueDDRef(unsigned CaseNum) {
  auto TRef = getCaseValueDDRef(CaseNum);

  if (TRef) {
    setCaseValueDDRef(nullptr, CaseNum);
  }

  return TRef;
}

void HLSwitch::addCase(RegDDRef *ValueRef) {
  unsigned NumOp;

  CaseBegin.push_back(Children.end());

  NumOp = getNumOperandsInternal();
  RegDDRefs.resize(NumOp, nullptr);

  setCaseValueDDRef(ValueRef, getNumCases());
}

void HLSwitch::removeCase(unsigned CaseNum) {
  assert((CaseNum != 0) && "Default case cannot be removed!");
  assert((CaseNum <= getNumCases()) && "CaseNum is out of range!");

  /// Erase CaseNum's HLNodes
  HLNodeUtils::erase(case_child_begin_internal(CaseNum),
                     case_child_end_internal(CaseNum));

  /// Remove the case value DDRef.
  removeCaseValueDDRef(CaseNum);
  /// Erase the DDRef slot.
  RegDDRefs.erase(RegDDRefs.begin() + CaseNum);

  /// Erase the separator for this case.
  CaseBegin.erase(CaseBegin.begin() + CaseNum - 1);
}
