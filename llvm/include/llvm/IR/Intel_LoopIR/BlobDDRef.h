//===--- BlobDDRef.h - Data dependency node for blobs in HIR ----*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the BlobDDRef node in high level IR.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_INTEL_LOOPIR_BLOBDDREF_H
#define LLVM_IR_INTEL_LOOPIR_BLOBDDREF_H

#include "llvm/IR/Intel_LoopIR/DDRef.h"

namespace llvm {

namespace loopopt {

class CanonExpr;
class RegDDRef;
class HLDDNode;

/// \brief Represents a blob present in a canonical expr of a RegDDRef
///
/// This DDRef is associated with a RegDDRef to expose data dependencies
/// present due to blobs.
class BlobDDRef : public DDRef {
private:
  const CanonExpr *CExpr;
  RegDDRef *ParentDDRef;

protected:
  explicit BlobDDRef(int SB, const CanonExpr *CE, RegDDRef *Parent);
  ~BlobDDRef() {}

  /// \brief Copy constructor used by cloning.
  BlobDDRef(const BlobDDRef &BlobDDRefObj);

  friend class DDRefUtils;

  /// \brief Sets the HLDDNode of BlobDDRef
  void setHLDDNode(HLDDNode *HNode) override;

public:
  /// \brief Prints BlobDDRef in a simple format.
  virtual void print(formatted_raw_ostream &OS) const override;

  /// \brief Prints BlobDDRef with much more information.
  virtual void detailedPrint(formatted_raw_ostream &OS) const override;

  /// \brief Returns HLDDNode this DDRef is attached to.
  HLDDNode *getHLDDNode() const override;

  /// TODO implementation
  /// Value *getLLVMValue() const override { return nullptr; }

  /// \brief Returns the canonical form associated with the blob.
  const CanonExpr *getCanonExpr() const { return CExpr; }

  /// \brief Returns the RegDDRef this is attached to.
  RegDDRef *getParentDDRef() { return ParentDDRef; }
  const RegDDRef *getParentDDRef() const { return ParentDDRef; }

  /// \brief Method for supporting type inquiry through isa, cast, and dyn_cast.
  static bool classof(const DDRef *Ref) {
    return Ref->getDDRefID() == DDRef::BlobDDRefVal;
  }

  /// clone() - Create a copy of 'this' BlobDDRef that is identical in all
  /// ways except the following:
  ///   * The Parent RegDDRef needs to be set explicitly
  BlobDDRef *clone() const override;
};

} // End namespace loopopt

} // End namespace llvm

#endif
