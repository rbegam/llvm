#include "CSATargetTransformInfo.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/CodeGen/BasicTTIImpl.h"
#include "llvm/Analysis/TargetTransformInfo.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetLowering.h"
#include "llvm/Target/TargetSubtargetInfo.h"
#include "CSATargetMachine.h"
#include "CSA.h"
#include <utility>
using namespace llvm;

static cl::opt<unsigned>
CSAUnrollingThreshold("csa-unrolling-threshold", cl::init(248),
cl::desc("Threshold for partial unrolling"), cl::Hidden);

bool CSATTIImpl::hasBranchDivergence() const { return false; }

bool CSATTIImpl::isLegalAddImmediate(int64_t imm) const {
  return getTLI()->isLegalAddImmediate(imm);
}

bool CSATTIImpl::isLegalICmpImmediate(int64_t imm) const {
  return getTLI()->isLegalICmpImmediate(imm);
}

bool CSATTIImpl::isTruncateFree(Type *Ty1, Type *Ty2) const {
  return getTLI()->isTruncateFree(Ty1, Ty2);
}

bool CSATTIImpl::isTypeLegal(Type *Ty) const {
  EVT T = getTLI()->getValueType(DL, Ty);
  return getTLI()->isTypeLegal(T);
}

unsigned CSATTIImpl::getJumpBufAlignment() const {
  return getTLI()->getJumpBufAlignment();
}

unsigned CSATTIImpl::getJumpBufSize() const {
  return getTLI()->getJumpBufSize();
}

bool CSATTIImpl::shouldBuildLookupTables() const {
  const TargetLoweringBase *TLI = getTLI();
  return TLI->isOperationLegalOrCustom(ISD::BR_JT, MVT::Other) ||
         TLI->isOperationLegalOrCustom(ISD::BRIND, MVT::Other);
}

bool CSATTIImpl::haveFastSqrt(Type *Ty) const {
  const TargetLoweringBase *TLI = getTLI();
  EVT VT = TLI->getValueType(DL, Ty);
  return TLI->isTypeLegal(VT) && TLI->isOperationLegalOrCustom(ISD::FSQRT, VT);
}

void CSATTIImpl::getUnrollingPreferences(Loop *L, TTI::UnrollingPreferences &UP) {
  // This unrolling functionality is target independent, but to provide some
  // motivation for its intended use, for x86:

  // According to the Intel 64 and IA-32 Architectures Optimization Reference
  // Manual, Intel Core models and later have a loop stream detector
  // (and associated uop queue) that can benefit from partial unrolling.
  // The relevant requirements are:
  //  - The loop must have no more than 4 (8 for Nehalem and later) branches
  //    taken, and none of them may be calls.
  //  - The loop can have no more than 18 (28 for Nehalem and later) uops.

  // According to the Software Optimization Guide for AMD Family 15h Processors,
  // models 30h-4fh (Steamroller and later) have a loop predictor and loop
  // buffer which can benefit from partial unrolling.
  // The relevant requirements are:
  //  - The loop must have fewer than 16 branches
  //  - The loop must have less than 40 uops in all executed loop branches

  // The number of taken branches in a loop is hard to estimate here, and
  // benchmarking has revealed that it is better not to be conservative when
  // estimating the branch count. As a result, we'll ignore the branch limits
  // until someone finds a case where it matters in practice.

  //  unsigned MaxOps = 0;
  if (ST->getSchedModel().LoopMicroOpBufferSize > 0)
    /* MaxOps = ST->getSchedModel().LoopMicroOpBufferSize */ ;
  //CSA edit: set up runtime unroll threshold for CSA target to UINT32_MAX
  else {
    assert(ST->getTargetTriple().str().substr(0, 3) == "csa");
    if (!CSAUnrollingThreshold) {
      return;
    } 
  }

  // Scan the loop: don't unroll loops with calls.
  for (Loop::block_iterator I = L->block_begin(), E = L->block_end();
       I != E; ++I) {
    BasicBlock *BB = *I;

    for (BasicBlock::iterator J = BB->begin(), JE = BB->end(); J != JE; ++J)
      if (isa<CallInst>(J) || isa<InvokeInst>(J)) {
        ImmutableCallSite CS(&*J);
        if (const Function *F = CS.getCalledFunction()) {
          if (!BaseT::isLoweredToCall(F))
            continue;
        }

        return;
      }
  }

  // Enable runtime and partial unrolling up to the specified size.
  UP.Partial = UP.Runtime = true;
  UP.PartialThreshold = UP.PartialOptSizeThreshold = CSAUnrollingThreshold;
}

