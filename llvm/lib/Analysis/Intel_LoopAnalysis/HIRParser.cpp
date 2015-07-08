//===----- HIRParser.cpp - Parses SCEVs into CanonExprs -----*- C++ -*-----===//
//
// Copyright (C) 2015 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements the HIRParser pass.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/Operator.h"
#include "llvm/IR/LLVMContext.h"

#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/Analysis/LoopInfo.h"

#include "llvm/Analysis/Intel_LoopAnalysis/HIRParser.h"
#include "llvm/Analysis/Intel_LoopAnalysis/LoopFormation.h"
#include "llvm/Analysis/Intel_LoopAnalysis/ScalarSymbaseAssignment.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Passes.h"

#include "llvm/Transforms/Intel_LoopTransforms/Utils/CanonExprUtils.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/DDRefUtils.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/HLNodeUtils.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/HLUtils.h"

using namespace llvm;
using namespace llvm::loopopt;

#define DEBUG_TYPE "hir-parser"

INITIALIZE_PASS_BEGIN(HIRParser, "hir-parser", "HIR Parser", false, true)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolution)
INITIALIZE_PASS_DEPENDENCY(ScalarSymbaseAssignment)
INITIALIZE_PASS_DEPENDENCY(HIRCreation)
INITIALIZE_PASS_DEPENDENCY(LoopFormation)
INITIALIZE_PASS_END(HIRParser, "hir-parser", "HIR Parser", false, true)

char HIRParser::ID = 0;

// define the static pointer
HIRParser *HLUtils::HIRPar = nullptr;

FunctionPass *llvm::createHIRParserPass() { return new HIRParser(); }

HIRParser::HIRParser() : FunctionPass(ID), CurLevel(0) {
  initializeHIRParserPass(*PassRegistry::getPassRegistry());
}

void HIRParser::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<LoopInfoWrapperPass>();
  AU.addRequiredTransitive<ScalarEvolution>();
  AU.addRequiredTransitive<ScalarSymbaseAssignment>();
  AU.addRequiredTransitive<HIRCreation>();
  AU.addRequiredTransitive<LoopFormation>();
}

bool HIRParser::isConstantIntBlob(CanonExpr::BlobTy Blob, int64_t *Val) const {

  // Check if this Blob is of Constant Type
  const SCEVConstant *SConst = dyn_cast<SCEVConstant>(Blob);
  if (!SConst)
    return false;

  if (Val)
    *Val = getSCEVConstantValue(SConst);

  return true;
}

bool HIRParser::isTempBlob(CanonExpr::BlobTy Blob) const {
  if (auto UnknownSCEV = dyn_cast<SCEVUnknown>(Blob)) {
    Type *Ty;
    Constant *FieldNo;

    if (!UnknownSCEV->isSizeOf(Ty) && !UnknownSCEV->isAlignOf(Ty) &&
        !UnknownSCEV->isOffsetOf(Ty, FieldNo) &&
        !ScalarSA->isConstant(UnknownSCEV->getValue())) {
      return true;
    }
  }

  return false;
}

CanonExpr::BlobTy HIRParser::createBlob(int64_t Val, bool Insert,
                                        unsigned *NewBlobIndex) {
  Type *Int64Type = IntegerType::get(SE->getContext(), 64);
  auto Blob = SE->getConstant(Int64Type, Val, false);

  if (Insert) {
    unsigned BlobIndex = CanonExprUtils::findOrInsertBlob(Blob);

    if (NewBlobIndex) {
      *NewBlobIndex = BlobIndex;
    }
  }

  return Blob;
}

CanonExpr::BlobTy HIRParser::createAddBlob(CanonExpr::BlobTy LHS,
                                           CanonExpr::BlobTy RHS, bool Insert,
                                           unsigned *NewBlobIndex) {
  assert(LHS && RHS && "Blob cannot by null!");
  unsigned BlobIndex;

  auto Blob = SE->getAddExpr(LHS, RHS);

  if (Insert) {
    BlobIndex = CanonExprUtils::findOrInsertBlob(Blob);

    if (NewBlobIndex) {
      *NewBlobIndex = BlobIndex;
    }
  }

  return Blob;
}

