//===-------------- BlobUtils.h - Blob utilities --------------*- C++ -*---===//
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
// This file contains interface for blob utilities.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_UTILS_BLOBUTILS_H
#define LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_UTILS_BLOBUTILS_H

#include "llvm/Support/Compiler.h"

namespace llvm {
class Value;
class Type;
class SCEV;
class Constant;
class ConstantFP;
class Function;
class Module;
class LLVMContext;
class DataLayout;

namespace loopopt {

class HIRParser;

typedef const SCEV *BlobTy;

/// Contains blob related utilities.
class BlobUtils {
private:
  HIRParser &HIRP;

  BlobUtils(HIRParser &HIRP) : HIRP(HIRP) {}

  /// Make class uncopyable.
  BlobUtils(const BlobUtils &) = delete;
  void operator=(const BlobUtils &) = delete;

  // Creates BlobUtils object.
  friend class HIRParser;
  friend class CanonExprUtils;

  HIRParser &getHIRParser() { return HIRP; }
  const HIRParser &getHIRParser() const { return HIRP; }

  // Only used by framework to create new temp blobs.
  BlobTy createBlob(Value *TempVal, unsigned Symbase, bool Insert,
                    unsigned *NewBlobIndex);

public:
  /// Returns Function object.
  Function &getFunction() const;

  /// Returns Module object.
  Module &getModule() const;

  /// Returns LLVMContext object.
  LLVMContext &getContext() const;

  /// Returns DataLayout object.
  const DataLayout &getDataLayout() const;

  /// Returns the index of Blob in the blob table. Index range is [1, UINT_MAX].
  /// Returns invalid value if the blob is not present in the table.
  unsigned findBlob(BlobTy Blob);

  /// Returns symbase corresponding to \p Blob. Asserts if a valid symbase is
  /// not found.
  unsigned findTempBlobSymbase(BlobTy Blob);

  /// Returns temp blob index corresponding to symbase. Returns InvalidBlobIndex
  /// if blob cannot be found.
  unsigned findTempBlobIndex(unsigned Symbase);

  /// Finds or inserts temp blob index corresponding to symbase and returns it.
  unsigned findOrInsertTempBlobIndex(unsigned Symbase);

  /// Returns the index of Blob in the blob table. Blob is first inserted, if it
  /// isn't already present in the blob table. Index range is [1, UINT_MAX].
  /// NOTE: New temp blobs can only be inserted by the framework.
  unsigned findOrInsertBlob(BlobTy Blob);

  /// Maps blobs in Blobs to their corresponding indices and inserts them in
  /// Indices.
  void mapBlobsToIndices(const SmallVectorImpl<BlobTy> &Blobs,
                         SmallVectorImpl<unsigned> &Indices);

  /// Returns blob corresponding to BlobIndex.
  BlobTy getBlob(unsigned BlobIndex);

  /// Returns symbase corresponding to BlobIndex. Asserts if a valid symbase is
  /// not found.
  unsigned getTempBlobSymbase(unsigned BlobIndex);

  /// Returns true if this is a valid blob index.
  bool isBlobIndexValid(unsigned BlobIndex);

  /// Prints blob.
  void printBlob(raw_ostream &OS, BlobTy Blob);

  /// Prints scalar corresponding to symbase.
  void printScalar(raw_ostream &OS, unsigned Symbase);

  /// Checks if the blob is constant or not.
  /// If blob is constant, sets the return value in Val.
  bool isConstantIntBlob(BlobTy Blob, int64_t *Val);

  /// Returns true if Blob is a temp.
  bool isTempBlob(BlobTy Blob);

  /// Returns true if this is a nested blob(SCEV tree with > 1 node).
  bool isNestedBlob(BlobTy Blob);

  /// Returns true if TempBlob always has a defined at level of zero.
  bool isGuaranteedProperLinear(BlobTy TempBlob);

  /// Returns true if Blob is a UndefValue.
  bool isUndefBlob(BlobTy Blob);

  /// Returns true if Blob represents a FP constant.
  bool isConstantFPBlob(BlobTy Blob, ConstantFP **Val = nullptr);

  /// Returns true if Blob represents a vector of constants.
  /// If yes, returns the underlying LLVM Value in Val
  bool isConstantVectorBlob(BlobTy Blob, Constant **Val = nullptr);

  /// Returns true if Blob represents a metadata.
  /// If blob is metadata, sets the return value in Val.
  bool isMetadataBlob(BlobTy Blob, MetadataAsValue **Val = nullptr);

  /// Returns true if \p Blob represents a signed extension value.
  /// If blob is sext, sets the return value in Val.
  bool isSignExtendBlob(BlobTy Blob, BlobTy *Val = nullptr);

