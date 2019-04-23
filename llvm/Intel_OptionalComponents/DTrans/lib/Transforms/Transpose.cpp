//===--------------- Transpose.cpp - DTransTransposePass------------------===//
//
// Copyright (C) 2019-2019 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive property
// of Intel Corporation and may not be disclosed, examined or reproduced in
// whole or in part without explicit written authorization from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements the DTrans Transpose optimization for Fortran
// multi-dimensional arrays.
//
//===----------------------------------------------------------------------===//
#include "Intel_DTrans/Transforms/Transpose.h"
#include "Intel_DTrans/DTransCommon.h"

#include "llvm/Analysis/Intel_WP.h"
#include "llvm/Pass.h"

using namespace llvm;

#define DEBUG_TYPE "dtrans-transpose"

// Trace messages regarding the analysis of the candidate variables.
#define DEBUG_ANALYSIS "dtrans-transpose-analysis"

// Trace messages about the dope vector object analysis
#define DEBUG_DOPE_VECTORS "dtrans-transpose-dopevectors"

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
// Print the list of candidates identified and their analysis result.
static cl::opt<bool> PrintCandidates("dtrans-transpose-print-candidates",
                                     cl::ReallyHidden);
#endif // !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)

namespace {

// Maximum rank for a Fortran array.
const uint32_t FortranMaxRank = 9;

// Argument positions for parameters to subscript intrinsic call.
const unsigned RankOpNum = 0;
const unsigned LBOpNum = 1;
const unsigned StrideOpNum = 2;
const unsigned PtrOpNum = 3;

// Type to store a Function and an argument number
using FuncArgPosPair = std::pair<Function *, unsigned int>;
using FuncArgPosPairSet = SmallSet<FuncArgPosPair, 8>;
using FuncArgPosPairSetIter = SmallSet<FuncArgPosPair, 8>::const_iterator;

// Type to store a collection of CallInst pointers.
using CallInstSet = SmallPtrSet<CallInst *, 16>;
using CallInstSetIter = CallInstSet::const_iterator;

// An uplevel variable is a structure type that holds values or pointers of
// variables in the parent routine of nested routines. This type is to describe
// an uplevel use of a specific dope vector.  It consists of a variable and the
// field number of the structure containing the dope vector.
using UpevelDVField = std::pair<Value *, uint64_t>;

// Helper routine to check if a CallInst is to llvm.intel.subscript.
bool isSubscriptIntrinsicCall(const CallInst &CI) {
  const Function *F = CI.getCalledFunction();
  return F && F->getIntrinsicID() == Intrinsic::intel_subscript;
}

// Helper routine for checking and getting a constant integer from a GEP
// operand. If the value is not a constant, returns an empty object.
Optional<uint64_t> getConstGEPIndex(const GetElementPtrInst &GEP,
                                    unsigned int OpNum) {
  auto FieldIndex = dyn_cast<ConstantInt>(GEP.getOperand(OpNum));
  if (FieldIndex)
    return Optional<uint64_t>(FieldIndex->getLimitedValue());
  return None;
}

// Helper routine to get the argument index corresponding to \p Val within the
// call \p CI. If the operand is not passed to the function, or is in more than
// one position, returns an empty object.
Optional<unsigned int> getArgumentPosition(const CallInst &CI,
                                           const Value *Val) {
  Optional<unsigned int> Pos;
  unsigned int ArgCount = CI.getNumArgOperands();
  for (unsigned int ArgNum = 0; ArgNum < ArgCount; ++ArgNum)
    if (CI.getArgOperand(ArgNum) == Val) {
      if (Pos)
        return None;

      Pos = ArgNum;
    }

  return Pos;
}

// Check for arguments of a subscript intrinsic call for the expected values.
// The intrinsic call is declared as:
//    declare <ty>* @llvm.intel.subscript...(i8 <rank>, <ty> <lb>,
//                                           <ty> <stride>, <ty>* <base>,
//                                           <ty> <index>)
//
// Return 'true' if call has the expected values for the Base, and Rank.
// If the LowerBound and Stride parameters are supplied, also check those.
//
bool isValidUseOfSubscriptCall(const CallInst &CI, const Value &Base,
                               uint32_t ArrayRank, uint32_t Rank,
                               Optional<uint64_t> LowerBound = None,
                               Optional<uint64_t> Stride = None) {
  DEBUG_WITH_TYPE(DEBUG_ANALYSIS, {
    dbgs().indent((ArrayRank - Rank) * 2 + 4);
    dbgs() << "Checking call: " << CI << "\n";
  });

  if (!isSubscriptIntrinsicCall(CI))
    return false;

  if (CI.getArgOperand(PtrOpNum) != &Base)
    return false;

  auto RankVal = dyn_cast<ConstantInt>(CI.getArgOperand(RankOpNum));
  if (!RankVal || RankVal->getLimitedValue() != Rank)
    return false;

  if (LowerBound) {
    auto LBVal = dyn_cast<ConstantInt>(CI.getArgOperand(LBOpNum));
    if (!LBVal || LBVal->getLimitedValue() != *LowerBound)
      return false;
  }

  if (Stride) {
    auto StrideVal = dyn_cast<ConstantInt>(CI.getArgOperand(StrideOpNum));
    if (!StrideVal || StrideVal->getLimitedValue() != *Stride)
      return false;
  }

  return true;
}

// Helper routine to check whether a variable type is a type for an
// uplevel variable.
bool isUplevelVarType(Type *Ty) {
  // For now, just check the type of the variable as being named
  // "%uplevel_type[.#]" In the future, the front-end should provide some
  // metadata indicator that a variable is an uplevel.
  auto *StTy = dyn_cast<StructType>(Ty);
  if (!StTy || !StTy->hasName())
    return false;

  StringRef TypeName = StTy->getName();
  // Strip a '.' and any characters that follow it from the name.
  TypeName = TypeName.take_until([](char C) { return C == '.'; });
  if (TypeName != "uplevel_type")
    return false;

  return true;
}

// Helper function to check whether \p V is a GEP that corresponds to a field
// within an uplevel type.
bool isFieldInUplevelTypeVar(Value *V) {
  auto *GEP = dyn_cast<GetElementPtrInst>(V);
  if (!GEP)
    return false;
  return isUplevelVarType(
      GEP->getPointerOperand()->getType()->getPointerElementType());
}

// This class is used to collect information about a single field address that
// points to one of the dope vector fields. This is used during dope vector
// analysis to track loads and stores of the field for safety.
class DopeVectorFieldUse {
public:
  using LoadInstSet = SmallPtrSet<LoadInst *, 8>;
  using LoadInstSetIter = LoadInstSet::const_iterator;

  // Normally, we expect at most 1 store instruction
  using StoreInstSet = SmallPtrSet<StoreInst *, 1>;
  using StoreInstSetIter = StoreInstSet::const_iterator;

  DopeVectorFieldUse()
      : IsBottom(false), IsRead(false), IsWritten(false), FieldAddr(nullptr) {}

  DopeVectorFieldUse(const DopeVectorFieldUse &) = delete;
  DopeVectorFieldUse(DopeVectorFieldUse &&) = default;
  DopeVectorFieldUse &operator=(const DopeVectorFieldUse &) = delete;
  DopeVectorFieldUse &operator=(DopeVectorFieldUse &&) = delete;

  bool getIsBottom() const { return IsBottom; }
  bool getIsRead() const { return IsRead; }
  bool getIsWritten() const { return IsWritten; }
  bool getIsSingleValue() const { return !getIsBottom() && Stores.size() == 1; }
  Value *getSingleValue() const {
    if (!getIsSingleValue())
      return nullptr;
    return (*Stores.begin())->getValueOperand();
  }

  void setFieldAddr(Value *V) {
    // If we already saw an object that holds a pointer to the field address,
    // then we go to bottom since we only expect a single Value object to hold
    // the address for the entire function being analyzed.
    if (FieldAddr)
      IsBottom = true;

    FieldAddr = V;
  }

  // Check if the field address has been set.
  bool hasFieldAddr() const { return FieldAddr != nullptr; }

  // Get the set of load instructions.
  iterator_range<LoadInstSetIter> loads() const {
    return iterator_range<LoadInstSetIter>(Loads);
  }

  // Get the set of store instructions.
  iterator_range<StoreInstSetIter> stores() const {
    return iterator_range<StoreInstSetIter>(Stores);
  }

  // Collect the load and store instructions that use the field address. Set the
  // field to Bottom if there are any unsupported uses.
  void analyzeUses() {
    if (IsBottom)
      return;

    if (!FieldAddr)
      return;

    for (auto *U : FieldAddr->users()) {
      if (auto *SI = dyn_cast<StoreInst>(U)) {
        // Make sure the store is to the field address, and that it's not the
        // field address being stored somewhere.
        if (SI->getValueOperand() != FieldAddr) {
          Stores.insert(SI);
          IsWritten = true;
        } else {
          IsBottom = true;
          break;
        }
      } else if (auto *LI = dyn_cast<LoadInst>(U)) {
        Loads.insert(LI);
        IsRead = true;
      } else {
        IsBottom = true;
        break;
      }
    }
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }
  void print(raw_ostream &OS, const Twine &Header) const {
    OS << Header;
    print(OS);
  }