CanonExpr::BlobTy HIRParser::createMinusBlob(CanonExpr::BlobTy LHS,
                                             CanonExpr::BlobTy RHS, bool Insert,
                                             unsigned *NewBlobIndex) {
  assert(LHS && RHS && "Blob cannot by null!");
  unsigned BlobIndex;

  auto Blob = SE->getMinusSCEV(LHS, RHS);

  if (Insert) {
    BlobIndex = CanonExprUtils::findOrInsertBlob(Blob);

    if (NewBlobIndex) {
      *NewBlobIndex = BlobIndex;
    }
  }

  return Blob;
}

CanonExpr::BlobTy HIRParser::createMulBlob(CanonExpr::BlobTy LHS,
                                           CanonExpr::BlobTy RHS, bool Insert,
                                           unsigned *NewBlobIndex) {
  assert(LHS && RHS && "Blob cannot by null!");
  unsigned BlobIndex;

  auto Blob = SE->getMulExpr(LHS, RHS);

  if (Insert) {
    BlobIndex = CanonExprUtils::findOrInsertBlob(Blob);

    if (NewBlobIndex) {
      *NewBlobIndex = BlobIndex;
    }
  }

  return Blob;
}

CanonExpr::BlobTy HIRParser::createUDivBlob(CanonExpr::BlobTy LHS,
                                            CanonExpr::BlobTy RHS, bool Insert,
                                            unsigned *NewBlobIndex) {
  assert(LHS && RHS && "Blob cannot by null!");
  unsigned BlobIndex;

  auto Blob = SE->getUDivExpr(LHS, RHS);

  if (Insert) {
    BlobIndex = CanonExprUtils::findOrInsertBlob(Blob);

    if (NewBlobIndex) {
      *NewBlobIndex = BlobIndex;
    }
  }

  return Blob;
}

CanonExpr::BlobTy HIRParser::createTruncateBlob(CanonExpr::BlobTy Blob,
                                                Type *Ty, bool Insert,
                                                unsigned *NewBlobIndex) {
  assert(Blob && "Blob cannot by null!");
  assert(Ty && "Type cannot by null!");
  unsigned BlobIndex;

  auto NewBlob = SE->getTruncateExpr(Blob, Ty);

  if (Insert) {
    BlobIndex = CanonExprUtils::findOrInsertBlob(NewBlob);

    if (NewBlobIndex) {
      *NewBlobIndex = BlobIndex;
    }
  }

  return NewBlob;
}

CanonExpr::BlobTy HIRParser::createZeroExtendBlob(CanonExpr::BlobTy Blob,
                                                  Type *Ty, bool Insert,
                                                  unsigned *NewBlobIndex) {
  assert(Blob && "Blob cannot by null!");
  assert(Ty && "Type cannot by null!");
  unsigned BlobIndex;

  auto NewBlob = SE->getZeroExtendExpr(Blob, Ty);

  if (Insert) {
    BlobIndex = CanonExprUtils::findOrInsertBlob(NewBlob);

    if (NewBlobIndex) {
      *NewBlobIndex = BlobIndex;
    }
  }

  return NewBlob;
}

CanonExpr::BlobTy HIRParser::createSignExtendBlob(CanonExpr::BlobTy Blob,
                                                  Type *Ty, bool Insert,
                                                  unsigned *NewBlobIndex) {
  assert(Blob && "Blob cannot by null!");
  assert(Ty && "Type cannot by null!");
  unsigned BlobIndex;

  auto NewBlob = SE->getSignExtendExpr(Blob, Ty);

  if (Insert) {
    BlobIndex = CanonExprUtils::findOrInsertBlob(NewBlob);

    if (NewBlobIndex) {
      *NewBlobIndex = BlobIndex;
    }
  }

  return NewBlob;
}

unsigned HIRParser::getMaxScalarSymbase() const {
  return ScalarSA->getMaxScalarSymbase();
}

unsigned HIRParser::getSymBaseForConstants() const {
  return ScalarSA->getSymBaseForConstants();
}

class HIRParser::PolynomialFinder {
private:
  bool Found;

public:
  PolynomialFinder() : Found(false) {}
  ~PolynomialFinder() {}

  bool follow(const SCEV *SC) {

    if (auto RecSCEV = dyn_cast<SCEVAddRecExpr>(SC)) {
      if (!RecSCEV->isAffine()) {
        Found = true;
      }
    }

    return !Found;
  }

  bool found() const { return Found; }
  bool isDone() const { return found(); }
};

