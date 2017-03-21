//===- HIRCompleteUnroll.cpp - Implements CompleteUnroll class ------------===//
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
// This file implements HIRCompleteUnroll class which unrolls a HIR loop
// with small trip count.
//
// For example:
//
// Original Loop                     Transformed
// for(i=0; i<5; i++)                A[0] = B[0];
//    A[i] = B[i];                   A[1] = B[1];
//                                   ...
//                                   A[4] = B[4];
//
// The general algorithm is as follows:
//  1. For each loopnest, gather legal candidates (inner to outer) based on trip
//  count analysis.
//  2. Refine candidates (outer to inner) based on profitability and legality
//  (dependence on outer loops).
//  3. For each final candidate loop (outer to inner):
//       3.1 Clone LoopChild and insert it before the loop.
//       3.2 Update CanonExprs of LoopChild and recursively visit the inner
//           loops.
//       3.3 Delete Loop
//
// Unrolling would increase the register pressure based on the unroll factor.
// Current heuristic just uses trip count to determine if loop needs to be
// unrolled.
//
// Works by unrolling transformation from outermost to inner loops.
// It avoids outer loops if any of the inner loops are not completely unrolled.
// No candidate loops should have a switch or call statement.
//
//===----------------------------------------------------------------------===//

// TODO: Extensions to be added later.
//  (1) Extend it for non normalized loops.
//  (2) Add opt report.

#include "HIRCompleteUnroll.h"
#include "llvm/Analysis/Intel_LoopAnalysis/HIRFramework.h"

#include "llvm/ADT/Statistic.h"

#include "llvm/IR/Function.h"
#include "llvm/IR/LLVMContext.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Transforms/Intel_LoopTransforms/Utils/BlobUtils.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/CanonExprUtils.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/DDRefUtils.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/HIRInvalidationUtils.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/HIRTransformUtils.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/HLNodeUtils.h"

#define DEBUG_TYPE "hir-complete-unroll"

using namespace llvm;
using namespace llvm::loopopt;

// This stat maintains the number of hir loopnests completely unrolled.
STATISTIC(LoopnestsCompletelyUnrolled,
          "Number of HIR loopnests completely unrolled");

static cl::opt<bool>
    DisableHIRCompleteUnroll("disable-hir-complete-unroll", cl::init(false),
                             cl::Hidden,
                             cl::desc("Disable HIR Loop Complete Unrolling"));

static cl::opt<bool> DisableHIRTriCompleteUnroll(
    "disable-hir-tri-complete-unroll", cl::init(false), cl::Hidden,
    cl::desc("Disable HIR Triangular Complete Unrolling"));

// The trip count threshold is intentionally set to a high value as profitablity
// should be driven by the combination of trip count and loop resource.
static cl::opt<unsigned> CompleteUnrollLoopTripThreshold(
    "hir-complete-unroll-loop-trip-threshold", cl::init(64), cl::Hidden,
    cl::desc("Don't unroll if trip count of any loop is bigger than this "
             "threshold."));

static cl::opt<unsigned> CompleteUnrollLoopnestTripThreshold(
    "hir-complete-unroll-loopnest-trip-threshold", cl::init(128), cl::Hidden,
    cl::desc("Don't unroll if total trip count of the loopnest is bigger than "
             "this threshold."));

static cl::opt<unsigned> PreVectorSavingsThreshold(
    "hir-complete-unroll-pre-vector-savings-threshold", cl::init(80),
    cl::Hidden,
    cl::desc(
        "Least amount of savings (in percentage) for complete unrolling "
        "of a loopnest to be deemed profitable before vectorizer kicks in."));

static cl::opt<unsigned> PostVectorSavingsThreshold(
    "hir-complete-unroll-post-vector-savings-threshold", cl::init(40),
    cl::Hidden,
    cl::desc(
        "Least amount of savings (in percentage) for complete unrolling "
        "of a loopnest to be deemed profitable after vectorizer kicks in."));

static cl::opt<unsigned> UnrolledLoopMemRefThreshold(
    "hir-complete-unroll-memref-threshold", cl::init(150), cl::Hidden,
    cl::desc("Maximum number of memory refs allowed in completely unrolled "
             "loopnest"));

static cl::opt<unsigned>
    UnrolledLoopDDRefThreshold("hir-complete-unroll-ddref-threshold",
                               cl::init(1000), cl::Hidden,
                               cl::desc("Maximum number of DDRefs allowed in "
                                        "completely unrolled loopnest"));

static cl::opt<unsigned> SmallLoopMemRefThreshold(
    "hir-complete-unroll-small-memref-threshold", cl::init(16), cl::Hidden,
    cl::desc("Threshold for memory refs in small loops (higher probability of "
             "unrolling)"));

static cl::opt<unsigned>
    SmallLoopDDRefThreshold("hir-complete-unroll-small-ddref-threshold",
                            cl::init(32), cl::Hidden,
                            cl::desc("Threshold for DDRefs in small loops "
                                     "(higher probability of unrolling)"));

static cl::opt<unsigned> SmallLoopAdditionalSavingsThreshold(
    "hir-complete-unroll-extra-savings-threshold", cl::init(5), cl::Hidden,
    cl::desc("Threshold for extra savings added to small loops to give them "
             "higher probability of unrolling)"));

static cl::opt<float> MaxThresholdScalingFactor(
    "hir-complete-unroll-max-threshold-scaling-factor", cl::init(10.0),
    cl::Hidden,
    cl::desc("Used to scale the thresholds of the loop based on how profitable "
             "the loop is over the base savings threshold"));

void HIRCompleteUnroll::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequiredTransitive<HIRFramework>();
}

/// Visitor to update the CanonExpr.
class HIRCompleteUnroll::CanonExprVisitor final : public HLNodeVisitorBase {
private:
  HLLoop *OuterLoop;
  SmallVectorImpl<int64_t> *TripValues;

  void processRegDDRef(RegDDRef *RegDD);
  void processCanonExpr(CanonExpr *CExpr, bool IsTerminal);

public:
  CanonExprVisitor(HLLoop *OutLoop, SmallVectorImpl<int64_t> &TripValVec)
      : OuterLoop(OutLoop), TripValues(&TripValVec) {}

  void visit(HLDDNode *Node);
  void visit(HLLoop *Loop);
  // No processing needed for Goto and Label's
  void visit(HLGoto *Goto){};
  void visit(HLLabel *Label){};
  void visit(HLNode *Node) {
    llvm_unreachable(" Node not supported for Complete Unrolling.");
  }
  void postVisit(HLNode *Node) {}
};

