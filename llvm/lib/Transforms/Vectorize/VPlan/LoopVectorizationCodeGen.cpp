//===------------------------------------------------------------*- C++ -*-===//
//
//   Copyright (C) 2015-2017 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation. and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//   Source file:
//   ------------
//   LoopVectorizationCodeGen.cpp -- LLVM IR Code generation from VPlan
//
//===----------------------------------------------------------------------===//

#include "LoopVectorizationCodeGen.h"
#include "llvm/Analysis/LoopAccessAnalysis.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/VectorUtils.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/Intel_IntrinsicUtils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include <tuple>

using namespace llvm;

#define DEBUG_TYPE "vpo-ir-loop-vectorize"

static bool isSupportedInstructionType(Type *Ty) {
  return !Ty->isVectorTy() || Ty->getVectorElementType()->isSingleValueType();
}

/// A helper function that returns value after skipping 'bitcast'.
static Value *getPtrThruBitCast(Value *Ptr) {
  while (isa<BitCastInst>(Ptr)) {
    Type *DestTy = Ptr->getType();
    Type *SrcTy = cast<BitCastInst>(Ptr)->getSrcTy();
    if (!isa<PointerType>(DestTy) || !isa<PointerType>(SrcTy))
      break;
    Type *Pointee1Ty = cast<PointerType>(DestTy)->getPointerElementType();
    Type *Pointee2Ty = cast<PointerType>(SrcTy)->getPointerElementType();
    const DataLayout &DL = cast<BitCastInst>(Ptr)->getModule()->getDataLayout();
    if (DL.getTypeSizeInBits(Pointee1Ty) != DL.getTypeSizeInBits(Pointee2Ty))
      break;
    Ptr = cast<BitCastInst>(Ptr)->getOperand(0);
  }
  return Ptr;
}

/// A helper function that returns GEP instruction and knows to skip a
/// 'bitcast'. The 'bitcast' may be skipped if the source and the destination
/// pointee types of the 'bitcast' have the same size.
/// For example:
///   bitcast double** %var to i64* - can be skipped
///   bitcast double** %var to i8*  - can not
static GetElementPtrInst *getGEPInstruction(Value *Ptr) {

  if (isa<GetElementPtrInst>(Ptr))
    return cast<GetElementPtrInst>(Ptr);
  return dyn_cast<GetElementPtrInst>(getPtrThruBitCast(Ptr));
}

/// A helper function that returns the pointer operand of a load or store
/// instruction.
static Value *getPointerOperand(Value *I) {
  if (auto *LI = dyn_cast<LoadInst>(I))
    return LI->getPointerOperand();
  if (auto *SI = dyn_cast<StoreInst>(I))
    return SI->getPointerOperand();
  return nullptr;
}

/// \brief Check that the instruction has outside loop users and is not an
/// identified reduction variable.
static bool hasOutsideLoopUser(const Loop *TheLoop, Instruction *Inst,
                               SmallPtrSetImpl<Value *> &AllowedExit) {
  // Reduction and Induction instructions are allowed to have exit users. All
  // other instructions must not have external users.
  if (!AllowedExit.count(Inst))
    // Check that all of the users of the loop are inside the BB.
    for (User *U : Inst->users()) {
      Instruction *UI = cast<Instruction>(U);
      // This user may be a reduction exit value.
      if (!TheLoop->contains(UI)) {
        DEBUG(dbgs() << "LV: Found an outside user for : " << *UI << '\n');
        return true;
      }
    }
  return false;
}

static bool isUsedInReductionScheme(PHINode *Phi,
   VPOVectorizationLegality::ExplicitReductionList& ReductionPhis) {
  return std::any_of(Phi->users().begin(), Phi->users().end(), [&](User* U) {
    return isa<PHINode>(U) && ReductionPhis.count(cast<PHINode>(U));
  });
}

/// Reduce vector \p Vec to a scalar value according to the
/// recurrence descriptor.
static Value *reduceVector(Value *Vec,
                           RecurrenceDescriptor::RecurrenceKind RK,
                           RecurrenceDescriptor::MinMaxRecurrenceKind MinMaxKind,
                           IRBuilder<>& Builder) {
  unsigned VF = Vec->getType()->getVectorNumElements();
  // Reduce all of the unrolled parts into a single vector.
  unsigned Op = RecurrenceDescriptor::getRecurrenceBinOp(RK);
  // VF is a power of 2 so we can emit the reduction using log2(VF) shuffles
  // and vector ops, reducing the set of values being computed by half each
  // round.
  assert(isPowerOf2_32(VF) &&
         "Reduction emission only supported for pow2 vectors!");
  SmallVector<Constant *, 32> ShuffleMask(VF, nullptr);
  for (unsigned i = VF; i != 1; i >>= 1) {
    // Move the upper half of the vector to the lower half.
    for (unsigned j = 0; j != i / 2; ++j)
      ShuffleMask[j] = Builder.getInt32(i / 2 + j);

    // Fill the rest of the mask with undef.
    std::fill(&ShuffleMask[i / 2], ShuffleMask.end(),
              UndefValue::get(Builder.getInt32Ty()));

    Value *Shuf = Builder.CreateShuffleVector(
      Vec, UndefValue::get(Vec->getType()),
      ConstantVector::get(ShuffleMask), "rdx.shuf");

    if (Op != Instruction::ICmp && Op != Instruction::FCmp)
      Vec = Builder.CreateBinOp((Instruction::BinaryOps)Op,
                                Vec, Shuf, "bin.rdx");
    else
      Vec = RecurrenceDescriptor::createMinMaxOp(Builder, MinMaxKind,
                                                 Vec, Shuf);
  }

  // The result is in the first element of the vector.
  return Builder.CreateExtractElement(Vec, Builder.getInt32(0));
}

static Value *joinVectors(ArrayRef<Value *> VectorsToJoin, IRBuilder<> &Builder,
                          Twine Name = "") {
  SmallVector<Value *, 8> VParts(VectorsToJoin.begin(), VectorsToJoin.end());
  unsigned VL = VParts.size();
  while (VL >= 2) {
    for (unsigned i = 0, j = 0; i < VL; i += 2, ++j) {
      unsigned NumElts = VParts[i]->getType()->getVectorNumElements();
      SmallVector<unsigned, 8> ShuffleMask(NumElts * 2);
      for (unsigned MaskInd = 0; MaskInd < NumElts * 2; ++MaskInd)
        ShuffleMask[MaskInd] = MaskInd;
      VParts[j] =
          Builder.CreateShuffleVector(VParts[i], VParts[i + 1], ShuffleMask);
    }
    VL /= 2;
  }
  VParts[0]->setName(Name);
  return VParts[0];
}

// {0, 1, 2, 3} -> { 0, 0, 1, 1, 2, 2, 3, 3}
static Value *replicateVectorElts(Value *OrigVal, unsigned Factor,
                                  IRBuilder<> &Builder,
                                  const Twine &Name = "") {
  if (Factor == 1)
    return OrigVal;
  unsigned NumElts = OrigVal->getType()->getVectorNumElements();
  SmallVector<unsigned, 8> ShuffleMask;
  for (unsigned i = 0; i < NumElts; ++i)
    for (unsigned j = 0; j < Factor; j++)
      ShuffleMask.push_back((signed)i);
  return Builder.CreateShuffleVector(OrigVal,
                                     UndefValue::get(OrigVal->getType()),
                                     ShuffleMask, Name + OrigVal->getName());
}

// {0, 1, 2, 3} -> { 0, 1, 2, 3, 0, 1, 2, 3}
static Value *replicateVector(Value *OrigVal, unsigned Factor,
                              IRBuilder<> &Builder, const Twine &Name = "") {
  if (Factor == 1)
    return OrigVal;
  unsigned NumElts = OrigVal->getType()->getVectorNumElements();
  SmallVector<unsigned, 8> ShuffleMask;
  for (unsigned j = 0; j < Factor; j++)
    for (unsigned i = 0; i < NumElts; ++i)
      ShuffleMask.push_back((signed)i);
  return Builder.CreateShuffleVector(OrigVal,
                                     UndefValue::get(OrigVal->getType()),
                                     ShuffleMask, Name + OrigVal->getName());
}

static bool checkCombinerOp(Value *CombinerV,
                            RecurrenceDescriptor::RecurrenceKind Kind) {
  switch (Kind) {
  case RecurrenceDescriptor::RK_FloatAdd:
    return isa<Instruction>(CombinerV) &&
      (cast<Instruction>(CombinerV)->getOpcode() == Instruction::FAdd ||
       cast<Instruction>(CombinerV)->getOpcode() == Instruction::FSub);
  case RecurrenceDescriptor::RK_IntegerAdd:
    return isa<Instruction>(CombinerV) &&
      (cast<Instruction>(CombinerV)->getOpcode() == Instruction::Add ||
       cast<Instruction>(CombinerV)->getOpcode() == Instruction::Sub);
  case RecurrenceDescriptor::RK_IntegerMult:
    return isa<Instruction>(CombinerV) &&
      cast<Instruction>(CombinerV)->getOpcode() == Instruction::Mul;
  case RecurrenceDescriptor::RK_FloatMult:
    return isa<Instruction>(CombinerV) &&
      cast<Instruction>(CombinerV)->getOpcode() == Instruction::FMul;
  case RecurrenceDescriptor::RK_IntegerAnd:
    return isa<Instruction>(CombinerV) &&
      cast<Instruction>(CombinerV)->getOpcode() == Instruction::And;
  case RecurrenceDescriptor::RK_IntegerOr:
    return isa<Instruction>(CombinerV) &&
      cast<Instruction>(CombinerV)->getOpcode() == Instruction::Or;
  case RecurrenceDescriptor::RK_IntegerXor:
    return isa<Instruction>(CombinerV) &&
      cast<Instruction>(CombinerV)->getOpcode() == Instruction::Xor;
  default:
    break;
  }
  return false;
}

/// The function collects Load and Store instruction that access the
/// reduction variable \p RedVarPtr.
static void collectAllRelevantUsers(Value *RedVarPtr,
                                    SmallVector<Value*, 4>& Users) {
  for (auto U : RedVarPtr->users()) {
    if (isa<LoadInst>(U) || isa<StoreInst>(U))
      Users.push_back(U);
    else if (isa<BitCastInst>(U)) {
      Value *Ptr = getPtrThruBitCast(RedVarPtr);
      if (Ptr && Ptr != RedVarPtr)
        for (auto U : Ptr->users())
          if (isa<LoadInst>(U) || isa<StoreInst>(U))
            Users.push_back(U);
    }
  }
}

/// Analyze reduction pattern for variable \p RedVarPtr and return true if we
/// have Phi nodes inside. If yes, return the Phi node in \p LoopHeaderPhiNode
/// and the initializer in \p StartV.
bool 
VPOVectorizationLegality::doesReductionUsePhiNodes(Value *RedVarPtr,
                                                   PHINode *& LoopHeaderPhiNode,
                                                   Value *& StartV) {
  auto usedInOnlyOnePhiNode = [](Value *V) {
    PHINode *Phi = nullptr;
    for (auto U : V->users())
      if (isa<PHINode>(U)) {
        if (Phi) // More than one Phi node
          return (PHINode *)nullptr;
        Phi = cast<PHINode>(U);
      }
    return Phi;
  };
  SmallVector<Value*, 4> Users;
  collectAllRelevantUsers(RedVarPtr, Users);
  for (auto U : Users)
    if (auto LI = dyn_cast<LoadInst>(U))
      if (TheLoop->isLoopInvariant(LI)) {   // Scenario (1)
        LoopHeaderPhiNode = usedInOnlyOnePhiNode(U);
        if (LoopHeaderPhiNode &&
            LoopHeaderPhiNode->getParent() == TheLoop->getHeader())
          StartV = LI;
      }
  return (StartV && LoopHeaderPhiNode);
}

/// Return true if the reduction variable \p RedVarPtr is stored inside the
/// loop. 
bool 
VPOVectorizationLegality::isReductionVarStoredInsideTheLoop(Value *RedVarPtr) {
  SmallVector<Value*, 4> Users;
  collectAllRelevantUsers(RedVarPtr, Users);
  // I assume that one load or one store being found inside loop is enough 
  // to say that we have them both. Since the reduction is explicit, deep 
  // analysis for a possible inconsistency is not required. 
  for (auto U : Users) {
    if (auto LI = dyn_cast<LoadInst>(U))
      if (!TheLoop->isLoopInvariant(LI))
        return true;
    if (auto SI = dyn_cast<StoreInst>(U))
      if (!TheLoop->isLoopInvariant(SI))
        return true;
  }
  return false;
}

void VPOVectorizationLegality::parseMinMaxReduction(
    AllocaInst *RedVarPtr,
    RecurrenceDescriptor::RecurrenceKind Kind,
    RecurrenceDescriptor::MinMaxRecurrenceKind Mrk) {

  // Analyzing 2 possible scenarios:
  // (1) 
  //  for.body:
  //  **REDUCTION PHI** -
  //  %LoopHeaderPhiNode = phi i32[%.pre, %PreHeader], [%MinMaxResultPhi, %for.inc]
  //  %cmp1 = icmp sgt i32 %LoopHeaderPhiNode, %Val
  //  br i1 %cmp1, label %if.then, label %for.inc
  //
  //  if.then:
  //   STORE i32 %Val, i32* %min, align 4
  //   br label %for.inc
  //
  //  for.inc:
  //   % MinMaxResultPhi = PHI i32[%Val, %if.then], [%Tmp, %for.body]
  //   ..
  //   br i1 %exitcond, label %for.end, label %for.body

  // (2)
  // 
  //  for.body:                                     
  //  ** NO REDUCTION PHI **
  //  %Current = LOAD i32, i32* %Min, align 4
  //  %cmp1 = icmp sgt i32 %Val, %Current
  //  br i1 %cmp1, label %if.then, label %for.inc
  //  if.then:
  //   STORE i32 %Val, i32* %min, align 4
  //   br label %for.inc
  //
  //  for.inc:
  //   NO PHI
  //   ..
  //   br i1 %exitcond, label %for.end, label %for.body

  PHINode *LoopHeaderPhiNode = nullptr;
  PHINode *MinMaxResultPhi = nullptr;
  Value *StartV = nullptr;
  if (doesReductionUsePhiNodes(RedVarPtr, LoopHeaderPhiNode, StartV)) {
    for (auto PnUser : LoopHeaderPhiNode->users())
      if (auto Phi = dyn_cast<PHINode>(PnUser))
        if (!TheLoop->isLoopInvariant(Phi))
          MinMaxResultPhi = Phi;
    SmallPtrSet<Instruction *, 4> CastInsts;
    RecurrenceDescriptor RD(StartV, MinMaxResultPhi, Kind, Mrk,
                            nullptr, StartV->getType(), true, CastInsts);
    ExplicitReductions[LoopHeaderPhiNode] = { RD, RedVarPtr };
  }
  InMemoryReductions[RedVarPtr] = { Kind, Mrk };
}

void VPOVectorizationLegality::parseBinOpReduction(
  AllocaInst *RedVarPtr,
  RecurrenceDescriptor::RecurrenceKind Kind) {

  // Analyzing 2 possible scenarios:
  // (1) -- Reduction Phi nodes, the new value is in reg
  // StartV = Load
  //  ** Inside loop body **
  //  **REDUCTION PHI** -
  // Current = phi (StartV, NewVal)
  // %NewVal = add nsw i32 %NextVal, %Current
  // eof loop
  // use %NewVal
  //
  // (2) -- The new value is always in memory
  // ** Inside loop body **
  // %Current = LOAD i32, i32* %Sum, align 4
  //  %NewVal = add nsw i32 %NextVal, %Current
  //  STORE i32 %NewVal, i32* %Sum, align 4
  // eof loop
  // load i32* %Sum

  Value *StartV = nullptr;
  PHINode *ReductionPhi = nullptr;
  bool UsePhi = false;
  bool UseMemory = false;
  if ((UsePhi = doesReductionUsePhiNodes(RedVarPtr, ReductionPhi, StartV))) {
    Value *CombinerV = (ReductionPhi->getIncomingValue(0) == StartV) ?
      ReductionPhi->getIncomingValue(1) : ReductionPhi->getIncomingValue(0);
    if (!checkCombinerOp(CombinerV, Kind)) {
      DEBUG(dbgs() << "LV: Combiner op does not match reduction type ");
      return;
    }
    Instruction *Combiner = cast<Instruction>(CombinerV);
    SmallPtrSet<Instruction *, 4> CastInsts;
    RecurrenceDescriptor RD(StartV, Combiner, Kind,
                            RecurrenceDescriptor::MRK_Invalid, nullptr,
                            ReductionPhi->getType(), true, CastInsts);
    ExplicitReductions[ReductionPhi] = { RD, cast<AllocaInst>(RedVarPtr) };
  }
  if ((UseMemory = isReductionVarStoredInsideTheLoop(RedVarPtr)))
    InMemoryReductions[RedVarPtr] = { Kind, RecurrenceDescriptor::MRK_Invalid };

  if (!UsePhi  && !UseMemory)
    DEBUG(dbgs() << "LV: Explicit reduction pattern is not recognized ");
}

void VPOVectorizationLegality::parseExplicitReduction(
    Value *RedVarPtr, RecurrenceDescriptor::RecurrenceKind Kind,
    RecurrenceDescriptor::MinMaxRecurrenceKind Mrk) {
  assert(isa<AllocaInst>(RedVarPtr) &&
         "Expected Alloca instruction as a pointer to reduction variable");

  if (Mrk != RecurrenceDescriptor::MRK_Invalid)
    return parseMinMaxReduction(cast<AllocaInst>(RedVarPtr), Kind, Mrk);

  return parseBinOpReduction(cast<AllocaInst>(RedVarPtr), Kind);
}

bool VPOVectorizationLegality::isExplicitReductionPhi(PHINode *Phi) {
  return ExplicitReductions.count(Phi);
}

void VPOVectorizationLegality::addReductionMult(Value *V) {
  if (V->getType()->getPointerElementType()->isIntegerTy())
    parseExplicitReduction(V, RecurrenceDescriptor::RK_IntegerMult);
  else
    parseExplicitReduction(V, RecurrenceDescriptor::RK_FloatMult);
}

void VPOVectorizationLegality::addReductionSum(Value *V) {
  if (V->getType()->getPointerElementType()->isIntegerTy())
    parseExplicitReduction(V, RecurrenceDescriptor::RK_IntegerAdd);
  else
    parseExplicitReduction(V, RecurrenceDescriptor::RK_FloatAdd);
}

void VPOVectorizationLegality::addReductionMin(Value *V, bool IsSigned) {
  if (V->getType()->getPointerElementType()->isIntegerTy()) {
    RecurrenceDescriptor::MinMaxRecurrenceKind Mrk =
      IsSigned ? RecurrenceDescriptor::MRK_SIntMin
      : RecurrenceDescriptor::MRK_UIntMin;
    parseExplicitReduction(V, RecurrenceDescriptor::RK_IntegerMinMax, Mrk);
  } else
    parseExplicitReduction(V, RecurrenceDescriptor::RK_FloatMinMax,
                           RecurrenceDescriptor::MRK_FloatMin);
}

void VPOVectorizationLegality::addReductionMax(Value *V, bool IsSigned) {
  if (V->getType()->getPointerElementType()->isIntegerTy()) {
    RecurrenceDescriptor::MinMaxRecurrenceKind Mrk =
      IsSigned ? RecurrenceDescriptor::MRK_SIntMax
      : RecurrenceDescriptor::MRK_UIntMax;
    parseExplicitReduction(V, RecurrenceDescriptor::RK_IntegerMinMax, Mrk);
  } else
    parseExplicitReduction(V, RecurrenceDescriptor::RK_FloatMinMax,
                           RecurrenceDescriptor::MRK_FloatMax);
}