class HIRParser::BlobLevelSetter {
private:
  HIRParser *HIRP;
  CanonExpr *CExpr;
  unsigned Level;

public:
  BlobLevelSetter(HIRParser *Par, CanonExpr *CE, unsigned NestingLevel)
      : HIRP(Par), CExpr(CE), Level(NestingLevel) {}
  ~BlobLevelSetter() {}

  bool follow(const SCEV *SC) const {

    assert(!isa<SCEVAddRecExpr>(SC) && "AddRec found inside blob!");

    if (HIRP->isTempBlob(SC)) {
      HIRP->setTempBlobLevel(cast<SCEVUnknown>(SC), CExpr, Level);
    }

    return !isDone();
  }

  bool isDone() const { return false; }
};

void HIRParser::printScalar(raw_ostream &OS, unsigned Symbase) const {
  ScalarSA->getBaseScalar(Symbase)->printAsOperand(OS, false);
}

void HIRParser::printBlob(raw_ostream &OS, CanonExpr::BlobTy Blob) const {

  if (isa<SCEVConstant>(Blob)) {
    OS << *Blob;

  } else if (auto CastSECV = dyn_cast<SCEVCastExpr>(Blob)) {
    auto SrcType = CastSECV->getOperand()->getType();
    auto DstType = CastSECV->getType();

    if (isa<SCEVZeroExtendExpr>(CastSECV)) {
      OS << "zext.";
    } else if (isa<SCEVSignExtendExpr>(CastSECV)) {
      OS << "sext.";
    } else if (isa<SCEVTruncateExpr>(CastSECV)) {
      OS << "trunc.";
    } else {
      llvm_unreachable("Unexptected casting operation!");
    }

    OS << *SrcType << "." << *DstType << "(";
    printBlob(OS, CastSECV->getOperand());
    OS << ")";

  } else if (auto NArySCEV = dyn_cast<SCEVNAryExpr>(Blob)) {
    const char *OpStr;

    if (isa<SCEVAddExpr>(NArySCEV)) {
      OS << "(";
      OpStr = " + ";
    } else if (isa<SCEVMulExpr>(NArySCEV)) {
      OS << "(";
      OpStr = " * ";
    } else if (isa<SCEVSMaxExpr>(NArySCEV)) {
      OS << "smax(";
      OpStr = ", ";
    } else if (isa<SCEVSMaxExpr>(NArySCEV)) {
      OS << "umax(";
      OpStr = ", ";
    } else {
      assert(false && "Blob contains AddRec!");
    }

    for (auto I = NArySCEV->op_begin(), E = NArySCEV->op_end(); I != E; ++I) {
      printBlob(OS, *I);

      if (std::next(I) != E) {
        OS << OpStr;
      }
    }
    OS << ")";

  } else if (auto UDivSCEV = dyn_cast<SCEVUDivExpr>(Blob)) {
    OS << "(";
    printBlob(OS, UDivSCEV->getLHS());
    OS << " /u ";
    printBlob(OS, UDivSCEV->getRHS());
    OS << ")";
  } else if (auto UnknownSCEV = dyn_cast<SCEVUnknown>(Blob)) {
    if (isTempBlob(Blob)) {
      auto Temp = UnknownSCEV->getValue();
      ScalarSA->getBaseScalar(Temp)->printAsOperand(OS, false);

    } else {
      OS << *Blob;
    }
  } else {
    llvm_unreachable("Unknown Blob type!");
  }
}

bool HIRParser::isPolyBlobDef(const Instruction *Inst) const {

  if (SE->isSCEVable(Inst->getType())) {
    auto SC = SE->getSCEV(const_cast<Instruction *>(Inst));

    // Non-GEP instructions containing non-affine(polynomial) addRecs are made
    // blobs.
    if (!isa<GetElementPtrInst>(Inst)) {
      PolynomialFinder PF;
      SCEVTraversal<PolynomialFinder> Searcher(PF);
      Searcher.visitAll(SC);

      if (PF.found()) {
        return true;
      }
    }
  }

  return false;
}

bool HIRParser::isBlobDef(const Instruction *Inst) const {

  if (SE->isSCEVable(Inst->getType())) {
    auto SC = SE->getSCEV(const_cast<Instruction *>(Inst));
    if (isa<SCEVUnknown>(SC)) {
      return true;
    } else if (isPolyBlobDef(Inst)) {
      return true;
    }
  } else {
    // If it isn't SCEVable we cannot eliminate it, so we mark it as a blob
    // definition.
    return true;
  }

  return false;
}

