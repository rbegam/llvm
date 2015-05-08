//===- CanonExpr.h - Closed form in high level IR ---------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the closed form representation in high level IR.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_INTEL_LOOPIR_CANONEXPR_H
#define LLVM_IR_INTEL_LOOPIR_CANONEXPR_H

#include "llvm/ADT/SmallVector.h"

#include "llvm/Support/Compiler.h"
#include "llvm/Support/FormattedStream.h"

#include <stdint.h>
#include <utility>
#include <set>
#include <vector>
#include <iterator>

namespace llvm {

class Type;
class SCEV;

namespace loopopt {

/// \brief The maximum loopnest level allowed in HIR.
const unsigned MaxLoopNestLevel = 9;

/// \brief Canonical form in high level IR
///
/// This class represents the closed form as a linear equation in terms of
/// induction variables and blobs. It is essentially an array of coefficients
/// of induction variables and blobs. A blob is usually a non-inductive,
/// loop invariant variable but is allowed to vary under some cases where a more
/// generic representation is required. Blob exprs are represented using SCEVs
/// and mapped to blob indexes.
/// The denominator is always stored as a positive value. If a client sets a
/// negative denominator value, the numerator is negated instead.
///
/// CanonExpr representation-
/// (C1 * i1 + C2 * i2 + ... + BC1 * b1 + BC2 * b2 + ... + C0) / D
///
/// Where:
/// - i1, i2 etc are induction variables of loop at level 1, 2 etc.
/// - C1, C2 etc are coefficients of i1, i2 etc.
/// - b1, b2 etc are blobs.
/// - BC1, BC2 etc are coefficients of b1, b2 etc.
/// - C0 is the constant additive.
/// - D is the denominator.
///
/// This class disallows creating objects on stack.
/// Objects are created/destroyed using CanonExprUtils friend class.
class CanonExpr {
public:
  struct BlobOrConstToVal {
    bool IsBlobCoeff;
    /// Represents BlobIndex if IsBlobCoeff is true else represents Coeff Value.
    int64_t Coeff;
    BlobOrConstToVal(bool IsBlobCoef, int64_t Coef);
    ~BlobOrConstToVal();
  };

  struct BlobIndexToCoeff {
    /// Index range is [1, UINT_MAX].
    unsigned Index;
    int64_t Coeff;
    BlobIndexToCoeff(unsigned Indx, int64_t Coef);
    ~BlobIndexToCoeff();
  };

  typedef const SCEV *BlobTy;
  typedef SmallVector<BlobOrConstToVal, 4> IVCoeffsTy;
  /// Kept sorted by blob index
  typedef SmallVector<BlobIndexToCoeff, 2> BlobCoeffsTy;

  /// Iterators to iterate over induction variables
  typedef IVCoeffsTy::iterator iv_iterator;
  typedef IVCoeffsTy::const_iterator const_iv_iterator;
  typedef IVCoeffsTy::reverse_iterator reverse_iv_iterator;
  typedef IVCoeffsTy::const_reverse_iterator const_reverse_iv_iterator;

  /// Iterators to iterate over blobs
  typedef BlobCoeffsTy::iterator blob_iterator;
  typedef BlobCoeffsTy::const_iterator const_blob_iterator;
  typedef BlobCoeffsTy::reverse_iterator reverse_blob_iterator;
  typedef BlobCoeffsTy::const_reverse_iterator const_reverse_blob_iterator;

private:
  /// \brief Copy constructor; only used for cloning.
  CanonExpr(const CanonExpr &);
  /// \brief Make class unassignable.
  void operator=(const CanonExpr &) = delete;

  /// \brief Destroys all objects of this class. Should only be
  /// called after code gen.
  static void destroyAll();
  /// Keeps track of objects of this class.
  static std::set<CanonExpr *> Objs;

  /// BlobTable - vector containing blobs for the function.
  /// TODO: Think about adding another vector sorted by blobs to provide faster
  /// lookup for Blob -> Index.
  /// Moved here from HIRParser to allow printer to print blobs without needing
  /// the parser.
  static SmallVector<BlobTy, 32> BlobTable;