  void print(raw_ostream &OS) const {
    if (!FieldAddr) {
      OS << "  Not set\n";
      return;
    }
    OS << *FieldAddr << "\n";
    OS << "  Analysis :";
    OS << (IsBottom ? " BOTTOM" : "");
    OS << (IsRead ? " READ" : "");
    OS << (IsWritten ? " WRITTEN" : "");
    OS << "\n";

    OS << "  Stores   : " << Stores.size() << "\n";
    for (auto *V : Stores)
      OS << "    " << *V << "\n";

    OS << "  Loads    : " << Loads.size() << "\n";
    for (auto *V : Loads)
      OS << "    " << *V << "\n";
  }
#endif // !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)

private:
  bool IsBottom;
  bool IsRead;
  bool IsWritten;

  // Value object that contains the address for the field.
  Value *FieldAddr;

  // Set of locations the field is written to. Used to check what
  // value(s) is stored.
  StoreInstSet Stores;

  // Set of locations the field is loaded. This will be used for examining the
  // usage for profitability heuristics and safety checks.
  LoadInstSet Loads;
};

// The class is for analyzing the uses of all the fields that make up a dope
// vector.
//
// The layout of a dope vector consists of a fixed size block followed by a
// variable sized array: The fixed sized block is (24 or 48 bytes depending on
// the platform):
//   Type* pointer;   /* pointer to array */
//   long length;    /* size of one element of array */
//   long codim;     /* number of co-dimensions, if coarray */
//   long flag;      /* flags */
//   long dim;       /* number of dimensions in array */
//   long reserved; /* used by the backend's openmp support */
//
// The variable sized array (12 or 24 bytes per dimension depending on the
// platform) that is stored at the end is a structure for each dimension of the
// source array containing:
//   long extent;     /* highest index for dimension */
//   long stride;     /* inter element spacing, in bytes */
//   long lower_bound /* lowest index for dimension*/
//
// This class collects the loads/stores to the fields to enable analysis for
// what values are stored, or whether the DV object is read-only.
class DopeVectorAnalyzer {
public:
  // Enumeration fields related to dope vectors. The first 7 items in this
  // list correspond exactly to the field layout of the corresponding dope
  // vector fields, and correspond to GEP indices. Do not re-order these because
  // we directly map GEP index values to them.
  enum DopeVectorFieldType {
    DV_ArrayPtr = 0, // Pointer to array
    DV_ElementSize,  // size of one element of array
    DV_Codim,        // number of co-dimensions
    DV_Flags,        // flag bits
    DV_Dimensions,   // Number of dimensions
    DV_Reserved,
    DV_PerDimensionArray, // Array of structures {extent, stride, lower bound}
                          // for
                          //  each dimension

    // The following field types are indices used to represent the extent,
    // stride or lower bound components for the variable-sized block array
    DV_ExtentBase,
    DV_StrideBase,
    DV_LowerBoundBase,
    DV_Invalid // End of enumeration
  };

  // Each dimension in the dope vector is composed of a structure containing
  // the fields listed in this enumeration.
  enum DopeVectorRankFields { DVR_Extent, DVR_Stride, DVR_LowerBound };

  DopeVectorAnalyzer(Value *DVObject) : DVObject(DVObject) {
    assert(
        DVObject->getType()->isPointerTy() &&
        DVObject->getType()->getPointerElementType()->isStructTy() &&
        DVObject->getType()->getPointerElementType()->getStructNumElements() ==
            7 &&
        DVObject->getType()
            ->getPointerElementType()
            ->getContainedType(6)
            ->isArrayTy() &&
        "Invalid type for dope vector object");

    // The rank of the dope vector can be determined by the array length of
    // array that is the last field of the dope vector.
    Rank = DVObject->getType()
               ->getPointerElementType()
               ->getContainedType(6)
               ->getArrayNumElements();

    // Set as invalid, until analyzed.
    IsValid = false;
  }

  DopeVectorAnalyzer(const DopeVectorAnalyzer &) = delete;
  DopeVectorAnalyzer(DopeVectorAnalyzer &&) = default;
  DopeVectorAnalyzer operator=(const DopeVectorAnalyzer &) = delete;
  DopeVectorAnalyzer operator=(DopeVectorAnalyzer &&) = delete;

  // Check whether the dope vector was able to be analyzed.
  bool getIsValid() const { return IsValid; }

  // The analysis can only set the state invalid, so only include a method that
  // sets 'IsValid' to false.
  void setInvalid() {
    DEBUG_WITH_TYPE(DEBUG_DOPE_VECTORS,
                    dbgs() << "  DV-Invalid: " << *DVObject << "\n");
    IsValid = false;
  }

  // Provide accessors for fields that the client of dope vector analyzer needs
  // to examine the uses of.
  //
  // Currently, the only field that need to be directly accessible is the array
  // pointer field.
  const DopeVectorFieldUse &getPtrAddrField() const { return PtrAddr; }

  // Helper functions for retrieving value stored to configure the
  // dope vector per-dimension info, if there is a single store to the field.
  //
  Value *getLowerBound(uint32_t Dim) {
    assert(LowerBoundAddr.size() > Dim && "Invalid dimension");
    if (LowerBoundAddr[Dim].hasFieldAddr())
      return LowerBoundAddr[Dim].getSingleValue();
    return nullptr;
  }

  Value *getStride(uint32_t Dim) {
    assert(StrideAddr.size() > Dim && "Invalid dimension");
    if (StrideAddr[Dim].hasFieldAddr())
      return StrideAddr[Dim].getSingleValue();
    return nullptr;
  }

  // Check whether information is available about the stride for the specified
  // dimension.
  bool hasStrideField(uint32_t Dim) const {
    if (StrideAddr.size() <= Dim)
      return false;
    return StrideAddr[Dim].hasFieldAddr();
  }

  // Get the stride field information for the specified dimension.
  const DopeVectorFieldUse &getStrideField(uint32_t Dim) const {
    assert(hasStrideField(Dim) && "Invalid request");
    return StrideAddr[Dim];
  }

  Value *getExtent(uint32_t Dim) {
    assert(ExtentAddr.size() > Dim && "Invalid dimension");
    if (ExtentAddr[Dim].hasFieldAddr())
      return ExtentAddr[Dim].getSingleValue();
    return nullptr;
  }

  // Accessor for uplevel variable.
  UpevelDVField getUplevelVar() const { return Uplevel; }

  // Check if any field of the dope vector may be written.
  bool checkMayBeModified() const {
    if (!IsValid)
      return true;

    if (PtrAddr.getIsBottom() || ElementSizeAddr.getIsBottom() ||
        CodimAddr.getIsBottom() || FlagsAddr.getIsBottom() ||
        DimensionsAddr.getIsBottom())
      return true;

    if (PtrAddr.getIsWritten() || ElementSizeAddr.getIsWritten() ||
        CodimAddr.getIsWritten() || FlagsAddr.getIsWritten() ||
        DimensionsAddr.getIsWritten())
      return true;

    for (const auto &Field : LowerBoundAddr)
      if (Field.getIsBottom() || Field.getIsWritten())
        return true;

    for (const auto &Field : StrideAddr)
      if (Field.getIsBottom() || Field.getIsWritten())
        return true;

    for (const auto &Field : ExtentAddr)
      if (Field.getIsBottom() || Field.getIsWritten())
        return true;

    return false;
  }

  // Populate \p ValueSet with all the objects that hold the value for the
  // specific dope vector field in \p Field. This set contains all the LoadInst
  // instructions that were identified as loading the value of the field, and
  // all the PHI node and SelectInst instructions the value gets moved to.
  // Returns 'false' if a PHI/Select gets a value that did not originate from a
  // load of the field. Otherwise, returns 'true'.
  bool getAllValuesHoldingFieldValue(const DopeVectorFieldUse &Field,
                                     SmallPtrSetImpl<Value *> &ValueSet) const {
    // Prime a worklist with all the direct loads of the field.
    SmallVector<Value *, 16> Worklist;
    llvm::copy(Field.loads(), std::back_inserter(Worklist));

    // Populate the set of objects containing the value loaded.
    while (!Worklist.empty()) {
      Value *V = Worklist.back();
      Worklist.pop_back();
      if (!ValueSet.insert(V).second)
        continue;

      for (auto *U : V->users())
        if ((isa<SelectInst>(U) || isa<PHINode>(U)) && !ValueSet.count(U))
          Worklist.push_back(U);
    }

    // Verify all the source nodes for PHI nodes and select instructions
    // originate from the field load (or another PHI/select).
    SmallVector<Value *, 4> IncomingVals;
    for (auto *V : ValueSet) {
      IncomingVals.clear();
      if (auto *Sel = dyn_cast<SelectInst>(V)) {
        IncomingVals.push_back(Sel->getTrueValue());
        IncomingVals.push_back(Sel->getFalseValue());
      } else if (auto *PHI = dyn_cast<PHINode>(V)) {
        for (Value *Val : PHI->incoming_values())
          IncomingVals.push_back(Val);
      }

      for (auto *ValIn : IncomingVals)
        if (!ValueSet.count(ValIn)) {
          DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                          dbgs() << "Failed during check of:\n"
                                 << *V
                                 << "\nExpected PHI/select source to also be "
                                    "in field value set: "
                                 << *ValIn << "\n");
          return false;
        }
    }

    return true;
  }

  // Get the number of calls the dope vector is passed to
  uint64_t getNumberCalledFunctions() const { return FuncsWithDVParam.size(); }

  // Accessor for the set of calls taking dope vector as parameter.
  iterator_range<FuncArgPosPairSetIter> funcsWithDVParam() const {
    return iterator_range<FuncArgPosPairSetIter>(FuncsWithDVParam);
  }