bool HIRParser::isRegionLiveOut(const Instruction *Inst) const {
  auto Symbase = ScalarSA->getScalarSymbase(Inst);

  if (Symbase && CurRegion->isLiveOut(Symbase)) {
    return true;
  }

  return false;
}

bool HIRParser::isRequired(const Instruction *Inst) const {

  if (isa<StoreInst>(Inst)) {
    return true;
  } else if (isa<CmpInst>(Inst)) {
    assert(hasExpectedUsers(cast<CmpInst>(Inst)) &&
           "Unexpected compare instruction user!");
    return false;
  }

  return (isRegionLiveOut(Inst) || isBlobDef(Inst));
}

bool HIRParser::hasExpectedUsers(const CmpInst *CInst) const {

  for (auto I = CInst->user_begin(), E = CInst->user_end(); I != E; ++I) {
    if (auto UseInst = dyn_cast<Instruction>(*I)) {
      if (!isa<BranchInst>(UseInst) && !isa<SelectInst>(UseInst)) {
        return false;
      }
    } else {
      assert(false && "Use is not an instruction!");
    }
  }

  return true;
}

int64_t HIRParser::getSCEVConstantValue(const SCEVConstant *ConstSCEV) const {
  return ConstSCEV->getValue()->getSExtValue();
}

void HIRParser::parseConstant(const SCEVConstant *ConstSCEV, CanonExpr *CE) {
  auto Const = getSCEVConstantValue(ConstSCEV);

  CE->setConstant(CE->getConstant() + Const);
}

void HIRParser::setCanonExprDefLevel(CanonExpr *CE, unsigned NestingLevel,
                                     unsigned DefLevel) const {
  if (DefLevel >= NestingLevel) {
    // Make non-linear instead.
    CE->setNonLinear();
  } else if (DefLevel > CE->getDefinedAtLevel()) {
    CE->setDefinedAtLevel(DefLevel);
  }
}

void HIRParser::addTempBlobEntry(unsigned Index, unsigned DefLevel) {
  CurTempBlobLevelMap.insert(std::make_pair(Index, DefLevel));
}

void HIRParser::setTempBlobLevel(const SCEVUnknown *TempBlobSCEV, CanonExpr *CE,
                                 unsigned Level) {
  unsigned DefLevel;
  HLLoop *HLoop;

  auto Temp = TempBlobSCEV->getValue();
  auto Symbase = ScalarSA->getOrAssignScalarSymbase(Temp);
  auto Index = CanonExprUtils::findOrInsertBlob(TempBlobSCEV);

  if (auto Inst = dyn_cast<Instruction>(Temp)) {
    auto Lp = LI->getLoopFor(Inst->getParent());

    if (Lp && (HLoop = LF->findHLLoop(Lp))) {
      DefLevel = HLoop->getNestingLevel();
      setCanonExprDefLevel(CE, Level, DefLevel);

      // Cache blob level for later reuse.
      addTempBlobEntry(Index, DefLevel);
    } else {
      // Blob lies outside the region.
      addTempBlobEntry(Index, 0);

      // Add this as a livein temp.
      CurRegion->addLiveInTemp(Symbase, Temp);
    }
  } else {
    // Blob is some global value.
    addTempBlobEntry(Index, 0);
  }
}

void HIRParser::parseBlob(CanonExpr::BlobTy Blob, CanonExpr *CE, unsigned Level,
                          unsigned IVLevel) {
  auto Index = CanonExprUtils::findOrInsertBlob(Blob);

  if (IVLevel) {
    bool IsBlobCoeff;
    assert(!CE->getIVCoeff(IVLevel, &IsBlobCoeff) &&
           "Canon Expr already has a blob coeff for this IV!");
    CE->setIVCoeff(IVLevel, Index, true);

  } else {
    CE->addBlob(Index, 1);
  }

  // Set defined at level.
  BlobLevelSetter BLS(this, CE, Level);
  SCEVTraversal<BlobLevelSetter> LevelSetter(BLS);
  LevelSetter.visitAll(Blob);
}