bool VPOVectorizationLegality::canVectorize() {

  if (TheLoop->getNumBackEdges() != 1 || !TheLoop->getExitingBlock()) {
    DEBUG(dbgs() << "loop control flow is not understood by vectorizer");
    return false;
  }
  // We only handle bottom-tested loops, i.e. loop in which the condition is
  // checked at the end of each iteration. With that we can assume that all
  // instructions in the loop are executed the same number of times.
  if (TheLoop->getExitingBlock() != TheLoop->getLoopLatch()) {
    DEBUG(dbgs() << "loop control flow is not understood by vectorizer");
    return false;
  }
  // ScalarEvolution needs to be able to find the exit count.
  const SCEV *ExitCount = PSE.getBackedgeTakenCount();
  if (ExitCount == PSE.getSE()->getCouldNotCompute()) {
    DEBUG(dbgs() << "LV: SCEV could not compute the loop exit count.\n");
    return false;
  }

  BasicBlock *Header = TheLoop->getHeader();
  // For each block in the loop.
  for (BasicBlock *BB : TheLoop->blocks()) {
    // Scan the instructions in the block and look for hazards.
    for (Instruction &I : *BB) {
      if (!isSupportedInstructionType(I.getType()))
        return false;  
      if (auto *Phi = dyn_cast<PHINode>(&I)) {

        // If this PHINode is not in the header block, then we know that we
        // can convert it to select during if-conversion. No need to check if
        // the PHIs in this block are induction or reduction variables.
        if (BB != Header) {
          // Check that this instruction has no outside users or is an
          // identified reduction value with an outside user.
          if (!hasOutsideLoopUser(TheLoop, Phi, AllowedExit))
            continue;
          if (isUsedInReductionScheme(Phi, ExplicitReductions))
            continue;
          DEBUG(dbgs() << "LV: PHI value could not be identified as" <<
                " an induction or reduction \n");
          return false;
        }

        // We only allow if-converted PHIs with exactly two incoming values.
        if (Phi->getNumIncomingValues() != 2) {
          DEBUG(dbgs() << "LV: Found an invalid PHI.\n");
          return false;
        }
 
        if (isExplicitReductionPhi(Phi))
          continue;

        RecurrenceDescriptor RedDes;
        if (RecurrenceDescriptor::isReductionPHI(Phi, TheLoop, RedDes)) {
          AllowedExit.insert(RedDes.getLoopExitInstr());
          Reductions[Phi] = RedDes;
          continue;
        }

        InductionDescriptor ID;
        if (InductionDescriptor::isInductionPHI(Phi, TheLoop, PSE, ID)) {
          addInductionPhi(Phi, ID, AllowedExit);
          continue;
        }

        DEBUG(dbgs() << "LV: Found an unidentified PHI." << *Phi << "\n");
        return false;
      } // end of PHI handling
    }
  }
  if (!Induction && Inductions.empty()) {
    DEBUG(dbgs() << "LV: Did not find one integer induction var.\n");
    return false;
  }

  collectLoopUniformsForAnyVF();
  return true;
}

static Type *convertPointerToIntegerType(const DataLayout &DL, Type *Ty) {
  if (Ty->isPointerTy())
    return DL.getIntPtrType(Ty);

  // It is possible that char's or short's overflow when we ask for the loop's
  // trip count, work around this by changing the type size.
  if (Ty->getScalarSizeInBits() < 32)
    return Type::getInt32Ty(Ty->getContext());

  return Ty;
}

static Type *getWiderType(const DataLayout &DL, Type *Ty0, Type *Ty1) {
  Ty0 = convertPointerToIntegerType(DL, Ty0);
  Ty1 = convertPointerToIntegerType(DL, Ty1);
  if (Ty0->getScalarSizeInBits() > Ty1->getScalarSizeInBits())
    return Ty0;
  return Ty1;
}

void VPOVectorizationLegality::addInductionPhi(
    PHINode *Phi, const InductionDescriptor &ID,
    SmallPtrSetImpl<Value *> &AllowedExit) {

  Inductions[Phi] = ID;

  Type *PhiTy = Phi->getType();
  const DataLayout &DL = Phi->getModule()->getDataLayout();

  // Get the widest type.
  if (!PhiTy->isFloatingPointTy()) {
    if (!WidestIndTy)
      WidestIndTy = convertPointerToIntegerType(DL, PhiTy);
    else
      WidestIndTy = getWiderType(DL, PhiTy, WidestIndTy);
  }

  // Int inductions are special because we only allow one IV.
  if (ID.getKind() == InductionDescriptor::IK_IntInduction &&
      ID.getConstIntStepValue() && ID.getConstIntStepValue()->isOne() &&
      isa<Constant>(ID.getStartValue()) &&
      cast<Constant>(ID.getStartValue())->isNullValue()) {

    // Use the phi node with the widest type as induction. Use the last
    // one if there are multiple (no good reason for doing this other
    // than it is expedient). We've checked that it begins at zero and
    // steps by one, so this is a canonical induction variable.
    if (!Induction || PhiTy == WidestIndTy)
      Induction = Phi;
  }

  // Both the PHI node itself, and the "post-increment" value feeding
  // back into the PHI node may have external users.
  AllowedExit.insert(Phi);
  AllowedExit.insert(Phi->getIncomingValueForBlock(TheLoop->getLoopLatch()));

  DEBUG(dbgs() << "LV: Found an induction variable.\n");
  return;
}

static void addBlockToParentLoop(Loop *L, BasicBlock *BB, LoopInfo& LI) {
  if (auto *ParentLoop = L->getParentLoop())
    ParentLoop->addBasicBlockToLoop(BB, LI);
}

void VPOCodeGen::emitEndOfVectorLoop(Value *Count, Value *CountRoundDown) {
  // Add a check in the middle block to see if we have completed
  // all of the iterations in the first vector loop.
  // If (N - N%VF) == N, then we *don't* need to run the remainder.
  Value *CmpN = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_EQ, Count,
                                CountRoundDown, "cmp.n",
                                LoopMiddleBlock->getTerminator());
  ReplaceInstWithInst(LoopMiddleBlock->getTerminator(),
                      BranchInst::Create(LoopExitBlock, LoopScalarPreHeader,
                                         CmpN));
}

void VPOCodeGen::emitResume(Value *CountRoundDown) {
  // We are going to resume the execution of the scalar loop.
  // Go over all of the induction variables that we found and fix the
  // PHIs that are left in the scalar version of the loop.
  // The starting values of PHI nodes depend on the counter of the last
  // iteration in the vectorized loop.
  // If we come from a bypass edge then we need to start from the original
  // start value.

  // This variable saves the new starting index for the scalar loop. It is used
  // to test if there are any tail iterations left once the vector loop has
  // completed.
  VPOVectorizationLegality::InductionList *List = Legal->getInductionVars();
  for (auto &InductionEntry : *List) {
    PHINode *OrigPhi = InductionEntry.first;
    InductionDescriptor II = InductionEntry.second;

    // Create phi nodes to merge from the  backedge-taken check block.
    PHINode *BCResumeVal =
        PHINode::Create(OrigPhi->getType(), 3, "bc.resume.val",
                        LoopScalarPreHeader->getTerminator());
    Value *&EndValue = IVEndValues[OrigPhi];
    if (OrigPhi == Legal->getInduction()) {
      // We know what the end value is.
      EndValue = CountRoundDown;

    } else {
      IRBuilder<> B(LoopBypassBlocks.back()->getTerminator());
      Type *StepType = II.getStep()->getType();
      Instruction::CastOps CastOp =
          CastInst::getCastOpcode(CountRoundDown, true, StepType, true);
      Value *CRD = B.CreateCast(CastOp, CountRoundDown, StepType, "cast.crd");
      const DataLayout &DL =
          OrigLoop->getHeader()->getModule()->getDataLayout();
      PredicatedScalarEvolution &PSE = Legal->getPSE();
      EndValue = II.transform(B, CRD, PSE.getSE(), DL);
      EndValue->setName("ind.end");
    }

    // The new PHI merges the original incoming value, in case of a bypass,
    // or the value at the end of the vectorized loop.
    BCResumeVal->addIncoming(EndValue, LoopMiddleBlock);

    // Fix the scalar body counter (PHI node).
    unsigned BlockIdx = OrigPhi->getBasicBlockIndex(LoopScalarPreHeader);

    // The old induction's phi node in the scalar body needs the truncated
    // value.
    for (BasicBlock *BB : LoopBypassBlocks)
      BCResumeVal->addIncoming(II.getStartValue(), BB);
    OrigPhi->setIncomingValue(BlockIdx, BCResumeVal);
  }
}

void VPOCodeGen::emitMinimumIterationCountCheck(Loop *L, Value *Count) {

  BasicBlock *VLoopFirstBB = L->getLoopPreheader();
  IRBuilder<> Builder(VLoopFirstBB->getTerminator());

  // Generate code to check that the loop's trip count that we computed by
  // adding one to the backedge-taken count will not overflow.
  Value *CheckMinIters = Builder.CreateICmpULT(
      Count, ConstantInt::get(Count->getType(), VF), "min.iters.check");

  BasicBlock *NewBB =
    VLoopFirstBB->splitBasicBlock(VLoopFirstBB->getTerminator(),
                                 "min.iters.checked");
  // Update dominator tree immediately if the generated block is a
  // LoopBypassBlock because SCEV expansions to generate loop bypass
  // checks may query it before the current function is finished.
  DT->addNewBlock(NewBB, VLoopFirstBB);
  addBlockToParentLoop(L, NewBB, *LI);

  BranchInst *Branch = BranchInst::Create(LoopScalarPreHeader, NewBB,
                                          CheckMinIters);
  ReplaceInstWithInst(VLoopFirstBB->getTerminator(), Branch);
                      
  LoopBypassBlocks.push_back(VLoopFirstBB);
}

void VPOCodeGen::emitVectorLoopEnteredCheck(Loop *L, BasicBlock *Bypass) {
  Value *TC = getOrCreateVectorTripCount(L);
  BasicBlock *BB = L->getLoopPreheader();
  IRBuilder<> Builder(BB->getTerminator());

  // Now, compare the new count to zero. If it is zero skip the vector loop and
  // jump to the scalar loop.
  Value *Cmp = Builder.CreateICmpEQ(TC, Constant::getNullValue(TC->getType()),
                                    "cmp.zero");

  // Generate code to check that the loop's trip count that we computed by
  // adding one to the backedge-taken count will not overflow.
  BasicBlock *NewBB = BB->splitBasicBlock(BB->getTerminator(), "vector.ph");
  // Update dominator tree immediately if the generated block is a
  // LoopBypassBlock because SCEV expansions to generate loop bypass
  // checks may query it before the current function is finished.
  DT->addNewBlock(NewBB, BB);
  addBlockToParentLoop(L, NewBB, *LI);
  ReplaceInstWithInst(BB->getTerminator(),
                      BranchInst::Create(Bypass, NewBB, Cmp));
  LoopBypassBlocks.push_back(BB);
}

void VPOCodeGen::initLinears(PHINode *Induction, Loop *VecLoop) {
  // The first value of the Induction PHINode is the initial loop index and
  // the second value is the index value from the loop latch.
  auto NextIndex = Induction->getIncomingValueForBlock(VecLoop->getLoopLatch());

  // Add the linear inupdate for the next vector iteration of the loop before the
  // the loop latch terminator.
  IRBuilder<> Builder(cast<Instruction>(NextIndex)->getParent()->getTerminator());

  for (auto It : *(Legal->getLinears())) {
    Value *LinPtr = It.first;
    int LinStep = It.second;

    // Load the initial value of the linears at the end of the loop preheader
    auto CurrIP = Builder.saveIP();
    Builder.SetInsertPoint(LoopVectorPreHeader->getTerminator());
    auto LinInitVal = Builder.CreateLoad(LinPtr);
    Builder.restoreIP(CurrIP);

    // Cast NextIndex to LinValType
    auto LinValType = LinInitVal->getType();
    Instruction::CastOps CastOp =
      CastInst::getCastOpcode(NextIndex, true, LinValType, true);
    auto ConvIndex = Builder.CreateCast(CastOp, NextIndex, LinValType, "lin.cast");

    // Linear value increment is NextIndex * LinStep
    Value *LinIncr;
    if (LinStep != 1) {
      auto LinStepVal = ConstantInt::get(LinValType, LinStep); 
      LinIncr = Builder.CreateMul(ConvIndex, LinStepVal);
    }
    else
      LinIncr = ConvIndex;

    Value *ValToStore = Builder.CreateAdd(LinInitVal, LinIncr);
    Builder.CreateStore(ValToStore, LinPtr);
  }
}

PHINode *VPOCodeGen::createInductionVariable(Loop *L, Value *Start, Value *End,
                                             Value *Step) {
  BasicBlock *Header = L->getHeader();
  BasicBlock *Latch = L->getLoopLatch();
  // As we're just creating this loop, it's possible no latch exists
  // yet. If so, use the header as this will be a single block loop.
  if (!Latch)
    Latch = Header;

  IRBuilder<> Builder(&*Header->getFirstInsertionPt());
  auto *Induction = Builder.CreatePHI(Start->getType(), 2, "index");

  Builder.SetInsertPoint(Latch->getTerminator());

  // Create i+1 and fill the PHINode.
  Value *Next = Builder.CreateAdd(Induction, Step, "index.next");
  Induction->addIncoming(Start, L->getLoopPreheader());
  Induction->addIncoming(Next, Latch);
  // Create the compare.
  Value *ICmp = Builder.CreateICmpEQ(Next, End);
  Builder.CreateCondBr(ICmp, L->getExitBlock(), Header);

  // Now we have two terminators. Remove the old one from the block.
  Latch->getTerminator()->eraseFromParent();

  return Induction;
}

void VPOCodeGen::createEmptyLoop() {

  LoopScalarBody = OrigLoop->getHeader();
  BasicBlock *LoopPreHeader = OrigLoop->getLoopPreheader();
  LoopExitBlock = OrigLoop->getExitBlock();

  assert(LoopPreHeader && "Must have loop preheader");
  assert(LoopExitBlock && "Must have an exit block");

  // Create vector loop body.
  LoopVectorBody =
    LoopPreHeader->splitBasicBlock(LoopPreHeader->getTerminator(),
                                   "vector.body");

  // Middle block comes after vector loop is done. It contains reduction tail
  // and checks if we need a scalar remainder.
  LoopMiddleBlock =
      LoopVectorBody->splitBasicBlock(LoopVectorBody->getTerminator(),
                                      "middle.block");
  
  // Scalar preheader contains phi nodes with incoming from vector version and
  // vector loop bypass blocks.
  LoopScalarPreHeader =
    LoopMiddleBlock->splitBasicBlock(LoopMiddleBlock->getTerminator(),
                                     "scalar.ph");

  Loop *Lp = new Loop();

  // Initialize NewLoop member
  NewLoop = Lp;

  Loop *ParentLoop = OrigLoop->getParentLoop();

  // Insert the new loop into the loop nest and register the new basic blocks
  // before calling any utilities such as SCEV that require valid LoopInfo.
  if (ParentLoop) {
    ParentLoop->addChildLoop(Lp);
    ParentLoop->addBasicBlockToLoop(LoopScalarPreHeader, *LI);
    ParentLoop->addBasicBlockToLoop(LoopMiddleBlock, *LI);
  } else
    LI->addTopLevelLoop(Lp);

  Lp->addBasicBlockToLoop(LoopVectorBody, *LI);

  // Find the loop boundaries.
  Value *Count = getOrCreateTripCount(Lp);

  emitMinimumIterationCountCheck(Lp, Count);

  // Now, compare the new count to zero. If it is zero skip the vector loop and
  // jump to the scalar loop.
  emitVectorLoopEnteredCheck(Lp, LoopScalarPreHeader);

  // CountRoundDown is a counter for the vectorized loop.
  // CountRoundDown = Count - Count % VF.
  Value *CountRoundDown = getOrCreateVectorTripCount(Lp);
  
  Type *IdxTy = Legal->getWidestInductionType();
  Value *StartIdx = ConstantInt::get(IdxTy, 0);
  Constant *Step = ConstantInt::get(IdxTy, VF);

  // Create an induction variable in vector loop with a step equal to VF.
  Induction = createInductionVariable(Lp, StartIdx, CountRoundDown, Step);

  // Add a check in the middle block to see if we have completed
  // all of the iterations in the first vector loop.
  // If (N - N%VF) == N, then we *don't* need to run the remainder.
  emitEndOfVectorLoop(Count, CountRoundDown);

  // Resume from vector loop. If vector loop was executed, the remainder
  // is Count - CountRoundDown. Otherwise the remainder is Count.
  emitResume(CountRoundDown);

  // Inform SCEV analysis to forget original loop
  PSE.getSE()->forgetLoop(OrigLoop);

  // Save the state.
  LoopVectorPreHeader = Lp->getLoopPreheader();

  // Initialize loop linears
  initLinears(Induction, Lp);

  // Get ready to start creating new instructions into the vector preheader.
  Builder.SetInsertPoint(&*LoopVectorPreHeader->getFirstInsertionPt());
}

void VPOCodeGen::finalizeLoop() {

  // Should come before fixCrossIterationPHIs().
  completeInMemoryReductions();

  fixCrossIterationPHIs();

  fixNonInductionPhis();

  updateAnalysis();

  // Fix-up external users of the induction variables.
  for (auto &Entry : *Legal->getInductionVars())
    fixupIVUsers(Entry.first, Entry.second,
                 getOrCreateVectorTripCount(LI->getLoopFor(LoopVectorBody)),
                 IVEndValues[Entry.first], LoopMiddleBlock);

  fixLCSSAPHIs();

  fixupLoopPrivates();

  predicateInstructions();
}

void VPOCodeGen::fixCrossIterationPHIs() {
  // In order to support recurrences we need to be able to vectorize Phi nodes.
  // Phi nodes have cycles, so we need to vectorize them in two stages. First,
  // we create a new vector PHI node with no incoming edges. We use this value
  // when we vectorize all of the instructions that use the PHI. Next, after
  // all of the instructions in the block are complete we add the new incoming
  // edges to the PHI. At this point all of the instructions in the basic block
  // are vectorized, so we can use them to construct the PHI.

  // At this point every instruction in the original loop is widened to a
  // vector form. Now we need to fix the recurrences. These PHI nodes are
  // currently empty because we did not want to introduce cycles.
  // This is the second stage of vectorizing recurrences.
  for (Instruction &I : *OrigLoop->getHeader()) {
    PHINode *Phi = dyn_cast<PHINode>(&I);
    if (!Phi)
      break;
    // Handle first-order recurrences and reductions that need to be fixed.
    // TODO: handle first-order recurrences
    if (Legal->isImplicitReductionVariable(Phi))
      fixReductionInReg(Phi, (*Legal->getReductionVars())[Phi]);

    else if (Legal->isExplicitReductionVariable(Phi)) {
      AllocaInst *Ptr = Legal->getReductionPtrByPhi(Phi);
      RecurrenceDescriptor& RD = Legal->getRecurrenceDescrByPhi(Phi);
      if (!Legal->isInMemoryReduction(Ptr))
        fixReductionInReg(Phi, RD);
      else {
        assert(ReductionVecInitVal.count(Ptr) && 
               ReductionEofLoopVal.count(Ptr) &&
               "Reduction is not handled properly");
        
        Value *VectorStart = ReductionVecInitVal[Ptr];
        fixReductionPhi(Phi, VectorStart);
        mergeReductionControlFlow(Phi, RD, ReductionEofLoopVal[Ptr]);
        Value *LoopExitInst = RD.getLoopExitInstr();
        fixReductionLCSSA(LoopExitInst, ReductionEofLoopVal[Ptr]);
      }
    }
      
  }
}