//===----------------------------------------------------------------------===//
//
// Calls used by the vectorizers.
//
//===----------------------------------------------------------------------===//

unsigned CSATTIImpl::getScalarizationOverhead(Type *Ty, bool Insert,
                                            bool Extract) {
  assert (Ty->isVectorTy() && "Can only scalarize vectors");
  unsigned Cost = 0;

  for (int i = 0, e = Ty->getVectorNumElements(); i < e; ++i) {
    if (Insert)
      Cost += BaseT::getVectorInstrCost(Instruction::InsertElement, Ty, i);
    if (Extract)
      Cost += BaseT::getVectorInstrCost(Instruction::ExtractElement, Ty, i);
  }

  return Cost;
}

unsigned CSATTIImpl::getNumberOfRegisters(bool Vector) {
  return 1;
}

unsigned CSATTIImpl::getRegisterBitWidth(bool Vector) {
  return 32;
}

unsigned CSATTIImpl::getMaxInterleaveFactor(unsigned VF) {
  return 1;
}

unsigned CSATTIImpl::getArithmeticInstrCost(unsigned Opcode, Type *Ty,
                                          TTI::OperandValueKind, TTI::OperandValueKind,
                                          TTI::OperandValueProperties,
                                          TTI::OperandValueProperties) {
  // Check if any of the operands are vector operands.
  const TargetLoweringBase *TLI = getTLI();
  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  assert(ISD && "Invalid opcode");

  std::pair<unsigned, MVT> LT = TLI->getTypeLegalizationCost(DL, Ty);

  bool IsFloat = Ty->getScalarType()->isFloatingPointTy();
  // Assume that floating point arithmetic operations cost twice as much as
  // integer operations.
  unsigned OpCost = (IsFloat ? 2 : 1);

  if (TLI->isOperationLegalOrPromote(ISD, LT.second)) {
    // The operation is legal. Assume it costs 1.
    // If the type is split to multiple registers, assume that there is some
    // overhead to this.
    // TODO: Once we have extract/insert subvector cost we need to use them.
    if (LT.first > 1)
      return LT.first * 2 * OpCost;
    return LT.first * 1 * OpCost;
  }

  if (!TLI->isOperationExpand(ISD, LT.second)) {
    // If the operation is custom lowered then assume
    // thare the code is twice as expensive.
    return LT.first * 2 * OpCost;
  }

  // Else, assume that we need to scalarize this op.
  if (Ty->isVectorTy()) {
    unsigned Num = Ty->getVectorNumElements();
    unsigned Cost = BaseT::getArithmeticInstrCost(Opcode, Ty->getScalarType());
    // return the cost of multiple scalar invocation plus the cost of inserting
    // and extracting the values.
    return getScalarizationOverhead(Ty, true, true) + Num * Cost;
  }

  // We don't know anything about this scalar instruction.
  return OpCost;
}

unsigned CSATTIImpl::getAltShuffleOverhead(Type *Ty) {
  assert(Ty->isVectorTy() && "Can only shuffle vectors");
  unsigned Cost = 0;
  // Shuffle cost is equal to the cost of extracting element from its argument
  // plus the cost of inserting them onto the result vector.

  // e.g. <4 x float> has a mask of <0,5,2,7> i.e we need to extract from index
  // 0 of first vector, index 1 of second vector,index 2 of first vector and
  // finally index 3 of second vector and insert them at index <0,1,2,3> of
  // result vector.
  for (int i = 0, e = Ty->getVectorNumElements(); i < e; ++i) {
    Cost += BaseT::getVectorInstrCost(Instruction::InsertElement, Ty, i);
    Cost += BaseT::getVectorInstrCost(Instruction::ExtractElement, Ty, i);
  }
  return Cost;
}

unsigned CSATTIImpl::getShuffleCost(TTI::ShuffleKind Kind, Type *Tp, int Index,
                                  Type *SubTp) {
  if (Kind == TTI::SK_Alternate) {
    return getAltShuffleOverhead(Tp);
  }
  return 1;
}