/// Determines if unrolling the loop nest would be profitable.
/// Profitability of the loopnest is determined by giving positive weight to
/// simplification opportunities and negative weight to chance of increase in
/// code size/register pressure. Loopnest is profitable if the accumulated
/// weight is positive.
///
/// Simplification opportunity includes the following cases-
/// 1) Substitution of IV by a constant.
/// 2) Presence of linear blobs (invariance can lead to CSE).
///
/// Code size increase includes the following cases-
/// 1) Presence of non-linear blobs.
class HIRCompleteUnroll::ProfitabilityAnalyzer final
    : public HLNodeVisitorBase {

  const HIRCompleteUnroll &HCU;
  const HLLoop *CurLoop;
  const HLLoop *OuterLoop;

  unsigned Cost;
  unsigned Savings;
  // Cost/Savings of GEP refs.
  // This is kept separate because it is already scaled unlike other
  // Cost/Savings which is scaled after processing a loop.
  unsigned GEPCost;
  unsigned GEPSavings;

  unsigned NumMemRefs;
  unsigned NumDDRefs;

  // Keeps track of non-linear blobs that we encounter during our traversal so
  // they aren't penalized multiple times. Blobs are removed from the set when
  // we encounter a redefinition of a contained temp. The mapped value is the
  // first non-unit coefficient they were multiplied with when visited.
  DenseMap<unsigned, int64_t> VisitedNonLinearBlobs;

  // Keeps track of temp blob definitions which get simplified to a constant due
  // to unrolling. This can result in simplification of other instructions.
  // Blobs which are simplified using rem (%) operation have their factor stored
  // as the mapped value.
  DenseMap<unsigned, unsigned> &SimplifiedTempBlobs;

  // Keep track of invariant GEP refs that have been visited to avoid
  // duplicating savings.
  SmallVector<const RegDDRef *, 16> VisitedLinearGEPRefs;

  // Structure to store blob related info.
  struct BlobInfo {
    bool Invariant;
    // Indicates whether the non-linear blob has been encountered before.
    bool Visited;
    // Indicates whether blob definition can be simplified to a constant.
    bool Simplified;
    // Number of operations in the non-linear blob.
    unsigned NumOperations;
    // Previous coefficient of visited blob.
    int64_t PrevCoeff;

    BlobInfo()
        : Invariant(true), Visited(false), Simplified(false), NumOperations(0),
          PrevCoeff(1) {}
  };

  // Private constructor used for children loops.
  ProfitabilityAnalyzer(const HIRCompleteUnroll &HCU, const HLLoop *CurLp,
                        const HLLoop *OuterLp,
                        DenseMap<unsigned, unsigned> &SimplifiedBlobs)
      : HCU(HCU), CurLoop(CurLp), OuterLoop(OuterLp), Cost(0), Savings(0),
        GEPCost(0), GEPSavings(0), NumMemRefs(0), NumDDRefs(0),
        SimplifiedTempBlobs(SimplifiedBlobs) {}

  /// Populates rem blobs present in \p Ref in \p RemBlobs. Returns the max
  /// level of any non-rem blob and populates max non-simplified blob in \p
  /// MaxNonSimplifiedBlobLevel.
  unsigned
  populateRemBlobs(const RegDDRef *Ref,
                   SmallVectorImpl<std::pair<unsigned, unsigned>> &RemBlobs,
                   unsigned &MaxNonSimplifiedBlobLevel) const;

  /// Returns true if linear \p Ref has been visited already.
  bool visited(const RegDDRef *Ref);

  /// Adds additional cost associated with a GEP ref.
  void addGEPCost(const RegDDRef *Ref);

  /// Processes RegDDRef for profitability. Returns true if Ref can be
  /// simplified to a constant.
  bool processRef(const RegDDRef *Ref);

  /// Processes CanonExpr for profitability. Returns true if CE can be
  /// simplified to a constant.
  bool processCanonExpr(const CanonExpr *CE, const RegDDRef *ParentRef);

  /// Processes IVs in the CE. Returns true if they can be simplified to a
  /// constant.
  bool processIVs(const CanonExpr *CE, const RegDDRef *ParentRef,
                  unsigned &NumSimplifiedTerms, unsigned &NumNonLinearTerms);

  /// Processes blobs in the CE. Returns true if they can be simplified to a
  /// constant.
  bool processBlobs(const CanonExpr *CE, const RegDDRef *ParentRef,
                    unsigned &NumSimplifiedTerms, unsigned &NumNonLinearTerms);

  /// Adds the cost of the blob given its info and coefficient in the CE.
  void addBlobCost(const BlobInfo &BInfo, int64_t Coeff,
                   unsigned &NumNonLinearTerms);

  /// Returns all the info assodicated with the blob.
  BlobInfo getBlobInfo(unsigned Index, int64_t Coeff, const RegDDRef *ParentRef,
                       bool CEIsLinear);

  /// \p HInst represents a simplified blob. Returns the divisior if this is a
  /// rem operation.
  unsigned getBlobFactor(HLInst *HInst) const;

  /// Updates all the visited blobs which contain the temp represented by self
  /// blob \p Ref. \p Simplified indicates whether the blob definition was
  /// simplified to a constant.
  void updateBlobs(const RegDDRef *Ref, bool Simplified);

  /// Returns percentage savings achieved by unrolling the loopnest.
  float getSavingsInPercentage() const;

  /// Returns true if this loop should be unrolled before vectorizer. This is a
  /// temporary workaround.
  bool isPreVectorProfitableLoop(const HLLoop *CurLoop) const;

  /// Scales the profitability by the given multiplier.
  void scale(unsigned Multiplier) {
    Cost *= Multiplier;
    Savings *= Multiplier;
    NumMemRefs *= Multiplier;
    NumDDRefs *= Multiplier;
  }

  // Adds profitability analysis results from PA to this.
  ProfitabilityAnalyzer &operator+=(const ProfitabilityAnalyzer &PA) {
    Cost += PA.Cost;
    Savings += PA.Savings;
    GEPCost += PA.GEPCost;
    GEPSavings += PA.GEPSavings;
    NumMemRefs += PA.NumMemRefs;
    NumDDRefs += PA.NumDDRefs;

    return *this;
  }

public:
  ProfitabilityAnalyzer(const HIRCompleteUnroll &HCU, const HLLoop *CurLp,
                        DenseMap<unsigned, unsigned> &SimplifiedTempBlobs)
      : ProfitabilityAnalyzer(HCU, CurLp, CurLp, SimplifiedTempBlobs) {}

  // Main interface of the analyzer.
  void analyze();

  // Returns true if loopnest is profitable.
  bool isProfitable() const;

  // Returns true if loop has a small body.
  bool isSmallLoop() const;

  void visit(const HLLoop *Lp);
  void visit(const HLDDNode *Node);

  // No processing needed for Gotos/Labels.
  void visit(const HLGoto *Goto) {}
  void visit(const HLLabel *Label) {}

  void visit(const HLNode *Node) {
    llvm_unreachable("Node not supported for Complete Unrolling.");
  }
  void postVisit(const HLNode *Node) {}
};