  // Walk the uses of the dope vector object to collect information about all
  // the field accesses to check for safety.
  //
  // If \p ForCreation is set, it means the analysis is for the construction of
  // the dope vector, and requires addresses for all fields to be identified.
  // When it is not set, it is allowed to only identify a subset of the Value
  // objects holding field addresses.
  void analyze(bool ForCreation) {
    // Assume valid, until proven otherwise.
    IsValid = true;

    GetElementPtrInst *PerDimensionBase = nullptr;
    GetElementPtrInst *ExtentBase = nullptr;
    GetElementPtrInst *StrideBase = nullptr;
    GetElementPtrInst *LowerBoundBase = nullptr;

    for (auto *DVUser : DVObject->users()) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                      dbgs() << "Check dope vector user: " << *DVUser << "\n");
      if (auto *GEP = dyn_cast<GetElementPtrInst>(DVUser)) {
        // Find which of the fields this GEP is the address of.
        // Note: We expect the field addresses to only be seen at most one
        // time for each field, otherwise we do not support it.
        DopeVectorFieldType DVFieldType = identifyDopeVectorField(*GEP);
        switch (DVFieldType) {
        default:
          setInvalid();
          return;

        case DV_ArrayPtr:
          PtrAddr.setFieldAddr(GEP);
          break;
        case DV_ElementSize:
          ElementSizeAddr.setFieldAddr(GEP);
          break;
        case DV_Codim:
          CodimAddr.setFieldAddr(GEP);
          break;
        case DV_Flags:
          FlagsAddr.setFieldAddr(GEP);
          break;
        case DV_Dimensions:
          DimensionsAddr.setFieldAddr(GEP);
          break;
        case DV_Reserved:
          // Ignore uses of reserved
          break;

          // The following fields require additional forward looking analysis to
          // get to the actual address-of objects.
        case DV_PerDimensionArray:
          if (PerDimensionBase) {
            setInvalid();
            return;
          }
          PerDimensionBase = GEP;
          break;
        case DV_LowerBoundBase:
          if (LowerBoundBase) {
            setInvalid();
            return;
          }
          LowerBoundBase = GEP;
          break;
        case DV_ExtentBase:
          if (ExtentBase) {
            setInvalid();
            return;
          }
          ExtentBase = GEP;
          break;

        case DV_StrideBase:
          if (StrideBase) {
            setInvalid();
            return;
          }
          StrideBase = GEP;
          break;
        }
      } else if (const auto *CI = dyn_cast<CallInst>(DVUser)) {
        Function *F = CI->getCalledFunction();
        if (!F) {
          DEBUG_WITH_TYPE(
              DEBUG_ANALYSIS,
              dbgs() << "Dope vector passed in indirect function call:\n"
                     << *CI << "\n");
          setInvalid();
          return;
        }

        Optional<unsigned int> ArgPos = getArgumentPosition(*CI, DVObject);
        if (!ArgPos) {
          DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                          dbgs() << "Dope vector argument not unique in call:\n"
                                 << *CI << "\n");
          setInvalid();
          return;
        }

        // Save the function for later analysis.
        FuncsWithDVParam.insert({F, *ArgPos});
      } else if (auto *SI = dyn_cast<StoreInst>(DVUser)) {
        // Check if the store is saving the dope vector object into an uplevel
        // var. Save the variable and field number for later analysis. (The
        // dope vector should only ever need to be stored to a single uplevel,
        // but make sure we didn't see one yet.)
        if (SI->getValueOperand() == DVObject) {
          Value *PtrOp = SI->getPointerOperand();
          if (isFieldInUplevelTypeVar(PtrOp) && Uplevel.first == nullptr) {
            DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                            dbgs() << "Dope vector needs uplevel analysis: "
                                   << *SI << "\n");
            auto PtrGEP = cast<GetElementPtrInst>(PtrOp);
            auto Idx0 = getConstGEPIndex(*PtrGEP, 1);
            auto Idx1 = getConstGEPIndex(*PtrGEP, 2);
            if (Idx0 && Idx1 && *Idx0 == 0) {
              Uplevel = UpevelDVField(PtrGEP->getPointerOperand(), *Idx1);
              continue;
            }
          }
        }

        DEBUG_WITH_TYPE(
            DEBUG_ANALYSIS,
            dbgs() << "Unsupported StoreInst using dope vector object\n"
                   << *DVUser << "\n");
        setInvalid();
        return;
      } else {
        DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                        dbgs() << "Unsupported use of dope vector object\n"
                               << *DVUser << "\n");
        setInvalid();
        return;
      }
    }

    // We expect either the per-dimension base or base addresses of the
    // individual components, not both.
    if (PerDimensionBase) {
      if (ExtentBase || StrideBase || LowerBoundBase) {
        setInvalid();
        return;
      }

      std::pair<GetElementPtrInst *, FindResult> Result;
      Result = findPerDimensionArrayFieldGEP(*PerDimensionBase, DVR_Extent);
      if (Result.second == FR_Valid)
        ExtentBase = Result.first;
      Result = findPerDimensionArrayFieldGEP(*PerDimensionBase, DVR_Stride);
      if (Result.second == FR_Valid)
        StrideBase = Result.first;
      Result = findPerDimensionArrayFieldGEP(*PerDimensionBase, DVR_LowerBound);
      if (Result.second == FR_Valid)
        LowerBoundBase = Result.first;
    }

    // Check the uses of the fields to make sure there are no unsupported uses,
    // and collect the loads and stores. For the PtrAddr field, we will need to
    // later analyze all the reads that get the address of the array to ensure
    // the address does not escape the module. For the dope vector strides, we
    // will need to analyze all the writes to the field to be sure the expected
    // value is being stored. For other fields, we may not need to collect all
    // the loads and stores, but for now, collect them all.
    PtrAddr.analyzeUses();
    ElementSizeAddr.analyzeUses();
    CodimAddr.analyzeUses();
    FlagsAddr.analyzeUses();
    DimensionsAddr.analyzeUses();

    // During dope vector creation, we expect to be see all the fields being set
    // up.
    if (ForCreation) {
      if (!PtrAddr.hasFieldAddr() || !ElementSizeAddr.hasFieldAddr() ||
          !CodimAddr.hasFieldAddr() || !FlagsAddr.hasFieldAddr() ||
          !DimensionsAddr.hasFieldAddr()) {
        DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                        dbgs() << "Unsupported use of dope vector object: "
                                  "Could not find addresses for all fields.\n");
        setInvalid();
        return;
      }
    }

    // Verify all the uses of the fields present were successfully analyzed.
    if (PtrAddr.getIsBottom() || ElementSizeAddr.getIsBottom() ||
        CodimAddr.getIsBottom() || FlagsAddr.getIsBottom() ||
        DimensionsAddr.getIsBottom()) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                      dbgs() << "Unsupported use of dope vector object: Could "
                                "not analyze all fields.\n");

      setInvalid();
      return;
    }

    // If a field was found that corresponds to the Extent, Stride or
    // LowerBounds fields, reserve space for all of them, then collect all the
    // loads/stores that use those fields.
    if (ExtentBase || StrideBase || LowerBoundBase) {
      ExtentAddr.resize(Rank);
      StrideAddr.resize(Rank);
      LowerBoundAddr.resize(Rank);
      for (unsigned Dim = 0; Dim < Rank; ++Dim) {
        if (ExtentBase) {
          Value *Ptr = findPerDimensionArrayFieldPtr(*ExtentBase, Dim);
          if (Ptr) {
            DopeVectorFieldUse &ExtentField = ExtentAddr[Dim];
            ExtentField.setFieldAddr(Ptr);
            ExtentField.analyzeUses();
            if (ExtentField.getIsBottom()) {
              setInvalid();
              return;
            }
          }
        }
        if (StrideBase) {
          Value *Ptr = findPerDimensionArrayFieldPtr(*StrideBase, Dim);
          if (Ptr) {
            DopeVectorFieldUse &StrideField = StrideAddr[Dim];
            StrideField.setFieldAddr(Ptr);
            StrideField.analyzeUses();
            if (StrideField.getIsBottom()) {
              setInvalid();
              return;
            }
          }
        }
        if (LowerBoundBase) {
          Value *Ptr = findPerDimensionArrayFieldPtr(*LowerBoundBase, Dim);
          if (Ptr) {
            DopeVectorFieldUse &LBField = LowerBoundAddr[Dim];
            LBField.setFieldAddr(Ptr);
            LBField.analyzeUses();
            if (LBField.getIsBottom()) {
              setInvalid();
              return;
            }
          }
        }

        // For dope vector creation, we expect to find writes for all the fields
        if (ForCreation) {
          if (!ExtentAddr[Dim].hasFieldAddr() ||
              !StrideAddr[Dim].hasFieldAddr() ||
              !LowerBoundAddr[Dim].hasFieldAddr()) {
            DEBUG_WITH_TYPE(
                DEBUG_ANALYSIS,
                dbgs() << "Unsupported use of dope vector object: Could "
                          "not find addresses for all ranks.\n");
            setInvalid();
            return;
          }

          if (!ExtentAddr[Dim].getIsWritten() ||
              !StrideAddr[Dim].getIsWritten() ||
              !LowerBoundAddr[Dim].getIsWritten()) {
            DEBUG_WITH_TYPE(
                DEBUG_ANALYSIS,
                dbgs() << "Unsupported use of dope vector object: Could "
                          "not find writes for all ranks.\n");
            setInvalid();
            return;
          }
        }
      }
    }
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() const { print(dbgs()); }

  void print(raw_ostream &OS) const {
    OS << "DopeVectorAnalyzer: " << *DVObject << "\n";
    OS << "IsValid: " << (IsValid ? "true" : "false") << "\n";

    PtrAddr.print(OS, "PtrAddr:");
    ElementSizeAddr.print(OS, "ElementSize:");
    CodimAddr.print(OS, "Codim:");
    FlagsAddr.print(OS, "Flags:");
    DimensionsAddr.print(OS, "Dimensions:");
    for (unsigned Dim = 0; Dim < LowerBoundAddr.size(); ++Dim) {
      std::string DimStr = std::to_string(Dim);
      LowerBoundAddr[Dim].print(OS, "LowerBound" + DimStr);
    }
    for (unsigned Dim = 0; Dim < StrideAddr.size(); ++Dim) {
      std::string DimStr = std::to_string(Dim);
      StrideAddr[Dim].print(OS, "Stride" + DimStr);
    }

    for (unsigned Dim = 0; Dim < ExtentAddr.size(); ++Dim) {
      std::string DimStr = std::to_string(Dim);
      ExtentAddr[Dim].print(OS, "Extent" + DimStr);
    }
    OS << "\n";
  }
