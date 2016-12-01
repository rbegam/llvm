//===-------- DDRefUtils.cpp - Implements DDRefUtils class ----------------===//
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
// This file implements DDRefUtils class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Debug.h"

#include "llvm/IR/Constants.h" // needed for UndefValue class
#include "llvm/IR/Metadata.h"  // needed for MetadataAsValue -> Value

#include "llvm/Analysis/Intel_LoopAnalysis/HIRFramework.h"

using namespace llvm;
using namespace loopopt;

#define DEBUG_TYPE "ddref-utils"

Function &DDRefUtils::getFunction() const {
  return getHIRParser().getFunction();
}

Module &DDRefUtils::getModule() const { return getHIRParser().getModule(); }

LLVMContext &DDRefUtils::getContext() const {
  return getHIRParser().getContext();
}

const DataLayout &DDRefUtils::getDataLayout() const {
  return getHIRParser().getDataLayout();
}

RegDDRef *DDRefUtils::createRegDDRef(unsigned SB) {
  return new RegDDRef(*this, SB);
}

RegDDRef *DDRefUtils::createScalarRegDDRef(unsigned SB, CanonExpr *CE) {
  assert(CE && " CanonExpr is null.");
  RegDDRef *RegDD = createRegDDRef(SB);
  RegDD->setSingleCanonExpr(CE);
  return RegDD;
}

RegDDRef *DDRefUtils::createConstDDRef(Type *Ty, int64_t Val) {
  RegDDRef *NewRegDD = createRegDDRef(ConstantSymbase);
  CanonExpr *CE = getCanonExprUtils().createCanonExpr(Ty, 0, Val);
  NewRegDD->setSingleCanonExpr(CE);

  return NewRegDD;
}

RegDDRef *DDRefUtils::createMetadataDDRef(MetadataAsValue *Val) {
  RegDDRef *NewRegDD = createRegDDRef(ConstantSymbase);
  // Create a linear self-blob constant canon expr.
  auto CE = getCanonExprUtils().createMetadataCanonExpr(Val);
  NewRegDD->setSingleCanonExpr(CE);

  return NewRegDD;
}

RegDDRef *DDRefUtils::createConstDDRef(ConstantAggregateZero *Val) {
  RegDDRef *NewRegDD = createRegDDRef(ConstantSymbase);
  // Create a linear self-blob constant canon expr.
  auto CE = getCanonExprUtils().createSelfBlobCanonExpr(Val, ConstantSymbase);
  NewRegDD->setSingleCanonExpr(CE);
  CE->setDefinedAtLevel(0);

  return NewRegDD;
}

RegDDRef *DDRefUtils::createConstDDRef(ConstantDataVector *Val) {
  RegDDRef *NewRegDD = createRegDDRef(ConstantSymbase);
  // Create a linear self-blob constant canon expr.
  auto CE = getCanonExprUtils().createSelfBlobCanonExpr(Val, ConstantSymbase);
  NewRegDD->setSingleCanonExpr(CE);
  CE->setDefinedAtLevel(0);

  return NewRegDD;
}

RegDDRef *DDRefUtils::createUndefDDRef(Type *Ty) {
  auto Blob = getBlobUtils().createBlob(UndefValue::get(Ty), false);
  unsigned BlobIndex = getBlobUtils().findBlob(Blob);

  if (BlobIndex != InvalidBlobIndex) {
    return createSelfBlobRef(BlobIndex, 0);
  }
  RegDDRef *Ref = createSelfBlobRef(UndefValue::get(Ty));
  Ref->getSingleCanonExpr()->setDefinedAtLevel(0);
  return Ref;
}

BlobDDRef *DDRefUtils::createBlobDDRef(unsigned Index, unsigned Level) {
  return new BlobDDRef(*this, Index, Level);
}

void DDRefUtils::destroy(DDRef *Ref) {
  auto Count = Objs.erase(Ref);
  assert(Count && "Ref not found in objects!");
  delete Ref;
}

void DDRefUtils::destroyAll() {
  for (auto &I : Objs) {
    delete I;
  }

  Objs.clear();

  getCanonExprUtils().destroyAll();
}

RegDDRef *DDRefUtils::createSelfBlobRef(Value *Temp) {
  unsigned Symbase = getHIRSymbaseAssignment().getNewSymbase();

  // Create a non-linear self-blob canon expr.
  auto CE = getCanonExprUtils().createSelfBlobCanonExpr(Temp, Symbase);

  // Create a RegDDRef with the new symbase and canon expr.
  auto Ref = createRegDDRef(Symbase);
  Ref->setSingleCanonExpr(CE);

  return Ref;
}

unsigned DDRefUtils::getNewSymbase() { return HIRSA->getNewSymbase(); }

bool DDRefUtils::areEqualImpl(const BlobDDRef *Ref1, const BlobDDRef *Ref2) {

  assert(Ref1 && Ref2 && "Ref1/Ref2 parameter is null.");

  if ((Ref1->getSymbase() != Ref2->getSymbase())) {
    return false;
  }

  // Additional check. Ideally, symbase match should be equal blobs.
  assert(CanonExprUtils::areEqual(Ref1->getCanonExpr(), Ref2->getCanonExpr()));

  return true;
}

