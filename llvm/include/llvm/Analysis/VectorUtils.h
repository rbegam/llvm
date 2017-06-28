//===- llvm/Transforms/Utils/VectorUtils.h - Vector utilities -*- C++ -*-=====//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines some vectorizer utilities.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_UTILS_VECTORUTILS_H
#define LLVM_TRANSFORMS_UTILS_VECTORUTILS_H

#include "llvm/ADT/MapVector.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/Intel_VectorVariant.h" // INTEL

namespace llvm {

template <typename T> class ArrayRef;
class DemandedBits;
class GetElementPtrInst;
class Loop;
class ScalarEvolution;
class TargetTransformInfo;
class Type;
class Value;
class SCEV; // INTEL
class Attribute; // INTEL

namespace Intrinsic {
enum ID : unsigned;
}

/// \brief Identify if the intrinsic is trivially vectorizable.
/// This method returns true if the intrinsic's argument types are all
/// scalars for the scalar form of the intrinsic and all vectors for
/// the vector form of the intrinsic.
bool isTriviallyVectorizable(Intrinsic::ID ID);

/// \brief Identifies if the intrinsic has a scalar operand. It checks for
/// ctlz,cttz and powi special intrinsics whose argument is scalar.
bool hasVectorInstrinsicScalarOpd(Intrinsic::ID ID, unsigned ScalarOpdIdx);

/// \brief Returns intrinsic ID for call.
/// For the input call instruction it finds mapping intrinsic and returns
/// its intrinsic ID, in case it does not found it return not_intrinsic.
Intrinsic::ID getVectorIntrinsicIDForCall(const CallInst *CI,
                                          const TargetLibraryInfo *TLI);

/// \brief Find the operand of the GEP that should be checked for consecutive
/// stores. This ignores trailing indices that have no effect on the final
/// pointer.
unsigned getGEPInductionOperand(const GetElementPtrInst *Gep);

/// \brief If the argument is a GEP, then returns the operand identified by
/// getGEPInductionOperand. However, if there is some other non-loop-invariant
/// operand, it returns that instead.
Value *stripGetElementPtr(Value *Ptr, ScalarEvolution *SE, Loop *Lp);

/// \brief If a value has only one user that is a CastInst, return it.
Value *getUniqueCastUse(Value *Ptr, Loop *Lp, Type *Ty);

/// \brief Get the stride of a pointer access in a loop. Looks for symbolic
/// strides "a[i*stride]". Returns the symbolic stride, or null otherwise.
Value *getStrideFromPointer(Value *Ptr, ScalarEvolution *SE, Loop *Lp);

/// \brief Given a vector and an element number, see if the scalar value is
/// already around as a register, for example if it were inserted then extracted
/// from the vector.
Value *findScalarElement(Value *V, unsigned EltNo);

/// \brief Get splat value if the input is a splat vector or return nullptr.
/// The value may be extracted from a splat constants vector or from
/// a sequence of instructions that broadcast a single value into a vector.
const Value *getSplatValue(const Value *V);

#if INTEL_CUSTOMIZATION
/// \brief Compute a map of integer instructions to their minimum legal type
/// size.
///
/// C semantics force sub-int-sized values (e.g. i8, i16) to be promoted to int
/// type (e.g. i32) whenever arithmetic is performed on them.
///
/// For targets with native i8 or i16 operations, usually InstCombine can shrink
/// the arithmetic type down again. However InstCombine refuses to create
/// illegal types, so for targets without i8 or i16 registers, the lengthening
/// and shrinking remains.
///
/// Most SIMD ISAs (e.g. NEON) however support vectors of i8 or i16 even when
/// their scalar equivalents do not, so during vectorization it is important to
/// remove these lengthens and truncates when deciding the profitability of
/// vectorization.
///
/// This function analyzes the given range of instructions and determines the
/// minimum type size each can be converted to. It attempts to remove or
/// minimize type size changes across each def-use chain, so for example in the
/// following code:
///
///   %1 = load i8, i8*
///   %2 = add i8 %1, 2
///   %3 = load i16, i16*
///   %4 = zext i8 %2 to i32
///   %5 = zext i16 %3 to i32
///   %6 = add i32 %4, %5
///   %7 = trunc i32 %6 to i16
///
/// Instruction %6 must be done at least in i16, so computeMinimumValueSizes
/// will return: {%1: 16, %2: 16, %3: 16, %4: 16, %5: 16, %6: 16, %7: 16}.
///
/// If the optional TargetTransformInfo is provided, this function tries harder
/// to do less work by only looking at illegal types.
MapVector<Instruction*, uint64_t>
computeMinimumValueSizes(ArrayRef<BasicBlock*> Blocks,
                         DemandedBits &DB,
                         const TargetTransformInfo *TTI=nullptr);

/// \brief This function marks the CallInst VecCall with the appropriate stride
/// information determined by getExprStride(), which is used later in LLVM IR
/// generation for loads/stores. Initial use of this information is used during
/// SVML translation for sincos vectorization, but could be applicable to any
/// situation where we need to analyze memory references.
void analyzeCallArgMemoryReferences(CallInst *CI, CallInst *VecCall,
                                    const TargetLibraryInfo *TLI,
                                    ScalarEvolution *SE, Loop *OrigLoop);

/// @brief Contains the names of the declared vector function variants
typedef std::vector<std::string> DeclaredVariants;

/// @brief Contains a mapping of a function to its vector function variants
typedef std::map<Function*, DeclaredVariants> FunctionVariants;

/// @brief Get all function attributes that specify a vector variant
/// @param F Function to inspect
/// @return A vector of all matching attributes
std::vector<Attribute> getVectorVariantAttributes(Function& F);

/// \brief Determine the characteristic type of the vector function as
/// specified according to the vector function ABI.
Type* calcCharacteristicType(Function& F, VectorVariant& Variant);

/// \brief Get all functions marked for vectorization in module and their
/// list of variants.
void getFunctionsToVectorize(
  Module &M, std::map<Function*, std::vector<StringRef> > &FuncVars);

/// \brief Widens the function call \p Call using a vector length of \p VL and
/// inserts the appropriate function declaration if not already created. This
/// function will insert functions for library calls, intrinsics, and simd
/// functions.
Function* getOrInsertVectorFunction(const CallInst *Call, unsigned VL,
                                    SmallVectorImpl<Type*> &ArgTys,
                                    TargetLibraryInfo *TLI,
                                    Intrinsic::ID ID,
                                    VectorVariant *VecVariant,
                                    bool Masked);

#endif // INTEL_CUSTOMIZATION

#if INTEL_OPENCL
/// \brief Return true if \p FnName is an OpenCL read channel function
bool isOpenCLReadChannel(StringRef FnName);

/// \brief Return true if \p FnName is an OpenCL write channel function
bool isOpenCLWriteChannel(StringRef FnName);

/// \brief Return true if the argument at \p Idx is the read destination for
/// an OpenCL read channel call.
bool isOpenCLReadChannelDest(StringRef FnName, unsigned Idx);

/// \brief Return true if the argument at \p Idx is the write source for an
/// OpenCL write channel call.
bool isOpenCLWriteChannelSrc(StringRef FnName, unsigned Idx);

/// \brief Returns the alloca associated with an OpenCL read channel call.
Value* getOpenCLReadChannelDestAlloc(const CallInst *Call);
#endif // INTEL_OPENCL
    
/// Specifically, let Kinds = [MD_tbaa, MD_alias_scope, MD_noalias, MD_fpmath,
/// MD_nontemporal].  For K in Kinds, we get the MDNode for K from each of the
/// elements of VL, compute their "intersection" (i.e., the most generic
/// metadata value that covers all of the individual values), and set I's
/// metadata for M equal to the intersection value.
///
/// This function always sets a (possibly null) value for each K in Kinds.
Instruction *propagateMetadata(Instruction *I, ArrayRef<Value *> VL);

} // llvm namespace

#endif