unsigned CSATTIImpl::getCastInstrCost(unsigned Opcode, Type *Dst,
                                    Type *Src) {
  const TargetLoweringBase *TLI = getTLI();
  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  assert(ISD && "Invalid opcode");

  std::pair<unsigned, MVT> SrcLT = TLI->getTypeLegalizationCost(DL, Src);
  std::pair<unsigned, MVT> DstLT = TLI->getTypeLegalizationCost(DL, Dst);

  // Check for NOOP conversions.
  if (SrcLT.first == DstLT.first &&
      SrcLT.second.getSizeInBits() == DstLT.second.getSizeInBits()) {

      // Bitcast between types that are legalized to the same type are free.
      if (Opcode == Instruction::BitCast || Opcode == Instruction::Trunc)
        return 0;
  }

  if (Opcode == Instruction::Trunc &&
      TLI->isTruncateFree(SrcLT.second, DstLT.second))
    return 0;

  if (Opcode == Instruction::ZExt &&
      TLI->isZExtFree(SrcLT.second, DstLT.second))
    return 0;

  // If the cast is marked as legal (or promote) then assume low cost.
  if (SrcLT.first == DstLT.first &&
      TLI->isOperationLegalOrPromote(ISD, DstLT.second))
    return 1;

  // Handle scalar conversions.
  if (!Src->isVectorTy() && !Dst->isVectorTy()) {

    // Scalar bitcasts are usually free.
    if (Opcode == Instruction::BitCast)
      return 0;

    // Just check the op cost. If the operation is legal then assume it costs 1.
    if (!TLI->isOperationExpand(ISD, DstLT.second))
      return  1;

    // Assume that illegal scalar instruction are expensive.
    return 4;
  }

  // Check vector-to-vector casts.
  if (Dst->isVectorTy() && Src->isVectorTy()) {

    // If the cast is between same-sized registers, then the check is simple.
    if (SrcLT.first == DstLT.first &&
        SrcLT.second.getSizeInBits() == DstLT.second.getSizeInBits()) {

      // Assume that Zext is done using AND.
      if (Opcode == Instruction::ZExt)
        return 1;

      // Assume that sext is done using SHL and SRA.
      if (Opcode == Instruction::SExt)
        return 2;

      // Just check the op cost. If the operation is legal then assume it costs
      // 1 and multiply by the type-legalization overhead.
      if (!TLI->isOperationExpand(ISD, DstLT.second))
        return SrcLT.first * 1;
    }

    // If we are converting vectors and the operation is illegal, or
    // if the vectors are legalized to different types, estimate the
    // scalarization costs.
    unsigned Num = Dst->getVectorNumElements();
    unsigned Cost = BaseT::getCastInstrCost(Opcode, Dst->getScalarType(),
                                             Src->getScalarType());

    // Return the cost of multiple scalar invocation plus the cost of
    // inserting and extracting the values.
    return getScalarizationOverhead(Dst, true, true) + Num * Cost;
  }

  // We already handled vector-to-vector and scalar-to-scalar conversions. This
  // is where we handle bitcast between vectors and scalars. We need to assume
  //  that the conversion is scalarized in one way or another.
  if (Opcode == Instruction::BitCast)
    // Illegal bitcasts are done by storing and loading from a stack slot.
    return (Src->isVectorTy()? getScalarizationOverhead(Src, false, true):0) +
           (Dst->isVectorTy()? getScalarizationOverhead(Dst, true, false):0);

  llvm_unreachable("Unhandled cast");
 }

unsigned CSATTIImpl::getCFInstrCost(unsigned Opcode) {
  // Branches are assumed to be predicted.
  return 0;
}

