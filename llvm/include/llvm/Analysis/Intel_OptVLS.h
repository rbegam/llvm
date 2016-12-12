//===- OptVLS.h - Optimization of Vector Loads/Stores ----------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
///
/// \file
/// OptVLS performs two optimizations:
/// 1. Replaces a set of complex loads/stores(indexed, strided) by a set of
///    simple loads/stores(contiguous) followed by shuffle/permute
/// 2. Replaces a set of overlapping accesses by a set of fewer loads/stores
///    followed by shuffle/permute.
///
/// OptVLS is IR agnostic. It provides abstract types to communicate with its
/// clients. Various clients of OptVLS are Intel loop optimizer, Intel
/// vectorizer, OptVLSPass, etc which have their own IR such as HIR, AL,
/// LLVM IR respectively.
/// This file contains the declarations of OptVLS abstract types and the
/// interface classes that operate on and return these OptVLS abstract types.
/// The interface classes expose the core functionalities of OptVLS such as
/// grouping, cost-analysis, sequence generation, etc which are used as the
/// common interfaces for all the clients of OptVLS.
///
/// Clients must implement the various virtual methods which provide the
/// necessary information of client APIs to OptVLS.
///
//===---------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_INTEL_OPTVLS_H
#define LLVM_ANALYSIS_INTEL_OPTVLS_H

#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Support/Casting.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include <map>

namespace llvm {

class TargetTransformInfo;
class Type;

// OptVLS data structures
template <typename T> class OVLSVector : public SmallVector<T, 8> {};

template <typename T> class OVLSSmallPtrSet : public SmallPtrSet<T, 2> {};

template <typename KeyT, typename ValueT>
class OVLSMap : public std::multimap<KeyT, ValueT> {};

// For printing under debug.
typedef class raw_ostream OVLSostream;
#define OVLSdbgs() dbgs()

#define OVLSDebug(x) DEBUG(x)

// Current maximum supported vector length is 64 bytes (512 bits).
#define MAX_VECTOR_LENGTH 64
#define BYTE 8

// OptVLS Abstract Types
typedef class OVLSMemref OVLSMemref;
typedef OVLSVector<OVLSMemref *> OVLSMemrefVector;

typedef class OVLSGroup OVLSGroup;
typedef OVLSVector<OVLSGroup *> OVLSGroupVector;

typedef class OVLSInstruction OVLSInstruction;
typedef OVLSVector<OVLSInstruction *> OVLSInstructionVector;

typedef OVLSMap<OVLSMemref *, OVLSGroup *> OVLSMemrefToGroupMap;

// AccessType: {Strided|Indexed}{Load|Store}
class OVLSAccessType {
private:
  // S:Strided I:Indexed
  enum ATypeE { Unknown, SLoad, SStore, ILoad, IStore };

  ATypeE AccType;

public:
  explicit OVLSAccessType(ATypeE AccType) { this->AccType = AccType; }
  bool operator==(ATypeE AccType) const { return this->AccType == AccType; }
  bool operator==(const OVLSAccessType &Rhs) const {
    return AccType == Rhs.AccType;
  }
  bool operator!=(ATypeE AccType) const { return this->AccType != AccType; }
  bool isUnknown() const {
    if (AccType > IStore || AccType < SLoad)
      return true;
    return false;
  }

  void print(OVLSostream &OS) const;

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  /// This method is used for debugging.
  ///
  void dump() const;
#endif

  static OVLSAccessType getStridedLoadTy() { return OVLSAccessType(SLoad); }
  static OVLSAccessType getStridedStoreTy() { return OVLSAccessType(SStore); }
  static OVLSAccessType getIndexedLoadTy() { return OVLSAccessType(ILoad); }
  static OVLSAccessType getIndexedStoreTy() { return OVLSAccessType(IStore); }
  static OVLSAccessType getUnknownTy() { return OVLSAccessType(Unknown); }

  bool isStridedAccess() const { return AccType == SLoad || AccType == SStore; }
  bool isStridedLoad() const { return AccType == SLoad; }