  /// Returns a new blob created from passed in Val.
  BlobTy createBlob(Value *Val, bool Insert = true,
                    unsigned *NewBlobIndex = nullptr);

  /// Returns a new blob created from a constant value.
  BlobTy createBlob(int64_t Val, Type *Ty, bool Insert = true,
                    unsigned *NewBlobIndex = nullptr);

  /// Returns a blob which represents (LHS + RHS). If Insert is true its index
  /// is returned via NewBlobIndex argument.
  BlobTy createAddBlob(BlobTy LHS, BlobTy RHS, bool Insert = true,
                       unsigned *NewBlobIndex = nullptr);

  /// Returns a blob which represents (LHS - RHS). If Insert is true its index
  /// is returned via NewBlobIndex argument.
  BlobTy createMinusBlob(BlobTy LHS, BlobTy RHS, bool Insert = true,
                         unsigned *NewBlobIndex = nullptr);

  /// Returns a blob which represents (LHS * RHS). If Insert is true its index
  /// is returned via NewBlobIndex argument.
  BlobTy createMulBlob(BlobTy LHS, BlobTy RHS, bool Insert = true,
                       unsigned *NewBlobIndex = nullptr);

  /// Returns a blob which represents (LHS / RHS). If Insert is true its index
  /// is returned via NewBlobIndex argument.
  BlobTy createUDivBlob(BlobTy LHS, BlobTy RHS, bool Insert = true,
                        unsigned *NewBlobIndex = nullptr);

  /// Returns a blob which represents (trunc Blob to Ty). If Insert is true its
  /// index is returned via NewBlobIndex argument.
  BlobTy createTruncateBlob(BlobTy Blob, Type *Ty, bool Insert = true,
                            unsigned *NewBlobIndex = nullptr);

  /// Returns a blob which represents (zext Blob to Ty). If Insert is true its
  /// index is returned via NewBlobIndex argument.
  BlobTy createZeroExtendBlob(BlobTy Blob, Type *Ty, bool Insert = true,
                              unsigned *NewBlobIndex = nullptr);

  /// Returns a blob which represents (sext Blob to Ty). If Insert is true its
  /// index is returned via NewBlobIndex argument.
  BlobTy createSignExtendBlob(BlobTy Blob, Type *Ty, bool Insert = true,
                              unsigned *NewBlobIndex = nullptr);

  /// Returns a new blob with appropriate cast (SExt, ZExt, Trunc) applied on
  /// top of \p Blob. If Insert is true its index is returned via NewBlobIndex
  /// argument.
  BlobTy createCastBlob(BlobTy Blob, bool IsSExt, Type *Ty, bool Insert = true,
                        unsigned *NewBlobIndex = nullptr);

  /// Returns a new smin blob for a pair \p BlobA and \p BlobB. If Insert is
  /// true its index is returned via NewBlobIndex argument.
  BlobTy createSMinBlob(BlobTy BlobA, BlobTy BlobB, bool Insert,
                        unsigned *NewBlobIndex);

  /// Returns a new smax blob for a pair \p BlobA and \p BlobB. If Insert is
  /// true its index is returned via NewBlobIndex argument.
  BlobTy createSMaxBlob(BlobTy BlobA, BlobTy BlobB, bool Insert,
                        unsigned *NewBlobIndex);

  /// Returns a new umin blob for a pair \p BlobA and \p BlobB. If Insert is
  /// true its index is returned via NewBlobIndex argument.
  BlobTy createUMinBlob(BlobTy BlobA, BlobTy BlobB, bool Insert,
                        unsigned *NewBlobIndex);

  /// Returns a new umax blob for a pair \p BlobA and \p BlobB. If Insert is
  /// true its index is returned via NewBlobIndex argument.
  BlobTy createUMaxBlob(BlobTy BlobA, BlobTy BlobB, bool Insert,
                        unsigned *NewBlobIndex);

  /// Returns true if Blob contains SubBlob or if Blob == SubBlob.
  bool contains(BlobTy Blob, BlobTy SubBlob);

  /// Returns all the temp blobs present in Blob via TempBlobs vector.
  void collectTempBlobs(BlobTy Blob, SmallVectorImpl<BlobTy> &TempBlobs);

  /// Returns all the temp blobs present in blob with index \p BlobIndex via \p
  /// TempBlobIndices vector.
  void collectTempBlobs(unsigned BlobIndex,
                        SmallVectorImpl<unsigned> &TempBlobIndices);

  /// Replaces \p OldTempIndex by \p NewTempIndex in \p BlobIndex and returns
  /// the new blob in \p NewBlobIndex. Returns true if substitution was
  /// performed.
  bool replaceTempBlob(unsigned BlobIndex, unsigned OldTempIndex,
                       unsigned NewTempIndex, unsigned &NewBlobIndex);
};

} // End namespace loopopt

} // End namespace llvm

#endif
