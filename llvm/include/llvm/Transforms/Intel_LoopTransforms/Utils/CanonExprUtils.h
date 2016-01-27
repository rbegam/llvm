//===------ CanonExprUtils.h - Utilities for CanonExpr class --*- C++ -*---===//
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
// This file defines the utilities for CanonExpr class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_UTILS_CANONEXPRUTILS_H
#define LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_UTILS_CANONEXPRUTILS_H

#include <stdint.h>
#include "llvm/Support/Compiler.h"

#include "llvm/IR/Intel_LoopIR/CanonExpr.h"

#include "llvm/Transforms/Intel_LoopTransforms/Utils/HLUtils.h"

#include <set>

namespace llvm {

class Type;
class APInt;

namespace loopopt {

/// \brief Defines utilities for CanonExpr class
///
/// It contains a bunch of static member functions which manipulate CanonExprs.
/// It does not store any state.
///
class CanonExprUtils : public HLUtils {
private:
  /// \brief Do not allow instantiation.
  CanonExprUtils() = delete;

  friend class HIRParser;
  friend class DDRefUtils;

  /// Keeps track of objects of the CanonExpr class.
  static std::set<CanonExpr *> GlobalCanonExprs;

  /// \brief Destroys all CanonExprs and BlobTable. Called during HIR cleanup.
  static void destroyAll();

  /// \brief Calculates the lcm of two positive inputs.
  static int64_t lcm(int64_t A, int64_t B);

  /// \brief Returns the index of Blob in the blob table. Blob is first
  /// inserted, if it isn't already present in the blob table. Index range is
  /// [1, UINT_MAX]. There is a 1-1 mapping of temp blob index and symbase. This
  /// information is stored in the blob table. This interface is private
  /// because only the framework is allowed to create temp blobs for insertion
  /// in the blob table.
  static unsigned findOrInsertBlob(CanonExpr::BlobTy Blob, unsigned Symbase);

  /// \brief Creates a non-linear self blob canon expr from the passed in Value.
  /// The new blob is associated with Symbase. New temp blobs from values are
  /// only created by framework.
  static CanonExpr *createSelfBlobCanonExpr(Value *Temp, unsigned Symbase);

  /// \brief Implements add()/cloneAndAdd() functionality.
  static CanonExpr *addImpl(CanonExpr *CE1, const CanonExpr *CE2,
                            bool CreateNewCE, bool IgnoreDestType);

public:
  /// \brief Returns a new CanonExpr with identical src and dest types. All
  /// canon exprs are created linear.
  static CanonExpr *createCanonExpr(Type *Ty, unsigned Level = 0,
                                    int64_t Const = 0, int64_t Denom = 1,
                                    bool IsSignedDiv = false);

  /// \brief Returns a new CanonExpr with zero or sign extension. All canon
  /// exprs are created linear.
  /// Note: Overloading createCanonExpr() causes ambiguous calls for constant
  /// arguments.
  static CanonExpr *createExtCanonExpr(Type *SrcType, Type *DestType,
                                       bool IsSExt, unsigned Level = 0,
                                       int64_t Const = 0, int64_t Denom = 1,
                                       bool IsSignedDiv = false);

  /// \brief Returns a new CanonExpr created from APInt Value.
  static CanonExpr *createCanonExpr(Type *Ty, const APInt &APVal,
                                    int Level = 0);

  /// \brief Returns a self-blob canon expr. Level is the defined at level for
  /// the blob. Level of -1 means non-linear blob.
  static CanonExpr *createSelfBlobCanonExpr(unsigned Index, int Level = -1);

  /// \brief Destroys the passed in CanonExpr.
  static void destroy(CanonExpr *CE);

  /// \brief Calculates the gcd of two positive inputs.
  static int64_t gcd(int64_t A, int64_t B);

  /// \brief Returns the index of Blob in the blob table. Index range is [1,
  /// UINT_MAX]. Returns invalid value if the blob is not present in the table.
  static unsigned findBlob(CanonExpr::BlobTy Blob);

  /// \brief Returns symbase corresponding to Blob. Returns invalid value for
  /// non-temp or non-present blobs.
  static unsigned findBlobSymbase(CanonExpr::BlobTy Blob);