  bool isIndexedAccess() const { return AccType == ILoad || AccType == IStore; }

  bool isGather() const { return AccType == ILoad || AccType == SLoad; }
};

/// Defines OVLS data type which is a vector type, representing a vector of
/// elements. A vector type requires a size (number of elements in the vector)
/// and an element size in bits. The kinds of instructions OVLS deals with (
/// i.e load/store permute/shift) do not require element type such as integer,
/// float. Knowing the element size is sufficient.
/// Please note that, since OVLS server works with target independent abstract
/// instructions (OVLSInstruction), it has no restrictions on the sizes.
/// Any size is considered as a valid size.
/// Syntax:  < <# elements> x <element-size> >
class OVLSType {
private:
  uint32_t ElementSize; // in bits
  uint32_t NumElements;

public:
  OVLSType() {
    ElementSize = 0;
    NumElements = 0;
  }
  OVLSType(uint32_t ESize, uint32_t NElems) {
    assert(NElems != 0 && "Number of elements cannot be zero in a vector");
    assert(ESize != 0 && "Element size cannot be zero in a vector");
    ElementSize = ESize;
    NumElements = NElems;
  }

  bool operator==(OVLSType Rhs) const {
    return ElementSize == Rhs.ElementSize && NumElements == Rhs.NumElements;
  }

  bool operator!=(OVLSType Rhs) const {
    return ElementSize != Rhs.ElementSize || NumElements != Rhs.NumElements;
  }

  bool isValid() const { return ElementSize != 0 && NumElements != 0; }

  uint32_t getElementSize() const { return ElementSize; }
  void setElementSize(uint32_t ESize) { ElementSize = ESize; }

  uint32_t getNumElements() const { return NumElements; }
  void setNumElements(uint32_t NElems) { NumElements = NElems; }

  uint32_t getSize() const { return NumElements * ElementSize; }

  /// \brief prints the type as "<NumElements x ElementSize>"
  void print(OVLSostream &OS) const {
    OS << "<" << NumElements << " x " << ElementSize << ">";
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  /// This method is used for debugging.
  ///
  void dump() const {
    print(OVLSdbgs());
    OVLSdbgs() << '\n';
  }
#endif
};

// Printing of OVLStypes.
static inline OVLSostream &operator<<(OVLSostream &OS, OVLSType T) {
  T.print(OS);
  return OS;
}

class OVLSMemref {
public:
  /// Discriminator for LLVM-style RTTI (dyn_cast<> et al.)
  enum OVLSMemrefKind { VLSK_ClientMemref, VLSK_HIRVLSClientMemref };

private:
  const OVLSMemrefKind Kind;

public:
  OVLSMemrefKind getKind() const { return Kind; }

  explicit OVLSMemref(OVLSMemrefKind K, OVLSType Type,
                      const OVLSAccessType &AccType);

  virtual ~OVLSMemref() {}

  OVLSType getType() const { return DType; }
  void setType(OVLSType T) { DType = T; }

  void setNumElements(uint32_t nelems) { DType.setNumElements(nelems); }
  OVLSAccessType getAccessType() const { return AccType; }
  void setAccessType(const OVLSAccessType &Type) { AccType = Type; }

  unsigned getId() const { return Id; }

  void print(OVLSostream &OS, unsigned SpaceCount) const;

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  /// This method is used for debugging.
  ///
  void dump() const;
#endif

