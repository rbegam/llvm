//===-- CSATargetTransformInfo.h - CSA specific TTI -------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
/// \file
/// This file a TargetTransformInfo::Concept conforming object specific to the
/// CSA target machine. It uses the target's detailed information to provide
/// more precise answers to certain TTI queries, while letting the target
/// independent and default TTI implementations handle the rest.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_CSA_CSATARGETTRANSFORMINFO_H
#define LLVM_LIB_TARGET_CSA_CSATARGETTRANSFORMINFO_H

#include "llvm/CodeGen/Passes.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "CSATargetMachine.h"
#include "CSA.h"
#include <utility>

namespace llvm {

class CSATTIImpl : public BasicTTIImplBase<CSATTIImpl> {
  typedef BasicTTIImplBase<CSATTIImpl> BaseT;
  typedef TargetTransformInfo TTI;
  friend BaseT;

  const CSASubtarget *ST;
  const CSATargetLowering *TLI;

  /// Estimate the overhead of scalarizing an instruction. Insert and Extract
  /// are set if the result needs to be inserted and/or extracted from vectors.
  unsigned getScalarizationOverhead(Type *Ty, bool Insert, bool Extract);

  /// Estimate the cost overhead of SK_Alternate shuffle.
  unsigned getAltShuffleOverhead(Type *Ty);

  const CSATargetLowering *getTLI() const {
    return TLI;
  }

public:
  explicit CSATTIImpl(const CSATargetMachine *TM, const Function &F)
      : BaseT(TM, F.getParent()->getDataLayout()), ST(TM->getSubtargetImpl()),
        TLI(ST->getTargetLowering()) {}

  // Provide value semantics. MSVC requires that we spell all of these out.
  CSATTIImpl(const CSATTIImpl &Arg)
      : BaseT(static_cast<const BaseT &>(Arg)), ST(Arg.ST), TLI(Arg.TLI) {}
  CSATTIImpl(CSATTIImpl &&Arg)
      : BaseT(std::move(static_cast<BaseT &>(Arg))), ST(std::move(Arg.ST)),
      TLI(std::move(Arg.TLI)) {}

  bool hasBranchDivergence() const;

  /// \name Scalar TTI Implementations
  /// @{

  bool isLegalAddImmediate(int64_t imm) const;
  bool isLegalICmpImmediate(int64_t imm) const;
  bool isTruncateFree(Type *Ty1, Type *Ty2) const;
  bool isTypeLegal(Type *Ty) const;
  unsigned getJumpBufAlignment() const;
  unsigned getJumpBufSize() const;
  bool shouldBuildLookupTables() const;
  bool haveFastSqrt(Type *Ty) const;
  void getUnrollingPreferences(Loop *L, TTI::UnrollingPreferences &UP);

  /// @}

  /// \name Vector TTI Implementations
  /// @{

  unsigned getNumberOfRegisters(bool Vector);
  unsigned getMaxInterleaveFactor(unsigned VF);
  unsigned getRegisterBitWidth(bool Vector);
  unsigned getArithmeticInstrCost(unsigned Opcode, Type *Ty,
          TTI::OperandValueKind = TTI::OK_AnyValue,
          TTI::OperandValueKind = TTI::OK_AnyValue,
          TTI::OperandValueProperties = TTI::OP_None,
          TTI::OperandValueProperties = TTI::OP_None);
  unsigned getShuffleCost(TTI::ShuffleKind Kind, Type *Tp,
                          int Index, Type *SubTp);
  unsigned getCastInstrCost(unsigned Opcode, Type *Dst,
                            Type *Src);
  unsigned getCFInstrCost(unsigned Opcode);
  unsigned getCmpSelInstrCost(unsigned Opcode, Type *ValTy,
                              Type *CondTy);
  unsigned getVectorInstrCost(unsigned Opcode, Type *Val,
                              unsigned Index);
  unsigned getMemoryOpCost(unsigned Opcode, Type *Src, unsigned Alignment,
                           unsigned AddressSpace);
  unsigned getIntrinsicInstrCost(Intrinsic::ID, Type *RetTy,
                                 ArrayRef<Type*> Tys, FastMathFlags FMF);
  unsigned getIntrinsicInstrCost(Intrinsic::ID, Type *RetTy,
                                 ArrayRef<Value*> Args, FastMathFlags FMF);
  unsigned getNumberOfParts(Type *Tp);
  unsigned getAddressComputationCost( Type *Ty, bool IsComplex);
  unsigned getReductionCost(unsigned Opcode, Type *Ty,
                            bool IsPairwise);

  /// @}
};

} // end namespace llvm

#endif