////// CanonExpr Visitor Start

void HIRCompleteUnroll::CanonExprVisitor::visit(HLLoop *Loop) {
  transformLoop(Loop, OuterLoop, *TripValues);
}

void HIRCompleteUnroll::CanonExprVisitor::visit(HLDDNode *Node) {

  assert(!isa<HLLoop>(Node) && "Loop node not expected!");

  for (auto Iter = Node->ddref_begin(), End = Node->ddref_end(); Iter != End;
       ++Iter) {
    processRegDDRef(*Iter);
  }
}

/// processRegDDRef - Processes RegDDRef to call the Canon Exprs
/// present inside it. This is an internal helper function.
void HIRCompleteUnroll::CanonExprVisitor::processRegDDRef(RegDDRef *RegDD) {
  bool IsTerminal = RegDD->isTerminalRef();

  // Process CanonExprs inside the RegDDRefs
  for (auto Iter = RegDD->canon_begin(), End = RegDD->canon_end(); Iter != End;
       ++Iter) {
    processCanonExpr(*Iter, IsTerminal);
  }

  RegDD->makeConsistent();

  // Example of an alternative way of updating DDRef which is useful when some
  // manual work is also involved-
  //
  // RegDD->updateBlobDDRefs(BlobDDRefs);
  // assert(BlobDDRefs.empty() && "New blobs found in DDRef after processing!");
  // RegDD->updateCELevel();
}

/// Processes CanonExpr to replace IV by TripVal.
/// This is an internal helper function.
void HIRCompleteUnroll::CanonExprVisitor::processCanonExpr(CanonExpr *CExpr,
                                                           bool IsTerminal) {

  // Start replacing the IV's from OuterLoop level to current loop level.
  auto LoopLevel = OuterLoop->getNestingLevel();
  for (auto &TripV : *TripValues) {
    CExpr->replaceIVByConstant(LoopLevel, TripV);
    CExpr->simplify(IsTerminal);
    LoopLevel++;
  }
}

///// CanonExpr Visitor End

///// ProfitabilityAnalyzer Visitor Start

bool HIRCompleteUnroll::ProfitabilityAnalyzer::isPreVectorProfitableLoop(
    const HLLoop *CurLoop) const {

  if (!CurLoop->isInnermost()) {
    return false;
  }

  auto Upper = CurLoop->getUpperCanonExpr();
  int64_t Val;

  if (!Upper->isIntConstant(&Val) || (Val != 3)) {
    return false;
  }

  unsigned NumIfs = 0;
  unsigned NumSelects = 0;
  unsigned NumRems = 0;
  unsigned NumXORs = 0;

  for (auto NodeIt = CurLoop->child_begin(), E = CurLoop->child_end();
       NodeIt != E; ++NodeIt) {
    auto Node = &*NodeIt;

    if (isa<HLIf>(Node)) {
      ++NumIfs;

    } else if (auto HInst = dyn_cast<HLInst>(Node)) {
      unsigned OpCode = HInst->getLLVMInstruction()->getOpcode();

      if (OpCode == Instruction::URem || OpCode == Instruction::SRem) {
        ++NumRems;
      } else if (OpCode == Instruction::Select) {
        ++NumSelects;
      } else if (OpCode == Instruction::Xor) {
        ++NumXORs;
      }
    }
  }

  return ((NumIfs == 4) && (NumRems == 2) && (NumSelects == 1) &&
          (NumXORs == 3));
}

void HIRCompleteUnroll::ProfitabilityAnalyzer::analyze() {
  // TODO: Think about visiting the linear instructions at the end of the loop
  // body first so that they are treated as simplified. This happens when IV is
  // parsed as blob.
  CurLoop->getHLNodeUtils().visitRange<true, false>(
      *this, CurLoop->child_begin(), CurLoop->child_end());

  // Scale results by loop's average trip count.
  auto Iter = HCU.AvgTripCount.find(CurLoop);
  assert((Iter != HCU.AvgTripCount.end()) && "Trip count of loop not found!");

  // Check if the loop is small enough to assign some extra profitability to it
  // (for eliminating loop control) and give it higher chance of unrolling.
  if (isSmallLoop()) {
    Savings +=
        std::min(static_cast<unsigned>(SmallLoopAdditionalSavingsThreshold),
                 Iter->second);
  }

  // Workaround to make loop profitable till vectorizer fixes its cost model.
  if (isPreVectorProfitableLoop(CurLoop)) {
    Savings *= 3;
  }

  scale(Iter->second);

  // Add ztt's profitability.
  if (CurLoop->hasZtt()) {
    for (auto RefIt = CurLoop->ztt_ddref_begin(), E = CurLoop->ztt_ddref_end();
         RefIt != E; ++RefIt) {
      processRef(*RefIt);
    }
    // Increment index by number of predicates eliminated.
    Savings += CurLoop->getNumZttPredicates();
  }
}

bool HIRCompleteUnroll::ProfitabilityAnalyzer::isSmallLoop() const {
  return !HCU.IsPreVec && (NumMemRefs <= SmallLoopMemRefThreshold) &&
         (NumDDRefs <= SmallLoopDDRefThreshold);
}

float HIRCompleteUnroll::ProfitabilityAnalyzer::getSavingsInPercentage() const {
  auto TotalCost = Cost + GEPCost;

  float SafeCost = (TotalCost == 0) ? 1 : TotalCost;
  return ((Savings + GEPSavings) * 100) / SafeCost;
}

bool HIRCompleteUnroll::ProfitabilityAnalyzer::isProfitable() const {

  auto SavingsPercentage = getSavingsInPercentage();

  DEBUG(dbgs() << "Cost: " << Cost << "\n");
  DEBUG(dbgs() << "GEPCost: " << GEPCost << "\n");
  DEBUG(dbgs() << "Savings: " << Savings << "\n");
  DEBUG(dbgs() << "GEPSavings: " << GEPSavings << "\n");

  DEBUG(dbgs() << "Savings in percentage: " << SavingsPercentage << "\n");

  DEBUG(dbgs() << "Number of memrefs: " << NumMemRefs << "\n");
  DEBUG(dbgs() << "Number of ddrefs: " << NumDDRefs << "\n");
  DEBUG(dbgs() << "Loop: \n"; CurLoop->dump(); dbgs() << "\n");

  if (SavingsPercentage <
      (HCU.IsPreVec ? PreVectorSavingsThreshold : PostVectorSavingsThreshold)) {
    return false;
  }

  // Use postvec(smaller) savings threshold to derive consistent scaling factor
  // for prevec and postvec passes.
  float ScalingFactor = (SavingsPercentage / PostVectorSavingsThreshold);

  ScalingFactor =
      std::min(ScalingFactor, static_cast<float>(MaxThresholdScalingFactor));

  auto Iter = HCU.TotalTripCount.find(OuterLoop);
  assert((Iter != HCU.TotalTripCount.end()) && "Trip count of loop not found!");

  return (Iter->second <=
          (ScalingFactor * CompleteUnrollLoopnestTripThreshold)) &&
         (NumMemRefs <= (ScalingFactor * UnrolledLoopMemRefThreshold)) &&
         (NumDDRefs <= (ScalingFactor * UnrolledLoopDDRefThreshold));
}