  /// \brief If the references are scalar, returns true if this and the Memref
  /// are a constant distance apart. If the memrefs are vectors returns true if
  /// all ith elements of this and the ith elements of the \p Memref are a
  /// constant distance apart. Otherwise, returns false. When true is returned
  /// the constant distance is returned in \p Dist in terms of bytes, otherwise
  /// \p Dist is undefined.
  ///
  /// Please note that, VLS requires the distance that LLVM-IR maintains between
  /// the memrefs. Therefore, this distance computation in the client should
  /// follow the LLVM/vectorizer standard address computation formula for the
  /// memrefs.
  ///
  /// This restriction can be waved in the future if it shows that optimizing
  /// memrefs with non-uniform distances between the ith elements is profitable.
  ///
  /// An example of non-uniform distances between the ith elements:
  /// int32_t a[n];
  /// for (i = 0, n)
  ///      = a[3i+1] {stride: j(12)-bytes} accessing every jth byte
  ///      = a[3i+2] {stride: k(16)-bytes} accessing every kth byte
  /// This function will return false for the above two memrefs since distances
  /// between the ith elements are not uniform(distance between the 1st two
  /// elements is 4 bytes, 2nd two elements is 8 bytes). But this function will
  /// return true for the following two memrefs,
  /// for (i = 0, n)
  ///      = a[3i+1] {stride: j(12)-bytes} accessing every jth byte
  ///      = a[3i+2] {stride: j(12)-bytes} accessing every jth byte
  ///
  virtual bool isAConstDistanceFrom(const OVLSMemref &Memref,
                                    int64_t *Dist) = 0;

  /// \brief Returns true if this and Memref have the same number of elements.
  virtual bool haveSameNumElements(const OVLSMemref &Memref) = 0;

  /// \brief Returns true if this can move to the location of \p Memref. This
  /// means it does not violate any program/control flow semantics nor any
  /// memory dependencies. I.e., this is still alive at the location of
  /// \p Memref and there are no loads/stores that may alias with this in
  /// between the location of this and the location of \p Memref.
  /// canMoveTo() only answers the individual legality question that it is
  /// asked; it does not know if the move will actually be carried out by the
  /// caller, and has no context/memory of moves that had been asked before.
  /// Therefore, if the caller uses canMoveTo multiple times to ask about
  /// accumulative moves, the answers may not be valid, unless the following
  /// two conditions are met:
  /// 1) caller only moves loads up, and only moves stores down, based on the
  /// getLocation() function; This will guarantee that no new Write-After-Read
  /// (WAR) dependencies will be introduced. (A TODO on the server side).
  /// 2) canMoveTo will not allow any moves in the face of any Read-After-Write
  /// (RAW) dependences. (A TODO on the client canMoveTo side)
  ///
  /// Here's an example where individual moves can be legal independently, but
  /// not together (accumulatively):
  ///
  /// For i:
  ///   …  =  b[4*i + 4]      // ld1
  ///   b[4*i - 1] = …        // st1
  ///   …  =  b[4*i + 1]      // ld2
  ///   b[4*i] = …            // st2
  ///
  /// (the only dependence is a forward Write-After-Read (WAR) dep between
  /// ld1-->st2); Consider the following sequence of calls to canMoveTo:
  ///
  /// ld1->canMoveTo(ld2): returns true
  /// st2->canMoveTo(st1): returns true, but this is wrong if previous
  ///                      canMoveTo was actually committed.
  ///
  /// Validity of canMoveTo answers upon multiple calls that assume
  /// accumulative moves will be guaranteed with the following sequence of
  /// calls, in which loads are hoisted up, namely -- moved towards the
  /// load with the smaller getLocation() between this and Memref; And
  /// stores are only sinked down, namely moved towards the store with
  /// larger getLocation() among this and Memref:
  ///
  /// ld2->canMoveTo(ld1): returns true
  /// st1->canMoveTo(st2): returns true, and this is valid even if previous
  ///                      move took place.
  virtual bool canMoveTo(const OVLSMemref &Memref) = 0;

  /// \brief Returns true if this is a strided access and it has a constant
  /// uniform distance between the elements, that constant integer distance (in
  /// bytes) is provided in \p Stride. Otherwise, returns false.
  /// Inverting the return value does not invert the functionality(false does
  /// not mean that it has a variable stride)
  virtual bool hasAConstStride(int64_t *Stride) = 0;