  Type *Ty;
  int DefinedAtLevel;
  IVCoeffsTy IVCoeffs;
  BlobCoeffsTy BlobCoeffs;
  int64_t Const;
  int64_t Denominator;

  /// \brief Internal method to check blob index range.
  static bool isBlobIndexValid(unsigned Index);

  /// \brief Internal method to check level range.
  static bool isLevelValid(unsigned Level);

protected:
  CanonExpr(Type *Typ, unsigned DefLevel, int64_t ConstVal, int64_t Denom);
  ~CanonExpr() {}

  friend class CanonExprUtils;
  friend class HIRParser;

  /// \brief Destroys the object.
  void destroy();

  /// \brief Implements find()/insert() functionality.
  static unsigned findOrInsertBlobImpl(BlobTy Blob, bool Insert);

  /// \brief Returns the index of Blob in the blob table. Index range is [1,
  /// UINT_MAX]. Returns 0 if the blob is not present in the table.
  static unsigned findBlob(BlobTy Blob);
  /// \brief Returns the index of Blob in the blob table. Blob is first
  /// inserted, if it isn't already present in the blob table. Index range is
  /// [1, UINT_MAX].
  static unsigned findOrInsertBlob(BlobTy Blob);

  /// \brief Returns blob corresponding to BlobIndex.
  static BlobTy getBlob(unsigned BlobIndex);

  /// \brief Resizes IVCoeffs to max loopnest level if the passed in level goes
  /// beyond the current size. This will avoid future reallocs.
  /// Returns true if we did resize.
  bool resizeIVCoeffsToMax(unsigned Lvl);

  /// \brief Sets an IV coefficient. Depending upon the overwrite flag the
  /// existing coefficient is either overwritten or added to.
  void addIVInternal(unsigned Lvl, int64_t Coeff, bool IsBlobCoeff,
                     bool overwrite);

  /// \brief Sets a blob coefficient. Depending upon the overwrite flag the
  /// existing coefficient is either overwritten or added to.
  void addBlobInternal(unsigned BlobIndex, int64_t BlobCoeff, bool overwrite);

  /// Non-const IV iterator methods
  iv_iterator iv_begin() { return IVCoeffs.begin(); }
  iv_iterator iv_end() { return IVCoeffs.end(); }
  reverse_iv_iterator iv_rbegin() { return IVCoeffs.rbegin(); }
  reverse_iv_iterator iv_rend() { return IVCoeffs.rend(); }

  /// Non-const blob iterator methods
  blob_iterator blob_begin() { return BlobCoeffs.begin(); }
  blob_iterator blob_end() { return BlobCoeffs.end(); }
  reverse_blob_iterator blob_rbegin() { return BlobCoeffs.rbegin(); }
  reverse_blob_iterator blob_rend() { return BlobCoeffs.rend(); }

public:
  CanonExpr *clone() const;
  /// \brief Dumps CanonExpr.
  void dump() const;
  /// \brief Prints CanonExpr.
  void print(formatted_raw_ostream &OS) const;

  /// \brief Returns the LLVM type of this canon expr.
  Type *getLLVMType() const { return Ty; }
  void setLLVMType(Type *Typ) { Ty = Typ; }

  /// \brief Returns the innermost level at which some blob present
  /// in this canon expr is defined. The canon expr in linear in all
  /// the inner loop levels w.r.t this level.
  unsigned getDefinedAtLevel() const {
    assert(isLinearAtLevel() &&
           "DefinedAtLevel is meaningless for non-linear types!");
    return DefinedAtLevel;
  }
  /// \brief Sets the defined at level.
  void setDefinedAtLevel(unsigned DefLvl) {
    assert((DefLvl <= MaxLoopNestLevel) && "DefLvl exceeds max level!");
    DefinedAtLevel = DefLvl;
  }

  /// \brief Returns true if this is linear at all levels.
  bool isProperLinear() const { return (DefinedAtLevel == 0); }
  /// \brief Returns true if this is not non-linear.
  bool isLinearAtLevel() const { return (DefinedAtLevel >= 0); }
  /// \brief Returns true if some blob in the canon expr is defined in
  /// the current loop level.
  bool isNonLinear() const { return (DefinedAtLevel == -1); }
  /// \brief Mark this canon expr as non-linear.
  void setNonLinear() { DefinedAtLevel = -1; }
  /// \brief Returns true if this Canon Expr only contains a constant
  bool isConstant() const {
    return !(hasIV() || hasBlob() || (getDenominator() != 1));
  }