void HIRCompleteUnroll::ProfitabilityAnalyzer::visit(const HLLoop *Lp) {
  // Analyze child loop.
  ProfitabilityAnalyzer PA(HCU, Lp, OuterLoop, SimplifiedTempBlobs);
  PA.analyze();

  // Add the result of child loop profitability analysis.
  *this += PA;
}

unsigned
HIRCompleteUnroll::ProfitabilityAnalyzer::getBlobFactor(HLInst *HInst) const {
  auto Inst = HInst->getLLVMInstruction();

  // Looking for something like this -
  // tmp = i1 % 4;

  auto OpCode = Inst->getOpcode();

  if ((OpCode != Instruction::URem) && (OpCode != Instruction::SRem)) {
    return 0;
  }

  auto RvalOp2 = HInst->getOperandDDRef(2);

  int64_t Factor;

  if (!RvalOp2->isIntConstant(&Factor) || (Factor < 0) || (Factor > UINT_MAX)) {
    return 0;
  }

  return Factor;
}

void HIRCompleteUnroll::ProfitabilityAnalyzer::updateBlobs(
    const RegDDRef *LvalRef, bool Simplified) {

  auto &BU = LvalRef->getBlobUtils();
  auto TempIndex = LvalRef->isSelfBlob()
                       ? LvalRef->getSelfBlobIndex()
                       : BU.findTempBlobIndex(LvalRef->getSymbase());

  if (TempIndex == InvalidBlobIndex) {
    return;
  }

  auto TempBlob = BU.getBlob(TempIndex);

  for (auto &BlobCoeffPair : VisitedNonLinearBlobs) {
    if (BU.contains(BU.getBlob(BlobCoeffPair.first), TempBlob)) {
      VisitedNonLinearBlobs.erase(BlobCoeffPair.first);
    }
  }

  if (Simplified) {
    unsigned Factor = getBlobFactor(cast<HLInst>(LvalRef->getHLDDNode()));
    SimplifiedTempBlobs.insert(std::make_pair(TempIndex, Factor));
  } else {
    SimplifiedTempBlobs.erase(TempIndex);
  }
}

void HIRCompleteUnroll::ProfitabilityAnalyzer::visit(const HLDDNode *Node) {
  auto HInst = dyn_cast<HLInst>(Node);
  auto Inst = HInst ? HInst->getLLVMInstruction() : nullptr;
  bool IsSelect = (Inst && isa<SelectInst>(Inst));

  auto RefIt = HInst ? HInst->rval_op_ddref_begin() : Node->op_ddref_begin();
  auto End = HInst ? HInst->rval_op_ddref_end() : Node->op_ddref_end();

  unsigned NumRvalOp = 0;
  const RegDDRef *LvalRef = nullptr;
  const RegDDRef *RvalRef = nullptr;
  bool CanSimplifyRvals = (!Inst || !isa<CallInst>(Inst));

  for (; RefIt != End; ++RefIt, ++NumRvalOp) {
    RvalRef = *RefIt;
    ++NumDDRefs;

    if (!processRef(RvalRef)) {
      // Only the first two operands of select are relavant for simplification.
      if (!IsSelect || (NumRvalOp < 2)) {
        CanSimplifyRvals = false;
      }
    }
  }

  if (HInst && (LvalRef = HInst->getLvalDDRef())) {
    ++NumDDRefs;
    // Terminal lval refs are only used to invalidate their encountered uses.
    if (LvalRef->isTerminalRef()) {
      updateBlobs(LvalRef, CanSimplifyRvals);
    } else {
      processRef(LvalRef);
    }
  }

  // Ignore load/store/gep/copy instructions as all the cost has been accounted
  // for in refs.
  // TODO: we may have additional register move cost but is it significant?
  if (Inst && (isa<LoadInst>(Inst) || isa<StoreInst>(Inst) ||
               isa<GetElementPtrInst>(Inst) || HInst->isCopyInst())) {
    return;
  }

  // Add 1 to cost/savings based on whether candidate can be simplified.
  if (CanSimplifyRvals) {
    ++Savings;
  } else {
    ++Cost;
  }
}

unsigned HIRCompleteUnroll::ProfitabilityAnalyzer::populateRemBlobs(
    const RegDDRef *Ref,
    SmallVectorImpl<std::pair<unsigned, unsigned>> &RemBlobs,
    unsigned &MaxNonSimplifiedBlobLevel) const {
  assert(Ref->hasGEPInfo() && "GEP ref expected!");

  unsigned MaxNonRemBlobLevel = 0;
  unsigned CurLevel = CurLoop->getNestingLevel();

  for (auto BIt = Ref->blob_cbegin(), End = Ref->blob_cend(); BIt != End;
       ++BIt) {
    auto Blob = *BIt;
    auto Index = Blob->getBlobIndex();
    unsigned BlobLevel =
        Blob->isNonLinear() ? CurLevel : Blob->getDefinedAtLevel();
    bool IsRemBlob = false;

    auto Iter = SimplifiedTempBlobs.find(Index);

    if (Iter != SimplifiedTempBlobs.end()) {
      if (Iter->second) {
        IsRemBlob = true;
        RemBlobs.push_back(std::make_pair(BlobLevel, Iter->second));
      }
    } else {
      MaxNonSimplifiedBlobLevel =
          std::max(MaxNonSimplifiedBlobLevel, BlobLevel);
    }

    if (!IsRemBlob) {
      MaxNonRemBlobLevel = std::max(MaxNonRemBlobLevel, BlobLevel);
    }
  }

  return MaxNonRemBlobLevel;
}

bool HIRCompleteUnroll::ProfitabilityAnalyzer::visited(const RegDDRef *Ref) {
  if (!Ref->hasGEPInfo()) {
    return false;
  }

  unsigned DefLevel = Ref->getDefinedAtLevel();

  if (DefLevel == NonLinearLevel) {
    return false;
  }

  for (auto VisitedRef : VisitedLinearGEPRefs) {
    if (DDRefUtils::areEqual(Ref, VisitedRef)) {
      return true;
    }
  }

  VisitedLinearGEPRefs.push_back(Ref);
  return false;
}