// TODO: refine logic
void HIRParser::parseRecursive(const SCEV *SC, CanonExpr *CE, unsigned Level,
                               bool IsTop) {

  if (auto ConstSCEV = dyn_cast<SCEVConstant>(SC)) {
    assert(IsTop && "Can't handle constants embedded in the SCEV tree!");
    parseConstant(ConstSCEV, CE);

  } else if (isa<SCEVUnknown>(SC)) {
    assert(IsTop && "Can't handle unknowns embedded in the SCEV tree!");
    parseBlob(SC, CE, Level);

  } else if (isa<SCEVCastExpr>(SC)) {
    assert(IsTop && "Can't handle casts embedded in the SCEV tree!");
    parseBlob(SC, CE, Level);

  } else if (auto AddSCEV = dyn_cast<SCEVAddExpr>(SC)) {
    assert(IsTop && "Can't handle adds embedded in the SCEV tree!");

    for (auto I = AddSCEV->op_begin(), E = AddSCEV->op_end(); I != E; ++I) {
      parseRecursive(*I, CE, Level, true);
    }

  } else if (auto MulSCEV = dyn_cast<SCEVMulExpr>(SC)) {
    assert(IsTop && "Can't handle multiplies embedded in the SCEV tree!");

    for (auto I = MulSCEV->op_begin(), E = MulSCEV->op_end(); I != E; ++I) {
      parseRecursive(*I, CE, Level, false);
    }

  } else if (isa<SCEVUDivExpr>(SC)) {
    assert(IsTop && "Can't handle divisions embedded in the SCEV tree!");
    parseBlob(SC, CE, Level);

  } else if (auto RecSCEV = dyn_cast<SCEVAddRecExpr>(SC)) {
    assert(RecSCEV->isAffine() && "Non-affine AddRecs not expected!");

    auto Lp = RecSCEV->getLoop();
    auto HLoop = LF->findHLLoop(Lp);

    assert(HLoop && "Non-HIR loop IVs not handled!");

    // Break linear addRec into base and step
    auto BaseSCEV = RecSCEV->getOperand(0);
    auto StepSCEV = RecSCEV->getOperand(1);

    parseRecursive(BaseSCEV, CE, Level, IsTop);

    // Set constant IV coeff
    if (isa<SCEVConstant>(StepSCEV)) {
      auto Coeff = getSCEVConstantValue(cast<SCEVConstant>(StepSCEV));
      CE->addIV(HLoop->getNestingLevel(), Coeff);
    }
    // Set blob IV coeff
    else {
      parseBlob(StepSCEV, CE, Level, HLoop->getNestingLevel());
    }

  } else if (isa<SCEVSMaxExpr>(SC) || isa<SCEVUMaxExpr>(SC)) {
    assert(IsTop && "Can't handle max embedded in the SCEV tree!");
    parseBlob(SC, CE, Level);

  } else {
    assert(false && "unexpected SCEV type!");
  }
}

void HIRParser::parseAsBlob(const Value *Val, CanonExpr *CE, unsigned Level) {
  auto BlobSCEV = SE->getUnknown(const_cast<Value *>(Val));
  CE->setType(BlobSCEV->getType());
  parseBlob(BlobSCEV, CE, Level);
}

CanonExpr *HIRParser::parse(const Value *Val, unsigned Level) {
  const SCEV *SC;
  CanonExpr *CE;

  if (auto Inst = dyn_cast<Instruction>(Val)) {
    // Parse polynomial blob definitions as (1 * blob)
    if (isPolyBlobDef(Inst)) {
      CE = CanonExprUtils::createCanonExpr(Val->getType());
      parseAsBlob(Val, CE, Level);
      return CE;
    }
  }

  // Parse as blob if the type is not SCEVable.
  // This is currently for handling floating types.
  if (!SE->isSCEVable(Val->getType())) {
    CE = CanonExprUtils::createCanonExpr(Val->getType());
    parseAsBlob(Val, CE, Level);
  } else {
    SC = SE->getSCEV(const_cast<Value *>(Val));
    CE = CanonExprUtils::createCanonExpr(SC->getType());
    parseRecursive(SC, CE, Level, true);
  }

  return CE;
}

void HIRParser::clearTempBlobLevelMap() { CurTempBlobLevelMap.clear(); }