void VPOCodeGen::fixReductionPhi(PHINode *Phi, Value *VectorStart) {
  Value *VecRdxPhi = getVectorValue(Phi);
  BasicBlock *Latch = OrigLoop->getLoopLatch();
  Value *LoopVal = Phi->getIncomingValueForBlock(Latch);
  Value *VecLoopVal = getVectorValue(LoopVal);
  cast<PHINode>(VecRdxPhi)->addIncoming(VectorStart, LoopVectorPreHeader);
  cast<PHINode>(VecRdxPhi)
    ->addIncoming(VecLoopVal, LI->getLoopFor(LoopVectorBody)->getLoopLatch());
}

void VPOCodeGen::mergeReductionControlFlow(PHINode *Phi,
                                           RecurrenceDescriptor& RdxDesc,
                                           Value *ReducedPartRdx) {
  Value *ReductionStartValue = RdxDesc.getRecurrenceStartValue();
  // Create a phi node that merges control-flow from the backedge-taken check
  // block and the middle block.
  PHINode *BCBlockPhi = PHINode::Create(ReductionStartValue->getType(), 2,
                                        "bc.merge.rdx",
                                        LoopScalarPreHeader->getTerminator());
  for (unsigned I = 0, E = LoopBypassBlocks.size(); I != E; ++I)
    BCBlockPhi->addIncoming(ReductionStartValue, LoopBypassBlocks[I]);
  BCBlockPhi->addIncoming(ReducedPartRdx, LoopMiddleBlock);

  // Fix the scalar loop reduction variable with the incoming reduction sum
  // from the vector body and from the backedge value.
  int IncomingEdgeBlockIdx = Phi->getBasicBlockIndex(OrigLoop->getLoopLatch());
  assert(IncomingEdgeBlockIdx >= 0 && "Invalid block index");
  // Pick the other block.
  int SelfEdgeBlockIdx = (IncomingEdgeBlockIdx ? 0 : 1);
  Phi->setIncomingValue(SelfEdgeBlockIdx, BCBlockPhi);
  Value *LoopExitInst = RdxDesc.getLoopExitInstr();
  Phi->setIncomingValue(IncomingEdgeBlockIdx, LoopExitInst);
}

void VPOCodeGen::fixReductionLCSSA(Value *LoopExitInst, Value *NewV) {
  // Now, we need to fix the users of the reduction variable
  // inside and outside of the scalar remainder loop.
  // We know that the loop is in LCSSA form. We need to update the
  // PHI nodes in the exit blocks.
  for (BasicBlock::iterator LEI = LoopExitBlock->begin(),
       LEE = LoopExitBlock->end();
       LEI != LEE; ++LEI) {
    PHINode *LCSSAPhi = dyn_cast<PHINode>(LEI);
    if (!LCSSAPhi)
      break;

    // All PHINodes need to have a single entry edge, or two if
    // we already fixed them.
    assert(LCSSAPhi->getNumIncomingValues() < 3 && "Invalid LCSSA PHI");

    // We found our reduction value exit-PHI. Update it with the
    // incoming bypass edge.
    if (LCSSAPhi->getIncomingValue(0) == LoopExitInst) {
      // Add an edge coming from the bypass.
      LCSSAPhi->addIncoming(NewV, LoopMiddleBlock);
      break;
    }
  } // end of the LCSSA phi scan.
}

void VPOCodeGen::fixReductionInReg(PHINode *Phi,
                                   RecurrenceDescriptor& RdxDesc) {
  Constant *Zero = Builder.getInt32(0);

  RecurrenceDescriptor::RecurrenceKind RK = RdxDesc.getRecurrenceKind();
  TrackingVH<Value> ReductionStartValue = RdxDesc.getRecurrenceStartValue();
  Instruction *LoopExitInst = RdxDesc.getLoopExitInstr();
  RecurrenceDescriptor::MinMaxRecurrenceKind MinMaxKind =
    RdxDesc.getMinMaxRecurrenceKind();

  // We need to generate a reduction vector from the incoming scalar.
  // To do so, we need to generate the 'identity' vector and override
  // one of the elements with the incoming scalar reduction. We need
  // to do it in the vector-loop preheader.
  Builder.SetInsertPoint(LoopBypassBlocks[1]->getTerminator());

  // This is the vector-clone of the value that leaves the loop.
  Value *VecExit = getVectorValue(LoopExitInst);
  Type *VecTy = VecExit->getType();

  // Find the reduction identity variable. Zero for addition, or, xor,
  // one for multiplication, -1 for And.
  Value *Identity;
  Value *VectorStart;
  if (RK == RecurrenceDescriptor::RK_IntegerMinMax ||
      RK == RecurrenceDescriptor::RK_FloatMinMax) {
    // MinMax reduction have the start value as their identify.
    VectorStart = Identity =
        Builder.CreateVectorSplat(VF, ReductionStartValue, "minmax.ident");
  } else {
    // Handle other reduction kinds:
    Constant *Iden =
      RecurrenceDescriptor::getRecurrenceIdentity(RK, VecTy->getScalarType());
    Identity = ConstantVector::getSplat(VF, Iden);

    // This vector is the Identity vector where the first element is the
    // incoming scalar reduction.
    VectorStart =
      Builder.CreateInsertElement(Identity, ReductionStartValue, Zero);
  }

  // Fix the vector-loop phi.
  fixReductionPhi(Phi, VectorStart);

  // Before each round, move the insertion point right between
  // the PHIs and the values we are going to write.
  // This allows us to write both PHINodes and the extractelement
  // instructions.
  Builder.SetInsertPoint(&*LoopMiddleBlock->getFirstInsertionPt());

  Value *ReducedPartRdx = reduceVector(VecExit, RK, MinMaxKind, Builder);
  
  // Create a phi node that merges control-flow from the backedge-taken check
  // block and the middle block.
  mergeReductionControlFlow(Phi, RdxDesc, ReducedPartRdx);

  // Now, we need to fix the users of the reduction variable
  // inside and outside of the scalar remainder loop.
  // We know that the loop is in LCSSA form. We need to update the
  // PHI nodes in the exit blocks.
  fixReductionLCSSA(LoopExitInst, ReducedPartRdx);

}

void VPOCodeGen::updateAnalysis() {
  // Forget the original basic block.
  PSE.getSE()->forgetLoop(OrigLoop);

  // Update the dominator tree information.
  assert(DT->properlyDominates(LoopBypassBlocks.front(), LoopExitBlock) &&
         "Entry does not dominate exit.");

  if (!DT->getNode(LoopVectorBody))
    DT->addNewBlock(LoopVectorBody, LoopVectorPreHeader);

  DT->addNewBlock(LoopMiddleBlock, LoopVectorBody);
  DT->addNewBlock(LoopScalarPreHeader, LoopBypassBlocks[0]);
  DT->changeImmediateDominator(LoopScalarBody, LoopScalarPreHeader);
  DT->changeImmediateDominator(LoopExitBlock, LoopBypassBlocks[0]);

  //DEBUG(DT->verifyDomTree());
}

Value *VPOCodeGen::getBroadcastInstrs(Value *V) {
  // We need to place the broadcast of invariant variables outside the loop.
  Instruction *Instr = dyn_cast<Instruction>(V);
  bool NewInstr = (Instr && NewLoop->contains(Instr));
  bool Invariant = OrigLoop->isLoopInvariant(V) && !NewInstr;

  auto OldIP = Builder.saveIP();
  // Place the code for broadcasting invariant variables in the new preheader.
  IRBuilder<>::InsertPointGuard Guard(Builder);
  if (Invariant)
    Builder.SetInsertPoint(LoopVectorPreHeader->getTerminator());

  // Broadcast the scalar into all locations in the vector.
  Value *Shuf = Builder.CreateVectorSplat(VF, V, "broadcast");

  Builder.restoreIP(OldIP);
  return Shuf;
}

Value *VPOCodeGen::getVectorPrivatePtrs(Value *ScalarPrivate) {
  assert(Legal->isLoopPrivate(ScalarPrivate) && "Loop private value expected");

  if (WidenMap.count(ScalarPrivate))
    return WidenMap[ScalarPrivate];

  auto PtrToVec = getVectorPrivateBase(ScalarPrivate);
  auto PtrType = cast<PointerType>(ScalarPrivate->getType());
  auto Base = Builder.CreateBitCast(PtrToVec, PtrType, "privaddr");
  // We will create a vector GEP with scalar base and a vector of indices.
  SmallVector<Constant *, 8> Indices;
  // Create a vector of consecutive numbers from zero to VF.
  for (unsigned i = 0; i < VF; ++i) {
    Indices.push_back(
        ConstantInt::get(Type::getInt32Ty(PtrType->getContext()), i));
  }
  // Add the consecutive indices to the vector value.
  Constant *Cv = ConstantVector::get(Indices);

  return Builder.CreateGEP(nullptr, Base, Cv);
}

Value *VPOCodeGen::getVectorPrivateBase(Value *V) {
  assert(Legal->isLoopPrivate(V) && "Loop private value expected");
  bool IsConditional = Legal->isCondLastPrivate(V);

  Type *TypeBeforeBitcast = V->getType();
  Type *ValueTy = TypeBeforeBitcast->getPointerElementType();
  Type *NewValueTy = ValueTy->isVectorTy()
                         ? VectorType::get(ValueTy->getScalarType(),
                                           ValueTy->getVectorNumElements() * VF)
                         : VectorType::get(ValueTy, VF);

  Type *NewType = PointerType::get(NewValueTy, 0);

  V = getPtrThruBitCast(V);

  if (LoopPrivateWidenMap.count(V)) {
    Value *PtrToVec = LoopPrivateWidenMap[V];
    return Builder.CreateBitCast(PtrToVec, NewType);
  }

  // If V is an alloca ptr for a loop private, alloca a VF wide vector and
  // use this alloca'd ptr as the vector value.
  auto OldIP = Builder.saveIP();
  auto OrigAllocaTy = V->getType()->getPointerElementType();
  auto VecTyForAlloca =
      OrigAllocaTy->isVectorTy()
          ? VectorType::get(OrigAllocaTy->getScalarType(),
                            OrigAllocaTy->getVectorNumElements() * VF)
          : VectorType::get(OrigAllocaTy, VF);
  Builder.SetInsertPoint((cast<Instruction>(V))->getNextNode());
  Value *PtrToVec =
      Builder.CreateAlloca(VecTyForAlloca, nullptr, V->getName() + ".vec");

  // Save alloca's result
  LoopPrivateWidenMap[V] = PtrToVec;

  Builder.SetInsertPoint(LoopVectorPreHeader->getTerminator());
  // Broadcast the initial value through the vector (for conditional LP only)
  if (IsConditional) {
    LoadInst *LoadInit = Builder.CreateLoad(V, V->getName() + "InitVal");

    if (ValueTy->isVectorTy()) {
      // Store the initial value in the transposed form:
      // { { x.0, x.0, x.0, x.0 }, { x.1, x.1, x.1, x.1 }, .. }
      Type *PtrToScalarTy = PointerType::get(ValueTy->getScalarType(), 0);
      Value *PtrToFirstEltInVec = Builder.CreateBitCast(
          PtrToVec, PtrToScalarTy, "PtrToFirstEltInPrivateVec");
      for (unsigned i = 0; i < ValueTy->getVectorNumElements(); ++i) {
        Value *DataElt = Builder.CreateExtractElement(LoadInit, i);
        Type *IdxTy = Type::getInt32Ty(DataElt->getContext());
        Value *PtrToSubVec =
            (i == 0) ? PtrToFirstEltInVec
                     : Builder.CreateGEP(PtrToFirstEltInVec,
                                         ConstantInt::get(IdxTy, i * VF),
                                         "PtrToFirstEltInNextLane");
        PtrToSubVec = Builder.CreateBitCast(
            PtrToSubVec,
            PointerType::get(VectorType::get(ValueTy->getScalarType(), VF), 0),
            "PtrToNextLane");
        Value *InitVec =
            Builder.CreateVectorSplat(VF, DataElt, V->getName() + "InitVec");
        Builder.CreateStore(InitVec, PtrToSubVec);
      }
    } else {
      Value *InitVec =
          Builder.CreateVectorSplat(VF, LoadInit, V->getName() + "InitVec");
      Builder.CreateStore(InitVec, PtrToVec);
    }

    Builder.SetInsertPoint((cast<Instruction>(PtrToVec))->getNextNode());
    // Create a memory location for last non-zero mask
    // We save mask as an integer value
    Type *MaskTy = IntegerType::get(V->getContext(), VF);
    Value *PtrToMask =
        Builder.CreateAlloca(MaskTy, nullptr, V->getName() + ".mask");
    Builder.CreateStore(Constant::getAllOnesValue(MaskTy), PtrToMask);
    LoopPrivateLastMask[V] = PtrToMask;
  }
  // Spread the initial value over the vector for in-memory reduction as well.
  Builder.SetInsertPoint(LoopVectorPreHeader->getTerminator());
  if (Legal->isInMemoryReduction(V)) {
    LoadInst *LoadInit = Builder.CreateLoad(V, V->getName() + "InitVal");
    Value *InitVec;
      RecurrenceDescriptor::RecurrenceKind RK =
          (*Legal->getInMemoryReductionVars())[cast<AllocaInst>(V)].first;
      if (RK == RecurrenceDescriptor::RK_IntegerMinMax ||
          RK == RecurrenceDescriptor::RK_FloatMinMax) {
        InitVec = Builder.CreateVectorSplat(VF, LoadInit,
                                            V->getName() + "InitVec");
      } else {
        Constant *Iden = RecurrenceDescriptor::getRecurrenceIdentity(
            RK, OrigAllocaTy->getScalarType());
        InitVec = ConstantVector::getSplat(VF, Iden);
        Constant *Zero = Builder.getInt32(0);
        InitVec = Builder.CreateInsertElement(InitVec, LoadInit, Zero);
      }
      Builder.CreateStore(InitVec, PtrToVec);
      ReductionVecInitVal[cast<AllocaInst>(V)] = InitVec;
  }

  PtrToVec = Builder.CreateBitCast(PtrToVec, NewType);

  Builder.restoreIP(OldIP);
  return PtrToVec;
}

void VPOCodeGen::vectorizeBitCast(Instruction *Inst) {
  // Do not vectorize bitcast of loop-private if
  // it is used in load/store only
  Type *VecTy = VectorType::get(Inst->getType(), VF);
  if (Legal->isLoopPrivate(Inst) && all_of(Inst->users(), [&](User *U) -> bool {
        return getPointerOperand(U) == Inst;
      }))
    return;
  Value *A = getVectorValue(Inst->getOperand(0));
  WidenMap[Inst] = Builder.CreateBitCast(A, VecTy);
}

Value *VPOCodeGen::getVectorValue(Value *V) {

  // If we have this scalar in the map, return it.
  if (WidenMap.count(V))
    return WidenMap[V];

  // Address of in memory private is needed. Construct a vector of addresses
  // on the fly.
  if (Legal->isLoopPrivate(V)) {
    Value *VectorValue = getVectorPrivatePtrs(V);
    WidenMap[V] = VectorValue;
    return VectorValue;
  }

  // If the value has not been vectorized, check if it has been scalarized
  // instead. If it has been scalarized, and we actually need the value in
  // vector form, we will construct the vector values on demand.
  if (ScalarMap.count(V)) {
    bool IsUniform = isUniformAfterVectorization(cast<Instruction>(V), VF) ||
      OrigLoop->hasLoopInvariantOperands(cast<Instruction>(V));

    Value *VectorValue = nullptr;
    if (IsUniform) {
      Value *ScalarValue = ScalarMap[V][0];
      if (ScalarValue->getType()->isVectorTy()) {
        VectorValue =
            replicateVectorElts(ScalarValue, VF, Builder,
                                "replicatedVal." + ScalarValue->getName());
      } else
        VectorValue = Builder.CreateVectorSplat(VF, ScalarValue, "broadcast");
    } else if (V->getType()->isVectorTy()) {
      SmallVector<Value *, 8> Parts;
      for (unsigned Lane = 0; Lane < VF; ++Lane)
        Parts.push_back(ScalarMap[V][Lane]);
      VectorValue = joinVectors(Parts, Builder);
    } else {
      VectorValue = UndefValue::get(VectorType::get(V->getType(), VF));
      for (unsigned Lane = 0; Lane < VF; ++Lane) {
        Value *ScalarValue = ScalarMap[V][Lane];
        VectorValue = Builder.CreateInsertElement(VectorValue, ScalarValue,
                                                  Builder.getInt32(Lane));
      }
    }

    WidenMap[V] = VectorValue;
    return VectorValue;
  }

  // If this scalar is unknown, assume that it is a constant or that it is
  // loop invariant. Broadcast V and save the value for future uses.
  if (V->getType()->isVectorTy()) {
    assert(V->getType()->getVectorElementType()->isSingleValueType() &&
           "Re-vectorization is supported for simple vectors only");
    WidenMap[V] =
        replicateVectorElts(V, VF, Builder, "replicatedVal." + V->getName());
  } else
    WidenMap[V] = getBroadcastInstrs(V);

  return WidenMap[V];
}

Value *VPOCodeGen::getScalarValue(Value *V, unsigned Lane) {
  // If the value is not an instruction contained in the loop, it should
  // already be scalar.
  if (OrigLoop->isLoopInvariant(V) && !Legal->isLoopPrivate(V))
    return V;

  if (ScalarMap.count(V)) {
    auto SV = ScalarMap[V];
    if (SV.count(Lane))
      return SV[Lane];
  }

  Value *VecV = getVectorValue(V);
  auto ScalarV = Builder.CreateExtractElement(VecV, Builder.getInt32(Lane));

  // Add to scalar map
  ScalarMap[V][Lane] = ScalarV;
  return ScalarV;
}

Value *VPOCodeGen::reverseVector(Value *Vec, unsigned Stride) {
  unsigned NumElts = Vec->getType()->getVectorNumElements();
  SmallVector<Constant *, 8> ShuffleMask;
  for (unsigned i = 0; i < NumElts; i += Stride)
    for (unsigned j = 0; j < Stride; j++)
      ShuffleMask.push_back(Builder.getInt32(NumElts - (i + 1) * Stride + j));

  return Builder.CreateShuffleVector(Vec, UndefValue::get(Vec->getType()),
                                     ConstantVector::get(ShuffleMask),
                                     "reverse");
}

// Transpose < A0, B0, A1, B1, A2, B2, A3, B3 >. In this case Factor = 2.
// The result will be < A0, A1, A2, A3, B0, B1, B2, B3>
static Value *transposeVector(Value *Vec, unsigned Factor,
                              IRBuilder<>& Builder) {
  unsigned NumElts = Vec->getType()->getVectorNumElements();
  SmallVector<unsigned, 8> ShuffleMask;
  for (unsigned j = 0; j < Factor; j++)
    for (unsigned i = 0; i < NumElts; i += Factor)
      ShuffleMask.push_back((signed)(i + j));
  return Builder.CreateShuffleVector(Vec, UndefValue::get(Vec->getType()),
                                     ShuffleMask,
                                     "transposed." + Vec->getName());
}

// Revert the transpose.
static Value *normalizeVector(Value *Vec, unsigned Factor,
                              IRBuilder<> &Builder) {
  unsigned NumElts = Vec->getType()->getVectorNumElements();
  SmallVector<unsigned, 8> ShuffleMask;
  unsigned Lane = NumElts / Factor;
  for (unsigned j = 0; j < Lane; j++)
    for (unsigned i = 0; i < NumElts; i += Lane)
      ShuffleMask.push_back((signed)(i + j));
  return Builder.CreateShuffleVector(Vec, UndefValue::get(Vec->getType()),
                                     ShuffleMask,
                                     "normalized." + Vec->getName());
}

