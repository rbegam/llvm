//===- CanonExpr.cpp - Implements the CanonExpr class -----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the CanonExpr class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/Debug.h"

#include "llvm/Analysis/ScalarEvolution.h"

#include "llvm/IR/Intel_LoopIR/CanonExpr.h"

using namespace llvm;
using namespace loopopt;

std::set<CanonExpr *> CanonExpr::Objs;
SmallVector<CanonExpr::BlobTy, 32> CanonExpr::BlobTable;

CanonExpr::BlobOrConstToVal::BlobOrConstToVal(bool IsBlobCoef, int64_t Coef)
    : IsBlobCoeff(IsBlobCoef), Coeff(Coef) {}

CanonExpr::BlobOrConstToVal::~BlobOrConstToVal() {}

CanonExpr::BlobIndexToCoeff::BlobIndexToCoeff(unsigned Indx, int64_t Coef)
    : Index(Indx), Coeff(Coef) {}

CanonExpr::BlobIndexToCoeff::~BlobIndexToCoeff() {}

CanonExpr::CanonExpr(Type *Typ, int DefLevel, int64_t ConstVal, int64_t Denom)
    : Ty(Typ), DefinedAtLevel(DefLevel), Const(ConstVal), Denominator(Denom) {

  Objs.insert(this);

  /// Start with size = capcity
  IVCoeffs.resize(IVCoeffs.capacity(), BlobOrConstToVal(false, 0));
}

CanonExpr::CanonExpr(const CanonExpr &CE)
    : Ty(CE.Ty), DefinedAtLevel(CE.DefinedAtLevel), IVCoeffs(CE.IVCoeffs),
      BlobCoeffs(CE.BlobCoeffs), Const(CE.Const), Denominator(CE.Denominator) {

  Objs.insert(this);
}

void CanonExpr::destroy() {
  Objs.erase(this);
  delete this;
}

void CanonExpr::destroyAll() {

  for (auto &I : Objs) {
    delete I;
  }

  Objs.clear();
}