#endif // !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)

  // Identify the field a getelementptr instruction corresponds to in the dope
  // vector object. Return DV_Invalid if it is not a valid dope vector field.
  static DopeVectorFieldType
  identifyDopeVectorField(const GetElementPtrInst &GEP) {
    assert(GEP.getSourceElementType()->isStructTy() && "Expected struct type");

    // Array index should always be zero.
    auto ArrayIdx = getConstGEPIndex(GEP, 1);
    if (!ArrayIdx || *ArrayIdx != 0)
      return DV_Invalid;

    unsigned NumIndices = GEP.getNumIndices();
    if (NumIndices < 2 || NumIndices > 4)
      return DV_Invalid;

    // The address for the first 6 fields of the dope vector are accessed
    // directly with a GEP of the form:
    //     %field4 = getelementptr
    //               { i32*, i64, i64, i64, i64, i64, [2 x { i64, i64, i64 }] },
    //               { i32*, i64, i64, i64, i64, i64, [2 x { i64, i64, i64 }] }*
    //               %"var$08", i64 0, i32 4
    if (NumIndices == 2) {
      auto FieldIdx = getConstGEPIndex(GEP, 2);
      assert(FieldIdx &&
             "Field index should always be constant for struct type");
      assert(FieldIdx <= static_cast<uint64_t>(DV_PerDimensionArray) &&
             "expected dope vector to have a maximum of 7 fields");
      return static_cast<DopeVectorFieldType>(*FieldIdx);
    }

    // The per-dimension array elements may be accessed using either of the
    // following forms:
    //   %16 = getelementptr
    //         { i32*, i64, i64, i64, i64, i64, [2 x { i64, i64, i64 }] },
    //         { i32*, i64, i64, i64, i64, i64, [2 x { i64, i64, i64 }] }* %2,
    //         i64 0, i32 6, i64 0
    //
    // or:
    //
    //   %14 = getelementptr { i32*, i64, i64, i64, i64, i64, [3 x { i64, i64,
    //   i64 }] },
    //         { i32*, i64, i64, i64, i64, i64, [3 x { i64, i64, i64 }] }* %3,
    //         i64 0, i32 6, i64 0, i32 1
    //
    // For the first form, another GEP will follow to get the index from the
    // per-array dimension. For the second form, the field may be passed
    // directly to a subscript intrinsic.
    if (NumIndices == 3) {
      auto FieldIdx = getConstGEPIndex(GEP, 2);
      if (FieldIdx != static_cast<uint64_t>(DV_PerDimensionArray))
        return DV_Invalid;

      // We only expect the GEP to use 0 for last index which corresponds to the
      // per-dimension array base, and then be followed by another GEP to get
      // the specific structure element.
      auto SubIdx = getConstGEPIndex(GEP, 3);
      assert(SubIdx && "Field index should always be constant for struct type");
      if (*SubIdx != 0)
        return DV_Invalid;
      return DV_PerDimensionArray;
    }

    assert(NumIndices == 4 && "Only expected case 4 to be left");

    // The second form of access directly gets the address of the Lower Bound,
    // Stride or Extent field of the first array element.
    auto SubIdx = getConstGEPIndex(GEP, 4);
    assert(SubIdx && "Field index should always be constant for struct type");
    switch (*SubIdx) {
    default:
      return DV_Invalid;
    case 0:
      return DV_ExtentBase;
    case 1:
      return DV_StrideBase;
    case 2:
      return DV_LowerBoundBase;
    }
    return DV_Invalid;
  }

  // For the per-dimension array, we expect to find a sequence of the following
  // form that gets the address of the per-dimensional fields: (This is the GEP
  // passed into this routine):
  //
  // %GEP = getelementptr
  //         {i32*, i64, i64, i64, i64, i64, [2 x { i64, i64, i64 }]},
  //         {i32*, i64, i64, i64, i64, i64, [2 x { i64, i64, i64 }]}* %2,
  //         i64 0, i32 6, i64 0
  //
  // This routine then traces the use of the GEP to the following pattern to get
  // the address of the a dope vector field {Extent, Stride, Lower Bound} of
  // the first element of the variable sized array.
  //
  // The structure is laid out as: {Extent, Stride, Lower Bound}
  //   %EXTENT = getelementptr {i64, i64, i64}, {i64, i64, i64}* %GEP,
  //               i64 0, i32 0
  //   %STRIDE = getelementptr {i64, i64, i64}, {i64, i64, i64}* %GEP,
  //               i64 0, i32 1
  //   %LB = getelementptr {i64, i64, i64}, {i64, i64, i64}* %GEP,
  //           i64 0, i32 2
  //
  enum FindResult { FR_Invalid, FR_Valid };
  std::pair<GetElementPtrInst *, FindResult>
  findPerDimensionArrayFieldGEP(GetElementPtrInst &GEP,
                                DopeVectorRankFields RankFieldType) {
    std::pair<GetElementPtrInst *, FindResult> InvalidResult = {nullptr,
                                                                FR_Invalid};
    unsigned int FieldNum;
    switch (RankFieldType) {
    case DVR_Extent:
      FieldNum = 0;
      break;
    case DVR_Stride:
      FieldNum = 1;
      break;
    case DVR_LowerBound:
      FieldNum = 2;
      break;
    }

    // Find the GEP that corresponds to the per-dimension element wanted. There
    // should only be one, if there are more, we do not support it.
    GetElementPtrInst *FieldGEP = nullptr;
    for (auto *U : GEP.users()) {
      if (auto *GEPU = dyn_cast<GetElementPtrInst>(U)) {
        if (GEPU->getNumIndices() != 2)
          return InvalidResult;

        auto ArIdx = getConstGEPIndex(*GEPU, 1);
        if (!ArIdx || *ArIdx != 0)
          return InvalidResult;

        // Check that there is only one instance of field being searched for.
        auto FieldIdx = getConstGEPIndex(*GEPU, 2);
        assert(FieldIdx && "Field index of struct must be constant");
        if (*FieldIdx == FieldNum) {
          if (FieldGEP)
            return InvalidResult;
          FieldGEP = GEPU;
        }
      } else {
        return InvalidResult;
      }
    }

    // No instances using field. Return a constructed value that holds a
    // nullptr, as a valid analysis result.
    if (!FieldGEP)
      return {nullptr, FR_Valid};

    return {FieldGEP, FR_Valid};
  }

  //
  // Find the object that holds the address for the element of the variable
  // sized array of the dimension desired.
  //
  // The input to this function is the address of the field in the first array
  // element, as computed by findPerDimensionArrayFieldGEP(). This is then used
  // in an IR sequence as follows: (Note, These are being done via the subscript
  // intrinsic rather than GEPs and get lowered later.)
  //
  // For example, on a 2 dimensional array we would have:
  // Getting the lower bound address for each dimension
  // %134 = call i64* @llvm.intel.subscript.p0i64.i64.i32.p0i64.i32(
  //                     i8 0, i64 0, i32 24, i64* %LB, i32 1)
  // %131 = call i64* @llvm.intel.subscript.p0i64.i64.i32.p0i64.i32(
  //                     i8 0, i64 0, i32 24, i64* %LB, i32 0)
  //
  // Getting the extent address
  // %135 = call i64* @llvm.intel.subscript.p0i64.i64.i32.p0i64.i32(
  //                     (i8 0, i64 0, i32 24, i64* %EXTENT, i32 1)
  // %132 = call i64* @llvm.intel.subscript.p0i64.i64.i32.p0i64.i32(
  //                     i8 0, i64 0, i32 24, i64* %EXTENT, i32 0)
  //
  // Getting the stride address
  // %133 = call i64* @llvm.intel.subscript.p0i64.i64.i32.p0i64.i32(
  //                     i8 0, i64 0, i32 24, i64* %STRIDE, i32 1)
  // %130 = call i64* @llvm.intel.subscript.p0i64.i64.i32.p0i64.i32(
  //                     i8 0, i64 0, i32 24, i64* %STRIDE, i32 0)

  Value *findPerDimensionArrayFieldPtr(GetElementPtrInst &FieldGEP,
                                       unsigned Dimension) {
    const unsigned IndexParamPos = 4;

    // Find the address element
    Instruction *Addr = nullptr;
    for (auto *U : FieldGEP.users()) {
      if (auto *CI = dyn_cast<CallInst>(U)) {
        if (!isSubscriptIntrinsicCall(*CI))
          return nullptr;
        auto *IdxVal = dyn_cast<ConstantInt>(CI->getArgOperand(IndexParamPos));
        if (!IdxVal)
          return nullptr;
        if (IdxVal->getLimitedValue() == Dimension) {
          if (Addr)
            return nullptr;
          Addr = CI;
        }
      } else {
        return nullptr;
      }
    }

    return Addr;
  }