// Return Value indicating that the mask is not all-zero
static Value *isNotAllZeroMask(IRBuilder<> &Builder, Value *MaskValue,
                               Value *&MaskInInt) {
  unsigned VF = MaskValue->getType()->getVectorNumElements();
  Type *IntTy = IntegerType::get(MaskValue->getContext(), VF);
  MaskInInt = Builder.CreateBitCast(MaskValue, IntTy);
  Value *NotAllZ = Builder.CreateICmp(CmpInst::ICMP_NE, MaskInInt,
                                      ConstantInt::get(IntTy, 0));
  return NotAllZ;
}

void VPOCodeGen::widenVectorStore(StoreInst *SI) {
  Value *Ptr = SI->getPointerOperand();
  unsigned Alignment = SI->getAlignment();
  // An alignment of 0 means target abi alignment. We need to use the scalar's
  // target abi alignment in such a case.
  const DataLayout &DL = SI->getModule()->getDataLayout();
  Value *DataOp = SI->getValueOperand();
  if (!Alignment)
    Alignment = DL.getABITypeAlignment(DataOp->getType());
  unsigned AddrSpace = Ptr->getType()->getPointerAddressSpace();

  Value *VecDataOp = getVectorValue(DataOp);
  Type *WideDataTy = VecDataOp->getType();
  unsigned OriginalVL = WideDataTy->getVectorNumElements() / VF;
  Type *ScalarTy = WideDataTy->getVectorElementType();

  auto GEP = getGEPInstruction(Ptr);
  int ConsecutiveStride = Legal->isConsecutivePtr(Ptr);
  if (ConsecutiveStride) {
    bool Reverse = (ConsecutiveStride == -1);
    bool IsPrivate = Legal->isLoopPrivate(Ptr);
    bool IsCondPrivate = Legal->isCondLastPrivate(Ptr);
    Value *VecPtr = nullptr;
    if (IsPrivate)
      VecPtr = getVectorPrivateBase(Ptr);
    else if (GEP) {
      GetElementPtrInst *Gep2 = cast<GetElementPtrInst>(GEP->clone());
      Gep2->setName("gep.indvar");

      for (unsigned i = 0; i < GEP->getNumOperands(); ++i)
        Gep2->setOperand(i, getScalarValue(GEP->getOperand(i), 0));
      VecPtr = Builder.Insert(Gep2);

    } else // No GEP
      VecPtr = getScalarValue(Ptr, 0);
    VecPtr = Builder.CreateBitCast(VecPtr, WideDataTy->getPointerTo(AddrSpace));

    if (!IsPrivate)
      VecDataOp = normalizeVector(VecDataOp, OriginalVL, Builder);

    if (Reverse)
      VecDataOp = reverseVector(VecDataOp, OriginalVL);

    if (MaskValue && !IsPrivate)
      Builder.CreateMaskedStore(VecDataOp, VecPtr, Alignment,
                                replicateVectorElts(MaskValue, OriginalVL,
                                                    Builder,
                                                    "replicatedMaskElts."));

    else if (MaskValue && IsCondPrivate) {
      // Private data. Should be conditional.

      Value *MaskInInt = nullptr;
      Value *NotAllZero = isNotAllZeroMask(Builder, MaskValue, MaskInInt);

      Builder.CreateMaskedStore(VecDataOp, VecPtr, Alignment,
                                replicateVector(MaskValue, OriginalVL, Builder,
                                                "replicatedMaskVec."));
      // Store the last written lane
      // We store only non-zero mask.
      Value *PrevMask = Builder.CreateLoad(LoopPrivateLastMask[Ptr]);
      Value *MaskToStore =
          Builder.CreateSelect(NotAllZero, MaskInInt, PrevMask);
      Builder.CreateStore(MaskToStore, LoopPrivateLastMask[Ptr]);

    } else {
      Builder.CreateAlignedStore(VecDataOp, VecPtr, Alignment);
      if (IsCondPrivate) {
        Type *MaskTy = IntegerType::get(Ptr->getContext(), VF);
        Builder.CreateStore(Constant::getAllOnesValue(MaskTy),
                            LoopPrivateLastMask[Ptr]);
      }
    }
    return;
  }
  // SCATTER
  if (GEP) {
    Value *BasePtr = GEP->getPointerOperand();
    if (!Legal->isLoopInvariant(BasePtr)) {
      BasePtr = getVectorValue(BasePtr);
      // Vectorized BasePtr looks like <ptr0, ptr1, ptr2, ptr3>.
      // Replicate the vector OriginalVL times.
      // If the OriginalVL is 2 it will look like:
      // <ptr0, ptr1, ptr2, ptr3, ptr0, ptr1, ptr2, ptr3>
      BasePtr = replicateVector(BasePtr, OriginalVL, Builder);
    } else {
      Type *BasePtrTy = BasePtr->getType()->getPointerElementType();
      if (BasePtrTy->isArrayTy()) {
        Type *ArrayEltTy = BasePtrTy->getArrayElementType();
        assert(ArrayEltTy->isVectorTy() && "Expected array of vectors");
        Type *ScalarEltTy = ArrayEltTy->getVectorElementType();
        Type *OneDimentionArrayTy =
            ArrayType::get(ScalarEltTy, BasePtrTy->getArrayNumElements());

        Type *NewBasePtrTy = PointerType::get(OneDimentionArrayTy, AddrSpace);
        BasePtr = Builder.CreateBitCast(BasePtr, NewBasePtrTy);

      } else
        BasePtr =
            Builder.CreateBitCast(BasePtr, ScalarTy->getPointerTo(AddrSpace));
    }
    // Loop invariant index remains as is. The IV-dependent index should
    // take a vector form.
    SmallVector<Value *, 2> NewIndices;
    // First, handle all indices except the last one
    for (unsigned i = 1; i < GEP->getNumOperands() - 1; ++i) {
      Value *GepIndex = GEP->getOperand(i);
      if (Legal->isLoopInvariant(GepIndex))
        NewIndices.push_back(getScalarValue(GepIndex, 0));
      else {
        Value *VecIndex = getVectorValue(GepIndex);
        // When the Loop-variant index is no the last it should be
        // replicated, as we did for the Loop-variant base pointer.
        VecIndex = replicateVector(VecIndex, OriginalVL, Builder,
                                   "replicatedGepIndex");
        NewIndices.push_back(VecIndex);
      }
    }

    // Now handle the last index.
    // For VF=4, OriginalVL=2 it should take the following form:
    // < Ind, Ind, Ind, Ind, Ind+1, Ind+1, Ind+1, Ind+1>

    Value *GepLastIndex = GEP->getOperand(GEP->getNumOperands() - 1);
    SmallVector<Value *, 4> Parts;
    Value *P0 = getVectorValue(GepLastIndex);
    Type *IndexTy = P0->getType();
    P0 = Builder.CreateMul(P0, ConstantInt::get(IndexTy, OriginalVL),
                           "Ind_" + Twine(0) + ".");
    Parts.push_back(P0);
    for (unsigned i = 1; i < OriginalVL; ++i)
      Parts.push_back(Builder.CreateAdd(P0, ConstantInt::get(IndexTy, i),
                                        "Ind_" + Twine(i) + "."));
    Value *VecIndex = joinVectors(Parts, Builder);
    NewIndices.push_back(VecIndex);

    GetElementPtrInst *VectorGEP = cast<GetElementPtrInst>(
        Builder.CreateGEP(BasePtr, NewIndices, "mm_vectorGEP"));

    VectorGEP->setIsInBounds(GEP->isInBounds());
    Value *WidenMask = MaskValue
                           ? replicateVector(MaskValue, OriginalVL, Builder,
                                             "replicatedMaskVec.")
                           : nullptr;
    Builder.CreateMaskedScatter(VecDataOp, VectorGEP, Alignment, WidenMask);
    return;
  }
  // No GEP
  Value *BasePtr = getVectorValue(Ptr);
  // Transform vector-of-pointers-to-vectors into vector-of-pointers-to-scalars
  // For example <4 x <2 x i32>*> should be transformed to <4 x i32*> because
  // the element type we are going to gather is i32.

  Type *NewTypeOfBasePtr =
      VectorType::get(PointerType::get(ScalarTy, AddrSpace), VF);
  BasePtr = Builder.CreateBitCast(BasePtr, NewTypeOfBasePtr);

  // Vectorized BasePtr looks like <ptr0, ptr1, ptr2, ptr3>.
  // Replicate the vector OriginalVL times.
  // If the OriginalVL is 2 it will look like:
  // <ptr0, ptr1, ptr2, ptr3, ptr0, ptr1, ptr2, ptr3>
  BasePtr = replicateVector(BasePtr, OriginalVL, Builder);
  // Build constant indices, Example for VF=4, OriginalVL=2:
  // <0, 0, 0, 0, 1, 1, 1, 1>
  SmallVector<Constant *, 4> Indices;
  for (unsigned j = 0; j < OriginalVL; ++j)
    for (unsigned i = 0; i < VF; ++i)
      Indices.push_back(Builder.getInt32(j));
  Value *VecInd = ConstantVector::get(Indices);
  GetElementPtrInst *VectorGEP = cast<GetElementPtrInst>(
      Builder.CreateGEP(BasePtr, VecInd, "mm_vectorGEP"));
  Value *WidenMask = MaskValue ? replicateVector(MaskValue, OriginalVL, Builder,
                                                 "replicatedMaskVec.")
                               : nullptr;
  Builder.CreateMaskedScatter(VecDataOp, VectorGEP, Alignment, WidenMask);
}

void VPOCodeGen::widenVectorLoad(LoadInst *LI) {
  Value *Ptr = LI->getPointerOperand();
  unsigned Alignment = LI->getAlignment();
  // An alignment of 0 means target abi alignment. We need to use the scalar's
  // target abi alignment in such a case.
  const DataLayout &DL = LI->getModule()->getDataLayout();
  if (!Alignment)
    Alignment = DL.getABITypeAlignment(LI->getType());
  unsigned AddrSpace = Ptr->getType()->getPointerAddressSpace();

  Type *ScalarTy = LI->getType()->getVectorElementType();
  unsigned OriginalVL = LI->getType()->getVectorNumElements();
  if (!ScalarTy->isSingleValueType())
    llvm_unreachable("Re-vectorization supports simple vectors only!");

  unsigned WideVF = VF * OriginalVL;
  Type *WideDataTy = VectorType::get(ScalarTy, WideVF);
  auto GEP = getGEPInstruction(Ptr);
  int ConsecutiveStride = Legal->isConsecutivePtr(Ptr);
  if (ConsecutiveStride) {
    // Long load and shuffles (G2S)
    bool Reverse = (ConsecutiveStride == -1);
    bool IsPrivate = false;
    if (Legal->isLoopPrivate(Ptr)) {
      Ptr = getVectorPrivateBase(Ptr);
      IsPrivate = true;
    } else if (GEP) {
      GetElementPtrInst *Gep2 = cast<GetElementPtrInst>(GEP->clone());
      for (unsigned i = 0; i < GEP->getNumOperands(); ++i)
        Gep2->setOperand(i, getScalarValue(GEP->getOperand(i), 0));
      Ptr = Builder.Insert(Gep2, "gep.indvar");

    } else // No GEP
      Ptr = getScalarValue(Ptr, 0);
    Ptr = Reverse
              ? Builder.CreateGEP(nullptr, Ptr, Builder.getInt32(1 - WideVF))
              : Ptr;
    Value *VecPtr =
        Builder.CreateBitCast(Ptr, WideDataTy->getPointerTo(AddrSpace));

    Value *NewLI;
    if (MaskValue && !IsPrivate) {
      // Masking not needed for privates.
      // Mask value should be replicated for each element.
      Value *RepMaskValue = replicateVectorElts(MaskValue, OriginalVL, Builder,
                                                "replicatedMaskElts.");
      NewLI = Builder.CreateMaskedLoad(VecPtr, Alignment, RepMaskValue, nullptr,
                                       "wide.masked.load");
    } else
      NewLI = Builder.CreateAlignedLoad(VecPtr, Alignment, "wide.load");

    if (Reverse)
      NewLI = reverseVector(NewLI, OriginalVL);

    if (!IsPrivate)
      NewLI = transposeVector(NewLI, OriginalVL, Builder);
    WidenMap[LI] = NewLI;
    return;
  }

  // Non-consecutive load. Build gathers, not need to transpose afterwards.

  if (GEP) {
    Value *BasePtr = GEP->getPointerOperand();
    if (!Legal->isLoopInvariant(BasePtr)) {
      BasePtr = getVectorValue(BasePtr);
      // Vectorized BasePtr looks like <ptr0, ptr1, ptr2, ptr3>.
      // Replicate the vector OriginalVL times.
      // If the OriginalVL is 2 it will look like:
      // <ptr0, ptr1, ptr2, ptr3, ptr0, ptr1, ptr2, ptr3>
      BasePtr =
          replicateVector(BasePtr, OriginalVL, Builder, "replicatedGepBasePtr");
    } else {
      Type *BasePtrTy = BasePtr->getType()->getPointerElementType();
      if (BasePtrTy->isArrayTy()) {
        Type *ArrayEltTy = BasePtrTy->getArrayElementType();
        assert(ArrayEltTy->isVectorTy() && "Expected array of vectors");
        Type *ScalarEltTy = ArrayEltTy->getVectorElementType();
        Type *OneDimentionArrayTy =
          ArrayType::get(ScalarEltTy, BasePtrTy->getArrayNumElements());
        Type *NewBasePtrTy = PointerType::get(OneDimentionArrayTy, AddrSpace);
        BasePtr = Builder.CreateBitCast(BasePtr, NewBasePtrTy);

      } else
        BasePtr =
            Builder.CreateBitCast(BasePtr, ScalarTy->getPointerTo(AddrSpace));
    }
    // Loop invariant index remains as is. The IV-dependent index should
    // take a vector form.
    SmallVector<Value *, 2> NewIndices;
    // First, handle all indices except the last one
    for (unsigned i = 1; i < GEP->getNumOperands() - 1; ++i) {
      Value *GepIndex = GEP->getOperand(i);
      if (Legal->isLoopInvariant(GepIndex))
        NewIndices.push_back(getScalarValue(GepIndex, 0));
      else {
        Value *VecIndex = getVectorValue(GepIndex);
        // When the Loop-variant index is no the last it should be
        // replicated, as we did for the Loop-variant base pointer.
        VecIndex = replicateVector(VecIndex, OriginalVL, Builder,
                                   "replicatedGepIndex");
        NewIndices.push_back(VecIndex);
      }
    }

    // Now handle the last index.
    // For VF=4, OriginalVL=2 it should take the following form:
    // < Ind, Ind, Ind, Ind, Ind+1, Ind+1, Ind+1, Ind+1>

    Value *GepLastIndex = GEP->getOperand(GEP->getNumOperands() - 1);
    SmallVector<Value *, 4> Parts;
    Value *P0 = getVectorValue(GepLastIndex);
    Type *IndexTy = P0->getType();
    P0 = Builder.CreateMul(P0, ConstantInt::get(IndexTy, OriginalVL),
                           "Ind_" + Twine(0) + ".");
    Parts.push_back(P0);
    for (unsigned i = 1; i < OriginalVL; ++i)
      Parts.push_back(Builder.CreateAdd(P0, ConstantInt::get(IndexTy, i),
                                        "Ind_" + Twine(i) + "."));
    Value *VecIndex = joinVectors(Parts, Builder);
    NewIndices.push_back(VecIndex);

    GetElementPtrInst *VectorGEP = cast<GetElementPtrInst>(
        Builder.CreateGEP(BasePtr, NewIndices, "mm_vectorGEP"));
    VectorGEP->setIsInBounds(GEP->isInBounds());
    Value *WidenMask = MaskValue
                           ? replicateVector(MaskValue, OriginalVL, Builder,
                                             "replicatedMaskVec.")
                           : nullptr;
    Value *NewVec = Builder.CreateMaskedGather(VectorGEP, Alignment, WidenMask,
                                               nullptr, "wide.masked.gather");
    WidenMap[LI] = NewVec;
    return;
  }
  // No GEP
  Value *BasePtr = getVectorValue(Ptr);
  // Transform vector-of-pointers-to-vectors into vector-of-pointers-to-scalars
  // For example <4 x <2 x i32>*> should be transformed to <4 x i32*> because
  // the element type we are going to gather is i32.

  Type *NewTypeOfBasePtr =
      VectorType::get(PointerType::get(ScalarTy, AddrSpace), VF);
  BasePtr = Builder.CreateBitCast(BasePtr, NewTypeOfBasePtr);
  // Vectorized BasePtr looks like <ptr0, ptr1, ptr2, ptr3>.
  // Replicate the vector OriginalVL times.
  // If the OriginalVL is 2 it will look like:
  // <ptr0, ptr1, ptr2, ptr3, ptr0, ptr1, ptr2, ptr3>
  BasePtr = replicateVector(BasePtr, OriginalVL, Builder);
  // Build constant indices, Example for VF=4, OriginalVL=2:
  // <0, 0, 0, 0, 1, 1, 1, 1>
  SmallVector<Constant *, 4> Indices;
  for (unsigned j = 0; j < OriginalVL; ++j)
    for (unsigned i = 0; i < VF; ++i)
      Indices.push_back(Builder.getInt32(j));
  Value *VecInd = ConstantVector::get(Indices);

  Value *VectorGEP = Builder.CreateGEP(BasePtr, VecInd, "mm_vectorGEP");

  Value *WidenMask = MaskValue ? replicateVector(MaskValue, OriginalVL, Builder,
                                                 "replicatedMaskVec.")
                               : nullptr;
  Value *NewVec = Builder.CreateMaskedGather(VectorGEP, Alignment, WidenMask,
                                             nullptr, "wide.masked.gather");
  WidenMap[LI] = NewVec;
}

void VPOCodeGen::vectorizeLinearLoad(Instruction *LinLdInst, int LinStep) {
  Instruction *LinLdClone = LinLdInst->clone();
  LinLdClone->setName(LinLdInst->getName() + "linload.clone");
  Builder.Insert(LinLdClone);

  // Generate vector value for the linear value loaded by broadcasting it and adding
  // LaneNum * LinStep
  auto LinValTy = LinLdClone->getType();
  Value *BroadcastVal = Builder.CreateVectorSplat(VF, LinLdClone);
  SmallVector<Constant *, 8> LinSteps;
  // Create the vector of steps from zero to VF in increments of LinStep.
  for (unsigned LaneNum = 0; LaneNum < VF; ++LaneNum) {
    LinSteps.push_back(ConstantInt::get(LinValTy, LaneNum * LinStep));
  }

  Constant *Cv = ConstantVector::get(LinSteps);

  auto LinVecValue = Builder.CreateAdd(BroadcastVal, Cv, "vec.linear");
  WidenMap[LinLdInst] = LinVecValue;
  
  // Add to UnitStepLinears if LinStep is 1/-1 - so that we can use it to infer
  // information about unit stride loads/stores
  if (LinStep == 1 || LinStep == -1) {
    addUnitStepLinear(LinLdInst, LinLdClone, LinStep);
  }
}