  /// \brief Return the location of this in the code. The location should be
  /// relative to other Memrefs sent by the client to the VLS engine.
  /// getLocation can be used for a location-based group-formation heuristic.
  /// A location-based heuristic can be useful in order to use canMoveTo()
  /// *multiple* times, to ask about *accumulative* moves (moves that are all
  /// assumed to take place, if approved). The scheme is to only move loads up
  /// and only move stores down.
  /// So, for every pair of loads (ld1,ld2) that the caller wants
  /// to put together in one group, the caller would ask about moving ld1 to
  /// the location of ld2 only if ld2->getLocation() < ld1->getLocation().
  /// Otherwise, the caller should ask about moving ld2 to the location of ld1.
  virtual unsigned getLocation() const = 0;

private:
  unsigned Id;            // A unique Id, helps debugging.
  OVLSType DType;         // represents the memref data type.
  OVLSAccessType AccType; // Access type of the Memref, e.g {S|I}{Load|store}
};

class OVLSGroup {
public:
  explicit OVLSGroup(int VLen, const OVLSAccessType &AType)
      : VectorLength(VLen), AccType(AType) {
    NByteAccessMask = 0;
    ElementMask = 0;
  }

  typedef OVLSMemrefVector::iterator iterator;
  inline iterator begin() { return MemrefVec.begin(); }
  inline iterator end() { return MemrefVec.end(); }

  typedef OVLSMemrefVector::const_iterator const_iterator;
  inline const_iterator begin() const { return MemrefVec.begin(); }
  inline const_iterator end() const { return MemrefVec.end(); }

  // Returns true if the group is empty.
  bool empty() const { return MemrefVec.empty(); }
  // Insert an element into the Group and set the masks accordingly.
  void insert(OVLSMemref *Mrf, uint64_t AMask, uint64_t EMask) {
    MemrefVec.push_back(Mrf);
    NByteAccessMask = AMask;
    ElementMask = EMask;
  }

  // Returns group access mask.
  uint64_t getNByteAccessMask() const { return NByteAccessMask; }
  void setAccessMask(uint64_t Mask) { NByteAccessMask = Mask; }
  OVLSAccessType getAccessType() const { return AccType; }
  uint32_t getVectorLength() const { return VectorLength; }
  uint64_t getElementMask() const { return ElementMask; }
  void setElementMask(uint64_t Mask) { ElementMask = Mask; }

  bool hasStridedAccesses() const { return AccType.isStridedAccess(); }

  // Gathers collectively refers to both indexed and strided loads.
  bool hasGathers() const { return AccType.isGather(); }

  // Returns the total number of memrefs that this group contains.
  uint32_t size() const { return MemrefVec.size(); }

  // Return the first OVLSMemref of this group.
  OVLSMemref *getFirstMemref() const {
    if (!MemrefVec.empty())
      return MemrefVec[0];
    return nullptr;
  }

  OVLSMemref *getMemref(uint32_t Id) const {
    assert(Id < MemrefVec.size() && "Invalid MemrefId!!!\n");
    return MemrefVec[Id];
  }

  /// \brief Return true and the constant stride in \p ConstStride if all of the
  /// memrefs in the group have the same constant stride. Otherwise, returns
  /// false and untouched GStride. Stride represents a uniform distance between
  /// the vector elements of a OVLSMemref.
  /// Inverting the function return does not invert the functionality
  /// (e.g. false does not mean the group has a variable stride).
  bool hasAConstStride(int64_t &ConstStride) const {
    int64_t Stride = 0;
    if (getFirstMemref()->hasAConstStride(&Stride)) {
      // A group only comprises the memrefs that have the same matching strides.
      // Therefore, checking whether the first memref in the group has a
      // constant stride is sufficient.
      ConstStride = Stride;
      return true;
    }
    return false;
  }

  // Assuming all members have the same element size.
  // TODO: Support heterogeneous types using GCD
  uint32_t getElemSize() const {
    return MemrefVec[0]->getType().getElementSize();
  }