private:
  // Value object that represents a dope vector.
  Value *DVObject;

  // Rank for the source array.
  unsigned long Rank;

  // Indicates whether all the uses were successfully analyzed.
  bool IsValid;

  // Information about all field accesses for the dope vector.
  DopeVectorFieldUse PtrAddr;
  DopeVectorFieldUse ElementSizeAddr;
  DopeVectorFieldUse CodimAddr;
  DopeVectorFieldUse FlagsAddr;
  DopeVectorFieldUse DimensionsAddr;
  SmallVector<DopeVectorFieldUse, 4> ExtentAddr;
  SmallVector<DopeVectorFieldUse, 4> StrideAddr;
  SmallVector<DopeVectorFieldUse, 4> LowerBoundAddr;

  // Set of functions that take a dope vector parameter that need to be
  // checked to ensure there is no modification to the dope vector within
  // the function. Pair is: { Function, Argument position }
  FuncArgPosPairSet FuncsWithDVParam;

  // Uplevel variable corresponding to this dope vector. We only expect a single
  // uplevel variable to be created for the dope vector being analyzed, because
  // even if there are multiple routines contained within the routine that
  // created the dope vector, the same uplevel variable is passed to all of
  // them.
  UpevelDVField Uplevel;
};

// This is the class that manages the analysis and transformation
// of the stride information for a candidate variable.
class TransposeCandidate {
public:
  TransposeCandidate(GlobalVariable *GV, uint32_t ArrayRank,
                     uint64_t ArrayLength, uint64_t ElementSize,
                     llvm::Type *ElementType)
      : GV(GV), ArrayRank(ArrayRank), ArrayLength(ArrayLength),
        ElementSize(ElementSize), ElementType(ElementType), IsValid(false) {
    assert(ArrayRank > 0 && ArrayRank <= FortranMaxRank && "Invalid Rank");
    uint64_t Stride = ElementSize;
    for (uint32_t RankNum = 0; RankNum < ArrayRank; ++RankNum) {
      Strides.push_back(Stride);
      Stride *= ArrayLength;
    }
  }

  ~TransposeCandidate() { cleanup(); }

  // Clean up memory allocated during analysis of the candidate.
  void cleanup() {
    for (auto *DVA : DopeVectorInstances)
      delete DVA;

    DopeVectorInstances.clear();
    SubscriptCalls.clear();
    DVSubscriptCalls.clear();
  }

  // This function analyzes a candidate to check whether all uses of the
  // variable are supported for the transformation.
  //
  // The only valid uses for the global variable itself are:
  // - Base pointer argument in outermost call of a llvm.intel.subscript
  //   intrinsic call chain.
  // - Storing the array's address into a dope vector that represents the
  //   entire array object using the default values for the lower bound/
  //   extent/stride.
  // - The dope vector object may be passed to a function that takes an
  //   assumed shape array. The called function will be checked that there
  //   are only reads of the dope vector structure elements, or the transfer
  //   of the dope vector pointer to an uplevel variable.
  // - The uplevel variable can be passed to a function, and again all uses
  //   of the dope vector fields will be checked to verify that only reads
  //   are done on the dope vector elements.
  //
  bool analyze(const DataLayout &DL) {
    DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                    dbgs() << "Analyzing variable: " << *GV << "\n");