void HIRCompleteUnroll::ProfitabilityAnalyzer::addGEPCost(const RegDDRef *Ref) {
  assert(Ref->hasGEPInfo() && "GEP ref expected!");

  unsigned BaseCost = Ref->isMemRef() ? 2 : 1;

  // Self blob refs shouldn't affect cost/savings.
  // processCanonExpr(Ref->getBaseCE(), Ref);

  // iterations. Consider this case-
  // DO i1 = 0, 10
  //   DO i2 = 0, 5
  //     A[i2] =
  //   END DO
  // END DO
  //
  // Unrolling of the i1 loopnest will yield redundant loads of A[i2] for each
  // i1 loop iteration.
  //
  // Another example with a rem blob-
  //
  // DO i1 = 0, 5
  //   %rem = i1 % 2;
  //   A[%rem]
  // END DO
  //
  // A[%rem] can yield at most two different memory locations due to the rem
  // operation in a loop with a trip count of 6. So there are (6 - 2) = 4
  // redundant memory accesses.
  //
  // This is just an estimate as computing redundancies accurately is
  // mathematically complicated.
  // There are additional kinds of redundancies currently not taken into
  // account.
  // For example-
  // 1) Subscripts containing multiple IVs.
  // 2) Subscripts with a combination of IV and rem blobs.

  const HLLoop *OutermostLoop = OuterLoop->getParentLoop();
  unsigned TotalOccurences = 1;
  unsigned UniqueOccurences = 0;
  unsigned MaxNonSimplifiedBlobLevel = 0;
  SmallVector<std::pair<unsigned, unsigned>, 4> RemBlobs;

  unsigned MaxNonRemBlobLevel =
      populateRemBlobs(Ref, RemBlobs, MaxNonSimplifiedBlobLevel);

  // Add additional penalty for non-linear refs.
  if (MaxNonSimplifiedBlobLevel == CurLoop->getNestingLevel()) {
    ++BaseCost;
  }

  // Accumulate cost/savings of ref based on how redundant it is across loop
  for (const HLLoop *ParentLoop = CurLoop; ParentLoop != OutermostLoop;
       ParentLoop = ParentLoop->getParentLoop()) {
    auto TCIt = HCU.AvgTripCount.find(ParentLoop);
    assert((TCIt != HCU.AvgTripCount.end()) && "Trip count of loop not found!");

    TotalOccurences *= TCIt->second;

    unsigned Level = ParentLoop->getNestingLevel();

    if ((MaxNonRemBlobLevel >= Level) || Ref->hasIV(Level)) {
      // If ref contains IV of a loop or a blob defined at that level, all
      // references of the ref are considered unique w.r.t that level.
      if (!UniqueOccurences) {
        UniqueOccurences = TCIt->second;
      } else {
        UniqueOccurences *= TCIt->second;
      }
      continue;
    }

    // Multiply the factors of all rem blobs at this level to get the final
    // factor.
    unsigned Factor = 1;
    for (auto &Blob : RemBlobs) {
      if ((Blob.first == Level) && Blob.second) {
        Factor *= Blob.second;
      }
    }

    // Ref is invariant w.r.t this loop so it doesn't yield additional unique
    // occurences.
    if (Factor == 1) {
      continue;
    }

    // At least one rem blob is present at this level. The max number of unique
    // references depend on the factor of the rem operation.
    if (!UniqueOccurences) {
      UniqueOccurences = Factor;
    } else {
      UniqueOccurences *= Factor;
    }
  }

  // This can happen if rem factor is greater than trip count.
  if (UniqueOccurences > TotalOccurences) {
    UniqueOccurences = TotalOccurences;
  }

  GEPCost += (UniqueOccurences * BaseCost);
  GEPSavings += ((TotalOccurences - UniqueOccurences) * BaseCost);
}

bool HIRCompleteUnroll::ProfitabilityAnalyzer::processRef(const RegDDRef *Ref) {

  bool CanSimplify = true;

  if (Ref->isMemRef()) {
    ++NumMemRefs;
  }

  if (visited(Ref)) {
    return false;
  }

  for (auto CEIt = Ref->canon_begin(), E = Ref->canon_end(); CEIt != E;
       ++CEIt) {
    if (!processCanonExpr(*CEIt, Ref)) {
      CanSimplify = false;
    }
  }

  if (Ref->hasGEPInfo()) {
    CanSimplify = false;
    addGEPCost(Ref);
  }

  return CanSimplify;
}

/// Evaluates profitability of CanonExpr.
/// Example 1-
/// The profitability index of CE: (3 * i1 + 1) is 3. It is computed as
/// follows-
/// +1 for substitution of i2 by constant.
/// +1 for simplification of (3 * i1) to a constant.
/// +1 for simplification of (3 * i1 + 1) to a constant.
///
/// Example 2-
/// The profitability index of CE: (b1 * i1 + 1) where b1 is a linear temp is 3.
/// It is computed as follows-
/// +1 for substitution of i1 by constant.
/// +1 for linear blob b1.
/// +1 for b1 * i1 possibly resulting in opportunity for CSE.
///
/// Example 3-
/// The profitability index of CE: (b1 * i1 + 1) where b1 is a non-linear temp
/// is -1. It is computed as follows-
/// +1 for substitution of i1 by constant.
/// -1 for non-linear blob b1.
/// -1 for b1 * i1 resulting in code size increase because of non-linearity of
///    blob.
///
/// Example 4-
/// The profitability index of CE: (i1 + 2 * i2 + b1) where i1 loopnest is
/// being unrolled and b1 is a non-linear temp is 3. It is computed as follows-
/// +1 for substitution of i1 by constant.
/// +1 for substitution of i2 by constant.
/// +1 for simplification of (2 * i2) to a constant.
/// +1 for simplification of (i1 + 2 * i2) to a constant.
/// -1 for non-linear blob.
///
/// Example 5-
/// The profitability index of CE: (i1 + 2 * i2 + b1) where i2 loop is being
/// unrolled and b1 is a non-linear temp is 1. It is computed as follows-
/// +1 for substitution of i2 by constant.
/// +1 for simplification of (2 * i2) to a constant.
/// -1 for non-linear blob.
///
bool HIRCompleteUnroll::ProfitabilityAnalyzer::processCanonExpr(
    const CanonExpr *CE, const RegDDRef *ParentRef) {

  unsigned NumSimplifiedTerms = 0;
  unsigned NumNonLinearTerms = 0;
  bool IsLinear = CE->isLinearAtLevel();

  bool CanSimplifyIVs =
      processIVs(CE, ParentRef, NumSimplifiedTerms, NumNonLinearTerms);

  bool CanSimplifyBlobs =
      processBlobs(CE, ParentRef, NumSimplifiedTerms, NumNonLinearTerms);

  bool NumeratorBecomesConstant = CanSimplifyIVs && CanSimplifyBlobs;

  // Add 1 to savings each, for number of simplified IV/Blob additions.
  if (NumSimplifiedTerms) {
    Savings += (NumSimplifiedTerms - 1);
  }

  // Add 1 to cost each, for number of non-linear IV/Blob additions.
  if (NumNonLinearTerms) {
    Cost += (NumNonLinearTerms - 1);
  }

  // Add 1 to cost/savings for the constant based on linearity and IV
  // simplifications.
  if (CE->getConstant()) {
    if (NumSimplifiedTerms) {
      ++Savings;
    } else if (!IsLinear) {
      ++Cost;
    }
  }

  // Add 1 to cost/savings for non-unit denominator based on linearity.
  if ((CE->getDenominator() != 1)) {
    if (NumeratorBecomesConstant) {
      ++Savings;
    } else if (!IsLinear) {
      ++Cost;
    }
  }

  // Add 1 to cost/savings based on whether there is a hidden cast.
  if (CE->getSrcType() != CE->getDestType()) {
    if (NumeratorBecomesConstant) {
      ++Savings;
    } else if (!IsLinear) {
      ++Cost;
    }
  }

  return NumeratorBecomesConstant;
}