void VPOCodeGen::vectorizeLoadInstruction(Instruction *Inst,
                                          bool EmitIntrinsic) {
  LoadInst *LI = cast<LoadInst>(Inst);
  Value *Ptr = LI->getPointerOperand();
  int LinStride = 0;

  // Handle vectorization of a linear value load
  if (Legal->isLinear(Ptr, &LinStride)) {
    vectorizeLinearLoad(Inst, LinStride);
    return;
  }

  if (Legal->isLoopInvariant(Ptr) || Legal->isUniformForTheLoop(Ptr)) {
    serializeInstruction(Inst);
    return;
  }

  int ConsecutiveStride = Legal->isConsecutivePtr(Ptr);
  bool Reverse = (ConsecutiveStride == -1);
  if (!MaskValue && ConsecutiveStride == 0 && !EmitIntrinsic) {
    serializeInstruction(Inst);
    return;
  }

  if (LI->getType()->isVectorTy())
    return widenVectorLoad(LI);

  Type *DataTy = VectorType::get(LI->getType(), VF);
  unsigned Alignment = LI->getAlignment();
  // An alignment of 0 means target abi alignment. We need to use the scalar's
  // target abi alignment in such a case.
  const DataLayout &DL = Inst->getModule()->getDataLayout();
  if (!Alignment)
    Alignment = DL.getABITypeAlignment(LI->getType());
  unsigned AddressSpace = Ptr->getType()->getPointerAddressSpace();

  // Handle consecutive loads.
  if (ConsecutiveStride != 0) {
    bool IsPrivate = false;
    if (Legal->isLoopPrivate(Ptr)) {
      Ptr = getVectorPrivateBase(Ptr);
      IsPrivate = true;
    } else {
      GetElementPtrInst *Gep = getGEPInstruction(Ptr);
      if (Gep) {
        GetElementPtrInst *Gep2 = cast<GetElementPtrInst>(Gep->clone());
        for (unsigned i = 0; i < Gep->getNumOperands(); ++i)
          Gep2->setOperand(i, getScalarValue(Gep->getOperand(i), 0));
        Ptr = Builder.Insert(Gep2, "gep.indvar");
      } else // No GEP
        Ptr = getScalarValue(Ptr, 0);

      Ptr = Reverse ? Builder.CreateGEP(nullptr, Ptr, Builder.getInt32(1 - VF))
                    : Ptr;
    }
    Value *VecPtr =
        Builder.CreateBitCast(Ptr, DataTy->getPointerTo(AddressSpace));

    Value *NewLI;
    if (MaskValue && !IsPrivate) {
      // Masking not needed for privates.
      NewLI = Builder.CreateMaskedLoad(VecPtr, Alignment, MaskValue, nullptr,
                                       "wide.masked.load");
    } else
      NewLI = Builder.CreateAlignedLoad(VecPtr, Alignment, "wide.load");

    if (Reverse)
      NewLI = reverseVector(NewLI);
    WidenMap[cast<Value>(Inst)] = NewLI;
    return;
  }

  // GATHER
  Value *VectorPtr = getVectorValue(Ptr);
  Instruction *NewLI = Builder.CreateMaskedGather(
      VectorPtr, Alignment, MaskValue, nullptr, "wide.masked.gather");

  WidenMap[cast<Value>(Inst)] = cast<Value>(NewLI);
}

void VPOCodeGen::vectorizeSelectInstruction(Instruction *Inst) {
  SelectInst *SelectI = cast<SelectInst>(Inst);
  // If the selector is loop invariant we can create a select
  // instruction with a scalar condition. Otherwise, use vector-select.
  auto *SE = PSE.getSE();
  Value *Cond = SelectI->getOperand(0);
  Value *VCond = getVectorValue(Cond);
  Value *Op0 = getVectorValue(SelectI->getOperand(1));
  Value *Op1 = getVectorValue(SelectI->getOperand(2));

  bool InvariantCond =
    SE->isLoopInvariant(PSE.getSCEV(Cond), OrigLoop);

  // The condition can be loop invariant  but still defined inside the
  // loop. This means that we can't just use the original 'cond' value.

  if (InvariantCond)
    VCond = getScalarValue(Cond, 0);

  Value *NewSelect = Builder.CreateSelect(VCond, Op0, Op1);

  WidenMap[Inst] = NewSelect;
}

void VPOCodeGen::vectorizeLinearStore(Instruction *Inst) {
  StoreInst *SI = cast<StoreInst>(Inst);
  Value *Ptr = SI->getPointerOperand();

  // Store the value that corresponds to lane 0 - any subsequent loads
  // will add in the linear step when generating the vector value for the load.
  Value *ValToStore =   getScalarValue(SI->getValueOperand(), 0);

  // If the store is masked, blend using the current linear value so that we can
  // do an unconditional store.
  if (MaskValue) {
    auto ScalMask =  Builder.CreateExtractElement(MaskValue, Builder.getInt32(VF-1), "lin.mask");
    auto CurrVal = Builder.CreateLoad(Ptr);

    ValToStore = Builder.CreateSelect(ScalMask, ValToStore,CurrVal);
  }
  
  Builder.CreateStore(ValToStore, Ptr);
}

void VPOCodeGen::vectorizeStoreInstruction(Instruction *Inst,
                                           bool EmitIntrinsic) {
  StoreInst *SI = cast<StoreInst>(Inst);
  Value *Ptr = SI->getPointerOperand();

  // Handle vectorization of a linear value store
  if (Legal->isLinear(Ptr)) {
    vectorizeLinearStore(Inst);
    return;
  }

  int ConsecutiveStride = Legal->isConsecutivePtr(Ptr);
  bool Reverse = (ConsecutiveStride == -1);
  if (!MaskValue && ConsecutiveStride == 0 && !EmitIntrinsic) {
    serializeInstruction(Inst);
    return;
  }

  const DataLayout &DL = Inst->getModule()->getDataLayout();
  if (SI->getValueOperand()->getType()->isVectorTy())
    return widenVectorStore(SI);

  Type *ScalarDataTy = SI->getValueOperand()->getType();
  Type *DataTy = VectorType::get(ScalarDataTy, VF);

  unsigned Alignment = SI->getAlignment();
  if (!Alignment)
    Alignment = DL.getABITypeAlignment(ScalarDataTy);
  unsigned AddressSpace = Ptr->getType()->getPointerAddressSpace();
  Value *VecDataOp = getVectorValue(SI->getValueOperand());

  // Handle consecutive stores.
  if (ConsecutiveStride) {
    bool IsPrivate = Legal->isLoopPrivate(Ptr);
    bool StoreMaskValue = Legal->isCondLastPrivate(Ptr);

    Value *VecPtr = nullptr;
    if (IsPrivate)
      VecPtr = getVectorPrivateBase(Ptr);
    else {
      GetElementPtrInst *Gep = getGEPInstruction(Ptr);
      if (Gep) {
        GetElementPtrInst *Gep2 = cast<GetElementPtrInst>(Gep->clone());
        Gep2->setName("gep.indvar");

        for (unsigned i = 0; i < Gep->getNumOperands(); ++i)
          Gep2->setOperand(i, getScalarValue(Gep->getOperand(i), 0));
        VecPtr = Builder.Insert(Gep2);
      } else // No GEP
        VecPtr = getScalarValue(Ptr, 0);

      VecPtr = Reverse ?
        Builder.CreateGEP(nullptr, VecPtr, Builder.getInt32(1 - VF)) : VecPtr;

      if (Reverse)
        // If we store to reverse consecutive memory locations, then we need
        // to reverse the order of elements in the stored value.
        VecDataOp = reverseVector(VecDataOp);
    }
    VecPtr = Builder.CreateBitCast(VecPtr, DataTy->getPointerTo(AddressSpace));
    if (MaskValue)
      Builder.CreateMaskedStore(VecDataOp, VecPtr, Alignment, MaskValue);
    else
      Builder.CreateAlignedStore(VecDataOp, VecPtr, Alignment);

    if (StoreMaskValue) {
      Value *MaskToStore = nullptr;
      if (MaskValue) {
        Value *MaskInInt = nullptr;
        Value *NotAllZero = isNotAllZeroMask(Builder, MaskValue, MaskInInt);

        // Store the last written lane
        // We store only non-zero mask.
        Value *PrevMask = Builder.CreateLoad(LoopPrivateLastMask[Ptr]);
        MaskToStore = Builder.CreateSelect(NotAllZero, MaskInInt, PrevMask);
      } else {
        Type *MaskTy = IntegerType::get(Ptr->getContext(), VF);
        MaskToStore = Constant::getAllOnesValue(MaskTy);
      }
      Builder.CreateStore(MaskToStore, LoopPrivateLastMask[Ptr]);
    }
    return;
  }

  // SCATTER
  Value *VectorPtr = getVectorValue(Ptr);
  VectorType *VTy = cast<VectorType>(VectorPtr->getType());
  PointerType *ElemTy = cast<PointerType>(VTy->getElementType());
  Type *PointedToTy = ElemTy->getElementType();
  VectorType *VecToTy = VectorType::get(PointedToTy, VF);
  if (VecDataOp->getType() != VecToTy) {
    VecDataOp = Builder.CreateBitCast(VecDataOp, VecToTy, "cast");
  }
  Builder.CreateMaskedScatter(VecDataOp, VectorPtr, Alignment, MaskValue);
}

void VPOCodeGen::vectorizeExtractElement(Instruction *Inst) {
  ExtractElementInst *ExtrEltInst = cast<ExtractElementInst>(Inst);
  Value *ExtrFrom = getVectorValue(ExtrEltInst->getVectorOperand());
  Value *IndexVal = ExtrEltInst->getIndexOperand();
  if (!isa<ConstantInt>(IndexVal))
    return llvm_unreachable(
        "Extract element with variable index is not supported");
  unsigned Index = cast<ConstantInt>(IndexVal)->getZExtValue();

  // Extract subvector. The subvector should include VF elements.
  // The start position for extracting is VF*Index.
  SmallVector<unsigned, 8> ShufMask;
  for (unsigned i = 0; i < VF; ++i)
    ShufMask.push_back(VF * Index + i);
  Type *VTy = ExtrFrom->getType();
  WidenMap[Inst] =
      Builder.CreateShuffleVector(ExtrFrom, UndefValue::get(VTy), ShufMask);
}

void VPOCodeGen::vectorizeShuffle(Instruction *Inst) {
  ShuffleVectorInst *Shuf = cast<ShuffleVectorInst>(Inst);
  unsigned OriginalVL = Shuf->getOperand(0)->getType()->getVectorNumElements();
  // Simple case - broadcast scalar elt into vector.
  if (getSplatValue(Inst)) {

    Value *SplVal = cast<InsertElementInst>(Shuf->getOperand(0))->getOperand(1);
    Value *Vec = getVectorValue(SplVal);
    SmallVector<unsigned, 8> ShufMask;
    for (unsigned i = 0; i < OriginalVL; ++i)
      for (unsigned j = 0; j < VF; ++j)
        ShufMask.push_back(j);

    WidenMap[Inst] = Builder.CreateShuffleVector(Vec,
                                                 UndefValue::get(Vec->getType()),
                                                 ShufMask);
    return;
  }

  Value *V0 = getVectorValue(Shuf->getOperand(0));
  
  Constant *Mask = Shuf->getMask();
  int InstVL = Inst->getType()->getVectorNumElements();
  // All-zero mask case
  if (isa<ConstantAggregateZero>(Mask)) {
    SmallVector<unsigned, 8> ShufMask;
    int Repeat = InstVL / OriginalVL;
    for (int k = 0; k < Repeat; k++)
      for (unsigned i = 0; i < OriginalVL; ++i)
        for (unsigned j = 0; j < VF; ++j)
          ShufMask.push_back(j + i);

    WidenMap[Inst] = Builder.CreateShuffleVector(V0,
                                                 UndefValue::get(V0->getType()),
                                                 ShufMask);
    return;
  }
  // General case - whole mask should be recalculated
  llvm_unreachable("Unsupported shuffle");
}

void VPOCodeGen::vectorizeInsertElement(Instruction *Inst) {
  InsertElementInst *InsEltInst = cast<InsertElementInst>(Inst);
  Value *InsertTo = getVectorValue(InsEltInst->getOperand(0));
  Value *NewSubVec = getVectorValue(InsEltInst->getOperand(1));
  Value *IndexVal = InsEltInst->getOperand(2);
  unsigned Index = cast<ConstantInt>(IndexVal)->getZExtValue();
  unsigned WideNumElts = InsertTo->getType()->getVectorNumElements();

  if (isa<UndefValue>(InsertTo)) {
    // Insert into Undef vector.
    SmallVector<unsigned, 8> ShufMask;
    for (unsigned i = 0; i < WideNumElts; ++i)
      ShufMask.push_back(i);
    unsigned StartInd = Index * VF;
    for (unsigned i = 0; i < VF; ++i)
      ShufMask[StartInd + i] = i;
    Value *Shuf = Builder.CreateShuffleVector(
        NewSubVec, UndefValue::get(NewSubVec->getType()), ShufMask);
    WidenMap[Inst] = Shuf;
    return;
  }

  // Two shuffles. The first one is extending the Subvector to the width of
  // the first source. And the second one is for merging.
  SmallVector<unsigned, 8> ShufMask;
  for (unsigned i = 0; i < VF; ++i)
    ShufMask.push_back(i);

  for (unsigned i = VF; i < WideNumElts; ++i)
    ShufMask.push_back(VF);
  Value *ExtendSubVec = Builder.CreateShuffleVector(
      NewSubVec, UndefValue::get(NewSubVec->getType()), ShufMask);

  SmallVector<unsigned, 8> ShufMask2;
  for (unsigned i = 0; i < WideNumElts; ++i)
    ShufMask2.push_back(i);
  unsigned StartInd = Index * VF;
  for (unsigned i = 0; i < VF; ++i)
    ShufMask2[StartInd + i] = WideNumElts + i;
  WidenMap[Inst] =
      Builder.CreateShuffleVector(InsertTo, ExtendSubVec, ShufMask2);
}

void VPOCodeGen::serializeWithPredication(Instruction *Inst) {
  if (!MaskValue)
    return serializeInstruction(Inst);

  assert(MaskValue->getType()->isVectorTy() &&
         MaskValue->getType()->getVectorNumElements() == VF &&
         "Unexpected Mask Type");
  for (unsigned Lane = 0; Lane < VF; ++Lane) {
    Value *Cmp = Builder.CreateExtractElement(MaskValue, Lane, "Predicate");
    Cmp = Builder.CreateICmp(ICmpInst::ICMP_EQ, Cmp,
                             ConstantInt::get(Cmp->getType(), 1));
    Instruction *Cloned = Inst->clone();
    if (!Inst->getType()->isVoidTy())
      Cloned->setName(Inst->getName() + ".cloned");

    // Replace the operands of the cloned instructions with their scalar
    // equivalents in the new loop.
    for (unsigned Op = 0, e = Inst->getNumOperands(); Op != e; ++Op) {
      auto *NewOp = getScalarValue(Inst->getOperand(Op), Lane);
      Cloned->setOperand(Op, NewOp);
    }

    // Place the cloned scalar in the new loop.
    Builder.Insert(Cloned);
    ScalarMap[Inst][Lane] = Cloned;

    PredicatedInstructions.push_back(std::make_pair(Cloned, Cmp));
  }
}

void VPOCodeGen::serializeInstruction(Instruction *Instr) {

  assert(!Instr->getType()->isAggregateType() && "Can't handle vectors");

  unsigned Lanes = OrigLoop->hasLoopInvariantOperands(Instr) ||
    isUniformAfterVectorization(Instr, VF) ? 1 : VF;

  // Does this instruction return a value ?
  bool IsVoidRetTy = Instr->getType()->isVoidTy();

  // For each scalar that we create:
  for (unsigned Lane = 0; Lane < Lanes; ++Lane) {

    Instruction *Cloned = Instr->clone();
    if (!IsVoidRetTy)
      Cloned->setName(Instr->getName() + ".cloned");

    // Replace the operands of the cloned instructions with their scalar
    // equivalents in the new loop.
    for (unsigned Op = 0, e = Instr->getNumOperands(); Op != e; ++Op) {
      auto *NewOp = getScalarValue(Instr->getOperand(Op), Lane);
      Cloned->setOperand(Op, NewOp);
    }
    // Place the cloned scalar in the new loop.
    Builder.Insert(Cloned);
    ScalarMap[Instr][Lane] = Cloned;
  }
}

Value *VPOCodeGen::getStrideVector(Value *Val, Value *Stride) {
  assert(Val->getType()->isVectorTy() && "Must be a vector");
  assert(Val->getType()->getScalarType()->isIntegerTy() &&
         "Elem must be an integer");
  assert(Stride->getType() == Val->getType()->getScalarType() &&
         "Stride has wrong type");

  // Create the types.
  Type *ITy = Val->getType()->getScalarType();
  SmallVector<Constant *, 8> Indices;

  // Create a vector of consecutive numbers from zero to VF.
  for (unsigned i = 0; i < VF; ++i) {
    Indices.push_back(ConstantInt::get(ITy, i));
  }

  // Add the consecutive indices to the vector value.
  Constant *Cv = ConstantVector::get(Indices);
  assert(Cv->getType() == Val->getType() && "Invalid consecutive vec");
  Stride = Builder.CreateVectorSplat(VF, Stride);
  assert(Stride->getType() == Val->getType() && "Invalid stride type");

  // TBD: The newly created binary instructions should contain nsw/nuw flags,
  // which can be found from the original scalar operations.
  Stride = Builder.CreateMul(Cv, Stride);
  return Builder.CreateAdd(Val, Stride, "induction");
}

Value *VPOCodeGen::getStepVector(Value *Val, int StartIdx, Value *Step,
                                          Instruction::BinaryOps BinOp) {
  // Create and check the types.
  assert(Val->getType()->isVectorTy() && "Must be a vector");
  int VLen = Val->getType()->getVectorNumElements();

  Type *STy = Val->getType()->getScalarType();
  assert((STy->isIntegerTy() || STy->isFloatingPointTy()) &&
         "Induction Step must be an integer or FP");
  assert(Step->getType() == STy && "Step has wrong type");

  SmallVector<Constant *, 8> Indices;

  if (STy->isIntegerTy()) {
    // Create a vector of consecutive numbers from zero to VF.
    for (int i = 0; i < VLen; ++i)
      Indices.push_back(ConstantInt::get(STy, StartIdx + i));

    // Add the consecutive indices to the vector value.
    Constant *Cv = ConstantVector::get(Indices);
    assert(Cv->getType() == Val->getType() && "Invalid consecutive vec");
    Step = Builder.CreateVectorSplat(VLen, Step);
    assert(Step->getType() == Val->getType() && "Invalid step vec");
    // FIXME: The newly created binary instructions should contain nsw/nuw flags,
    // which can be found from the original scalar operations.
    Step = Builder.CreateMul(Cv, Step);
    return Builder.CreateAdd(Val, Step, "induction");
  }

  // Floating point induction.
  assert((BinOp == Instruction::FAdd || BinOp == Instruction::FSub) &&
         "Binary Opcode should be specified for FP induction");
  // Create a vector of consecutive numbers from zero to VF.
  for (int i = 0; i < VLen; ++i)
    Indices.push_back(ConstantFP::get(STy, (double)(StartIdx + i)));

  // Add the consecutive indices to the vector value.
  Constant *Cv = ConstantVector::get(Indices);

  Step = Builder.CreateVectorSplat(VLen, Step);

  // Floating point operations had to be 'fast' to enable the induction.
  FastMathFlags Flags;
  Flags.setUnsafeAlgebra();

  Value *MulOp = Builder.CreateFMul(Cv, Step);
  if (isa<Instruction>(MulOp))
    // Have to check, MulOp may be a constant
    cast<Instruction>(MulOp)->setFastMathFlags(Flags);

  Value *BOp = Builder.CreateBinOp(BinOp, Val, MulOp, "induction");
  if (isa<Instruction>(BOp))
    cast<Instruction>(BOp)->setFastMathFlags(Flags);
  return BOp;
}

/// A helper function that adds a 'fast' flag to floating-point operations.
static Value *addFastMathFlag(Value *V) {
  if (isa<FPMathOperator>(V)) {
    FastMathFlags Flags;
    Flags.setUnsafeAlgebra();
    cast<Instruction>(V)->setFastMathFlags(Flags);
  }
  return V;
}

/// A helper function that returns an integer or floating-point constant with
/// value C.
static Constant *getSignedIntOrFpConstant(Type *Ty, int64_t C) {
  return Ty->isIntegerTy() ? ConstantInt::getSigned(Ty, C)
    : ConstantFP::get(Ty, C);
}