    // Check all the direct uses of the global. This loop will also collect
    // the functions that take a dope vector which need to be checked.
    IsValid = true;
    for (auto *U : GV->users()) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                      dbgs() << "Checking global var use: " << *U << "\n");

      // Uses of the global should be in the form of a GEP operator which should
      // only be getting the base address of the array. For example:
      //   i32* getelementptr ([9 x [9 x i32]],
      //                       [9 x [9 x i32]]* @var1, i64 0, i64 0, i64 0)

      auto *GepOp = dyn_cast<GEPOperator>(U);
      if (!GepOp) {
        DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                        dbgs() << "  Invalid: Unsupported instruction: " << *U
                               << "\n");
        IsValid = false;
        break;
      }

      if (!GepOp->hasAllZeroIndices()) {
        DEBUG_WITH_TYPE(
            DEBUG_ANALYSIS,
            dbgs() << "  Invalid: Global variable GEP not getting base "
                      "pointer address\n");
        IsValid = false;
        break;
      }

      // Now check the users of the pointer address for safety
      for (auto *GepOpUser : GepOp->users()) {
        DEBUG_WITH_TYPE(DEBUG_ANALYSIS, {
          dbgs() << "  Checking global var address use: " << *GepOpUser << "\n";
          if (auto *I = dyn_cast<Instruction>(GepOpUser))
            dbgs() << "  in function: "
                   << I->getParent()->getParent()->getName() << "\n";
        });

        if (auto *CI = dyn_cast<CallInst>(GepOpUser)) {
          // Check that the call is to llvm.intel.subscript.
          //
          // This could be extended in the future to allow the address to be
          // passed without a dope vector, but that is not needed for the case
          // of interest, at the moment.
          if (!isSubscriptIntrinsicCall(*CI)) {
            DEBUG_WITH_TYPE(
                DEBUG_ANALYSIS,
                dbgs() << "  Invalid: Call with pointer address may only be "
                          "subscript intrinsic call\n");

            IsValid = false;
            break;
          }

          // The global variable should only be accessed with a subscript call
          // that uses the rank of the variable, and the array should only be
          // using default values for the lower bound and stride, rather than a
          // user defined value for the lower bound. It should not be required
          // for the transform, but it avoids cases such as:
          //     integer :: my_array(2:10, 9, 11:19)
          if (!isValidUseOfSubscriptForGlobal(*CI, *GepOp)) {
            DEBUG_WITH_TYPE(
                DEBUG_ANALYSIS,
                dbgs() << "  Invalid: Subscript call values not supported\n");

            IsValid = false;
            break;
          }

          // Save the subscript call because we will need this for computing
          // profitability and transforming the arguments later.
          SubscriptCalls.insert(CI);
        } else if (auto *SI = dyn_cast<StoreInst>(GepOpUser)) {
          // The only case the address of the variable may be saved is into a
          // dope vector, check that case here.
          if (!isValidStoreForGlobal(*SI, GepOp, DL)) {
            DEBUG_WITH_TYPE(
                DEBUG_ANALYSIS,
                dbgs()
                    << "  Invalid: Store of pointer address not supported\n");

            IsValid = false;
            break;
          }
        } else {
          // Other uses are not allowed.
          DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                          dbgs() << "Unsupported use of global: " << *GepOpUser
                                 << "\n");
          IsValid = false;
          break;
        }
      }
    }

    if (IsValid) {
      // Analyze all the functions that the dope vector was passed to. Collate
      // them to a single set in case the function was called multiple times.
      FuncArgPosPairSet FuncsWithDopeVector;
      for (DopeVectorAnalyzer *DVA : DopeVectorInstances) {
        auto Range = DVA->funcsWithDVParam();
        FuncsWithDopeVector.insert(Range.begin(), Range.end());
      }

      for (auto &FuncPos : FuncsWithDopeVector)
        if (!analyzeDopeVectorCallArgument(*FuncPos.first, FuncPos.second)) {
          IsValid = false;
          break;
        }
    }

    LLVM_DEBUG(dbgs() << "Candidate " << (IsValid ? "PASSED" : "FAILED")
                      << " safety tests: " << GV->getName() << "\n");

    if (!IsValid)
      cleanup();

    return IsValid;
  }

  // Check that \p CI is a supported subscript call on the global array base
  // address \p BasePtr. For a global variable, we expect the subscript call to
  // contain the constant values for the lower bound and stride that represent
  // the full array, and a lower bound index of 1.
  bool isValidUseOfSubscriptForGlobal(const CallInst &CI,
                                      const Value &BasePtr) {

    // Helper that checks constants for one subscript call, and recurse if
    // there are more ranks to check.
    std::function<bool(const CallInst &, const Value &, uint32_t)>

        IsValidUseForRank = [this, &IsValidUseForRank](const CallInst &Call,
                                                       const Value &Ptr,
                                                       uint32_t Rank) -> bool {
      if (!isValidUseOfSubscriptCall(Call, Ptr, ArrayRank, Rank, 1,
                                     Strides[Rank]))
        return false;

      // Verify the subscript result is only fed to another subscript call. In
      // the future this could be extended to support PHI nodes/select
      // instructions, but for now that is not needed.
      if (Rank > 0) {
        for (const auto *U : Call.users()) {
          const auto *CI = dyn_cast<CallInst>(U);
          if (!CI)
            return false;

          if (!IsValidUseForRank(*CI, Call, Rank - 1))
            return false;
        }
      }
      return true;
    };

    // Check the use of this subscript call, and all the subscript calls the
    // result is fed to. Note, subscript call rank parameter value starts at 0,
    // not 1.
    return IsValidUseForRank(CI, BasePtr, ArrayRank - 1);
  }

  // The only supported use of storing the address of the array's base pointer
  // into another memory location is when the address is being stored into a
  // dope vector, and the dope vector is describing the entire array (Lower
  // Bound = 1, Extent = array length, and Stride is each element for each array
  // dimension).
  bool isValidStoreForGlobal(StoreInst &SI, const Value *BasePtr,
                             const DataLayout &DL) {
    if (SI.getValueOperand() != BasePtr)
      return false;

    Value *DVObject = isPotentialDVStore(SI, DL);
    if (!DVObject)
      return false;

    // Collect the use of the dope vector pointer.
    std::unique_ptr<DopeVectorAnalyzer> DVA(new DopeVectorAnalyzer(DVObject));
    DVA->analyze(/*ForCreation = */ true);
    DEBUG_WITH_TYPE(DEBUG_DOPE_VECTORS, {
      dbgs() << "Analysis of potential dope vector:\n";
      DVA->dump();
      dbgs() << "\n";
    });

    if (!DVA->getIsValid()) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                      dbgs() << "Invalid: Unsupported dope vector\n");
      return false;
    }

    // Check that the only write of the pointer field is to store the address
    // we expect for the array object.
    if (DVA->getPtrAddrField().getSingleValue() != BasePtr)
      return false;

    // Check that the dope vector is set up using the lower bound, stride and
    // extent that represents the complete object, and not a sub-object.
    auto MatchesConstant = [](Value *V, unsigned long Expect) {
      if (auto *C = dyn_cast<ConstantInt>(V))
        return C->getLimitedValue() == Expect;
      return false;
    };

    for (uint32_t Dim = 0; Dim < ArrayRank; ++Dim) {
      Value *LB = DVA->getLowerBound(Dim);
      Value *Extent = DVA->getExtent(Dim);
      Value *Stride = DVA->getStride(Dim);
      if (!LB || !Extent || !Stride) {
        DEBUG_WITH_TYPE(
            DEBUG_ANALYSIS,
            dbgs() << "Invalid: Unable to analyze dope vector fields\n");
        return false;
      }

      if (!MatchesConstant(LB, 1) || !MatchesConstant(Extent, ArrayLength) ||
          !MatchesConstant(Stride, Strides[Dim])) {
        DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                        dbgs() << "Invalid: DV does not capture entire "
                                  "array with unit strides\n");
        return false;
      }
    }

    // Save the dope vector info for analysis of the called functions, and
    // updates to the setup.
    DopeVectorInstances.insert(DVA.release());
    return true;
  }

  // Check whether the store of the variable is potentially to a dope vector
  // structure. Currently, the front-end does not add metadata tags to indicate
  // dope vectors, so we will pattern match this. (The later analysis on the
  // usage and limitations of usage will filter out any false positive matches.)
  //
  // For a store of the form:
  //   store i32* getelementptr inbounds(
  //        [9 x[9 x[9 x i32]]], [9 x[9 x[9 x i32]]] * @block,
  //           i64 0, i64 0, i64 0, i64 0),
  //        i32** %ptr, align 8
  //
  // Look for the pointer operand of the form:
  //     %ptr = getelementptr inbounds
  //          { i32*, i64, i64, i64, i64, i64, [N x { i64, i64, i64 }] },
  //          { i32*, i64, i64, i64, i64, i64, [N x { i64, i64, i64 }] }*
  //            %object, i64 0, i32 0
  //
  //  where the type for %ptr matches a dope vector type, %object is a locally
  //  allocated object.
  //
  Value *isPotentialDVStore(StoreInst &SI, const DataLayout &DL) {
    // Check that the store is a field that matches a dope vector type
    Value *Ptr = SI.getPointerOperand();
    auto *FieldGEP = dyn_cast<GetElementPtrInst>(Ptr);
    if (!FieldGEP)
      return nullptr;

    llvm::Type *GEPType = FieldGEP->getSourceElementType();
    if (!isDopeVectorType(GEPType, DL))
      return nullptr;

    // Check that the field address is where we expect the array address to be
    // stored within the dope vector.
    if (DopeVectorAnalyzer::identifyDopeVectorField(*FieldGEP) !=
        DopeVectorAnalyzer::DV_ArrayPtr)
      return nullptr;

    Value *DVObject = FieldGEP->getPointerOperand();
    if (!isa<AllocaInst>(DVObject))
      return nullptr;

    return DVObject;
  }

  // Check if the type matches the signature for a dope vector.
  // Dope vector types look like:
  // { i32*, i64, i64, i64, i64, i64, [3 x { i64, i64, i64 }] }
  // where:
  //  - the pointer field will be a pointer to the type of the data stored in
  //    the source array.
  //  - the array dimension varies based on the Rank of the source array.
  //  - the integer types in the structure are i64 when compiling with targets
  //    that use 64-bit pointers, and i32 for targets using 32-bit pointers.
  //
  // In the future the FE will provide some metadata to avoid the need to
  // pattern match this.
  bool isDopeVectorType(const llvm::Type *Ty, const DataLayout &DL) {
    const unsigned int DVFieldCount = 7;
    const unsigned int PerDimensionCount = 3;

    // Helper to check that all types contained in the structure in the range
    // of (Begin, End) are of type \p TargType
    auto ContainedTypesMatch = [](const StructType *StTy,
                                  const llvm::Type *TargType,
                                  unsigned int Begin, unsigned int End) {
      for (unsigned Idx = Begin; Idx < End; ++Idx)
        if (StTy->getContainedType(Idx) != TargType)
          return false;

      return true;
    };

    auto *StTy = dyn_cast<StructType>(Ty);
    if (!StTy)
      return false;

    unsigned ContainedCount = StTy->getNumContainedTypes();
    if (ContainedCount != DVFieldCount)
      return false;

    llvm::Type *FirstType = StTy->getContainedType(0);
    if (ElementType->getPointerTo() != FirstType)
      return false;

    // All fields are "long" type?
    llvm::Type *LongType =
        Type::getIntNTy(Ty->getContext(), DL.getPointerSizeInBits());
    if (!ContainedTypesMatch(StTy, LongType, 1U, ContainedCount - 1))
      return false;

    // Array of structures for each rank?
    llvm::Type *LastType = StTy->getContainedType(ContainedCount - 1);
    auto *ArType = dyn_cast<ArrayType>(LastType);
    if (!ArType)
      return false;
    if (ArType->getArrayNumElements() != ArrayRank)
      return false;

    // Structure for extent, stride, and lower bound?
    llvm::Type *ElemTy = ArType->getArrayElementType();
    auto *StElemTy = dyn_cast<StructType>(ElemTy);
    if (!StElemTy)
      return false;
    if (StElemTy->getNumContainedTypes() != PerDimensionCount)
      return false;
    if (!ContainedTypesMatch(StElemTy, LongType, 0U, PerDimensionCount))
      return false;

    return true;
  }

  // A dope vector passed to a function is allowed to have the following uses:
  // - Load the fields of the dope vector object. (No field writes allowed).
  // - The loaded fields are also checked to be sure the array does not escape
  //   and the stride value used for the accesses comes from the dope vector.
  // - Store the address of the dope vector into an uplevel variable, and
  //   pass the uplevel variable to another function.
  bool analyzeDopeVectorCallArgument(Function &F, unsigned int ArgPos) {
    DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                    dbgs() << "  Checking use of dope vector in function: "
                           << F.getName() << " Arg: " << ArgPos << "\n");
    if (F.isDeclaration()) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                      dbgs() << "IR not available for function: " << F.getName()
                             << "\n");
      return false;
    }

    assert(ArgPos < F.arg_size() && "Invalid argument position");
    auto Args = F.arg_begin();
    std::advance(Args, ArgPos);
    Argument *FormalArg = &(*Args);

    // Collect all the uses of the dope vector in the function.
    DopeVectorAnalyzer DVA(FormalArg);
    return analyzeDVUseInFunction(F, FormalArg);
  }

  // This checks the use of a dope vector in a function to verify the fields are
  // not modified and the address of the array does not escape. The dope vector
  // object can either be one that was passed directly into Function \p F or it
  // can be a GEP field from an uplevel variable. Returns 'true' if uses are
  // safe.
  bool analyzeDVUseInFunction(const Function &F, Value *DVObject) {
    DopeVectorAnalyzer DVA(DVObject);
    DVA.analyze(/*ForCreation = */ false);
    DEBUG_WITH_TYPE(DEBUG_DOPE_VECTORS, {
      dbgs() << "\nDope vector collection for function: " << F.getName() << "\n"
             << *DVObject << "\n";
      DVA.dump();
    });

    // Verify that the dope vector fields are not written.
    if (DVA.checkMayBeModified()) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                      dbgs() << "Dope vector fields modified in function: "
                             << F.getName() << "\n");
      return false;
    }

    // Check that the DV object was not forwarded to another function call. We
    // could allow this by analyzing all the uses within that function,
    // but we currently do not.
    if (DVA.getNumberCalledFunctions()) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                      dbgs()
                          << "Dope vector passed to another function within: "
                          << F.getName() << "\n");
      return false;
    }

    // Check that the array pointer does not escape to another memory location.
    // This call will also collect the set of subscript calls that use the array
    // pointer from the dope vector.
    CallInstSet SubscriptCalls;
    if (!checkArrayPointerUses(DVA, SubscriptCalls)) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                      dbgs() << "Array pointer address may escape from: "
                             << F.getName() << "\n");
      return false;
    }

    if (!SubscriptCalls.empty()) {
      // Check the stride value used in the subscript calls.
      if (!checkSubscriptStrideValues(DVA, SubscriptCalls)) {
        DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                        dbgs() << "Subscript call with unsupported stride in: "
                               << F.getName() << "\n");
        return false;
      }

      // Save the set of subscript calls that use the dope vector for
      // profitability analysis.
      DVSubscriptCalls.insert(SubscriptCalls.begin(), SubscriptCalls.end());
    }

    // If there was a store of the dope vector into an uplevel variable, check
    // the uses of the uplevel variable.
    UpevelDVField Uplevel = DVA.getUplevelVar();
    if (Uplevel.first) {
      if (!analyzeUplevelVar(F, Uplevel, DVObject))
        return false;
    }

    return true;
  }

  // This checks the uses of an uplevel variable for safety. Safe uses are:
  // - If \p DVObject is non-null, we are analyzing the function that
  //   initialized the uplevel var. In this case the dope vector member of the
  //   uplevel can be written. Otherwise, writes are not allowed.
  // - If the dope vector object is loaded from the uplevel variable, the uses
  //   of the dope vector are checked to ensure the dope vector fields are not
  //   modified.
  // - If the uplevel variable is passed in a function call, a recursive call
  //   will be made to this routine to check the usage of the uplevel in the
  //   called function.
  bool analyzeUplevelVar(const Function &F, UpevelDVField &Uplevel,
                         Value *DVObject) {
    Value *Var = Uplevel.first;
    uint64_t FieldNum = Uplevel.second;

    DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                    dbgs() << "\nChecking use of uplevel variable in function: "
                           << F.getName() << " Field: " << FieldNum << "\n");

    // If the function makes use of the uplevel, then we expect there should be
    // an Instruction that is a GEP which gets the address of the DV field from
    // the uplevel variable. Collect all these GEPs into this vector for
    // analysis.
    SmallVector<GetElementPtrInst *, 4> DVFieldAddresses;

    // The uplevel variable may be passed to another function, collect the set
    // of {Function*, argument pos} pairs for functions that take this uplevel
    // as a parameter.
    FuncArgPosPairSet FuncsWithUplevelParams;

    for (auto *U : Var->users()) {
      auto *I = dyn_cast<Instruction>(U);
      assert(I && "Expected instruction\n");

      DEBUG_WITH_TYPE(DEBUG_ANALYSIS, dbgs()
                                          << "Upevel var use: " << *I << "\n");

      if (auto *GEP = dyn_cast<GetElementPtrInst>(I)) {
        if (GEP->getNumIndices() == 2) {
          auto Idx0 = getConstGEPIndex(*GEP, 1);
          auto Idx1 = getConstGEPIndex(*GEP, 2);
          if (Idx0 && Idx1 && *Idx0 == 0) {
            // Ignore uses of other uplevel fields.
            if (*Idx1 != FieldNum)
              continue;

            DVFieldAddresses.push_back(GEP);
            continue;
          }
        }
        DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                        dbgs() << "Unsupported usage of uplevel var:\n"
                               << *I << "\n");
        return false;
      } else if (auto *CI = dyn_cast<CallInst>(I)) {
        Function *F = CI->getCalledFunction();
        if (!F) {
          DEBUG_WITH_TYPE(
              DEBUG_ANALYSIS,
              dbgs() << "Uplevel var passed in indirect function call:\n"
                     << *CI << "\n");
          return false;
        }
        Optional<unsigned int> ArgPos = getArgumentPosition(*CI, Var);
        if (!ArgPos) {
          DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                          dbgs() << "Uplevel var argument not unique in call:\n"
                                 << *CI << "\n");
          return false;
        }
        FuncsWithUplevelParams.insert(FuncArgPosPair(F, *ArgPos));
      } else {
        DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                        dbgs() << "Unsupported usage of uplevel var:\n"
                               << *I << "\n");

        return false;
      }
    }

    // Check the usage for all the GEPs that get the address of the dope vector
    // variable.
    // If the dope vector pointer field is loaded, check that all uses of the
    // dope vector are safe. If the dope vector pointer field is stored, check
    // that it is the write we expected that is initializing the uplevel.
    for (auto *DVFieldAddr : DVFieldAddresses)
      for (auto *U : DVFieldAddr->users()) {
        auto *I = dyn_cast<Instruction>(U);
        assert(I && "Expected instruction\n");

        if (auto *LI = dyn_cast<LoadInst>(I)) {
          analyzeDVUseInFunction(F, LI);
        } else if (auto *SI = dyn_cast<StoreInst>(I)) {
          // The only store we expect to the DV field is the dope vector object
          // currently being analyzed.
          if (!DVObject || SI->getValueOperand() != DVObject) {
            DEBUG_WITH_TYPE(
                DEBUG_ANALYSIS,
                dbgs()
                    << "Store into uplevel var dope vector field no allowed\n");
            return false;
          }
        } else {
          return false;
        }
      }

    // Check all the functions that take the uplevel variable.
    for (auto &FuncArg : FuncsWithUplevelParams)
      if (!analyzeUplevelCallArg(*FuncArg.first, FuncArg.second, FieldNum))
        return false;

    return true;
  }

  // Check a called function for usage of the uplevel variable for safety.
  bool analyzeUplevelCallArg(Function &F, uint64_t ArgPos, uint64_t FieldNum) {
    if (F.isDeclaration())
      return false;

    assert(ArgPos < F.arg_size() && "Invalid argument position");
    auto Args = F.arg_begin();
    std::advance(Args, ArgPos);
    Argument *FormalArg = &(*Args);

    // Check the called function for its use of the uplevel passed in. We do
    // not allow the called function to store a new dope vector into the field,
    // so pass 'nullptr' for the DVObject.
    UpevelDVField LocalUplevel(FormalArg, FieldNum);
    if (!analyzeUplevelVar(F, LocalUplevel, nullptr))
      return false;

    return true;
  }

  // Check if the uses of the pointer address field results in a load
  // instruction that may result in the address of the array pointer being used
  // for something other than a supported subscript call. Return 'true' if all
  // the uses are supported.
  // This function also collects the set of subscript calls taking the address
  // of the array pointer into \p SubscriptCalls.
  bool checkArrayPointerUses(const DopeVectorAnalyzer &DVA,
                             CallInstSet &SubscriptCalls) {
    // Get a set of Value objects that hold the address of the array pointer.
    SmallPtrSet<Value *, 8> ArrayPtrValues;
    const DopeVectorFieldUse &PtrAddr = DVA.getPtrAddrField();
    if (!DVA.getAllValuesHoldingFieldValue(PtrAddr, ArrayPtrValues)) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                      dbgs() << "Unsupported use of array pointer address:\n");
      return false;
    }

    // Now check all uses of the address to be sure they are only used to move
    // the address to another var (Select or PhiNode), or are used in a
    // subscript intrinsic call.
    for (auto *ArrPtr : ArrayPtrValues) {
      DEBUG_WITH_TYPE(DEBUG_ANALYSIS, dbgs() << "  Uses: " << *ArrPtr << "\n");
      for (auto *PtrUser : ArrPtr->users()) {
        if (isa<SelectInst>(PtrUser) || isa<PHINode>(PtrUser)) {
          continue;
        } else if (auto *CI = dyn_cast<CallInst>(PtrUser)) {
          if (!isValidUseOfSubscriptCall(*CI, *ArrPtr, ArrayRank,
                                         ArrayRank - 1)) {
            dbgs() << "Array address: " << *ArrPtr
                   << " not in subscript call: " << *CI << "\n";
            return false;
          }

          SubscriptCalls.insert(CI);
        } else {
          DEBUG_WITH_TYPE(DEBUG_ANALYSIS,
                          dbgs()
                              << "Unsupported use of array pointer address:\n"
                              << *PtrUser << "\n");
          return false;
        }
      }
    }

    return true;
  }

  // Check that the subscript calls are using stride values from the dope
  // vector. This should always be true, until dope vector constant
  // propagation is implemented, in which case this transform needs to occur
  // first. Otherwise, this check will invalidate candidates that have
  // had constants substituted into the subscript calls.
  bool checkSubscriptStrideValues(const DopeVectorAnalyzer &DVA,
                                  const CallInstSet &SubscriptCalls) {
    SmallVector<SmallPtrSet<Value *, 4>, FortranMaxRank> StrideLoads;

    // Function to check one subscript call, and recurse to checks subscript
    // calls that use the result to verify the stride to the call is a member of
    // \p StrideLoads.
    std::function<bool(const SmallVectorImpl<SmallPtrSet<Value *, 4>> &,
                       const CallInst &, uint32_t)>
        CheckCall =
            [&CheckCall](
                const SmallVectorImpl<SmallPtrSet<Value *, 4>> &StrideLoads,
                const CallInst &CI, uint32_t Rank) -> bool {
      if (!isSubscriptIntrinsicCall(CI))
        return false;

      Value *StrideOp = CI.getArgOperand(StrideOpNum);
      if (!StrideLoads[Rank].count(StrideOp))
        return false;

      if (Rank == 0)
        return true;

      for (auto *UU : CI.users())
        if (auto *CI2 = dyn_cast<CallInst>(UU))
          if (!CheckCall(StrideLoads, *CI2, Rank - 1))
            return false;

      return true;
    };

    // For each dimension of the variable, get the set of objects that hold the
    // value for the stride loaded from the dope vector object
    for (unsigned Dim = 0; Dim < ArrayRank; ++Dim) {
      if (!DVA.hasStrideField(Dim))
        return false;

      const DopeVectorFieldUse &StrideField = DVA.getStrideField(Dim);
      StrideLoads.push_back(SmallPtrSet<Value *, 4>());
      auto &LoadSet = StrideLoads.back();
      bool Valid = DVA.getAllValuesHoldingFieldValue(StrideField, LoadSet);
      if (!Valid)
        return false;
    }

    // Check all the subscript calls to ensure the stride value comes from the
    // dope vector.
    for (auto *Call : SubscriptCalls)
      if (!CheckCall(StrideLoads, *Call, ArrayRank - 1))
        return false;

    return true;
  }

  // Transform the strides in the subscript calls and dope vector creation, if
  // the candidate is valid for being transposed.
  bool transform() {
    if (!IsValid)
      return false;

    LLVM_DEBUG(dbgs() << "Transforming candidate:" << GV->getName() << "\n");
    transposeStrides();
    return true;
  }

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
  void dump() { print(dbgs()); }

  void print(raw_ostream &OS) {
    OS << "Transpose candidate: " << GV->getName() << "\n";
    OS << "Type         : " << *GV->getType() << "\n";
    OS << "Rank         : " << ArrayRank << "\n";
    OS << "Length       : " << ArrayLength << "\n";
    OS << "Element size : " << ElementSize << "\n";
    OS << "Element type : " << *ElementType << "\n";
    OS << "Strides      :";
    for (uint32_t RankNum = 0; RankNum < ArrayRank; ++RankNum)
      OS << " " << Strides[RankNum];
    OS << "\n";

    OS << "Transposition:";
    if (!Transposition.empty())
      for (uint32_t RankNum = 0; RankNum < ArrayRank; ++RankNum)
        OS << " " << Transposition[RankNum];
    OS << "\n";
    OS << "IsValid      : " << (IsValid ? "true" : "false") << "\n";
    OS << "--------------\n";
  }