bool HIRCompleteUnroll::ProfitabilityAnalyzer::processIVs(
    const CanonExpr *CE, const RegDDRef *ParentRef,
    unsigned &NumSimplifiedTerms, unsigned &NumNonLinearTerms) {

  bool CanSimplifyIVs = true;
  unsigned NodeLevel = CurLoop->getNestingLevel();
  unsigned OuterLevel = OuterLoop->getNestingLevel();
  bool IsLinear = CE->isLinearAtLevel();

  for (unsigned Level = 1; Level <= NodeLevel; ++Level) {
    unsigned BlobIndex;
    int64_t Coeff;
    bool IsUnrollLoopLevel = (Level >= OuterLevel);

    CE->getIVCoeff(Level, &BlobIndex, &Coeff);

    if (!Coeff) {
      continue;
    }

    if (IsUnrollLoopLevel) {
      // This IV belongs to one of the unroll candidates, add 1 for substitution
      // of IV by constant.
      ++Savings;

      if (Coeff != 1) {
        // Add one more for simplfication of (Coeff * IV).
        ++Savings;
      }

      // Keep track of inductive terms simplified to constant.
      if (BlobIndex == InvalidBlobIndex) {
        ++NumSimplifiedTerms;
      }
    } else {
      CanSimplifyIVs = false;
      // IV multiplication gives us opportunity for CSE.
      if (Coeff != 1) {
        ++Savings;
      }
    }

    if (BlobIndex != InvalidBlobIndex) {
      // For unroll loop levels, constant will be multiplied by simplified IV so
      // we conservatively pass the coeff as 1.
      auto BInfo = getBlobInfo(BlobIndex, IsUnrollLoopLevel ? 1 : Coeff,
                               ParentRef, IsLinear);

      if (IsUnrollLoopLevel && BInfo.Simplified) {
        ++NumSimplifiedTerms;
      } else {
        CanSimplifyIVs = false;
      }

      // Coefficient of blob is passed as zero for unroll loop levels but any
      // value other than 1 will do. This is just to indicate whether we are
      // multiplying the blob with anything. In this case it is being multiplied
      // by the IV.
      addBlobCost(BInfo, IsUnrollLoopLevel ? 0 : Coeff, NumNonLinearTerms);
    }
  }

  return CanSimplifyIVs;
}

bool HIRCompleteUnroll::ProfitabilityAnalyzer::processBlobs(
    const CanonExpr *CE, const RegDDRef *ParentRef,
    unsigned &NumSimplifiedTerms, unsigned &NumNonLinearTerms) {
  bool CanSimplifyBlobs = true;
  bool IsLinear = CE->isLinearAtLevel();

  for (auto Blob = CE->blob_begin(), E = CE->blob_end(); Blob != E; ++Blob) {
    auto BInfo = getBlobInfo(Blob->Index, Blob->Coeff, ParentRef, IsLinear);

    if (BInfo.Simplified) {
      ++Savings;
      ++NumSimplifiedTerms;
    } else {
      CanSimplifyBlobs = false;
    }

    addBlobCost(BInfo, Blob->Coeff, NumNonLinearTerms);
  }

  return CanSimplifyBlobs;
}

HIRCompleteUnroll::ProfitabilityAnalyzer::BlobInfo
HIRCompleteUnroll::ProfitabilityAnalyzer::getBlobInfo(unsigned Index,
                                                      int64_t Coeff,
                                                      const RegDDRef *ParentRef,
                                                      bool CEIsLinear) {
  BlobInfo BInfo;

  BInfo.Simplified = SimplifiedTempBlobs.count(Index);

  if (CEIsLinear) {
    return BInfo;
  }

  SmallVector<unsigned, 8> Indices;

  auto &BU = ParentRef->getBlobUtils();

  BU.collectTempBlobs(Index, Indices);

  // Add non-linear blobs as visited so we only penalize them once. The blobs
  // are added at the top level and the leaf (temp) level. This is an
  // approximation to save compile time. To generate more accurate results we
  // would have to compare sub-expression trees which would be very expensive.
  bool Invariant = true;
  for (auto Idx : Indices) {
    unsigned DefLevel;
    bool Found = ParentRef->findTempBlobLevel(Idx, &DefLevel);
    (void)Found;
    assert(Found && "Temp blob not found in Ref!");

    if ((DefLevel == NonLinearLevel) && !SimplifiedTempBlobs.count(Idx)) {
      Invariant = false;
      VisitedNonLinearBlobs.insert(std::make_pair(Idx, 1));
    }
  }

  if (!Invariant) {
    BInfo.Invariant = false;
    BInfo.NumOperations = BU.getNumOperations(Index);

    auto Iter = VisitedNonLinearBlobs.find(Index);

    if (Iter != VisitedNonLinearBlobs.end()) {
      BInfo.Visited = true;
      BInfo.PrevCoeff = Iter->second;
      // Update previous coefficient if it was 1.
      if (Iter->second == 1) {
        Iter->second = Coeff;
      }
    } else {
      VisitedNonLinearBlobs.insert(std::make_pair(Index, Coeff));
    }
  }

  return BInfo;
}