void VPOCodeGen::createVectorIntOrFpInductionPHI(const InductionDescriptor &ID,
                                                 Value *Step,
                                                 Instruction *&VectorInd) {
  Value *Start = ID.getStartValue();

  // Construct the initial value of the vector IV in the vector loop preheader
  auto CurrIP = Builder.saveIP();
  Builder.SetInsertPoint(LoopVectorPreHeader->getTerminator());
  Value *SplatStart = Builder.CreateVectorSplat(VF, Start);
  Value *SteppedStart =
    getStepVector(SplatStart, 0, Step, ID.getInductionOpcode());

  // We create vector phi nodes for both integer and floating-point induction
  // variables. Here, we determine the kind of arithmetic we will perform.
  Instruction::BinaryOps AddOp;
  Instruction::BinaryOps MulOp;
  if (Step->getType()->isIntegerTy()) {
    AddOp = Instruction::Add;
    MulOp = Instruction::Mul;
  } else {
    AddOp = ID.getInductionOpcode();
    MulOp = Instruction::FMul;
  }

  // Multiply the vectorization factor by the step using integer or
  // floating-point arithmetic as appropriate.
  Value *ConstVF = getSignedIntOrFpConstant(Step->getType(), VF);
  Value *Mul = addFastMathFlag(Builder.CreateBinOp(MulOp, Step, ConstVF));

  // Create a vector splat to use in the induction update.
  //
  // FIXME: If the step is non-constant, we create the vector splat with
  //        IRBuilder. IRBuilder can constant-fold the multiply, but it doesn't
  //        handle a constant vector splat.
  Value *SplatVF = isa<Constant>(Mul)
    ? ConstantVector::getSplat(VF, cast<Constant>(Mul))
    : Builder.CreateVectorSplat(VF, Mul);
  Builder.restoreIP(CurrIP);

  // We may need to add the step a number of times, depending on the unroll
  // factor. The last of those goes into the PHI.
  VectorInd = PHINode::Create(SteppedStart->getType(), 2, "vec.ind",
                              &*LoopVectorBody->getFirstInsertionPt());

  Instruction *LastInduction = cast<Instruction>(addFastMathFlag(
    Builder.CreateBinOp(AddOp, VectorInd, SplatVF, "step.add")));

  // Move the last step to the end of the latch block. This ensures consistent
  // placement of all induction updates.
  auto *LoopVectorLatch = LI->getLoopFor(LoopVectorBody)->getLoopLatch();
  auto *Br = cast<BranchInst>(LoopVectorLatch->getTerminator());
  auto *ICmp = cast<Instruction>(Br->getCondition());
  LastInduction->moveBefore(ICmp);
  LastInduction->setName("vec.ind.next");

  cast<PHINode>(VectorInd)->addIncoming(SteppedStart, LoopVectorPreHeader);
  cast<PHINode>(VectorInd)->addIncoming(LastInduction, LoopVectorLatch);
}

void VPOCodeGen::buildScalarSteps(Value *ScalarIV, Value *Step, Value *EntryVal,
                                  const InductionDescriptor &ID) {

  // We shouldn't have to build scalar steps if we aren't vectorizing.
  assert(VF > 1 && "VF should be greater than one");

  // Get the value type and ensure it and the step have the same integer type.
  Type *ScalarIVTy = ScalarIV->getType()->getScalarType();
  assert(ScalarIVTy == Step->getType() &&
         "Val and Step should have the same type");

  // We build scalar steps for both integer and floating-point induction
  // variables. Here, we determine the kind of arithmetic we will perform.
  Instruction::BinaryOps AddOp;
  Instruction::BinaryOps MulOp;
  if (ScalarIVTy->isIntegerTy()) {
    AddOp = Instruction::Add;
    MulOp = Instruction::Mul;
  } else {
    AddOp = ID.getInductionOpcode();
    MulOp = Instruction::FMul;
  }

  // Determine the number of scalars we need to generate for each unroll
  // iteration. If EntryVal is uniform, we only need to generate the first
  // lane. Otherwise, we generate all VF values.
  unsigned Lanes =
    isUniformAfterVectorization(cast<Instruction>(EntryVal), VF) ? 1 : VF;

  for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
    auto *StartIdx = getSignedIntOrFpConstant(ScalarIVTy, Lane);
    auto *Mul = addFastMathFlag(Builder.CreateBinOp(MulOp, StartIdx, Step));
    auto *Add = addFastMathFlag(Builder.CreateBinOp(AddOp, ScalarIV, Mul));
    ScalarMap[EntryVal][Lane] = Add;
  }
}

void VPOCodeGen::widenIntOrFpInduction(PHINode *IV) {

  auto II = Legal->getInductionVars()->find(IV);
  assert(II != Legal->getInductionVars()->end() && "IV is not an induction");

  auto ID = II->second;
  assert(IV->getType() == ID.getStartValue()->getType() && "Types must match");
  auto &DL = OrigLoop->getHeader()->getModule()->getDataLayout();

  // The step of the induction.
  Value *Step = nullptr;
  if (PSE.getSE()->isSCEVable(IV->getType())) {
    SCEVExpander Exp(*PSE.getSE(), DL, "induction");
    Step = Exp.expandCodeFor(ID.getStep(), ID.getStep()->getType(),
                             LoopVectorPreHeader->getTerminator());
  } else {
    Step = cast<SCEVUnknown>(ID.getStep())->getValue();
  }

  Instruction *VectorInd = nullptr;
  createVectorIntOrFpInductionPHI(ID, Step, VectorInd);
  WidenMap[cast<Value>(IV)] = VectorInd;

  Value *ScalarIV = Induction;
  if (IV != Legal->getInduction()) {
    ScalarIV = IV->getType()->isIntegerTy()
      ? Builder.CreateSExtOrTrunc(ScalarIV, IV->getType())
      : Builder.CreateCast(Instruction::SIToFP, Induction,
                           IV->getType());
    ScalarIV = ID.transform(Builder, ScalarIV, PSE.getSE(), DL);
    ScalarIV->setName("offset.idx");
  }

  buildScalarSteps(ScalarIV, Step, IV, ID);
}

void VPOCodeGen::fixNonInductionPhis() {
  // When checking for uniformity below, we should be using the original
  // phi in the scalar loop.
  for (auto OrigPhi : OrigInductionPhisToFix) {
    PHINode *NewPhi;
    bool IsUniform = isUniformAfterVectorization(OrigPhi, VF);

    if (IsUniform)
      NewPhi = cast<PHINode>(getScalarValue(OrigPhi, 0));
    else
      NewPhi = cast<PHINode>(getVectorValue(OrigPhi));

    unsigned NumIncomingValues = OrigPhi->getNumIncomingValues();

    SmallVector<BasicBlock *, 2> ScalarBBPredecessors;
    for (auto BB : predecessors(OrigPhi->getParent()))
      ScalarBBPredecessors.push_back(BB);
    SmallVector<BasicBlock *, 2> VectorBBPredecessors;
    for (auto BB : predecessors(NewPhi->getParent()))
      VectorBBPredecessors.push_back(BB);

    assert(
      ScalarBBPredecessors.size() == VectorBBPredecessors.size() &&
      "Scalar and Vector BB should have the same number of predecessors");

    // We assume that blocks layout is preserved and search the incoming BB
    // basing on the predecessors order in scalar blocks.
    for (unsigned i = 0; i < NumIncomingValues; ++i) {
      auto BB = VectorBBPredecessors[i];

      // When looking up the new scalar/vector values to fix up use incoming
      // values from original phi.
      Value *ScIncV =
          OrigPhi->getIncomingValueForBlock(ScalarBBPredecessors[i]);
      Value *NewIncV;
      if (IsUniform) {
        NewIncV = getScalarValue(ScIncV, 0);
        NewPhi->setIncomingBlock(i, BB);
        NewPhi->setIncomingValue(i, NewIncV);
      } else {
        NewIncV = getVectorValue(ScIncV);
        NewPhi->addIncoming(NewIncV, BB);
      }
    }
  }
}

void VPOCodeGen::setEdgeMask(BasicBlock *From, BasicBlock *To, Value *Mask) {
  EdgeToMaskMap[std::make_pair(From, To)] = Mask;
}

Value *VPOCodeGen::getEdgeMask(BasicBlock *From, BasicBlock *To) {
  auto Edge = std::make_pair(From, To);
  if (EdgeToMaskMap.count(Edge))
    return EdgeToMaskMap[Edge];
  return nullptr;
}

void VPOCodeGen::widenNonInductionPhi(PHINode *Phi) {
  unsigned NumIncomingValues = Phi->getNumIncomingValues();
  
  if (isUniformAfterVectorization(Phi, VF)) {
    Instruction *Cloned = Phi->clone();
    Cloned->setName(Phi->getName() + ".cloned");
    Builder.Insert(Cloned);
    ScalarMap[Phi][0] = Cloned;
    // Set incoming values later, they may be not ready yet in case of back-edges.
    OrigInductionPhisToFix.push_back(Phi);
    return;
  }

  Value *Entry;
  bool ConvertablePhi = true;
  // Generate a sequence of selects of the form:
  // SELECT(Mask3, In3,
  //      SELECT(Mask2, In2,
  //                   ( ...)))
  for (unsigned In = 0; In < NumIncomingValues; In++) {
    auto IncBlock = Phi->getIncomingBlock(In);
    Value *Cond = getEdgeMask(IncBlock, Phi->getParent());
    if (!Cond) {
      ConvertablePhi = false;
      break;
    }
    Value *In0 = getVectorValue(Phi->getIncomingValue(In));
    if (In == 0)
      Entry = In0;
    else {
      // Select between the current value and the previous incoming edge
      // based on the incoming mask.
      auto IncBlock = Phi->getIncomingBlock(In);
      Value *Cond = getEdgeMask(IncBlock, Phi->getParent());
      assert(Cond && "Edge not in predicate map");
      Entry = Builder.CreateSelect(Cond, In0, Entry, "predphi");
    }
  }

  if (!ConvertablePhi) {
    Type *Ty = Phi->getType();
    Type *VecTy = VectorType::get(Ty, VF);
    Entry =
      Builder.CreatePHI(VecTy, NumIncomingValues, Phi->getName() + ".vec");
    // Set incoming values later, they may be not ready yet in case of back-edges.
    OrigInductionPhisToFix.push_back(Phi);
  }
  WidenMap[Phi] = Entry;
}

void VPOCodeGen::vectorizePHIInstruction(Instruction *Inst) {

  PHINode *P = cast<PHINode>(Inst);
  // Handle recurrences.
  if (Legal->isReductionVariable(P)) {
    Type *VecTy = VectorType::get(P->getType(), VF);
    PHINode *VecPhi = PHINode::Create(
      VecTy, 2, "vec.phi", &*LoopVectorBody->getFirstInsertionPt());
    WidenMap[P] = VecPhi;
    return;
  }

  if (!Legal->getInductionVars()->count(P)) {
    // The Phi node is not induction. It combines 2 basic blocks ruled out
    // by uniform branch.
    return widenNonInductionPhi(P);
  }

  InductionDescriptor II = Legal->getInductionVars()->lookup(P);
  const DataLayout &DL = OrigLoop->getHeader()->getModule()->getDataLayout();

  switch (II.getKind()) {
  default:
    llvm_unreachable("Unknown induction");
  case InductionDescriptor::IK_IntInduction:
  case InductionDescriptor::IK_FpInduction:
    return widenIntOrFpInduction(P);
  case InductionDescriptor::IK_PtrInduction: {
    // Handle the pointer induction variable case.
    assert(P->getType()->isPointerTy() && "Unexpected type.");
    // This is the normalized GEP that starts counting at zero.
    Value *PtrInd = Induction;
    PtrInd = Builder.CreateSExtOrTrunc(PtrInd, II.getStep()->getType());
    // Determine the number of scalars we need to generate for each unroll
    // iteration. If the instruction is uniform, we only need to generate the
    // first lane. Otherwise, we generate all VF values.
    unsigned Lanes = VF;
    // These are the scalar results. Notice that we don't generate vector GEPs
    // because scalar GEPs result in better code.
    for (unsigned Lane = 0; Lane < Lanes; ++Lane) {
      Constant *Idx = ConstantInt::get(PtrInd->getType(), Lane);
      Value *GlobalIdx = Builder.CreateAdd(PtrInd, Idx);
      Value *SclrGep = II.transform(Builder, GlobalIdx, PSE.getSE(), DL);
      SclrGep->setName("next.gep");
      ScalarMap[Inst][Lane] = SclrGep;
    }
    return;
  }

  }
}

VectorVariant* VPOCodeGen::matchVectorVariant(Function *CalledFunc,
                                              bool Masked) {

  DEBUG(dbgs() << "\nCall VF: " << VF << "\n");
  unsigned TargetMaxRegWidth = TTI->getRegisterBitWidth(true);
  DEBUG(dbgs() << "Target Max Register Width: " << TargetMaxRegWidth << "\n");

  VectorVariant::ISAClass TargetIsaClass;
  switch (TargetMaxRegWidth) {
    case 128:
      TargetIsaClass = VectorVariant::ISAClass::XMM;
      break;
    case 256:
      // Important Note: there is no way to inspect CPU or FeatureBitset from
      // the LLVM compiler middle end (i.e., lib/Analysis, lib/Transforms). This
      // can only be done from the front-end or from lib/Target. Thus, we select
      // avx2 by default for 256-bit vector register targets. Plus, I don't
      // think we currently have anything baked in to TTI to differentiate avx
      // vs. avx2. Namely, whether or not for 256-bit register targets there is
      // 256-bit integer support.
      TargetIsaClass = VectorVariant::ISAClass::YMM2;
      break;
    case 512:
      TargetIsaClass = VectorVariant::ISAClass::ZMM;
      break;
    default:
      llvm_unreachable("Invalid target vector register width");
  }
  DEBUG(dbgs() << "Target ISA Class: "
               << VectorVariant::ISAClassToString(TargetIsaClass) << "\n\n");

  if (CalledFunc->hasFnAttribute("vector-variants")) {
    Attribute Attr = CalledFunc->getFnAttribute("vector-variants");
    StringRef VariantsStr = Attr.getValueAsString();
    SmallVector<StringRef, 4> Variants;
    VariantsStr.split(Variants, ",");
    VectorVariant::ISAClass SelectedIsaClass = VectorVariant::ISAClass::XMM;
    int VariantIdx = -1;
    for (unsigned i = 0; i < Variants.size(); i++) {
      VectorVariant *Variant = new VectorVariant(Variants[i]);
      VectorVariant::ISAClass VariantIsaClass = Variant->getISA();
      DEBUG(dbgs() << "Variant ISA Class: "
                   << VectorVariant::ISAClassToString(VariantIsaClass) << "\n");
      unsigned IsaClassMaxRegWidth =
        VectorVariant::ISAClassMaxRegisterWidth(VariantIsaClass);
      DEBUG(dbgs() << "Isa Class Max Vector Register Width: "
                   << IsaClassMaxRegWidth << "\n");
      unsigned FuncVF = Variant->getVlen();
      DEBUG(dbgs() << "Func VF: " << FuncVF << "\n\n");

      // Select the largest supported ISA Class for this target.
      if (FuncVF == VF && VariantIsaClass <= TargetIsaClass &&
        Variant->isMasked() == Masked && VariantIsaClass >= SelectedIsaClass) {
        DEBUG(dbgs() << "Candidate Function: " << Variant->encode() << "\n");
        SelectedIsaClass = VariantIsaClass;
        VariantIdx = i;
      }
      delete Variant;
    }

    assert(VariantIdx >= 0 && "Invalid vector variant index");
    VectorVariant *SelectedVariant = new VectorVariant(Variants[VariantIdx]);
    return SelectedVariant;
  }

  llvm_unreachable("Function has vector variants but could not find a match");
}

void VPOCodeGen::vectorizeCallArgs(CallInst *Call, VectorVariant *VecVariant,
                                   SmallVectorImpl<Value*> &VecArgs,
                                   SmallVectorImpl<Type*> &VecArgTys) {

  std::vector<VectorKind> Parms;
  if (VecVariant) {
    Parms = VecVariant->getParameters();
  }

  for (unsigned i = 0; i < Call->getNumArgOperands(); i++) {
    if ((VecVariant && Parms[i].isVector()) || !VecVariant) {
      // This is a vector call arg, so vectorize it.
      Value *Arg = Call->getArgOperand(i);
      Value *VecArg = getVectorValue(Arg);
      VecArgs.push_back(VecArg);
      VecArgTys.push_back(VecArg->getType());
    } else {
      // Linear and uniform parameters must be passed as scalars according to
      // the vector function abi. CodeGen currently vectorizes all instructions,
      // so the scalar arguments for the vector function must be extracted from
      // them. For both linear and uniform args, extract from lane 0. Linear
      // args can use the value at lane 0 because this will be the starting
      // value for which the stride will be added.
      Value *Arg = Call->getArgOperand(i);
      Value *ScalarArg = getScalarValue(Arg, 0);
      VecArgs.push_back(ScalarArg);
      VecArgTys.push_back(ScalarArg->getType());
    }
  }
}

void VPOCodeGen::vectorizeCallInstruction(CallInst *Call) {

  SmallVector<Value *, 2> VecArgs;
  SmallVector<Type *, 2> VecArgTys;
  Function *CalledFunc = Call->getCalledFunction();

  // Don't attempt vector function matching for SVML.
  VectorVariant *MatchedVariant = nullptr;
  if (!TLI->isFunctionVectorizable(CalledFunc->getName())) {
    MatchedVariant = matchVectorVariant(CalledFunc, false);
    DEBUG(dbgs() << "Matched Variant: " << MatchedVariant->encode() << "\n");
  }

  vectorizeCallArgs(Call, MatchedVariant, VecArgs, VecArgTys);

  Function *VectorF = getOrInsertVectorFunction(CalledFunc, VF, VecArgTys, TLI,
                                                Intrinsic::not_intrinsic,
                                                MatchedVariant,
                                                false/*non-masked*/);
  if (MatchedVariant)
    delete MatchedVariant;

  assert(VectorF && "Can't create vector function.");
  CallInst *VecCall = Builder.CreateCall(VectorF, VecArgs);

  if (isa<FPMathOperator>(VecCall))
    VecCall->copyFastMathFlags(Call);

  Loop *Lp = LI->getLoopFor(Call->getParent());
  analyzeCallArgMemoryReferences(Call, VecCall, TLI, PSE.getSE(), Lp);

  WidenMap[cast<Value>(Call)] = VecCall;
}