void HIRParser::populateBlobDDRefs(RegDDRef *Ref) {
  for (auto I = CurTempBlobLevelMap.begin(), E = CurTempBlobLevelMap.end();
       I != E; ++I) {
    auto Blob = CanonExprUtils::getBlob(I->first);
    assert(isa<SCEVUnknown>(Blob) && "Unexpected temp blob!");

    auto Symbase =
        ScalarSA->getOrAssignScalarSymbase(cast<SCEVUnknown>(Blob)->getValue());
    auto CE = CanonExprUtils::createCanonExpr(Blob->getType());

    CE->addBlob(I->first, 1);
    CE->setDefinedAtLevel(I->second);

    auto BRef = DDRefUtils::createBlobDDRef(Symbase, CE);
    Ref->addBlobDDRef(BRef);
  }
}

RegDDRef *HIRParser::createLowerDDRef(const SCEV *BETC) {
  auto CE = CanonExprUtils::createCanonExpr(BETC->getType());
  auto Ref = DDRefUtils::createRegDDRef(getSymBaseForConstants());
  Ref->setSingleCanonExpr(CE);

  return Ref;
}

RegDDRef *HIRParser::createStrideDDRef(const SCEV *BETC) {
  auto CE = CanonExprUtils::createCanonExpr(BETC->getType(), 0, 1);
  auto Ref = DDRefUtils::createRegDDRef(getSymBaseForConstants());
  Ref->setSingleCanonExpr(CE);

  return Ref;
}

RegDDRef *HIRParser::createUpperDDRef(const SCEV *BETC, unsigned Level) {
  const Value *Val;

  clearTempBlobLevelMap();

  if (auto ConstSCEV = dyn_cast<SCEVConstant>(BETC)) {
    Val = ConstSCEV->getValue();
  } else if (auto UnknownSCEV = dyn_cast<SCEVUnknown>(BETC)) {
    Val = UnknownSCEV->getValue();
  } else {
    Val = ScalarSA->getGenericLoopUpperVal();
  }

  auto Symbase = ScalarSA->getOrAssignScalarSymbase(Val);

  auto Ref = DDRefUtils::createRegDDRef(Symbase);
  auto CE = CanonExprUtils::createCanonExpr(BETC->getType());

  parseRecursive(BETC, CE, Level, true);
  Ref->setSingleCanonExpr(CE);

  if (!CE->isSelfBlob()) {
    populateBlobDDRefs(Ref);
  }

  return Ref;
}

void HIRParser::parse(HLLoop *HLoop) {

  auto Lp = HLoop->getLLVMLoop();
  assert(Lp && "HLLoop doesn't contain LLVM loop!");

  if (SE->hasLoopInvariantBackedgeTakenCount(Lp)) {
    auto BETC = SE->getBackedgeTakenCount(Lp);

    // Initialize Lower to 0.
    auto LowerRef = createLowerDDRef(BETC);
    HLoop->setLowerDDRef(LowerRef);

    // Initialize Stride to 1.
    auto StrideRef = createStrideDDRef(BETC);
    HLoop->setStrideDDRef(StrideRef);

    // Set the upper bound
    auto UpperRef = createUpperDDRef(BETC, CurLevel);
    HLoop->setUpperDDRef(UpperRef);
  }

  CurLevel++;
}

void HIRParser::parseCompare(const Value *Cond, unsigned Level,
                             CmpInst::Predicate *Pred, RegDDRef **LHSDDRef,
                             RegDDRef **RHSDDRef) {

  *LHSDDRef = *RHSDDRef = nullptr;

  if (auto ConstVal = dyn_cast<Constant>(Cond)) {
    if (ConstVal->isOneValue()) {
      *Pred = CmpInst::Predicate::FCMP_TRUE;
      return;
    } else if (ConstVal->isZeroValue()) {
      *Pred = CmpInst::Predicate::FCMP_FALSE;
      return;
    } else {
      llvm_unreachable("Unexpected conditional branch value");
    }
  } else if (auto CInst = dyn_cast<CmpInst>(Cond)) {
    *Pred = CInst->getPredicate();

    if ((*Pred == CmpInst::Predicate::FCMP_TRUE) ||
        (*Pred == CmpInst::Predicate::FCMP_FALSE)) {
      return;
    }

    *LHSDDRef = createRvalDDRef(CInst, 0, Level);
    *RHSDDRef = createRvalDDRef(CInst, 1, Level);

  } else {
    llvm_unreachable("Unexpected i1 value type!");
  }
}