#endif // !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)

private:
  // The global variable that is a possible candidate
  GlobalVariable *GV;

  // Number of dimensions (Fortran Rank) for the array
  uint32_t ArrayRank;

  // Number of elements in each dimension of the array. (Candidates must have
  // the same length in all dimensions)
  uint64_t ArrayLength;

  // Size of one element in the array, in bytes.
  uint64_t ElementSize;

  // Element type in the array
  llvm::Type *ElementType;

  // This vector stores the stride values used when operating on the complete
  // array. For this optimization, we do not support cases where a sub-object is
  // passed to a function as a portion of the array.
  SmallVector<uint64_t, FortranMaxRank> Strides;

  // This vector stores the transpose index that will be used to access the
  // stride for a particular rank. For example, the regular layout of an array
  // that accesses 'block[i][j][k]', uses 'i' for the Rank 2 element, 'j' for
  // the Rank 1 element, and 'k' for the Rank 0 element, which would be
  // represented as accessing elements 0, 1, and 2 from the 'Strides' array.
  // Transposing the strides for the i and k elements would correspond to this
  // index lookup array being {2, 1, 0}
  SmallVector<uint32_t, FortranMaxRank> Transposition;

  // Set of calls to the subscript intrinsic that directly access the array
  // address. These have the highest 'rank' value for the subscript calls. The
  // result of this instruction is fed to the subscript call of the next lower
  // rank, so we only need to store the initial call to get to all the others
  // for computing profitability and transposing the stride values.
  CallInstSet SubscriptCalls;

  // Set of calls to the subscript intrinsic that access the candidate via a
  // dope vector. These calls should be analyzed for profitability but do not
  // need to be transformed because they take their parameters from the dope
  // vector.
  CallInstSet DVSubscriptCalls;

  // Set of dope vector objects that were directly created from the global
  // variable.
  SmallPtrSet<DopeVectorAnalyzer *, 4> DopeVectorInstances;

  // Indicates whether the analysis determined the candidate is safe to
  // transpose.
  bool IsValid;

  // This function will swap the strides used for indexing into the array. These
  // need to be changed for subscript operators that directly index into the
  // global variable, and for the setup of the dope vectors used when passing
  // the global variable to another function.
  void transposeStrides() {
    // TODO: transformation of uses goes here.
  }
};