void VPOCodeGen::vectorizeInstruction(Instruction *Inst) {
  // Diego: Why are we blindly vectorizing any instruction?
  //if (isUniformAfterVectorization(Inst, VF)) {
  //  return;
  //}

  switch (Inst->getOpcode()) {
  case Instruction::GetElementPtr: {
    // Consecutive Load/Store will clone the GEP
    if (all_of(Inst->users(), [&](User *U) -> bool {
          return getPointerOperand(U) == Inst;
        }) && Legal->isConsecutivePtr(Inst))
      break;
    if (all_of(Inst->users(), [&](User *U) -> bool {
          return getPointerOperand(U) == Inst &&
                 Legal->isUniformForTheLoop(U);
      })) {
      serializeInstruction(Inst);
      break;
    }

    // Create the vector GEP, keeping all constant arguments scalar.
    GetElementPtrInst *GEP = dyn_cast<GetElementPtrInst>(Inst);
    if (all_of(GEP->operands(), [&](Value *Op) -> bool {
          return Legal->isLoopInvariant(Op) && !Legal->isLoopPrivate(Op);
      })) {
      auto *Clone = Builder.Insert(GEP->clone());
      WidenMap[cast<Value>(Inst)] = Builder.CreateVectorSplat(VF, Clone);
    } else {
      SmallVector<Value*, 4> OpsV;

      for (Value *Op : GEP->operands()) {
        // Mixing up scalar/vector operands trips up downstream optimizations,
        // vectorize all operands.
        if (Legal->isLoopInvariant(Op) && !Legal->isLoopPrivate(Op))
          OpsV.push_back(Op);
        else
          OpsV.push_back(getVectorValue(Op));
      }
      Value *GepBasePtr = OpsV[0];
      OpsV.erase(OpsV.begin());
      GetElementPtrInst *VectorGEP = cast<GetElementPtrInst>(
          Builder.CreateGEP(GepBasePtr, OpsV, "mm_vectorGEP"));
      VectorGEP->setIsInBounds(GEP->isInBounds());
      WidenMap[cast<Value>(Inst)] = VectorGEP;
    }
    break;
  }

  case Instruction::ZExt:
  case Instruction::SExt:
  case Instruction::FPToUI:
  case Instruction::FPToSI:
  case Instruction::FPExt:
  case Instruction::PtrToInt:
  case Instruction::IntToPtr:
  case Instruction::SIToFP:
  case Instruction::UIToFP:
  case Instruction::Trunc:
  case Instruction::FPTrunc: {
    CastInst *CI = dyn_cast<CastInst>(Inst);
    auto Opcode = CI->getOpcode();

    /// Vectorize casts.
    Type *ScalTy = CI->getType();
    Type *VecTy = VectorType::get(ScalTy, VF);
    Value *ScalOp = Inst->getOperand(0);
    Value *VecOp = getVectorValue(ScalOp);
    WidenMap[cast<Value>(Inst)] = Builder.CreateCast(Opcode, VecOp, VecTy);

    // If the cast is a SExt/ZExt of a unit step linear item, add the cast value to
    // UnitStepLinears - so that we can use it to infer information about unit stride
    // loads/stores. For the scalar cast value 
    Value *NewScalar;
    int LinStep;

    if ((Opcode == Instruction::SExt || Opcode == Instruction::ZExt) &&
        Legal->isUnitStepLinear(ScalOp, &LinStep, &NewScalar)) {
      // NewScalar is the scalar linear iterm corresponding to ScalOp - apply cast 
      auto ScalCast = Builder.CreateCast(Opcode, NewScalar, ScalTy);
      addUnitStepLinear(Inst, ScalCast, LinStep);
    }
    break;
  }

  case Instruction::BitCast:
    vectorizeBitCast(Inst);
    break;
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction::FSub:
  case Instruction::Mul:
  case Instruction::FMul:
  case Instruction::UDiv:
  case Instruction::SDiv:
  case Instruction::FDiv:
  case Instruction::URem:
  case Instruction::SRem:
  case Instruction::FRem:
  case Instruction::Shl:
  case Instruction::LShr:
  case Instruction::AShr:
  case Instruction::And:
  case Instruction::Or:
  case Instruction::Xor: {

    if (isUniformAfterVectorization(Inst, VF)) {
      serializeInstruction(Inst);
      break;
    }
    // Widen binary operands
    BinaryOperator *BinOp = dyn_cast<BinaryOperator>(Inst);
    Value *A = getVectorValue(Inst->getOperand(0));
    Value *B = getVectorValue(Inst->getOperand(1));

    // Create wide instruction
    Value *V = Builder.CreateBinOp(BinOp->getOpcode(), A, B);

    if (BinaryOperator *VecOp = dyn_cast<BinaryOperator>(V))
      VecOp->copyIRFlags(BinOp);

    WidenMap[cast<Value>(Inst)] = V;
    break;
  }

  case Instruction::Load: {
    vectorizeLoadInstruction(Inst, true);
    break;
  }

  case Instruction::Store: {
    vectorizeStoreInstruction(Inst, true);
    break;
  }

  case Instruction::PHI: {
    vectorizePHIInstruction(Inst);
    break;
  }
  case Instruction::ExtractElement:
    vectorizeExtractElement(Inst);
    break;
  case Instruction::InsertElement:
    vectorizeInsertElement(Inst);
    break;
  case Instruction::ShuffleVector:
    vectorizeShuffle(Inst);
    break;
  case Instruction::ICmp: {
    auto *Cmp = dyn_cast<ICmpInst>(Inst);
    Value *A = getVectorValue(Cmp->getOperand(0));
    Value *B = getVectorValue(Cmp->getOperand(1));
    WidenMap[Inst] = Builder.CreateICmp(Cmp->getPredicate(), A, B);
    break;
  }

  case Instruction::FCmp: {
    auto *FCmp = dyn_cast<FCmpInst>(Inst);
    Value *A = getVectorValue(FCmp->getOperand(0));
    Value *B = getVectorValue(FCmp->getOperand(1));
    Value *NewFCmp = Builder.CreateFCmp(FCmp->getPredicate(), A, B);
    cast<FCmpInst>(NewFCmp)->copyFastMathFlags(FCmp);
    WidenMap[Inst] = NewFCmp;
    break;
  }

  case Instruction::Select: {
    vectorizeSelectInstruction(Inst);
    break;
  }

  case Instruction::Call: {
    // TODO: Masked vector function call support needs to be added.
    CallInst *Call = cast<CallInst>(Inst);
    Function *F = Call->getCalledFunction();
    StringRef CalledFunc = F->getName();
    if (TLI->isFunctionVectorizable(CalledFunc, VF) ||
        F->hasFnAttribute("vector-variants"))
      vectorizeCallInstruction(Call);
    else
      serializeWithPredication(Call);
    break;
  }

#if 0
  case Instruction::Trunc: {
    CastInst *CI = dyn_cast<CastInst>(Inst);
    /// Optimize the special case where the source is the induction
    /// variable. Notice that we can only optimize the 'trunc' case
    /// because: a. FP conversions lose precision, b. sext/zext may wrap,
    /// c. other casts depend on pointer size.
    if (CI->getOperand(0) == OldInduction) {
      Value *ScalarCast = Builder.CreateCast(CI->getOpcode(), Induction,
                                             CI->getType());
      Value *Broadcasted = getBroadcastInstrs(ScalarCast, Builder);
      Constant *Stride = ConstantInt::getSigned(CI->getType(), 1);
      Value *tv = getStrideVector(Broadcasted, 0, Stride, Builder);
      WidenMap[cast<Value>(Inst)] = tv;
      
    }
    break;
  }
#endif

  default: {
    serializeInstruction(Inst);
    break;
  }
  }
}
Value *VPOCodeGen::getOrCreateVectorTripCount(Loop *L) {
  if (VectorTripCount)
    return VectorTripCount;

  Value *TC = getOrCreateTripCount(L);
  IRBuilder<> Builder(L->getLoopPreheader()->getTerminator());

  // Now we need to generate the expression for the part of the loop that the
  // vectorized body will execute. This is equal to N - (N % Step) if scalar
  // iterations are not required for correctness, or N - Step, otherwise. Step
  // is equal to the vectorization factor (number of SIMD elements) times the
  // unroll factor (number of SIMD instructions).
  Constant *Step = ConstantInt::get(TC->getType(), VF);
  Value *R = Builder.CreateURem(TC, Step, "n.mod.vf");

  VectorTripCount = Builder.CreateSub(TC, R, "n.vec");
  return VectorTripCount;
}

void VPOCodeGen::collectTriviallyDeadInstructions(
    Loop *OrigLoop, VPOVectorizationLegality *Legal,
    SmallPtrSetImpl<Instruction *> &DeadInstructions) {
  BasicBlock *Latch = OrigLoop->getLoopLatch();

  // We create new control-flow for the vectorized loop, so the original
  // condition will be dead after vectorization if it's only used by the
  // branch.
  auto *Cmp = dyn_cast<Instruction>(Latch->getTerminator()->getOperand(0));
  if (Cmp && Cmp->hasOneUse())
    DeadInstructions.insert(Cmp);

  // We create new "steps" for induction variable updates to which the original
  // induction variables map. An original update instruction will be dead if
  // all its users except the induction variable are dead.
  for (auto &Induction : *Legal->getInductionVars()) {
    PHINode *Ind = Induction.first;
    auto *IndUpdate = cast<Instruction>(Ind->getIncomingValueForBlock(Latch));
    if (all_of(IndUpdate->users(), [&](User *U) -> bool {
      return U == Ind || DeadInstructions.count(cast<Instruction>(U));
    }))
      DeadInstructions.insert(IndUpdate);
  }
}

bool VPOVectorizationLegality::isLoopInvariant(Value *V) {
  // Each lane gets its own copy of the private value
  if (isLoopPrivate(V))
    return false;
  
  return (PSE.getSE()->isLoopInvariant(PSE.getSCEV(V), TheLoop));
}

bool VPOVectorizationLegality::isLoopPrivate(Value *V) const {
  return Privates.count(getPtrThruBitCast(V)) ||
      isInMemoryReduction(V);
}

bool VPOVectorizationLegality::isInMemoryReduction(Value *V) const {
  V = getPtrThruBitCast(V);
  return isa<AllocaInst>(V) &&
    InMemoryReductions.count(cast<AllocaInst>(V));
}


bool VPOVectorizationLegality::isLastPrivate(Value *V) const {
  return LastPrivates.count(getPtrThruBitCast(V)) != 0;
}

bool VPOVectorizationLegality::isCondLastPrivate(Value *V) const {
  return CondLastPrivates.count(getPtrThruBitCast(V));
}

bool VPOVectorizationLegality::isLinear(Value *Val, int *Step) {
  auto PtrThruBitCast = getPtrThruBitCast(Val);
  if (Linears.count(PtrThruBitCast)) {
    if (Step)
      *Step = Linears[PtrThruBitCast];
    return true;
  }

  return false;
}

bool VPOVectorizationLegality::isUnitStepLinear(Value *Val, int *Step, Value **NewScal) {
  if (UnitStepLinears.count(Val)) {
    auto NewValStep = UnitStepLinears[Val];
    if (Step)
      *Step = NewValStep.second;
    if (NewScal)
      *NewScal = NewValStep.first;

    return true;
  }

  return false;
}

int VPOVectorizationLegality::isConsecutivePtr(Value *Ptr) {
  // An in memory loop private is expanded to a vector of consecutive ptrs.
  if (isLoopPrivate(Ptr))
    return 1;
  
  const ValueToValueMap &Strides = ValueToValueMap();

  int Stride = getPtrStride(PSE, Ptr, TheLoop, Strides, false);
  if (Stride == 1 || Stride == -1)
    return Stride;

  // See if we can use linear information to check if we have a consecutive pointer
  auto *PtrTy = cast<PointerType>(Ptr->getType());
  if (PtrTy->getElementType()->isAggregateType())
    return 0;

  // We are looking for a GEP whose last operand is a unit step linear item
  if (!isa<GetElementPtrInst>(Ptr))
    return 0;

  auto Gep = cast<GetElementPtrInst>(Ptr);
  unsigned NumOperands = Gep->getNumOperands();
  Value *LastGepOper = Gep->getOperand(NumOperands - 1);
  
  // If the last operand is not a unit stride linear bail out
  int LinStep = 0;

  if (!isUnitStepLinear(LastGepOper, &LinStep))  
    return 0;

  // If any of the Gep operands other than the last one is not loop invariant -
  // bail out
  for (unsigned Index = 0; Index < NumOperands - 1; ++Index) {
    auto Op = Gep->getOperand(Index);
    if (!isLoopInvariant(Op))
      return 0;
  }

  return LinStep;
}

bool VPOVectorizationLegality::isInductionVariable(const Value *V) {
  Value *In0 = const_cast<Value *>(V);
  PHINode *PN = dyn_cast_or_null<PHINode>(In0);
  if (!PN)
    return false;
  return Inductions.count(PN);
}

uint64_t VPOCodeGen::getConstTripCount() const {
  if (auto *C = dyn_cast<ConstantInt>(TripCount))
    return C->getZExtValue();
  return 0;
}

Value *VPOCodeGen::getOrCreateTripCount(Loop *L) {
  if (TripCount)
    return TripCount;

  IRBuilder<> Builder(L->getLoopPreheader()->getTerminator());
  // Find the loop boundaries.
  PredicatedScalarEvolution &PSE = Legal->getPSE();
  const SCEV *BackedgeTakenCount = PSE.getBackedgeTakenCount();
  assert(BackedgeTakenCount != PSE.getSE()->getCouldNotCompute() &&
         "Invalid loop count");

  Type *IdxTy = Legal->getWidestInductionType();

  // The exit count might have the type of i64 while the phi is i32. This can
  // happen if we have an induction variable that is sign extended before the
  // compare. The only way that we get a backedge taken count is that the
  // induction variable was signed and as such will not overflow. In such a case
  // truncation is legal.
  if (BackedgeTakenCount->getType()->getPrimitiveSizeInBits() >
      IdxTy->getPrimitiveSizeInBits())
    BackedgeTakenCount = PSE.getSE()->getTruncateOrNoop(BackedgeTakenCount, IdxTy);
  BackedgeTakenCount = PSE.getSE()->getNoopOrZeroExtend(BackedgeTakenCount, IdxTy);

  // Get the total trip count from the count by adding 1.
  const SCEV *ExitCount = PSE.getSE()->getAddExpr(
      BackedgeTakenCount, PSE.getSE()->getOne(BackedgeTakenCount->getType()));

  const DataLayout &DL = L->getHeader()->getModule()->getDataLayout();

  // Expand the trip count and place the new instructions in the preheader.
  // Notice that the pre-header does not change, only the loop body.
  SCEVExpander Exp(*PSE.getSE(), DL, "induction");

  // Count holds the overall loop count (N).
  TripCount = Exp.expandCodeFor(ExitCount, ExitCount->getType(),
                                L->getLoopPreheader()->getTerminator());

  if (TripCount->getType()->isPointerTy())
    TripCount =
        CastInst::CreatePointerCast(TripCount, IdxTy, "exitcount.ptrcnt.to.int",
                                    L->getLoopPreheader()->getTerminator());

  return TripCount;
}

void VPOVectorizationLegality::collectLoopUniformsForAnyVF() {
  // We now know that the loop is vectorizable!
  // Collect instructions inside the loop that will remain uniform after
  // vectorization.

  // Global values, params and instructions outside of current loop are out of
  // scope.
  auto isOutOfScope = [&](Value *V) -> bool {
    Instruction *I = dyn_cast<Instruction>(V);
    return (!I || !TheLoop->contains(I));
  };

  SetVector<Instruction *> Worklist;
  BasicBlock *Latch = TheLoop->getLoopLatch();

  // Start with the conditional branch. If the branch condition is an
  // instruction contained in the loop that is only used by the branch, it is
  // uniform.
  auto *Cmp = dyn_cast<Instruction>(Latch->getTerminator()->getOperand(0));
  if (Cmp && TheLoop->contains(Cmp) && Cmp->hasOneUse()) {
    Worklist.insert(Cmp);
    DEBUG(dbgs() << "LV: Found uniform instruction: " << *Cmp << "\n");
  }

  for (auto *BB : TheLoop->blocks())
    for (auto &I : *BB) {
      auto isInnerLoopInduction = [&](PHINode *Phi, const Loop *&InnerL) -> bool {
        if (isInductionVariable(Phi))
          return false;

        if (!PSE.getSE()->isSCEVable(Phi->getType()))
          return false;

        const SCEV *PhiScev = PSE.getSCEV(Phi);
        if (auto AR = dyn_cast<SCEVAddRecExpr>(PhiScev)) {
          InnerL = AR->getLoop();
          return (InnerL != TheLoop && TheLoop->contains(InnerL));
        }
        return false;
      };
      // Add non-induction phis to the list
      if (auto Phi = dyn_cast<PHINode>(&I)) {
        const Loop *InnerLoop = nullptr;
        if (isInnerLoopInduction(Phi, InnerLoop)) {
          Worklist.insert(Phi);
          DEBUG(dbgs() << "LV: Found uniform instruction: " << *Phi << "\n");
          BasicBlock *InnerLoopLatch = InnerLoop->getLoopLatch();
          BranchInst *Br = cast<BranchInst>(InnerLoopLatch->getTerminator());
          Worklist.insert(Br);
          auto *Cmp = dyn_cast<Instruction>(Br->getOperand(0));
          if (Cmp && InnerLoop->contains(Cmp) && Cmp->hasOneUse()) {
            Worklist.insert(Cmp);
            DEBUG(dbgs() << "LV: Found uniform instruction: " << *Cmp << "\n");
          }
          auto *IndUpdate =
            cast<Instruction>(Phi->getIncomingValueForBlock(InnerLoopLatch));
          DEBUG(dbgs() << "LV: Found uniform instruction: " << *IndUpdate <<
                "\n");
          Worklist.insert(IndUpdate);
        }
      } else if (auto Br = dyn_cast<BranchInst>(&I)) {
        if (!Br->isConditional())
          continue;
        Value *Cond = Br->getCondition();
        if (TheLoop->isLoopInvariant(Cond))
          Worklist.insert(Br);
      }

      // Load with loop invariant pointer
      if (auto Ptr = getPointerOperand(&I)) {
        const SCEV *PtrScevAtTheLoopScope =
          PSE.getSE()->getSCEVAtScope(Ptr, TheLoop);
        if (PSE.getSE()->isLoopInvariant(PtrScevAtTheLoopScope, TheLoop) &&
            isa<LoadInst>(&I))
          Worklist.insert(&I);
      }
    }
  // Expand Worklist in topological order: whenever a new instruction
  // is added , its users should be either already inside Worklist, or
  // out of scope. It ensures a uniform instruction will only be used
  // by uniform instructions or out of scope instructions.
  unsigned idx = 0;
  while (idx != Worklist.size()) {
    Instruction *I = Worklist[idx++];

    for (auto OV : I->operand_values()) {
      if (auto *OI = dyn_cast<Instruction>(OV)) {
        if (all_of(OI->users(), [&](User *U) -> bool {
              return isOutOfScope(U) || Worklist.count(cast<Instruction>(U));
            })) {
          Worklist.insert(OI);
          DEBUG(dbgs() << "LV: Found uniform instruction: " << *OI << "\n");
          if (all_of(OV->users(), [&](User *U) -> bool {
                return isOutOfScope(U) || Worklist.count(cast<Instruction>(U));
              })) {
            Worklist.insert(OI);
            DEBUG(dbgs() << "LV: Found uniform instruction: " << *OI << "\n");
          }
        }
      }
    }
  }

  UniformForAnyVF.insert(Worklist.begin(), Worklist.end());
}