  /// \brief Returns the constant additive of the canon expr.
  int64_t getConstant() const { return Const; }
  void setConstant(int64_t Val) { Const = Val; }

  /// \brief Returns the denominator of the canon expr.
  int64_t getDenominator() const { return Denominator; }
  // \brief Sets canon expr's denominator. Negates it for negative denominators.
  void setDenominator(int64_t Val);

  /// \brief Returns true if this contains any IV.
  bool hasIV() const;
  /// \brief Returns true if this contains any Blob IV Coeffs.
  /// Examples: -M*i, N*j
  bool hasBlobIVCoeffs() const;
  /// \brief Returns true if this contains any blobs.
  bool hasBlob() const { return !BlobCoeffs.empty(); }

  /// \brief Returns the IV coefficient at a particular loop level. Lvl's
  /// range is [1, MaxLoopNestLevel].
  int64_t getIVCoeff(unsigned Lvl, bool *IsBlobCoeff) const;
  /// \brief Sets the IV coefficient at a particular loop level. Lvl's range
  /// is [1, MaxLoopNestLevel].
  void setIVCoeff(unsigned Lvl, int64_t Coeff, bool IsBlobCoeff);

  /// \brief Adds to the existing constant IV coefficient at a particular loop
  /// level.
  void addIV(unsigned Lvl, int64_t Coeff);
  /// \brief Removes IV at a particular loop level.
  void removeIV(unsigned Lvl);

  /// \brief Replaces IV by a constant at a particular loop level.
  void replaceIVByConstant(unsigned Lvl, int64_t Val);

  /// \brief Returns the blob coefficient.
  int64_t getBlobCoeff(unsigned BlobIndex) const;
  /// \brief Sets the blob coefficient.
  void setBlobCoeff(unsigned BlobIndex, int64_t BlobCoeff);

  /// \brief Adds to the existing blob coefficient.
  void addBlob(unsigned BlobIndex, int64_t BlobCoeff);
  /// \brief Removes a blob.
  void removeBlob(unsigned BlobIndex);

  /// \brief Replaces an old blob with a new one.
  void replaceBlob(unsigned OldBlobIndex, unsigned NewBlobIndex);

  /// \brief Clears everything from the CanonExpr except Type. Denominator is
  /// set to 1.
  void clear();

  /// \brief Shifts the canon expr by a constant offset at a particular loop
  /// level.
  void shift(unsigned Lvl, int64_t Val);

  /// \brief Multiplies this canon expr by a blob.
  void multiplyByBlob(unsigned BlobIndex);

  /// \brief Populates BlobIndices with all blobs contained in the CanonExpr
  /// (including blob IV coeffs).
  void extractBlobIndices(SmallVectorImpl<unsigned> &BlobIndices);

  /// IV iterator methods
  /// c-version allows use of "auto" keyword and doesn't conflict with protected
  /// non-const begin() / end().
  const_iv_iterator iv_cbegin() const { return IVCoeffs.begin(); }
  const_iv_iterator iv_cend() const { return IVCoeffs.end(); }
  const_reverse_iv_iterator iv_crbegin() const { return IVCoeffs.rbegin(); }
  const_reverse_iv_iterator iv_crend() const { return IVCoeffs.rend(); }

  /// Blob iterator methods
  /// c-version allows use of "auto" keyword and doesn't conflict with protected
  /// non-const begin() / end().
  const_blob_iterator blob_cbegin() const { return BlobCoeffs.begin(); }
  const_blob_iterator blob_cend() const { return BlobCoeffs.end(); }
  const_reverse_blob_iterator blob_crbegin() const {
    return BlobCoeffs.rbegin();
  }
  const_reverse_blob_iterator blob_crend() const { return BlobCoeffs.rend(); }
};

} // End loopopt namespace

} // End llvm namespace

#endif