  /// \brief Returns the index of Blob in the blob table. Blob is first
  /// inserted, if it isn't already present in the blob table. Index range is
  /// [1, UINT_MAX].
  /// NOTE: New temp blobs can only be inserted by the framework.
  static unsigned findOrInsertBlob(CanonExpr::BlobTy Blob);

  /// \brief Maps blobs in Blobs to their corresponding indices and inserts
  /// them in Indices.
  static void mapBlobsToIndices(const SmallVectorImpl<CanonExpr::BlobTy> &Blobs,
                                SmallVectorImpl<unsigned> &Indices);

  /// \brief Returns blob corresponding to BlobIndex.
  static CanonExpr::BlobTy getBlob(unsigned BlobIndex);

  /// \brief Returns symbase corresponding to BlobIndex. Returns invalid value
  /// for non-temp blobs.
  static unsigned getBlobSymbase(unsigned BlobIndex);

  /// \brief Prints blob.
  static void printBlob(raw_ostream &OS, CanonExpr::BlobTy Blob);

  /// \brief Prints scalar corresponding to symbase.
  static void printScalar(raw_ostream &OS, unsigned Symbase);

  /// \brief Checks if the blob is constant or not.
  /// If blob is constant, sets the return value in Val.
  static bool isConstantIntBlob(CanonExpr::BlobTy Blob, int64_t *Val);

  /// \brief Returns true if Blob is a temp.
  static bool isTempBlob(CanonExpr::BlobTy Blob);

  /// \brief Returns true if TempBlob always has a defined at level of zero.
  static bool isGuaranteedProperLinear(CanonExpr::BlobTy TempBlob);

  /// \brief Returns true if Blob is a UndefValue.
  static bool isUndefBlob(CanonExpr::BlobTy Blob);

  /// \brief Returns true if Blob represents a FP constant.
  static bool isConstantFPBlob(CanonExpr::BlobTy Blob);

  /// \brief Returns a new blob created from passed in Val.
  static CanonExpr::BlobTy createBlob(Value *Val, bool Insert = true,
                                      unsigned *NewBlobIndex = nullptr);

  /// \brief Returns a new blob created from a constant value.
  static CanonExpr::BlobTy createBlob(int64_t Val, Type *Ty, bool Insert = true,
                                      unsigned *NewBlobIndex = nullptr);

  /// \brief Returns a blob which represents (LHS + RHS). If Insert is true its
  /// index is returned via NewBlobIndex argument.
  static CanonExpr::BlobTy createAddBlob(CanonExpr::BlobTy LHS,
                                         CanonExpr::BlobTy RHS,
                                         bool Insert = true,
                                         unsigned *NewBlobIndex = nullptr);

  /// \brief Returns a blob which represents (LHS - RHS). If Insert is true its
  /// index is returned via NewBlobIndex argument.
  static CanonExpr::BlobTy createMinusBlob(CanonExpr::BlobTy LHS,
                                           CanonExpr::BlobTy RHS,
                                           bool Insert = true,
                                           unsigned *NewBlobIndex = nullptr);
  /// \brief Returns a blob which represents (LHS * RHS). If Insert is true its
  /// index is returned via NewBlobIndex argument.
  static CanonExpr::BlobTy createMulBlob(CanonExpr::BlobTy LHS,
                                         CanonExpr::BlobTy RHS,
                                         bool Insert = true,
                                         unsigned *NewBlobIndex = nullptr);
  /// \brief Returns a blob which represents (LHS / RHS). If Insert is true its
  /// index is returned via NewBlobIndex argument.
  static CanonExpr::BlobTy createUDivBlob(CanonExpr::BlobTy LHS,
                                          CanonExpr::BlobTy RHS,
                                          bool Insert = true,
                                          unsigned *NewBlobIndex = nullptr);
  /// \brief Returns a blob which represents (trunc Blob to Ty). If Insert is
  /// true its index is returned via NewBlobIndex argument.
  static CanonExpr::BlobTy createTruncateBlob(CanonExpr::BlobTy Blob, Type *Ty,
                                              bool Insert = true,
                                              unsigned *NewBlobIndex = nullptr);
  /// \brief Returns a blob which represents (zext Blob to Ty). If Insert is
  /// true its index is returned via NewBlobIndex argument.
  static CanonExpr::BlobTy
  createZeroExtendBlob(CanonExpr::BlobTy Blob, Type *Ty, bool Insert = true,
                       unsigned *NewBlobIndex = nullptr);
  /// \brief Returns a blob which represents (sext Blob to Ty). If Insert is
  /// true its index is returned via NewBlobIndex argument.
  static CanonExpr::BlobTy
  createSignExtendBlob(CanonExpr::BlobTy Blob, Type *Ty, bool Insert = true,
                       unsigned *NewBlobIndex = nullptr);