  // Currently, a group is formed only if the members have the same number
  // of elements.
  uint32_t getNumElems() const {
    return MemrefVec[0]->getType().getNumElements();
  }

  /// \brief Return the vector of memrefs of this group.
  const OVLSMemrefVector &getMemrefVec() const { return MemrefVec; }

  void print(OVLSostream &OS, unsigned SpaceCount) const;

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  /// This method is used for debugging.
  ///
  void dump() const;
#endif

private:
  /// \brief Group element-vector
  OVLSMemrefVector MemrefVec;

  /// \brief Vector length in bytes, default/maximum supported length is 64.
  /// VectorLength can be the maximum length of the underlying vector register
  /// or any other desired size that clients want to consider.
  uint32_t VectorLength;

  /// \brief NByteAccessMask is a byte mask, represents the access pattern for
  /// each N bytes comprising the i-th element of the memrefs in the MemrefVec,
  /// here N <= VectorLength. Each bit in the mask corresponds to a byte.
  /// Specifically, it tells us if there are any gaps in between the i-th
  /// accesses (since access pattern information is not recorded in the
  /// MemrefVec to save memory) Maximum 64 bytes can be represented.
  uint64_t NByteAccessMask;

  /// \brief AccessType of the group.
  OVLSAccessType AccType;

  /// \brief Represents an element-wise mask for the ith elements of the
  /// MemrefVec. Memrefs can have different element sizes but they will have
  /// common divisors. The greatest common divisor is considered as an element
  /// in the ElementMask.
  uint64_t ElementMask;
};

/// OVLSOperand is used to define an operand object for OVLSInstruction.
/// TODO: Support Operand Type.
class OVLSOperand {

public:
  /// An operand can be an address or a temp.
  enum OperandKind { OK_Address, OK_Instruction, OK_Constant };

  explicit OVLSOperand(OperandKind K, OVLSType T) : Kind(K), Type(T) {}

  explicit OVLSOperand(OperandKind K) : Kind(K) {}
  OVLSOperand() {}

  ~OVLSOperand() {}

  OperandKind getKind() const { return Kind; }
  OVLSType getType() const { return Type; }

  virtual void print(OVLSostream &OS, unsigned NumSpaces) const {}

  virtual void printAsOperand(OVLSostream &OS) const { OS << Type << "undef"; }

private:
  OperandKind Kind;

protected:
  OVLSType Type;
};

/// OVLSConstant provides a raw bitstream to represent a constant of
/// any type.
class OVLSConstant : public OVLSOperand {
private:
  static const int32_t BitWidth = 1024;
  uint8_t ConstValue[BitWidth / 8];

public:
  explicit OVLSConstant(OVLSType T, int8_t *V) : OVLSOperand(OK_Constant, T) {
    assert(T.getSize() <= BitWidth && "Unsupported OVLSConstant size!");
    memcpy(ConstValue, V, T.getSize());
  }

  static bool classof(const OVLSOperand *Operand) {
    return Operand->getKind() == OK_Constant;
  }

  void print(OVLSostream &OS, unsigned NumSpaces) const {
    OVLSType Type = getType();
    uint32_t NumElems = Type.getNumElements();
    OS << Type;

    switch (Type.getElementSize()) {
    case 32: {
      OS << "<" << *(reinterpret_cast<const int *>(&ConstValue[0]));
      for (uint32_t i = 1; i < NumElems; i++)
        OS << ", " << *(reinterpret_cast<const int *>(&ConstValue[i * 4]));

      OS << ">";
      break;
    }
    default:
      OVLSdbgs() << "Not supported\n";
      break;
    }
  }
};

/// OVLSAddress{Base, Offset} represents an address that is Offset
/// bytes from the Base(which is an address of an OVLSMemref).
class OVLSAddress : public OVLSOperand {
public:
  explicit OVLSAddress(OVLSMemref *B, int64_t O)
      : OVLSOperand(OK_Address), Base(B), Offset(O) {}

  explicit OVLSAddress() {}