unsigned CSATTIImpl::getCmpSelInstrCost(unsigned Opcode, Type *ValTy,
                                      Type *CondTy) {
  const TargetLoweringBase *TLI = getTLI();
  int ISD = TLI->InstructionOpcodeToISD(Opcode);
  assert(ISD && "Invalid opcode");

  // Selects on vectors are actually vector selects.
  if (ISD == ISD::SELECT) {
    assert(CondTy && "CondTy must exist");
    if (CondTy->isVectorTy())
      ISD = ISD::VSELECT;
  }

  std::pair<unsigned, MVT> LT = TLI->getTypeLegalizationCost(DL, ValTy);

  if (!(ValTy->isVectorTy() && !LT.second.isVector()) &&
      !TLI->isOperationExpand(ISD, LT.second)) {
    // The operation is legal. Assume it costs 1. Multiply
    // by the type-legalization overhead.
    return LT.first * 1;
  }

  // Otherwise, assume that the cast is scalarized.
  if (ValTy->isVectorTy()) {
    unsigned Num = ValTy->getVectorNumElements();
    if (CondTy)
      CondTy = CondTy->getScalarType();
    unsigned Cost = BaseT::getCmpSelInstrCost(Opcode, ValTy->getScalarType(),
                                               CondTy);

    // Return the cost of multiple scalar invocation plus the cost of inserting
    // and extracting the values.
    return getScalarizationOverhead(ValTy, true, false) + Num * Cost;
  }

  // Unknown scalar opcode.
  return 1;
}

unsigned CSATTIImpl::getVectorInstrCost(unsigned Opcode, Type *Val,
                                      unsigned Index) {
  std::pair<unsigned, MVT> LT =  getTLI()->getTypeLegalizationCost(DL, Val->getScalarType());

  return LT.first;
}

unsigned CSATTIImpl::getMemoryOpCost(unsigned Opcode, Type *Src,
                                   unsigned Alignment,
                                   unsigned AddressSpace) {
  assert(!Src->isVoidTy() && "Invalid type");
  std::pair<unsigned, MVT> LT = getTLI()->getTypeLegalizationCost(DL, Src);

  // Assuming that all loads of legal types cost 1.
  unsigned Cost = LT.first;

  if (Src->isVectorTy() &&
      Src->getPrimitiveSizeInBits() < LT.second.getSizeInBits()) {
    // This is a vector load that legalizes to a larger type than the vector
    // itself. Unless the corresponding extending load or truncating store is
    // legal, then this will scalarize.
    TargetLowering::LegalizeAction LA = TargetLowering::Expand;
    EVT MemVT = getTLI()->getValueType(DL, Src, true);
    if (MemVT.isSimple() && MemVT != MVT::Other) {
      if (Opcode == Instruction::Store)
        LA = getTLI()->getTruncStoreAction(LT.second, MemVT.getSimpleVT());
      else
        LA = getTLI()->getLoadExtAction(ISD::EXTLOAD, LT.second, MemVT);
    }

    if (LA != TargetLowering::Legal && LA != TargetLowering::Custom) {
      // This is a vector load/store for some illegal type that is scalarized.
      // We must account for the cost of building or decomposing the vector.
      Cost += getScalarizationOverhead(Src, Opcode != Instruction::Store,
                                            Opcode == Instruction::Store);
    }
  }

  return Cost;
}

unsigned CSATTIImpl::getIntrinsicInstrCost(Intrinsic::ID IID, Type *RetTy,
                                         ArrayRef<Value *> Args,
                                         FastMathFlags FMF) {
  return BaseT::getIntrinsicInstrCost(IID, RetTy, Args, FMF);
}