//
// The array stride transpose optimization for Fortran.
//
// This optimization swaps the stride values used for multi-dimensional Fortran
// arrays to improve cache utilization or enable loop unrolling by having unit
// stride memory access patterns.
//
// For example, the default memory layout for the Fortran array declared as
// "integer block(3,3)" is stored in column-major order resulting in the access
// to block(i,j) being computed as:
//     &block + j * 3 * sizeof(integer) + i * sizeof(integer)
//
// For a loop iterating along 'j', transposing the strides may enable downstream
// optimizations so that iterations along 'j' will be a unit stride.
//
// This class will heuristically estimate the benefit and swap the stride values
// when beneficial.
class TransposeImpl {
public:
  TransposeImpl(std::function<LoopInfo &(Function &)> &GetLI) : GetLI(GetLI) {
    (void)this->GetLI;
  }

  bool run(Module &M) {
    const DataLayout &DL = M.getDataLayout();

    IdentifyCandidates(M);

    bool ValidCandidate = false;
    for (auto &Cand : Candidates) {
      ValidCandidate |= Cand.analyze(DL);

      // TODO: Analyze the candidate for profitability

#if !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
      if (PrintCandidates)
        Cand.dump();
#endif // !defined(NDEBUG) || defined(LLVM_ENABLE_DUMP)
    }

    bool Changed = false;
    if (ValidCandidate)
      for (auto &Cand : Candidates)
        Changed |= Cand.transform();

    return Changed;
  }

private:
  std::function<LoopInfo &(Function &)> &GetLI;

  // Global variable candidates for the transformation.
  SmallVector<TransposeCandidate, 8> Candidates;

  // Identify potential candidates for the transpose optimization.
  //
  // The initial set of candidates meet the following criteria:
  // - Global Variable with internal linkage
  // - Multi-dimensional array of integer type
  // - The array lengths in all dimensions are equal
  // - Variable uses zero initializer
  void IdentifyCandidates(Module &M) {
    const DataLayout &DL = M.getDataLayout();

    for (auto &GV : M.globals()) {
      if (!GV.hasInitializer() || !GV.getInitializer()->isZeroValue())
        continue;

      // All uses of the variable need to be analyzed, therefore we need
      // internal linkage.
      if (!GV.hasInternalLinkage())
        continue;

      // All global variables are pointers
      llvm::Type *Ty = GV.getType()->getPointerElementType();
      auto *ArrType = dyn_cast<llvm::ArrayType>(Ty);
      if (!ArrType)
        continue;

      uint32_t Dimensions = 1;
      bool AllSame = true;
      uint64_t Length = ArrType->getArrayNumElements();
      llvm::Type *ElemType = ArrType->getArrayElementType();
      while (ElemType->isArrayTy()) {
        auto *InnerArrType = cast<llvm::ArrayType>(ElemType);
        if (InnerArrType->getArrayNumElements() != Length) {
          AllSame = false;
          break;
        }
        Dimensions++;
        ElemType = InnerArrType->getArrayElementType();
      }

      if (AllSame && Dimensions > 1 && Dimensions <= FortranMaxRank &&
          ElemType->isIntegerTy()) {
        LLVM_DEBUG(dbgs() << "Adding candidate: " << GV << "\n");
        uint64_t ElemSize = DL.getTypeStoreSize(ElemType);
        TransposeCandidate Candidate(&GV, Dimensions, Length, ElemSize,
                                     ElemType);
        Candidates.push_back(Candidate);
      }
    }
  }
};

// Legacy pass manager wrapper for invoking the Transpose pass.
class DTransTransposeWrapper : public ModulePass {
private:
  dtrans::TransposePass Impl;

public:
  static char ID;
  DTransTransposeWrapper() : ModulePass(ID) {
    initializeDTransTransposeWrapperPass(*PassRegistry::getPassRegistry());
  }

  bool runOnModule(Module &M) override {
    if (skipModule(M))
      return false;

    dtrans::LoopInfoFuncType GetLI = [this](Function &F) -> LoopInfo & {
      return this->getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
    };

    return Impl.runImpl(M, GetLI);
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    // Note, this transformation is not dependent on Whole Program Analysis.
    // The only candidates that may be selected for the transformation will
    // have internal linkage, and the analysis will be verifying all uses of
    // the candidate, which will ensure that the candidate is not escaped to
    // an external routine.

    AU.addRequired<LoopInfoWrapperPass>();

    // The swapping of the stride values in the dope vectors and
    // llvm.intel.subscript intrinsic call should not invalidate any analysis.
    AU.setPreservesAll();
  }
};

} // end anonymous namespace

char DTransTransposeWrapper::ID = 0;
INITIALIZE_PASS_BEGIN(DTransTransposeWrapper, "dtrans-transpose",
                      "DTrans multi-dimensional array transpose for Fortran",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_END(DTransTransposeWrapper, "dtrans-transpose",
                    "DTrans multi-dimensional array transpose for Fortran",
                    false, false)

ModulePass *llvm::createDTransTransposeWrapperPass() {
  return new DTransTransposeWrapper();
}

namespace llvm {

namespace dtrans {

PreservedAnalyses TransposePass::run(Module &M, ModuleAnalysisManager &AM) {
  FunctionAnalysisManager &FAM =
      AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  LoopInfoFuncType GetLI = [&FAM](Function &F) -> LoopInfo & {
    return FAM.getResult<LoopAnalysis>(F);
  };

  runImpl(M, GetLI);

  // The swapping of the stride values in the dope vectors and
  // llvm.intel.subscript intrinsic call should not invalidate any analysis.
  return PreservedAnalyses::all();
}

bool TransposePass::runImpl(Module &M,
                            std::function<LoopInfo &(Function &)> &GetLI) {
  TransposeImpl Transpose(GetLI);
  return Transpose.run(M);
}

} // end namespace dtrans
} // end namespace llvm