  /// \brief Returns true if Blob contains SubBlob or if Blob == SubBlob.
  static bool contains(CanonExpr::BlobTy Blob, CanonExpr::BlobTy SubBlob);

  /// \brief Returns all the temp blobs present in Blob via TempBlobs vector.
  static void collectTempBlobs(CanonExpr::BlobTy Blob,
                               SmallVectorImpl<CanonExpr::BlobTy> &TempBlobs);

  /// \brief Returns the size of the type in bits.
  /// NOTE: This function asserts that the incoming type is sized.
  static uint64_t getTypeSizeInBits(Type *Ty);

  /// \brief Returns true if the type of both Canon Expr matches.
  /// Ignores dest types of CE1 and CE2 if IgnoreDestType is set.
  static bool isTypeEqual(const CanonExpr *CE1, const CanonExpr *CE2,
                          bool IgnoreDestType = false);

  /// \brief Returns true if CE1 and CE2 can be merged (added/subtracted etc).
  /// Ignores dest types of CE1 and CE2 if IgnoreDestType is set.
  static bool mergeable(const CanonExpr *CE1, const CanonExpr *CE2,
                        bool IgnoreDestType = false);

  /// \brief Returns true if passed in canon cxprs are equal to each other.
  /// Ignores dest types of CE1 and CE2 if IgnoreDestType is set.
  static bool areEqual(const CanonExpr *CE1, const CanonExpr *CE2,
                       bool IgnoreDestType = false);

  /// \brief Modifies and returns CE1 to reflect sum of CE1 and CE2.
  /// CE1 = CE1 + CE2
  /// Resulting canon expr retains CE1's dest type if IgnoreDestType is true.
  static void add(CanonExpr *CE1, const CanonExpr *CE2,
                  bool IgnoreDestType = false);

  /// \brief Returns a canon expr which represents the sum of CE1 and CE2.
  /// Result = CE1 + CE2
  /// Resulting canon expr retains CE1's dest type if IgnoreDestType is true.
  static CanonExpr *cloneAndAdd(const CanonExpr *CE1, const CanonExpr *CE2,
                                bool IgnoreDestType = false);

  /// \brief Modifies and returns CE1 to reflect difference of CE1 and CE2.
  /// CE1 = CE1 - CE2
  /// Resulting canon expr retains CE1's dest type if IgnoreDestType is true.
  static void subtract(CanonExpr *CE1, const CanonExpr *CE2,
                       bool IgnoreDestType = false);

  /// \brief Returns a canon expr which represents the difference of CE1 and
  /// CE2.
  /// Result = CE1 - CE2
  /// Resulting canon expr retains CE1's dest type if IgnoreDestType is true.
  static CanonExpr *cloneAndSubtract(const CanonExpr *CE1, const CanonExpr *CE2,
                                     bool IgnoreDestType = false);

  /// \brief Returns a canon expr which represents the negation of CE.
  /// Result = -CE
  static CanonExpr *cloneAndNegate(const CanonExpr *CE);

  /// \brief Returns true if this CE should be considered non-linear given
  /// DefLevel and NestingLevel. DefLevel is the definition level of a blob
  /// contained in the CE. NestingLevel is the level where the CE is attached to
  /// HIR.
  static bool hasNonLinearSemantics(int DefLevel, unsigned NestingLevel);
};

} // End namespace loopopt

} // End namespace llvm

#endif