CanonExpr *CanonExpr::clone() const {

  CanonExpr *CE = new CanonExpr(*this);
  return CE;
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void CanonExpr::dump() const {
  formatted_raw_ostream OS(dbgs());
  print(OS);
}
#endif

void CanonExpr::print(formatted_raw_ostream &OS) const {
  auto C0 = getConstant();
  auto Denom = getDenominator();
  unsigned Level = 1;
  bool Printed = false;

  if (Denom != 1) {
    OS << "(";
  }

  for (auto I = iv_cbegin(), E = iv_cend(); I != E; I++, Level++) {
    if (I->Coeff != 0) {
      if (Printed) {
        OS << "+";
      } else {
        Printed = true;
      }

      if (I->IsBlobCoeff) {
        getBlob(I->Coeff)->print(OS);
        OS << "*";
      } else if (I->Coeff != 1) {
        OS << I->Coeff << "*";
      }
      OS << "i" << Level;
    }
  }

  for (auto I = blob_cbegin(), E = blob_cend(); I != E; I++) {
    if (I->Coeff != 0) {
      if (Printed) {
        OS << "+";
      } else {
        Printed = true;
      }

      if (I->Coeff != 1) {
        OS << I->Coeff << "*";
      }
      getBlob(I->Index)->print(OS);
    }
  }

  if (!Printed) {
    OS << C0;
  } else if (C0 != 0) {
    OS << "+" << C0;
  }

  if (Denom != 1) {
    OS << ")/" << Denom;
  }
}

unsigned CanonExpr::findOrInsertBlobImpl(BlobTy Blob, bool Insert) {
  assert(Blob && "Blob is null!");

  for (auto I = BlobTable.begin(), E = BlobTable.end(); I != E; I++) {
    if (*I == Blob) {
      return (I - BlobTable.begin() + 1);
    }
  }

  if (Insert) {
    BlobTable.push_back(Blob);
    return BlobTable.size();
  }

  return 0;
}

unsigned CanonExpr::findBlob(BlobTy Blob) {
  return findOrInsertBlobImpl(Blob, false);
}

unsigned CanonExpr::findOrInsertBlob(BlobTy Blob) {
  return findOrInsertBlobImpl(Blob, true);
}

CanonExpr::BlobTy CanonExpr::getBlob(unsigned BlobIndex) {
  assert((BlobIndex > 0) && (BlobIndex <= BlobTable.size()) &&
         "BlobIndex is out of range!");
  return BlobTable[BlobIndex - 1];
}

bool CanonExpr::hasIV() const {

  bool ret = false;

  for (auto &I : IVCoeffs) {
    if (I.Coeff != 0) {
      ret = true;
      break;
    }
  }

  return ret;
}

int64_t CanonExpr::getIVCoeff(unsigned Lvl, bool *IsBlobCoeff) const {

  assert(IsBlobCoeff && "Non-null IsBlobCoeff ptr expected!");
  assert((Lvl <= MaxLoopNestLevel) && "Level is out of bounds!");

  if (IVCoeffs.size() < Lvl) {
    return 0;
  }

  *IsBlobCoeff = IVCoeffs[Lvl - 1].IsBlobCoeff;

  return IVCoeffs[Lvl - 1].Coeff;
}

bool CanonExpr::resizeIVCoeffsToMax(unsigned Lvl) {

  assert((Lvl <= MaxLoopNestLevel) && "Level is out of bounds!");

  if (IVCoeffs.size() < Lvl) {
    IVCoeffs.resize(MaxLoopNestLevel, BlobOrConstToVal(false, 0));
    return true;
  }

  return false;
}

void CanonExpr::addIVInternal(unsigned Lvl, int64_t Coeff, bool IsBlobCoeff,
                              bool overwrite) {

  bool resized;

  resized = resizeIVCoeffsToMax(Lvl);

  if (!overwrite && !resized) {
    assert((!IVCoeffs[Lvl - 1].IsBlobCoeff) && "Blob coefficients cannot be "
                                               "added!");
  }

  if (overwrite) {
    IVCoeffs[Lvl - 1].IsBlobCoeff = IsBlobCoeff;
    IVCoeffs[Lvl - 1].Coeff = Coeff;
  } else {
    IVCoeffs[Lvl - 1].Coeff += Coeff;
  }
}

void CanonExpr::setIVCoeff(unsigned Lvl, int64_t Coeff, bool IsBlobCoeff) {
  addIVInternal(Lvl, Coeff, IsBlobCoeff, true);
}

void CanonExpr::addIV(unsigned Lvl, int64_t Coeff) {
  addIVInternal(Lvl, Coeff, false, false);
}

void CanonExpr::removeIV(unsigned Lvl) {

  assert((Lvl <= MaxLoopNestLevel) && "Level is out of bounds!");

  /// Nothing to do as the IV is not present.
  /// Should we assert on this?
  if (IVCoeffs.size() < Lvl) {
    return;
  }

  IVCoeffs[Lvl - 1].Coeff = 0;
}

void CanonExpr::replaceIVByConstant(unsigned Lvl, int64_t Val) {

  int64_t Coeff;

  assert(((IVCoeffs.size() >= Lvl) && (IVCoeffs[Lvl - 1].Coeff != 0)) &&
         "IV at this level not found!");

  Coeff = IVCoeffs[Lvl - 1].Coeff;

  /// IV coefficient is blob index
  if (IVCoeffs[Lvl - 1].IsBlobCoeff) {
    addBlob(Coeff, Val);
  }
  /// IV coefficient is constant
  else {
    Const += (Coeff * Val);
  }

  removeIV(Lvl);
}

namespace {
struct BlobIndexCompareLess {
  bool operator()(const CanonExpr::BlobIndexToCoeff &B1,
                  const CanonExpr::BlobIndexToCoeff &B2) {
    return B1.Index < B2.Index;
  }
};

struct BlobIndexCompareEqual {
  bool operator()(const CanonExpr::BlobIndexToCoeff &B1,
                  const CanonExpr::BlobIndexToCoeff &B2) {
    return B1.Index == B2.Index;
  }
};
}

int64_t CanonExpr::getBlobCoeff(unsigned BlobIndex) const {

  BlobIndexToCoeff Blob(BlobIndex, 0);

  auto I = std::lower_bound(BlobCoeffs.begin(), BlobCoeffs.end(), Blob,
                            BlobIndexCompareLess());

  if ((I != BlobCoeffs.end()) && BlobIndexCompareEqual()(*I, Blob)) {
    return I->Coeff;
  }

  return 0;
}

void CanonExpr::addBlobInternal(unsigned BlobIndex, int64_t BlobCoeff,
                                bool overwrite) {

  BlobIndexToCoeff Blob(BlobIndex, BlobCoeff);

  /// No blobs present, add this one
  if (BlobCoeffs.empty()) {
    BlobCoeffs.push_back(Blob);
    return;
  }

  auto I = std::lower_bound(BlobCoeffs.begin(), BlobCoeffs.end(), Blob,
                            BlobIndexCompareLess());

  /// The blob already exists so just change the coeff
  if ((I != BlobCoeffs.end()) && BlobIndexCompareEqual()(*I, Blob)) {
    if (overwrite) {
      I->Coeff = BlobCoeff;
    } else {
      I->Coeff += BlobCoeff;
    }
  }
  /// We need to insert new blob at this (sorted) position
  else {
    BlobCoeffs.insert(I, Blob);
  }
}

void CanonExpr::setBlobCoeff(unsigned BlobIndex, int64_t BlobCoeff) {
  addBlobInternal(BlobIndex, BlobCoeff, true);
}

void CanonExpr::addBlob(unsigned BlobIndex, int64_t BlobCoeff) {
  addBlobInternal(BlobIndex, BlobCoeff, false);
}

void CanonExpr::removeBlob(unsigned BlobIndex) {

  BlobIndexToCoeff Blob(BlobIndex, 0);

  auto I = std::lower_bound(BlobCoeffs.begin(), BlobCoeffs.end(), Blob,
                            BlobIndexCompareLess());

  if ((I != BlobCoeffs.end()) && BlobIndexCompareEqual()(*I, Blob)) {
    BlobCoeffs.erase(I);
  }
}

void CanonExpr::replaceBlob(unsigned OldBlobIndex, unsigned NewBlobIndex) {

  int64_t Coeff;
  bool found = false;
  BlobIndexToCoeff Blob(OldBlobIndex, 0);

  assert(!BlobCoeffs.empty() && "Old blob index not found!");

  auto I = std::lower_bound(BlobCoeffs.begin(), BlobCoeffs.end(), Blob,
                            BlobIndexCompareLess());

  if ((I != BlobCoeffs.end()) && BlobIndexCompareEqual()(*I, Blob)) {
    /// Store the coeff as the iterator might get invalidated after erase.
    Coeff = I->Coeff;
    BlobCoeffs.erase(I);
    addBlob(NewBlobIndex, Coeff);
    found = true;
  }

  /// Replace in IV coefficients
  for (auto &I : IVCoeffs) {
    if ((I.IsBlobCoeff) &&
        BlobIndexCompareEqual()(BlobIndexToCoeff(I.Coeff, 0), Blob)) {

      I.Coeff = NewBlobIndex;
      found = true;
    }
  }

  if (!found) {
    assert("Old blob index not found!");
  }
}

void CanonExpr::shift(unsigned Lvl, int64_t Val) {

  /// Nothing to do as the IV is not present.
  if (IVCoeffs.size() < Lvl) {
    return;
  }

  /// Handle blob coefficient of IV
  if (IVCoeffs[Lvl - 1].IsBlobCoeff) {
    addBlob(IVCoeffs[Lvl - 1].Coeff, Val);
  }
  /// Handle constant coefficient of IV
  else {
    Const += (IVCoeffs[Lvl - 1].Coeff * Val);
  }
}

void CanonExpr::extractBlobIndices(SmallVectorImpl<unsigned> &BlobIndices) {

  bool Inserted;

  /// Push all blobs from BlobCoeffs.
  for (auto &I : BlobCoeffs) {
    BlobIndices.push_back(I.Index);
  }

  /// Push all blobs from IVCoeffs which haven't already been inserted.
  for (auto &I : IVCoeffs) {
    if (I.IsBlobCoeff) {
      Inserted = false;

      /// Check whether it has already been inserted.
      for (auto &J : BlobIndices) {
        if (BlobIndexCompareEqual()(BlobIndexToCoeff(J, 0),
                                    BlobIndexToCoeff(I.Coeff, 0))) {
          Inserted = true;
          break;
        }
      }

      if (!Inserted) {
        BlobIndices.push_back(I.Coeff);
      }
    }
  }
}