void HIRParser::parse(HLIf *If) {
  CmpInst::Predicate Pred;
  RegDDRef *LHSDDRef, *RHSDDRef;

  auto SrcBB = HIR->getSrcBBlock(If);
  assert(SrcBB && "Could not find If's src basic block!");

  auto BeginPredIter = If->pred_begin();
  auto IfCond = cast<BranchInst>(SrcBB->getTerminator())->getCondition();

  parseCompare(IfCond, CurLevel, &Pred, &LHSDDRef, &RHSDDRef);

  If->replacePredicate(If->pred_begin(), Pred);
  If->setPredicateOperandDDRef(LHSDDRef, BeginPredIter, true);
  If->setPredicateOperandDDRef(RHSDDRef, BeginPredIter, false);
}

void HIRParser::collectStrides(Type *GEPType,
                               SmallVectorImpl<uint64_t> &Strides) {
  assert(isa<PointerType>(GEPType));
  GEPType = cast<PointerType>(GEPType)->getElementType();

  if (ArrayType *GEPArrType = dyn_cast<ArrayType>(GEPType)) {
    GEPType = GEPArrType->getElementType();
  }

  // Collect number of elements in each dimension
  for (; ArrayType *GEPArrType = dyn_cast<ArrayType>(GEPType);
       GEPType = GEPArrType->getElementType()) {
    Strides.push_back(GEPArrType->getNumElements());
  }

  assert((GEPType->isIntegerTy() || GEPType->isFloatingPointTy()) &&
         "Unexpected GEP type!");

  auto ElementSize = GEPType->getPrimitiveSizeInBits() / 8;

  // Multiple number of elements in each dimension by the size of each element
  // in the dimension.
  // We need to do a reverse traversal from the smallest(innermost) to
  // largest(outermost) dimension.
  for (auto I = Strides.rbegin(), E = Strides.rend(); I != E; ++I) {
    (*I) *= ElementSize;
    ElementSize = (*I);
  }

  Strides.push_back(ElementSize);
}

// NOTE: AddRec->delinearize() doesn't work with constant bound arrays.
/// TODO: Add blob DDRef logic
RegDDRef *HIRParser::createGEPDDRef(const Value *Val, unsigned Level) {
  const Value *GEPVal = nullptr;
  SmallVector<uint64_t, 9> Strides;

  clearTempBlobLevelMap();

  if (auto SInst = dyn_cast<StoreInst>(Val)) {
    GEPVal = SInst->getPointerOperand();
  } else if (auto LInst = dyn_cast<LoadInst>(Val)) {
    GEPVal = LInst->getPointerOperand();
  } else {
    // Might need to handle getelementptr instruction later.
    assert(false && "Unhandled instruction!");
  }

  // In some cases float* is converted into int32* before loading/storing.
  if (auto BCInst = dyn_cast<BitCastInst>(GEPVal)) {
    GEPVal = BCInst->getOperand(0);
  }

  // TODO: handle A[0] which doesn't have a GEP.
  assert(GEPVal && isa<GEPOperator>(GEPVal) && "Could not find GEP operator!");
  auto GEPOp = cast<GEPOperator>(GEPVal);

  auto Ref = DDRefUtils::createRegDDRef(0);

  auto BaseCE = parse(GEPOp->getPointerOperand(), Level);
  Ref->setBaseCE(BaseCE);

  collectStrides(GEPOp->getPointerOperandType(), Strides);

  unsigned GEPNumOp = GEPOp->getNumOperands();
  unsigned Count = Strides.size();
  auto CI = dyn_cast<ConstantInt>(GEPOp->getOperand(1));

  // Check that the number of GEP operands match with the number of strides we
  // have collected, accounting for cases where the first GEP operand is zero.
  assert(((Count == GEPNumOp - 1) ||
          (CI && CI->isZero() && (Count == GEPNumOp - 2))) &&
         "Number of subscripts snd strides do not match!");

  for (auto I = GEPNumOp - 1; Count > 0; --I, --Count) {
    auto IndexCE = parse(GEPOp->getOperand(I), Level);

    auto StrideCE = CanonExprUtils::createCanonExpr(IndexCE->getType(), 0,
                                                    Strides[Count - 1]);
    Ref->addDimension(IndexCE, StrideCE);
  }

  Ref->setInBounds(GEPOp->isInBounds());
  populateBlobDDRefs(Ref);

  return Ref;
}