void HIRCompleteUnroll::ProfitabilityAnalyzer::addBlobCost(
    const BlobInfo &BInfo, int64_t Coeff, unsigned &NumNonLinearTerms) {

  if (BInfo.Simplified) {
    ++Savings;
    if (Coeff != 1) {
      ++Savings;
    }

  } else if (BInfo.Visited) {
    if ((Coeff != 1) && (Coeff != BInfo.PrevCoeff)) {
      ++Cost;
    }
    ++NumNonLinearTerms;

  } else if (!BInfo.Invariant) {
    Cost += (BInfo.NumOperations);

    if (Coeff != 1) {
      ++Cost;
    }
    ++NumNonLinearTerms;
  }
}

///// ProfitabilityAnalyzer Visitor End

bool HIRCompleteUnroll::runOnFunction(Function &F) {
  // Skip if DisableHIRCompleteUnroll is enabled
  if (DisableHIRCompleteUnroll || skipFunction(F)) {
    DEBUG(dbgs() << "HIR LOOP Complete Unroll Transformation Disabled \n");
    return false;
  }

  DEBUG(dbgs() << "Complete unrolling for Function : " << F.getName() << "\n");

  auto HIRF = &getAnalysis<HIRFramework>();

  // Storage for Outermost Loops
  SmallVector<HLLoop *, 64> OuterLoops;
  // Gather the outermost loops
  HIRF->getHLNodeUtils().gatherOutermostLoops(OuterLoops);

  // Process Loop Complete Unrolling
  processCompleteUnroll(OuterLoops);

  return false;
}

/// processCompleteUnroll - Main routine to perform unrolling.
/// First, performs cost analysis and then do the transformation.
void HIRCompleteUnroll::processCompleteUnroll(
    SmallVectorImpl<HLLoop *> &OuterLoops) {

  // Walk over the outermost loops across the regions.
  for (auto &Lp : OuterLoops) {
    if (performTripCountAnalysis(Lp).first >= 0) {
      CandidateLoops.push_back(Lp);
    }
  }

  refineCandidates();

  transformLoops();
}

void HIRCompleteUnroll::refineCandidates() {

  for (unsigned Index = 0; Index != CandidateLoops.size();) {
    HLLoop *OuterCandidateLoop = CandidateLoops[Index];

    // If this loop is either not a top level candidate or is not profitable, we
    // remove it as a candidate and add its children as candidates instead.
    if (TopLevelCandidates.count(OuterCandidateLoop) &&
        isProfitable(OuterCandidateLoop)) {
      Index++;
      continue;
    }

    if (!OuterCandidateLoop->isInnermost()) {
      OuterCandidateLoop->getHLNodeUtils().gatherLoopsWithLevel(
          OuterCandidateLoop, CandidateLoops,
          OuterCandidateLoop->getNestingLevel() + 1);
    }

    CandidateLoops.erase(CandidateLoops.begin() + Index);
  }
}

bool HIRCompleteUnroll::isApplicable(const HLLoop *Loop) const {

  // Throttle multi-exit/unknown loops.
  if (!Loop->isDo()) {
    return false;
  }

  // Ignore loops with SIMD directive.
  if (Loop->isSIMD()) {
    return false;
  }

  // Handle normalized loops only.
  if (!Loop->isNormalized()) {
    return false;
  }

  return true;
}

std::pair<int64_t, unsigned>
HIRCompleteUnroll::computeAvgTripCount(const HLLoop *Loop) {

  auto UpperCE = Loop->getUpperCanonExpr();
  unsigned LoopLevel = Loop->getNestingLevel();
  unsigned DepLevel = LoopLevel;

  if (UpperCE->hasBlob() || UpperCE->hasIVBlobCoeffs() ||
      (UpperCE->getDenominator() != 1)) {
    return std::make_pair(-1, DepLevel);
  }

  int64_t UpperVal = 0;

  if (UpperCE->isIntConstant(&UpperVal)) {
    int64_t TC = UpperVal + 1;
    if (TC >= CompleteUnrollLoopTripThreshold) {
      TC = -1;
    }

    return std::make_pair(TC, DepLevel);
  }

  // If triangular loop is disabled, we simply return high trip count,
  // to avoid unrolling triangular loops.
  if (DisableHIRTriCompleteUnroll) {
    return std::make_pair(-1, DepLevel);
  }

  auto ParLoop = Loop->getParentLoop();
  bool CanUnrollParents = true;

  // This is a triangular loop unrolling candidate. Check whether all parent
  // loops on which this loop's upper canon is dependent can be unrolled as
  // well. CanUnrollParents is set to false by the first parent loop which
  // cannot be unrolled. Any occurence of parent loop IVs from then on makes
  // the loop ineligible for unrolling. Here's an example-
  //
  // DO i1 = 1, 5
  //   DO i2 = 1, %n
  //     DO i3 = 1, i1
  //
  // CanUnrollParents is set to false by i2 loop. Therefore, presence of i1 in
  // i3 loop's upper canon makes it ineligible for complete unrolling.
  for (unsigned Level = LoopLevel - 1; Level > 0; --Level) {

    if (!AvgTripCount.count(ParLoop)) {
      CanUnrollParents = false;
    }

    if (UpperCE->getIVConstCoeff(Level)) {
      if (!CanUnrollParents) {
        return std::make_pair(-1, DepLevel);
      }

      // Set dependence level to the level of the outermost loop which has a IV
      // in UB.
      if (DepLevel == LoopLevel) {
        DepLevel = Level;
      }
    }

    ParLoop = ParLoop->getParentLoop();
  }

  int64_t MinUpper = 0, MaxUpper = 0, AvgTripCnt = 0;

  // If we reached here, we should be able to compute the min/max trip count of
  // this loop.
  bool HasMin =
      Loop->getHLNodeUtils().getExactMinValue(UpperCE, Loop, MinUpper);
  (void)HasMin;
  assert(HasMin && "Could not compute min value of upper!");

  // MinUpper can evaluate to a negative value. For purposes of calculating
  // average trip count for profitability analysis, we take the absolute value.
  MinUpper = std::llabs(MinUpper);

  bool HasMax =
      Loop->getHLNodeUtils().getExactMaxValue(UpperCE, Loop, MaxUpper);
  (void)HasMax;
  assert(HasMax && "Could not compute max value of upper!");

  // Loop never executes.
  if (MaxUpper < 0) {
    AvgTripCnt = 0;
  } else {
    AvgTripCnt = ((MinUpper + MaxUpper) / 2) + 1;
  }

  if (AvgTripCnt > CompleteUnrollLoopTripThreshold) {
    AvgTripCnt = -1;
  }

  return std::make_pair(AvgTripCnt, DepLevel);
}