  static bool classof(const OVLSOperand *Operand) {
    return Operand->getKind() == OK_Address;
  }

  void setBase(OVLSMemref *B) { Base = B; }
  void setOffset(int64_t O) { Offset = O; }
  const OVLSMemref *getBase() const { return Base; }
  int64_t getOffset() const { return Offset; }

  OVLSAddress &operator=(const OVLSOperand &Operand) {
    assert(isa<OVLSAddress>(&Operand) && "Expected An Address Operand!!!");
    const OVLSAddress *AddrOperand = cast<const OVLSAddress>(&Operand);
    Base = AddrOperand->Base;
    Offset = AddrOperand->Offset;

    return *this;
  }

  void print(OVLSostream &OS) const {
    OS << "<Base:" << Base << " Offset:" << Offset << ">";
  }
#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const {
    print(OVLSdbgs());
    OVLSdbgs() << '\n';
  }
#endif

private:
  /// \brief Represents the address of the memory reference that is pointed to
  /// by the Base.
  const OVLSMemref *Base;
  /// \brief Represents a distance in bytes from Base.
  int64_t Offset;
};

class OVLSInstruction : public OVLSOperand {
public:
  enum OperationCode { OC_Load, OC_Store, OC_Shuffle };

  explicit OVLSInstruction(OperationCode OC, OVLSType T)
      : OVLSOperand(OK_Instruction, T), OPCode(OC) {
    static uint64_t InstructionId = 1;
    Id = InstructionId++;
  }

  virtual ~OVLSInstruction() {}

  static bool classof(const OVLSOperand *Operand) {
    return Operand->getKind() == OK_Instruction;
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  /// This method is used for debugging.
  ///
  virtual void dump() const = 0;
#endif

  uint64_t getId() const { return Id; }

  void printAsOperand(OVLSostream &OS) const { OS << Type << " %" << Id; }
  OperationCode getKind() const { return OPCode; }

private:
  OperationCode OPCode;

  /// \brief Class identification, helps debugging.
  uint64_t Id;
};

class OVLSLoad : public OVLSInstruction {

public:
  /// \brief Load <ESize x NElems> bits from S using \p EMask (element mask).
  explicit OVLSLoad(OVLSType T, const OVLSOperand &S, uint64_t EMask)
      : OVLSInstruction(OC_Load, T), ElemMask(EMask) {
    Src = S;
  }

  /// \brief Return the Address (Src) member of the Load.
  OVLSAddress getSrc() const { return Src; }

  static bool classof(const OVLSInstruction *I) {
    return I->getKind() == OC_Load;
  }

  void print(OVLSostream &OS, unsigned NumSpaces) const;

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const {
    print(OVLSdbgs(), 0);
    OVLSdbgs() << '\n';
  }
#endif

  uint64_t getMask() const { return ElemMask; }
  void setMask(uint64_t Mask) { ElemMask = Mask; }

private:
  OVLSAddress Src;

  /// \brief Reads a vector from memory using this mask. This mask holds a bit
  /// for each element.  When a bit is set the corresponding element in memory
  /// is accessed.
  uint64_t ElemMask;
};

/// OVLSShuffle instruction combines elements from the first two input vectors
/// into a new vector, with the selection and ordering of elements determined
/// by the 3rd vector, referred to as the shuffle mask. The first two operands
/// are vectors with the same type. The length of the shuffle mask can be of any
/// length that is less than or equal to twice the input vectors.
/// Therefore, the length of the result vector can be of any size that is the
/// same as the shuffle mask and the element size is the same as the element
/// size of the first two input vectors. The shuffle mask operand is required to
/// be a constant vector with either constant integer or undef values(~0).
/// For input vectors of width N, mask selector can be of 0..N-1
/// referring to the elements from the 1st input, and selector from N to 2N-1
/// refer to the 2nd input vector.
/// The mask value of -1 is treated as undef (meaning don't care), any value
/// can be put in the corresponding element of the result.
/// Second source vector can also be undef(NULL), that will mean shuffle only
/// from one vector.
/// Example:
/// <result> = shuffle <4 x i32> s1, // vector indices: 0, 1, 2, 3
///                    <4 x i32> s2, // vector indices: 4, 5, 6, 7
///                    <uint32_t*> mask // mask values: 0, 1, 4, 5
/// This shuffle instruction constructs an output vector of 4 elements, where
/// the first two elements are the 1st two elements of the 1st input vector
/// and the second two elements of the result vector are the first two elements
/// of the 2nd input vector.
class OVLSShuffle : public OVLSInstruction {

public:
  explicit OVLSShuffle(OVLSOperand *O1, OVLSOperand *O2, OVLSOperand *O3)
      : OVLSInstruction(OC_Shuffle, OVLSType(O1->getType().getElementSize(),
                                             O3->getType().getNumElements())) {
    assert(hasValidOperands(O1, O2, O3) &&
           "Invalid shuffle vector instruction operand!");
    Op1 = O1;
    Op2 = O2;
    Op3 = O3;
  }