unsigned CSATTIImpl::getIntrinsicInstrCost(Intrinsic::ID IID, Type *RetTy,
                                         ArrayRef<Type *> Tys,
                                         FastMathFlags  FMF) {
  unsigned ISD = 0;
  switch (IID) {
  default: {
    // Assume that we need to scalarize this intrinsic.
    unsigned ScalarizationCost = 0;
    unsigned ScalarCalls = 1;
    if (RetTy->isVectorTy()) {
      ScalarizationCost = getScalarizationOverhead(RetTy, true, false);
      ScalarCalls = std::max(ScalarCalls, RetTy->getVectorNumElements());
    }
    for (unsigned i = 0, ie = Tys.size(); i != ie; ++i) {
      if (Tys[i]->isVectorTy()) {
        ScalarizationCost += getScalarizationOverhead(Tys[i], false, true);
        ScalarCalls = std::max(ScalarCalls, RetTy->getVectorNumElements());
      }
    }

    return ScalarCalls + ScalarizationCost;
  }
  // Look for intrinsics that can be lowered directly or turned into a scalar
  // intrinsic call.
  case Intrinsic::sqrt:    ISD = ISD::FSQRT;  break;
  case Intrinsic::sin:     ISD = ISD::FSIN;   break;
  case Intrinsic::cos:     ISD = ISD::FCOS;   break;
  case Intrinsic::exp:     ISD = ISD::FEXP;   break;
  case Intrinsic::exp2:    ISD = ISD::FEXP2;  break;
  case Intrinsic::log:     ISD = ISD::FLOG;   break;
  case Intrinsic::log10:   ISD = ISD::FLOG10; break;
  case Intrinsic::log2:    ISD = ISD::FLOG2;  break;
  case Intrinsic::fabs:    ISD = ISD::FABS;   break;
  case Intrinsic::minnum:  ISD = ISD::FMINNUM; break;
  case Intrinsic::maxnum:  ISD = ISD::FMAXNUM; break;
  case Intrinsic::copysign: ISD = ISD::FCOPYSIGN; break;
  case Intrinsic::floor:   ISD = ISD::FFLOOR; break;
  case Intrinsic::ceil:    ISD = ISD::FCEIL;  break;
  case Intrinsic::trunc:   ISD = ISD::FTRUNC; break;
  case Intrinsic::nearbyint:
                           ISD = ISD::FNEARBYINT; break;
  case Intrinsic::rint:    ISD = ISD::FRINT;  break;
  case Intrinsic::round:   ISD = ISD::FROUND; break;
  case Intrinsic::pow:     ISD = ISD::FPOW;   break;
  case Intrinsic::fma:     ISD = ISD::FMA;    break;
  case Intrinsic::fmuladd: ISD = ISD::FMA;    break;
  // FIXME: We should return 0 whenever getIntrinsicCost == TTI::TCC_Free.
  case Intrinsic::lifetime_start:
  case Intrinsic::lifetime_end:
    return 0;
  }

  const TargetLoweringBase *TLI = getTLI();
  std::pair<unsigned, MVT> LT = TLI->getTypeLegalizationCost(DL, RetTy);

  if (TLI->isOperationLegalOrPromote(ISD, LT.second)) {
    // The operation is legal. Assume it costs 1.
    // If the type is split to multiple registers, assume that there is some
    // overhead to this.
    // TODO: Once we have extract/insert subvector cost we need to use them.
    if (LT.first > 1)
      return LT.first * 2;
    return LT.first * 1;
  }

  if (!TLI->isOperationExpand(ISD, LT.second)) {
    // If the operation is custom lowered then assume
    // thare the code is twice as expensive.
    return LT.first * 2;
  }

  // If we can't lower fmuladd into an FMA estimate the cost as a floating
  // point mul followed by an add.
  if (IID == Intrinsic::fmuladd)
    return BaseT::getArithmeticInstrCost(BinaryOperator::FMul, RetTy) +
           BaseT::getArithmeticInstrCost(BinaryOperator::FAdd, RetTy);

  // Else, assume that we need to scalarize this intrinsic. For math builtins
  // this will emit a costly libcall, adding call overhead and spills. Make it
  // very expensive.
  if (RetTy->isVectorTy()) {
    unsigned Num = RetTy->getVectorNumElements();
    unsigned Cost = BaseT::getIntrinsicInstrCost(IID, RetTy->getScalarType(),
                                                  Tys, FMF);
    return 10 * Cost * Num;
  }

  // This is going to be turned into a library call, make it expensive.
  return 10;
}

unsigned CSATTIImpl::getNumberOfParts(Type *Tp) {
  std::pair<unsigned, MVT> LT = getTLI()->getTypeLegalizationCost(DL, Tp);
  return LT.first;
}

unsigned CSATTIImpl::getAddressComputationCost(Type *Ty, bool IsComplex) {
  return 0;
}

unsigned CSATTIImpl::getReductionCost(unsigned Opcode, Type *Ty,
                                    bool IsPairwise) {
  assert(Ty->isVectorTy() && "Expect a vector type");
  unsigned NumVecElts = Ty->getVectorNumElements();
  unsigned NumReduxLevels = Log2_32(NumVecElts);
  unsigned ArithCost = NumReduxLevels *
    BaseT::getArithmeticInstrCost(Opcode, Ty);
  // Assume the pairwise shuffles add a cost.
  unsigned ShuffleCost =
      NumReduxLevels * (IsPairwise + 1) *
      BaseT::getShuffleCost(TTI::SK_ExtractSubvector, Ty, NumVecElts / 2, Ty);
  return ShuffleCost + ArithCost + getScalarizationOverhead(Ty, false, true);
}