std::pair<int64_t, unsigned>
HIRCompleteUnroll::performTripCountAnalysis(HLLoop *Loop) {
  SmallVector<HLLoop *, 8> CandidateChildLoops;

  int64_t AvgTripCnt = -1, TotalTripCnt = -1;
  int64_t MaxChildTripCnt = 1;
  unsigned LoopLevel = Loop->getNestingLevel();
  unsigned MinDepLevel = LoopLevel;

  bool IsLoopCandidate = isApplicable(Loop);

  if (IsLoopCandidate) {
    std::tie(AvgTripCnt, MinDepLevel) = computeAvgTripCount(Loop);

    if (AvgTripCnt >= 0) {
      AvgTripCount.insert(std::make_pair(Loop, AvgTripCnt));
    } else {
      IsLoopCandidate = false;
    }
  }

  if (!Loop->isInnermost()) {
    SmallVector<HLLoop *, 8> ChildLoops;
    Loop->getHLNodeUtils().gatherLoopsWithLevel(Loop, ChildLoops,
                                                LoopLevel + 1);

    for (auto &ChildLp : ChildLoops) {
      int64_t ChildTripCnt;
      unsigned ChildDepLevel;
      std::tie(ChildTripCnt, ChildDepLevel) = performTripCountAnalysis(ChildLp);

      if (ChildTripCnt >= 0) {
        CandidateChildLoops.push_back(ChildLp);

        MaxChildTripCnt = std::max(MaxChildTripCnt, ChildTripCnt);
        MinDepLevel = std::min(MinDepLevel, ChildDepLevel);

      } else {
        IsLoopCandidate = false;
      }
    }
  }

  if (IsLoopCandidate) {
    TotalTripCnt = AvgTripCnt * MaxChildTripCnt;
    IsLoopCandidate = (TotalTripCnt <= (CompleteUnrollLoopnestTripThreshold *
                                        MaxThresholdScalingFactor));
  }

  if (IsLoopCandidate) {
    TotalTripCount.insert(std::make_pair(Loop, TotalTripCnt));

    // Loop is not dependent on any outer loop so we add it as a top level
    // candidate.
    if (MinDepLevel == LoopLevel) {
      TopLevelCandidates.insert(Loop);
    }
  } else {
    TotalTripCnt = -1;
    // If current loop is not a candidate, store the children loops for
    // transformation.
    CandidateLoops.append(CandidateChildLoops.begin(),
                          CandidateChildLoops.end());
  }

  return std::make_pair(TotalTripCnt, MinDepLevel);
}

bool HIRCompleteUnroll::isProfitable(const HLLoop *Loop) const {
  DenseMap<unsigned, unsigned> SimplifiedTempBlobs;

  ProfitabilityAnalyzer PA(*this, Loop, SimplifiedTempBlobs);

  PA.analyze();

  return PA.isProfitable();
}

// Transform (Complete Unroll) each loop inside the CandidateLoops vector
void HIRCompleteUnroll::transformLoops() {
  SmallVector<int64_t, MaxLoopNestLevel> TripValues;

  LoopnestsCompletelyUnrolled += CandidateLoops.size();

  // Transform the loop nest from outer to inner.
  for (auto &Loop : CandidateLoops) {
    // Generate code for the parent region and invalidate parent
    Loop->getParentRegion()->setGenCode();
    HIRInvalidationUtils::invalidateParentLoopBodyOrRegion(Loop);

    HLNode *Parent = Loop->getParent();
    HLLoop *ParentLoop = Loop->getParentLoop();

    transformLoop(Loop, Loop, TripValues);

    if (ParentLoop) {
      HIRTransformUtils::eliminateRedundantPredicates(ParentLoop->child_begin(),
                                                      ParentLoop->child_end());
    }
    // complete unroll can produce empty ifs.
    HLNodeUtils::removeEmptyNodes(Parent);
  }
}

int64_t HIRCompleteUnroll::computeUB(HLLoop *Loop, HLLoop *OuterLoop,
                                     SmallVectorImpl<int64_t> &TripValues) {
  int64_t UBVal = 0;

  const CanonExpr *UBCE = Loop->getUpperCanonExpr();
  if (UBCE->isIntConstant(&UBVal)) {
    return UBVal;
  }

  assert(!UBCE->hasBlob() && !UBCE->hasIVBlobCoeffs() &&
         (UBCE->getDenominator() == 1) &&
         "Blobs or non-unit denominator in loop upper not expected!");

  UBVal = UBCE->getConstant();

  auto LoopLevel = OuterLoop->getNestingLevel();

  for (auto TripV : TripValues) {
    UBVal += (TripV * UBCE->getIVConstCoeff(LoopLevel));
    LoopLevel++;
  }

  return UBVal;
}

// Complete Unroll the given Loop, using provided LD as helper data
void HIRCompleteUnroll::transformLoop(HLLoop *Loop, HLLoop *OuterLoop,
                                      SmallVectorImpl<int64_t> &TripValues) {

  // Guard against the scanning phase setting it appropriately.
  assert(Loop && " Loop is null.");

  // Container for cloning body.
  HLContainerTy LoopBody;
  HLNodeUtils &HNU = Loop->getHLNodeUtils();

  CanonExprVisitor CEVisit(OuterLoop, TripValues);

  int64_t LB = Loop->getLowerCanonExpr()->getConstant();
  int64_t UB = computeUB(Loop, OuterLoop, TripValues);
  int64_t Step = Loop->getStrideCanonExpr()->getConstant();

  // At this point loop preheader has been visited already but postexit is not,
  // so we need to handle postexit explicitly.
  if (UB < 0) {
    Loop->removePostexit();
    HNU.remove(Loop);
    return;
  }

  if (Loop != OuterLoop) {
    HNU.visitRange(CEVisit, Loop->post_begin(), Loop->post_end());
  }

  // Ztt is not needed since it has ateast one trip.
  Loop->removeZtt();
  Loop->extractPreheaderAndPostexit();

  // Iterate over Loop Child for unrolling with trip value incremented
  // each time. Thus, loop body will be expanded by no. of stmts x TripCount.
  for (int64_t TripVal = LB; TripVal <= UB; TripVal += Step) {
    // Clone iteration
    HNU.cloneSequence(&LoopBody, Loop->getFirstChild(), Loop->getLastChild());

    // Store references as LoopBody will be empty after insertion.
    HLNode *CurFirstChild = &(LoopBody.front());
    HLNode *CurLastChild = &(LoopBody.back());

    HNU.insertBefore(Loop, &LoopBody);

    // Trip Values vector is used to store the current IV
    // trip value for substitution inside the canon expr.
    TripValues.push_back(TripVal);

    // Update the CanonExpr
    CanonExprVisitor CEVisit(OuterLoop, TripValues);
    HNU.visitRange<true, false>(CEVisit, CurFirstChild, CurLastChild);

    TripValues.pop_back();
  }

  HNU.remove(Loop);
}

void HIRCompleteUnroll::releaseMemory() {
  CandidateLoops.clear();
  AvgTripCount.clear();
  TopLevelCandidates.clear();
}
