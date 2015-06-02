//===- DDRef.cpp - Implements the DDRef class -------------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the DDRef class.
//
//===----------------------------------------------------------------------===//

#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Debug.h"

#include "llvm/IR/Intel_LoopIR/DDRef.h"
#include "llvm/IR/Intel_LoopIR/BlobDDRef.h"
#include "llvm/IR/Intel_LoopIR/RegDDRef.h"
#include "llvm/IR/Intel_LoopIR/CanonExpr.h"

using namespace llvm;
using namespace llvm::loopopt;

std::set<DDRef *> DDRef::Objs;

DDRef::DDRef(unsigned SCID, int SB) : SubClassID(SCID), SymBase(SB) {

  Objs.insert(this);
}

DDRef::DDRef(const DDRef &DDRefObj)
    : SubClassID(DDRefObj.SubClassID), SymBase(DDRefObj.SymBase) {

  Objs.insert(this);
}

void DDRef::destroy() {
  Objs.erase(this);
  delete this;
}

void DDRef::destroyAll() {

  for (auto &I : Objs) {
    delete I;
  }

  Objs.clear();
}

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
void DDRef::dump() const {
  formatted_raw_ostream OS(dbgs());
  print(OS);
}

void DDRef::detailedDump() const {
  formatted_raw_ostream OS(dbgs());
  detailedPrint(OS);
}
#endif

Type *DDRef::getLLVMType() const {
  const CanonExpr *CE;

  if (auto BRef = dyn_cast<BlobDDRef>(this)) {
    CE = BRef->getCanonExpr();
    assert(CE && "DDRef is empty!");
    return CE->getLLVMType();
  } else if (auto RRef = dyn_cast<RegDDRef>(this)) {
    if (RRef->hasGEPInfo()) {
      CE = RRef->getBaseCE();
      assert(CE && "BaseCE is absent in RegDDRef containing GEPInfo!");
      return CE->getLLVMType();
    } else {
      CE = RRef->getSingleCanonExpr();
      assert(CE && "DDRef is empty!");
      return CE->getLLVMType();
    }
  }

  llvm_unreachable("Unknown DDRef kind!");
}