void VPOCodeGen::collectLoopUniforms(unsigned VF) {

  // We should not collect Uniforms more than once per VF. Right now,
  // this function is called from collectUniformsAndScalars(), which 
  // already does this check. Collecting Uniforms for VF=1 does not make any
  // sense.

  assert(VF >= 2 && !Uniforms.count(VF) &&
          "This function should not be visited twice for the same VF");

  // Visit the list of Uniforms. If we'll not find any uniform value, we'll 
  // not analyze again.  Uniforms.count(VF) will return 1.
  Uniforms[VF].clear();

  // We now know that the loop is vectorizable!
  // Collect instructions inside the loop that will remain uniform after
  // vectorization.

  // Global values, params and instructions outside of current loop are out of
  // scope.
  auto isOutOfScope = [&](Value *V) -> bool {
    Instruction *I = dyn_cast<Instruction>(V);
    return (!I || !OrigLoop->contains(I));
  };

  SetVector<Instruction *> Worklist;

  // Start from Uniforms that alerady collected for any VF.
  //for (Instruction *I : Legal->uniforms())
    Worklist.insert(Legal->UniformForAnyVF.begin(), Legal->UniformForAnyVF.end());

  // Holds consecutive and consecutive-like pointers. Consecutive-like pointers
  // are pointers that are treated like consecutive pointers during
  // vectorization. The pointer operands of interleaved accesses are an
  // example.
  SmallSetVector<Instruction *, 8> ConsecutiveLikePtrs;

  // Holds pointer operands of instructions that are possibly non-uniform.
  SmallPtrSet<Instruction *, 8> PossibleNonUniformPtrs;

  // Iterate over the instructions in the loop, and collect all
  // consecutive-like pointer operands in ConsecutiveLikePtrs. If it's possible
  // that a consecutive-like pointer operand will be scalarized, we collect it
  // in PossibleNonUniformPtrs instead. We use two sets here because a single
  // getelementptr instruction can be used by both vectorized and scalarized
  // memory instructions. For example, if a loop loads and stores from the same
  // location, but the store is conditional, the store will be scalarized, and
  // the getelementptr won't remain uniform.
  for (auto *BB : OrigLoop->blocks())
    for (auto &I : *BB) {
      
      // If there's no pointer operand, there's nothing to do.
      auto *Ptr = dyn_cast_or_null<Instruction>(getPointerOperand(&I));
      if (!Ptr)
        continue;

      // True if all users of Ptr are memory accesses that have Ptr as their
      // pointer operand.
      auto UsersAreMemAccesses = all_of(Ptr->users(), [&](User *U) -> bool {
        return getPointerOperand(U) == Ptr;
      });

      // Ensure the memory instruction will not be scalarized or used by
      // gather/scatter, making its pointer operand non-uniform. If the pointer
      // operand is used by any instruction other than a memory access, we
      // conservatively assume the pointer operand may be non-uniform.
      if (!UsersAreMemAccesses || !Legal->isConsecutivePtr(Ptr))
        PossibleNonUniformPtrs.insert(Ptr);

      // If the memory instruction will be vectorized and its pointer operand
      // is consecutive-like, or interleaving - the pointer operand should
      // remain uniform.
      else
        ConsecutiveLikePtrs.insert(Ptr);
    }

  // Add to the Worklist all consecutive and consecutive-like pointers that
  // aren't also identified as possibly non-uniform.
  for (auto *V : ConsecutiveLikePtrs)
    if (!PossibleNonUniformPtrs.count(V)) {
      DEBUG(dbgs() << "LV: Found uniform instruction: " << *V << "\n");
      Worklist.insert(V);
    }

  // Expand Worklist in topological order: whenever a new instruction
  // is added , its users should be either already inside Worklist, or
  // out of scope. It ensures a uniform instruction will only be used
  // by uniform instructions or out of scope instructions.
  unsigned idx = 0;
  while (idx != Worklist.size()) {
    Instruction *I = Worklist[idx++];

    for (auto OV : I->operand_values()) {
      if (auto *OI = dyn_cast<Instruction>(OV)) {
        if (all_of(OI->users(), [&](User *U) -> bool {
              return isOutOfScope(U) || Worklist.count(cast<Instruction>(U));
            })) {
          Worklist.insert(OI);
          DEBUG(dbgs() << "LV: Found uniform instruction: " << *OI << "\n");
        }
      }
    }
  }

  // Returns true if Ptr is the pointer operand of a memory access instruction
  // I, and I is known to not require scalarization.
  auto isVectorizedMemAccessUse = [&](Instruction *I, Value *Ptr) -> bool {
    return getPointerOperand(I) == Ptr && Legal->isConsecutivePtr(Ptr);
  };

  // For an instruction to be added into Worklist above, all its users inside
  // the loop should also be in Worklist. However, this condition cannot be
  // true for phi nodes that form a cyclic dependence. We must process phi
  // nodes separately. An induction variable will remain uniform if all users
  // of the induction variable and induction variable update remain uniform.
  // The code below handles both pointer and non-pointer induction variables.
  for (auto &Induction : *Legal->getInductionVars()) {
    auto *Ind = Induction.first;
    BasicBlock *Latch = OrigLoop->getLoopLatch();
    auto *IndUpdate = cast<Instruction>(Ind->getIncomingValueForBlock(Latch));

    // Determine if all users of the induction variable are uniform after
    // vectorization.
    auto UniformInd = all_of(Ind->users(), [&](User *U) -> bool {
      auto *I = cast<Instruction>(U);
      return I == IndUpdate || !OrigLoop->contains(I) || Worklist.count(I) ||
        isVectorizedMemAccessUse(I, Ind);
    });
    if (!UniformInd)
      continue;

    // Determine if all users of the induction variable update instruction are
    // uniform after vectorization.
    auto UniformIndUpdate = all_of(IndUpdate->users(), [&](User *U) -> bool {
      auto *I = cast<Instruction>(U);
      return I == Ind || !OrigLoop->contains(I) || Worklist.count(I) ||
        isVectorizedMemAccessUse(I, IndUpdate);
    });
    if (!UniformIndUpdate)
      continue;

    // The induction variable and its update instruction will remain uniform.
    Worklist.insert(Ind);
    Worklist.insert(IndUpdate);
    DEBUG(dbgs() << "LV: Found uniform instruction: " << *Ind << "\n");
    DEBUG(dbgs() << "LV: Found uniform instruction: " << *IndUpdate << "\n");
  }

  Uniforms[VF].insert(Worklist.begin(), Worklist.end());
}

void VPOCodeGen::collectUniformsAndScalars(unsigned VF) {
  collectLoopUniforms(VF);
}

/// Returns true if \p I is known to be uniform after vectorization.
bool VPOCodeGen::isUniformAfterVectorization(Instruction *I,
                                             unsigned VF) const {
  assert(Uniforms.count(VF) && "VF not yet analyzed for uniformity");
  auto UniformsPerVF = Uniforms.find(VF);
  return UniformsPerVF->second.count(I);
}

// Fix up external users of the induction variable. At this point, we are
// in LCSSA form, with all external PHIs that use the IV having one input value,
// coming from the remainder loop. We need those PHIs to also have a correct
// value for the IV when arriving directly from the middle block.
void VPOCodeGen::fixupIVUsers(PHINode *OrigPhi,
                              const InductionDescriptor &II,
                              Value *CountRoundDown, Value *EndValue,
                              BasicBlock *MiddleBlock) {
  // There are two kinds of external IV usages - those that use the value
  // computed in the last iteration (the PHI) and those that use the penultimate
  // value (the value that feeds into the phi from the loop latch).
  // We allow both, but they, obviously, have different values.

  assert(OrigLoop->getExitBlock() && "Expected a single exit block");

  DenseMap<Value *, Value *> MissingVals;

  // An external user of the last iteration's value should see the value that
  // the remainder loop uses to initialize its own IV.
  Value *PostInc = OrigPhi->getIncomingValueForBlock(OrigLoop->getLoopLatch());
  for (User *U : PostInc->users()) {
    Instruction *UI = cast<Instruction>(U);
    if (!OrigLoop->contains(UI)) {
      assert(isa<PHINode>(UI) && "Expected LCSSA form");
      MissingVals[UI] = EndValue;
    }
  }

  // An external user of the penultimate value need to see EndValue - Step.
  // The simplest way to get this is to recompute it from the constituent SCEVs,
  // that is Start + (Step * (CRD - 1)).
  for (User *U : OrigPhi->users()) {
    auto *UI = cast<Instruction>(U);
    if (!OrigLoop->contains(UI)) {
      const DataLayout &DL =
        OrigLoop->getHeader()->getModule()->getDataLayout();
      assert(isa<PHINode>(UI) && "Expected LCSSA form");

      IRBuilder<> B(MiddleBlock->getTerminator());
      Value *CountMinusOne = B.CreateSub(
        CountRoundDown, ConstantInt::get(CountRoundDown->getType(), 1));
      Value *CMO = B.CreateSExtOrTrunc(CountMinusOne, II.getStep()->getType(),
                                       "cast.cmo");
      Value *Escape = II.transform(B, CMO, PSE.getSE(), DL);
      Escape->setName("ind.escape");
      MissingVals[UI] = Escape;
    }
  }

  for (auto &I : MissingVals) {
    PHINode *PHI = cast<PHINode>(I.first);
    // One corner case we have to handle is two IVs "chasing" each-other,
    // that is %IV2 = phi [...], [ %IV1, %latch ]
    // In this case, if IV1 has an external use, we need to avoid adding both
    // "last value of IV1" and "penultimate value of IV2". So, verify that we
    // don't already have an incoming value for the middle block.
    if (PHI->getBasicBlockIndex(MiddleBlock) == -1)
      PHI->addIncoming(I.second, MiddleBlock);
  }
}

void VPOCodeGen::fixLCSSAPHIs() {
  for (Instruction &LEI : *LoopExitBlock) {
    auto *LCSSAPhi = dyn_cast<PHINode>(&LEI);
    if (!LCSSAPhi)
      break;
    if (LCSSAPhi->getNumIncomingValues() == 1)
      LCSSAPhi->addIncoming(UndefValue::get(LCSSAPhi->getType()),
                            LoopMiddleBlock);
  }
}

void VPOCodeGen::predicateInstructions() {

  // For each instruction I marked for predication on value C, split I into its
  // own basic block to form an if-then construct over C. Since I may be fed by
  // an extractelement instruction or other scalar operand, we try to
  // iteratively sink its scalar operands into the predicated block. If I feeds
  // an insertelement instruction, we try to move this instruction into the
  // predicated block as well. For non-void types, a phi node will be created
  // for the resulting value (either vector or scalar).
  //
  // So for some predicated instruction, e.g. the conditional sdiv in:
  //
  // for.body:
  //  ...
  //  %add = add nsw i32 %mul, %0
  //  %cmp5 = icmp sgt i32 %2, 7
  //  br i1 %cmp5, label %if.then, label %if.end
  //
  // if.then:
  //  %div = sdiv i32 %0, %1
  //  br label %if.end
  //
  // if.end:
  //  %x.0 = phi i32 [ %div, %if.then ], [ %add, %for.body ]
  //
  // the sdiv at this point is scalarized and if-converted using a select.
  // The inactive elements in the vector are not used, but the predicated
  // instruction is still executed for all vector elements, essentially:
  //
  // vector.body:
  //  ...
  //  %17 = add nsw <2 x i32> %16, %wide.load
  //  %29 = extractelement <2 x i32> %wide.load, i32 0
  //  %30 = extractelement <2 x i32> %wide.load51, i32 0
  //  %31 = sdiv i32 %29, %30
  //  %32 = insertelement <2 x i32> undef, i32 %31, i32 0
  //  %35 = extractelement <2 x i32> %wide.load, i32 1
  //  %36 = extractelement <2 x i32> %wide.load51, i32 1
  //  %37 = sdiv i32 %35, %36
  //  %38 = insertelement <2 x i32> %32, i32 %37, i32 1
  //  %predphi = select <2 x i1> %26, <2 x i32> %38, <2 x i32> %17
  //
  // Predication will now re-introduce the original control flow to avoid false
  // side-effects by the sdiv instructions on the inactive elements, yielding
  // (after cleanup):
  //
  // vector.body:
  //  ...
  //  %5 = add nsw <2 x i32> %4, %wide.load
  //  %8 = icmp sgt <2 x i32> %wide.load52, <i32 7, i32 7>
  //  %9 = extractelement <2 x i1> %8, i32 0
  //  br i1 %9, label %pred.sdiv.if, label %pred.sdiv.continue
  //
  // pred.sdiv.if:
  //  %10 = extractelement <2 x i32> %wide.load, i32 0
  //  %11 = extractelement <2 x i32> %wide.load51, i32 0
  //  %12 = sdiv i32 %10, %11
  //  %13 = insertelement <2 x i32> undef, i32 %12, i32 0
  //  br label %pred.sdiv.continue
  //
  // pred.sdiv.continue:
  //  %14 = phi <2 x i32> [ undef, %vector.body ], [ %13, %pred.sdiv.if ]
  //  %15 = extractelement <2 x i1> %8, i32 1
  //  br i1 %15, label %pred.sdiv.if54, label %pred.sdiv.continue55
  //
  // pred.sdiv.if54:
  //  %16 = extractelement <2 x i32> %wide.load, i32 1
  //  %17 = extractelement <2 x i32> %wide.load51, i32 1
  //  %18 = sdiv i32 %16, %17
  //  %19 = insertelement <2 x i32> %14, i32 %18, i32 1
  //  br label %pred.sdiv.continue55
  //
  // pred.sdiv.continue55:
  //  %20 = phi <2 x i32> [ %14, %pred.sdiv.continue ], [ %19, %pred.sdiv.if54 ]
  //  %predphi = select <2 x i1> %8, <2 x i32> %20, <2 x i32> %5

  for (auto KV : PredicatedInstructions) {
    BasicBlock::iterator I(KV.first);
    BasicBlock *Head = I->getParent();
    auto *BB = SplitBlock(Head, &*std::next(I), DT, LI);
    auto *T = SplitBlockAndInsertIfThen(KV.second, &*I, /*Unreachable=*/false,
                                        /*BranchWeights=*/nullptr, DT, LI);
    I->moveBefore(T);
    //sinkScalarOperands(&*I);

    I->getParent()->setName(Twine("pred.") + I->getOpcodeName() + ".if");
    BB->setName(Twine("pred.") + I->getOpcodeName() + ".continue");

    // If the instruction is non-void create a Phi node at reconvergence point.
    if (!I->getType()->isVoidTy()) {
      Value *IncomingTrue = nullptr;
      Value *IncomingFalse = nullptr;

      if (I->hasOneUse() && isa<InsertElementInst>(*I->user_begin())) {
        // If the predicated instruction is feeding an insert-element, move it
        // into the Then block; Phi node will be created for the vector.
        InsertElementInst *IEI = cast<InsertElementInst>(*I->user_begin());
        IEI->moveBefore(T);
        IncomingTrue = IEI; // the new vector with the inserted element.
        IncomingFalse = IEI->getOperand(0); // the unmodified vector
      } else {
        // Phi node will be created for the scalar predicated instruction.
        IncomingTrue = &*I;
        IncomingFalse = UndefValue::get(I->getType());
      }

      BasicBlock *PostDom = I->getParent()->getSingleSuccessor();
      assert(PostDom && "Then block has multiple successors");
      PHINode *Phi =
        PHINode::Create(IncomingTrue->getType(), 2, "", &PostDom->front());
      IncomingTrue->replaceAllUsesWith(Phi);
      Phi->addIncoming(IncomingFalse, Head);
      Phi->addIncoming(IncomingTrue, I->getParent());
    }
  }
}

// Unconditional last private variable
void VPOCodeGen::writePrivateValAfterLoop(Value *OrigPrivate) {
  IRBuilder<> Builder(LoopMiddleBlock->getTerminator());
  Value *PtrToVec = LoopPrivateWidenMap[OrigPrivate];
  Type *Int64Ty = Type::getInt64Ty(LoopMiddleBlock->getContext());

  Value *LastUpdatedLane = ConstantInt::get(Int64Ty, VF - 1);
  Value *PtrToFirstElt = Builder.CreateBitCast(PtrToVec, OrigPrivate->getType());
  Value *PtrToLane = Builder.CreateGEP(PtrToFirstElt, LastUpdatedLane,
                                       "LastUpdatedLanePtr");
  Value *ValueToWriteIn = Builder.CreateLoad(PtrToLane, "LastVal");
  Builder.CreateStore(ValueToWriteIn, OrigPrivate);
}

void VPOCodeGen::completeInMemoryReductions() {
  for (auto V : (*Legal->getInMemoryReductionVars())) {
    AllocaInst *Ptr = V.first;
    RecurrenceDescriptor::RecurrenceKind Kind = V.second.first;
    RecurrenceDescriptor::MinMaxRecurrenceKind Mrk = V.second.second;
    Value *Res = buildInMemoryReductionTail(Ptr, Kind, Mrk);
    ReductionEofLoopVal[Ptr] = Res;
  }
}

Value *VPOCodeGen::buildInMemoryReductionTail(
    Value *OrigRedV,
    RecurrenceDescriptor::RecurrenceKind Kind,
    RecurrenceDescriptor::MinMaxRecurrenceKind Mrk) {

  IRBuilder<> Builder(&*LoopMiddleBlock->getFirstInsertionPt());
  Value *PtrToVec = LoopPrivateWidenMap[OrigRedV];
  Value *WideLoad = Builder.CreateLoad(PtrToVec, "Red.vec");
  Value *ScalarV = reduceVector(WideLoad, Kind, Mrk, Builder);
  Builder.CreateStore(ScalarV, OrigRedV);
  return ScalarV;
}


Value *VPOCodeGen::getLastLaneFromMask(Value *MaskPtr) {

  Value *MaskValue = Builder.CreateLoad(MaskPtr);
  assert(MaskValue->getType()->isIntegerTy() &&
         "Mask should be an integer value");
  // Count leading zeroes. Since we always write non-zero mask,
  // the number of leading zeroes should be smaller than VF.
  Module *M = LoopMiddleBlock->getParent()->getParent();
  Value *F = Intrinsic::getDeclaration(M, Intrinsic::ctlz, MaskValue->getType());
  Value *LeadingZeroes = Builder.CreateCall(F, { MaskValue, Builder.getTrue() },
                                            "ctlz");

  // Last written lane is most-significant '1' in the mask.
  return Builder.CreateSub(ConstantInt::get(MaskValue->getType(), VF - 1),
                           LeadingZeroes, "LaneToCopyFrom");
}

void VPOCodeGen::writeCondPrivateValAfterLoop(Value *OrigPrivate) {
  Builder.SetInsertPoint(LoopMiddleBlock->getTerminator());

  // Here we keep the vector value:
  Value *PtrToVec = LoopPrivateWidenMap[OrigPrivate];

  Value *LastLane = getLastLaneFromMask(LoopPrivateLastMask[OrigPrivate]);

  // Get the type of original private value.
  Type *OrigPrivateTy = OrigPrivate->getType()->getPointerElementType();

  // Load the last lane element.
  if (!OrigPrivateTy->isVectorTy()) {
    Type *ScalarTy = OrigPrivate->getType();
    Value *PtrToLastVal =
      Builder.CreateGEP(Builder.CreateBitCast(PtrToVec, ScalarTy), LastLane);
    Value *ValueToWiteIn = Builder.CreateLoad(PtrToLastVal, "LastVal");

    // Store the result in original location of the private variable.
    Builder.CreateStore(ValueToWiteIn, OrigPrivate);
    return;
  }
  // The private variable is a vector.
  Type *EltTy = OrigPrivateTy->getVectorElementType();
  Type *PtrToEltTy = PointerType::get(EltTy, 0);
  unsigned OriginalVL = OrigPrivateTy->getVectorNumElements();
  Value *PtrToFirstElt = Builder.CreateBitCast(PtrToVec, PtrToEltTy,
                                               "PtrToFirstEltInPrivateVec");
  Value *PtrToFirstOrigElt = Builder.CreateBitCast(OrigPrivate, PtrToEltTy,
                                                   "PtrToFirstEltInOrigPrivate");
  for (unsigned i = 0; i < OriginalVL; ++i) {
    Value *LaneToCopyFrom = (i == 0) ? LastLane :
      Builder.CreateAdd(LastLane, ConstantInt::get(LastLane->getType(), i*VF),
                        "LaneToCopyFrom");
    Value *PtrToLastVal =
      Builder.CreateGEP(PtrToFirstElt, LaneToCopyFrom, "PtrInsidePrivVec");
    Value *ValueToWiteIn = Builder.CreateLoad(PtrToLastVal, "LastVal");
    Type *IdxTy = Type::getInt32Ty(OrigPrivate->getContext());
    Value *PtrToOrigLoc = (i==0) ? PtrToFirstOrigElt :
      Builder.CreateGEP(PtrToFirstOrigElt, ConstantInt::get(IdxTy, i),
                        "PtrToNextEltInOrigPrivate");
    // Store the result in original location of the private variable.
    Builder.CreateStore(ValueToWiteIn, PtrToOrigLoc);
  }
}

void VPOCodeGen::fixupLoopPrivates() {
  for (auto It : LoopPrivateWidenMap) {
    Value *OrigV = It.first;
    if (Legal->isLastPrivate(OrigV))
      writePrivateValAfterLoop(OrigV);
    else if (Legal->isCondLastPrivate(OrigV))
      writeCondPrivateValAfterLoop(OrigV);
  }
}