bool DDRefUtils::areEqualImpl(const RegDDRef *Ref1, const RegDDRef *Ref2,
                              bool RelaxedMode) {

  // Match the symbase. Type checking is done inside the CEUtils.
  if ((Ref1->getSymbase() != Ref2->getSymbase())) {
    return false;
  }

  // Check if one is memory ref and other is not.
  if (Ref1->hasGEPInfo() != Ref2->hasGEPInfo()) {
    return false;
  }

  // Check Base Canon Exprs.
  if (Ref1->hasGEPInfo() &&
      !CanonExprUtils::areEqual(Ref1->getBaseCE(), Ref2->getBaseCE(),
                                RelaxedMode)) {
    return false;
  }

  if (Ref1->getNumDimensions() != Ref2->getNumDimensions()) {
    return false;
  }

  for (auto Ref1Iter = Ref1->canon_begin(), End = Ref1->canon_end(),
            Ref2Iter = Ref2->canon_begin();
       Ref1Iter != End; ++Ref1Iter, ++Ref2Iter) {

    const CanonExpr *Ref1CE = *Ref1Iter;
    const CanonExpr *Ref2CE = *Ref2Iter;

    if (!CanonExprUtils::areEqual(Ref1CE, Ref2CE, RelaxedMode)) {
      return false;
    }
  }

  // All the canon expr match.
  return true;
}

bool DDRefUtils::areEqual(const DDRef *Ref1, const DDRef *Ref2,
                          bool RelaxedMode) {

  assert(Ref1 && Ref2 && "Ref1/Ref2 parameter is null.");

  if (auto BRef1 = dyn_cast<BlobDDRef>(Ref1)) {
    auto BRef2 = dyn_cast<BlobDDRef>(Ref2);
    // Ref2 is Reg/Unknown Type, whereas Ref1 is Blob.
    if (!BRef2) {
      assert(isa<RegDDRef>(Ref2) && "Ref2 is unknown type.");
      return false;
    }

    return areEqualImpl(BRef1, BRef2);

  } else if (auto RRef1 = dyn_cast<RegDDRef>(Ref1)) {
    auto RRef2 = dyn_cast<RegDDRef>(Ref2);
    // Ref2 is Blob/Unknown Type, whereas Ref1 is Blob.
    if (!RRef2) {
      assert(isa<BlobDDRef>(Ref2) && "Ref2 is unknown type.");
      return false;
    }

    return areEqualImpl(RRef1, RRef2, RelaxedMode);

  } else {
    llvm_unreachable("Unknown DDRef kind!");
  }

  return false;
}

bool DDRefUtils::getConstDistance(const RegDDRef *Ref1, const RegDDRef *Ref2,
                                  int64_t *Distance) {
  // Dealing with memrefs only
  if (!Ref1->hasGEPInfo() || !Ref2->hasGEPInfo()) {
    return false;
  }

  // TODO: Compare bases instead of expecting them to be equal?
  const CanonExpr *BaseCE1 = Ref1->getBaseCE();
  const CanonExpr *BaseCE2 = Ref2->getBaseCE();
  if (!CanonExprUtils::areEqual(BaseCE1, BaseCE2))
    return false;

  // TODO: Extend to support different # of dimensions?
  if (Ref1->getNumDimensions() != Ref2->getNumDimensions())
    return false;

  int64_t Delta = 0;

  // Compare the subscripts
  for (unsigned I = 1; I <= Ref1->getNumDimensions(); ++I) {
    const CanonExpr *Ref1CE = Ref1->getDimensionIndex(I);
    const CanonExpr *Ref2CE = Ref2->getDimensionIndex(I);

    // The BaseCE and getNumDimestions() match so we know that
    // getDimensionStride is the same in both.
    uint64_t DimStride = Ref1->getDimensionStride(I);

    // Diff the CanonExprs.
    CanonExpr *Result = getCanonExprUtils().cloneAndSubtract(Ref1CE, Ref2CE);

    // Subtract operation can fail
    if (!Result) {
      return false;
    }

    // TODO: Being conservative with Denom.
    if (Result->getDenominator() > 1) {
      getCanonExprUtils().destroy(Result);
      return false;
    }

    // DEBUG(dbgs() << "\n    Delta for Dim = "; Result->dump());

    if (!Result->isIntConstant()) {
      getCanonExprUtils().destroy(Result);
      return false;
    }

    int64_t Diff = Result->getConstant();
    Delta += Diff * DimStride;
    getCanonExprUtils().destroy(Result);
  }

  *Distance = Delta;
  return true;
}

RegDDRef *DDRefUtils::createSelfBlobRef(unsigned Index, unsigned Level) {
  auto CE = getCanonExprUtils().createSelfBlobCanonExpr(Index, Level);
  unsigned Symbase = getBlobUtils().getTempBlobSymbase(Index);

  auto Ref = createRegDDRef(Symbase);
  Ref->setSingleCanonExpr(CE);

  return Ref;
}

void DDRefUtils::printMDNodes(formatted_raw_ostream &OS,
                              const RegDDRef::MDNodesTy &MDNodes) const {

  SmallVector<StringRef, 8> MDNames;
  auto &HIRP = getHIRParser();

  HIRP.getContext().getMDKindNames(MDNames);

  for (auto const &I : MDNodes) {
    OS << " ";
    if (I.first < MDNames.size()) {
      OS << "!";
      OS << MDNames[I.first] << " ";
    }

    I.second->printAsOperand(OS, &HIRP.getModule());
  }
}