/// TODO: Add blob DDRef logic
RegDDRef *HIRParser::createScalarDDRef(const Value *Val, unsigned Level,
                                       bool IsLval) {
  CanonExpr *CE;

  clearTempBlobLevelMap();

  auto Symbase = ScalarSA->getOrAssignScalarSymbase(Val);
  auto Ref = DDRefUtils::createRegDDRef(Symbase);

  CE = parse(Val, Level);

  if (IsLval) {
    // If the lval is non-linear, make it a self-blob to reduce number of
    // BlobDDRefs and DD edges.
    if (CE->isNonLinear() && !CE->isSelfBlob()) {
      CE->clear();
      parseAsBlob(Val, CE, Level);
    }
  }

  Ref->setSingleCanonExpr(CE);

  if (!CE->isSelfBlob()) {
    populateBlobDDRefs(Ref);
  }

  return Ref;
}

RegDDRef *HIRParser::createRvalDDRef(const Instruction *Inst, unsigned OpNum,
                                     unsigned Level) {
  RegDDRef *Ref;

  if (isa<LoadInst>(Inst)) {
    Ref = createGEPDDRef(Inst, Level);
  } else {
    Ref = createScalarDDRef(Inst->getOperand(OpNum), Level, false);
  }

  return Ref;
}

RegDDRef *HIRParser::createLvalDDRef(const Instruction *Inst, unsigned Level) {
  RegDDRef *Ref;

  if (!isRequired(Inst)) {
    return nullptr;

  } else if (isa<StoreInst>(Inst)) {
    Ref = createGEPDDRef(Inst, Level);
  } else {
    Ref = createScalarDDRef(Inst, Level, true);
  }

  return Ref;
}

void HIRParser::parse(HLInst *HInst) {
  const Value *Val;
  RegDDRef *Ref;
  bool HasLval = false;
  auto Inst = HInst->getLLVMInstruction();
  unsigned Level = CurLevel;

  assert(!Inst->getType()->isVectorTy() && "Vector types not supported!");

  if (HInst->isInPreheaderOrPostexit()) {
    --Level;
  }

  /// Process lval
  if (HInst->hasLval()) {
    HasLval = true;

    Ref = createLvalDDRef(Inst, Level);

    if (!Ref) {
      /// Eliminate useless instructions.
      EraseSet.push_back(HInst);
      return;
    }

    HInst->setLvalDDRef(Ref);
  }

  unsigned NumRvalOp =
      HasLval ? HInst->getNumOperands() - 1 : HInst->getNumOperands();

  /// Process rvals
  for (unsigned I = 0; I < NumRvalOp; ++I) {

    Val = Inst->getOperand(I);

    if (isa<SelectInst>(Inst) && (I == 0)) {
      CmpInst::Predicate Pred;
      RegDDRef *LHSDDRef, *RHSDDRef;

      parseCompare(Val, Level, &Pred, &LHSDDRef, &RHSDDRef);

      HInst->setPredicate(Pred);
      HInst->setOperandDDRef(LHSDDRef, 1);
      HInst->setOperandDDRef(RHSDDRef, 2);
      continue;
    }

    Ref = createRvalDDRef(Inst, I, Level);

    // To translate Instruction's operand number into HLInst's operand number we
    // add one offset each for having an lval and being a select instruction.
    auto OpNum = HasLval ? (isa<SelectInst>(Inst) ? (I + 2) : (I + 1)) : I;

    HInst->setOperandDDRef(Ref, OpNum);
  }
}

void HIRParser::eraseUselessNodes() {
  for (auto &I : EraseSet) {
    HLNodeUtils::erase(I);
  }
}

bool HIRParser::runOnFunction(Function &F) {
  SE = &getAnalysis<ScalarEvolution>();
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
  ScalarSA = &getAnalysis<ScalarSymbaseAssignment>();
  HIR = &getAnalysis<HIRCreation>();
  LF = &getAnalysis<LoopFormation>();

  HLUtils::setHIRParser(this);

  Visitor PV(this);
  HLNodeUtils::visitAll(&PV);

  eraseUselessNodes();

  return false;
}

void HIRParser::releaseMemory() {
  /// Destroy all DDRefs and CanonExprs.
  DDRefUtils::destroyAll();
  CanonExprUtils::destroyAll();

  EraseSet.clear();
  CurTempBlobLevelMap.clear();
}

void HIRParser::print(raw_ostream &OS, const Module *M) const {
  HIR->printWithIRRegion(OS);
}

void HIRParser::verifyAnalysis() const {
  /// TODO: implement later
}