  /// isValidOperands - Return true if a shufflevector instruction can be
  /// formed with the specified operands.
  bool hasValidOperands(OVLSOperand *O1, OVLSOperand *O2,
                        OVLSOperand *Mask) const;

  static bool classof(const OVLSInstruction *I) {
    return I->getKind() == OC_Shuffle;
  }

  void print(OVLSostream &OS, unsigned NumSpaces) const;

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const {
    print(OVLSdbgs(), 0);
    OVLSdbgs() << '\n';
  }
#endif

private:
  const OVLSOperand *Op1;
  const OVLSOperand *Op2;

  /// \p Op3 defines the shuffle mask, specifies for each element of the result
  /// vector, which element of the two source vectors the result element gets.
  /// Having -1 as a shuffle selector means "don't care".
  const OVLSOperand *Op3;
};

// Printing of OVLSOperand.
static inline OVLSostream &operator<<(OVLSostream &OS, const OVLSOperand &Op) {
  Op.print(OS, 2);
  return OS;
}

/// OVLS server works in a target independent manner. In order to estimate
/// more accurate cost for a specific target (architecture), client needs to
/// provide the necessary target-specific information.
/// This cost-model interface class defines all the necessary
/// parameters/functions that are needed by the server to estimate more accurate
/// cost with a default implementation of some of the member functions. In
/// order to get cost, client needs to provide an object of this class
/// filled up with the necessary target-specific cost information that are
/// defined by the underlying targets. Consequently, it's the clients that
/// decide on the cost accuracy level.
class OVLSCostModel {
  const TargetTransformInfo &TTI;

  /// Example of a 4-element reversed-mask {3, 2, 1, 0}
  // Please note that undef elements don't prevent us from matching
  // the reverse pattern.
  bool isReverseVectorMask(SmallVectorImpl<int> &Mask) const {
    for (unsigned i = 0, MaskSize = Mask.size(); i < MaskSize; ++i)
      if (Mask[i] >= 0 && Mask[i] != (int)(MaskSize - 1 - i))
        return false;
    return true;
  }

  // This detects alternate elements from the vectors such as:
  // a element alternate mask: <0, 5, 2, 7> or <4,1,6,3>.
  // Please note that undef elements don't prevent us from matching
  // the alternating pattern.
  bool isAlternateVectorMask(SmallVectorImpl<int> &Mask) const {
    bool IsAlternate = true;
    unsigned MaskSize = Mask.size();
    // A<0, 1, 2, 3>, B<4, 5, 6, 7>
    // Example of an alternate vector mask <0,5,2,7>
    for (unsigned i = 0; i < MaskSize && IsAlternate; ++i) {
      if (Mask[i] < 0)
        continue;
      IsAlternate = Mask[i] == (int)((i & 1) ? MaskSize + i : i);
    }

    if (IsAlternate)
      return true;

    IsAlternate = true;
    // Example: shufflevector <4xT>A, <4xT>B, <4,1,6,3>
    for (unsigned i = 0; i < MaskSize && IsAlternate; ++i) {
      if (Mask[i] < 0)
        continue;
      IsAlternate = Mask[i] == (int)((i & 1) ? i : MaskSize + i);
    }
    return IsAlternate;
  }

public:
  explicit OVLSCostModel(const TargetTransformInfo &TargetTI) : TTI(TargetTI) {}

  /// \brief Returns target-specific cost for an OVLSInstruction, different
  /// cost parameters are defined by each specific target.
  /// Returns -1 if the cost is unknown. This function needs to be overriden by
  /// the OVLS clients to help getting the target-specific instruction cost.
  virtual uint64_t getInstructionCost(const OVLSInstruction *I) const {
    return -1;
  }

  /// \brief Returns target-specific cost for loading/storing \p Mrf
  /// using a gather/scatter.
  virtual uint64_t getGatherScatterOpCost(const OVLSMemref &Mrf) const {
    return -1;
  }

  virtual uint64_t getShuffleCost(SmallVectorImpl<int> &Mask, Type *Tp) const;
};

// OptVLS public Interface class that operates on OptVLS Abstract types.
class OptVLSInterface {
public:
  /// \brief getGroups() groups the memrefs that are adjacent and returns
  /// the formed groups in \p Grps. It also optionally returns a map in
  /// \p MemrefToGroupMap which maps memref to the group that it belongs to.
  /// getGroups() takes a vector of OVLSMemrefs, a vector of OVLSGroups for
  /// containing the return group-vector and a vector length in bytes (which is
  /// the maximum length of the underlying vector register or any other
  /// desired size that clients want to consider, maximum size can be 64).
  /// Each group contains one or more OVLSMemrefs, and each OVLSMemref is
  /// contained by 1 (and only 1) OVLSGroup such that being
  /// together these memrefs in a group do not violate any program semantics or
  /// memory dependencies.
  /// Current grouping is done using a greedy approach; i.e. it keeps inserting
  /// adjacent memrefs into the same group until the total element size(
  /// considering a single element from each memref) is less than or equal to
  /// vector length. Currently, it only tries to form a group at the location
  /// of a memref that has a lowest distance from the base, it does not try
  /// other adjacent-memref-locations. Because of this greediness it can miss
  /// some opportunities. This can be improved in the future if needed.
  static void getGroups(const OVLSMemrefVector &Memrefs, OVLSGroupVector &Grps,
                        uint32_t VectorLength,
                        OVLSMemrefToGroupMap *MemrefToGroupMap = nullptr);

  /// \brief getGroupCost() examines if it is beneficial to perform
  /// adjacent gather/scatter optimization for a group (of gathers/scatters).
  /// Adj. gather/scatter optimization replaces a set of gathers/scatters by a
  /// set of contiguous loads/stores followed by a sequence of shuffle
  /// instructions. This method returns the minimum between these two costs;
  /// It computes the cost of the load/store+shuffle sequence, it computes the
  /// cost of the gathers/shuffles, and returns the lower of the two. This is
  /// how the vectorizer client currently uses this method: it assumes that it
  /// provides the absolute cost of the best way to vectorize this group.
  static int64_t getGroupCost(const OVLSGroup &Group, const OVLSCostModel &CM);

  /// \brief getSequence() takes a group of gathers/scatters and a cost model,
  /// returns true if it is able to generate a vector of instructions
  /// (basically a set of contiguous loads/stores followed by shuffles) that
  /// can replace (which is semantically equivalent) the gathers/scatters.
  /// Returns false if it is unable to generate the sequence. This function
  /// tries to generate the best optimized sequence(using the costmodel) without
  /// doing any relative cost/benefit analysis (which is gather/scatter vs. the
  /// generated sequence). The main purpose of this function is to help
  /// diagnostics.
  static bool getSequence(const OVLSGroup &Group, const OVLSCostModel &CM,
                          OVLSInstructionVector &InstVector);
};
}
#endif
