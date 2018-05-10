//===----- HIRUnrollAndJam.cpp - Implements UnrollAndJam class ------------===//
//
// Copyright (C) 2015-2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
// Unroll & Jam unrolls the outer loop by some factor and then fuses (jams) the
// unrolled body. For example-
//
// Original loop-
// for(i=0; i<n; i++) {
//   for(j=0; j<m; j++) {
//     A[i] = A[i] + B[j];
//   }
// }
//
// Modified loop-
// t = n/2;
// for(i=0; i<t; i++) {
//   for(j=0; j<m; j++) {
//     A[2*i] = A[2*i] + B[j];
//     A[2*i+1] = A[2*i+1] + B[j];
//   }
// }
//
// for(i=2*t; i<n; i++) {
//   for(j=0; j<m; j++) {
//     A[i] = A[i] + B[j];
//   }
// }
//
// The algorithm is as follows-
//
// 1) Gather outermost loops and then traverse them one at a time. Store the
// loop hierarchy along with the unroll factors in a data structure.
//
// 2) Initialize unroll factors to the max unroll factor for each loop as we
// visit them.
//
// 3) Throttle loops by doing some quick legality checks. Throttling can happen
// recursively by following the parent loop chain.
//
// 4) During postVisit(), analyze the legality and profitability of loops which
// were not throttled in visit() and refine the unroll factor accordingly. The
// main analysis therefore happens in inner-to-outer order. More loops can be
// throttled in this stage.
//
// 5) Unroll non-throttled loops in outer-to-inner order.
//
// TODO: Add opt-report messages.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/Statistic.h"

#include "llvm/IR/Function.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/MathExtras.h"
#include "llvm/Support/raw_ostream.h"

#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/DDTests.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRDDAnalysis.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRLocalityAnalysis.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRLoopResource.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Analysis/HIRLoopStatistics.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Framework/HIRFramework.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/HIRInvalidationUtils.h"
#include "llvm/Analysis/Intel_LoopAnalysis/Utils/HLNodeUtils.h"

#include "llvm/Transforms/Intel_LoopTransforms/HIRTransformPass.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/HIRTransformUtils.h"

#include "HIRUnroll.h"

#define DEBUG_TYPE "hir-unroll-and-jam"

using namespace llvm;
using namespace llvm::loopopt;

const unsigned DefaultMaxUnrollFactor = 8;
const unsigned AbsoluteMaxUnrollFactor = 8;

STATISTIC(LoopsUnrolledAndJammed, "Number of HIR loops unrolled and jammed");

static cl::opt<bool>
    DisableHIRUnrollAndJam("disable-hir-unroll-and-jam", cl::init(false),
                           cl::Hidden, cl::desc("Disable HIR Unroll And Jam"));

// This is the maximum unroll factor that we use for any loop.
static cl::opt<unsigned> MaxUnrollFactor(
    "hir-unroll-and-jam-max-factor", cl::init(DefaultMaxUnrollFactor),
    cl::Hidden, cl::desc("Max unroll factor for loops (should be power of 2)"));

// This is the minimum trip count threshold.
static cl::opt<unsigned> MinTripCountThreshold(
    "hir-unroll-and-jam-min-trip-count-threshold", cl::init(16), cl::Hidden,
    cl::desc("Min trip count of loops which can be unrolled (absolute minimum "
             "depends on max unroll factor)"));

// This determines the unroll factor of loops inside the loopnest.
static cl::opt<unsigned> MaxUnrolledLoopNestCost(
    "hir-unroll-and-jam-max-unrolled-loopnest-cost", cl::init(700), cl::Hidden,
    cl::desc(
        "Max allowed cost of the loopnest with the unroll factor factored in"));

// This ensures that most of the code is in the innermost loop.
static cl::opt<unsigned> MaxOuterLoopCost(
    "hir-unroll-and-jam-max-outer-loop-cost", cl::init(30), cl::Hidden,
    cl::desc("Max allowed cost of an outer loop in the loopnest"));

typedef SmallVector<std::pair<HLLoop *, HLLoop *>, 16> LoopMapTy;

// Implements unroll/unroll & jam for \p Loop.
void unrollLoopImpl(HLLoop *Loop, unsigned UnrollFactor, LoopMapTy *LoopMap);

// External interface
namespace llvm {
namespace loopopt {
namespace unroll {
void unrollLoop(HLLoop *Loop, unsigned UnrollFactor) {
  unrollLoopImpl(Loop, UnrollFactor, nullptr);
}
} // namespace unroll
} // namespace loopopt
} // namespace llvm

namespace {

// Main unroll and jam class.
class HIRUnrollAndJam : public HIRTransformPass {
public:
  static char ID;

  HIRUnrollAndJam() : HIRTransformPass(ID), HaveUnrollCandidates(false) {
    initializeHIRUnrollAndJamPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void releaseMemory() override {}

  void getAnalysisUsage(AnalysisUsage &AU) const {
    AU.setPreservesAll();
    AU.addRequiredTransitive<HIRFrameworkWrapperPass>();
    AU.addRequiredTransitive<HIRLoopStatisticsWrapperPass>();
    AU.addRequiredTransitive<HIRLoopResourceWrapperPass>();
    AU.addRequiredTransitive<HIRLoopLocalityWrapperPass>();
    AU.addRequiredTransitive<HIRDDAnalysisWrapperPass>();
  }

private:
  typedef std::pair<HLLoop *, unsigned> LoopUFPairTy;
  typedef SmallVector<LoopUFPairTy, 6> LoopUFInfoPerLevelTy;
  // Stores the info for each loop in the loopnest by loop level.
  typedef std::array<LoopUFInfoPerLevelTy, MaxLoopNestLevel> LoopNestUFInfoTy;

  HIRLoopStatistics *HLS;
  HIRLoopResource *HLR;
  HIRLoopLocality *HLA;
  HIRDDAnalysis *DDA;

  LoopNestUFInfoTy LoopNestUFInfo;
  bool HaveUnrollCandidates;

  class Analyzer;

  /// Processes and santitizes command line options.
  void sanitizeOptions();

  /// Returns true if \p Lp's unroll factor is uninitialized.
  bool isUninitialized(HLLoop *Lp) const;

  /// Either retrieve or updates unroll factor of \p Lp according to \p Update
  /// argument.
  unsigned getOrUpdateUnrollFactor(HLLoop *Lp, unsigned UnrollFactor,
                                   bool Update);

  /// Replaces existing loops in LoopNestUFInfo with new loops based on \p
  /// LoopMap.
  void replaceLoops(LoopMapTy &LoopMap);

  /// Perform unroll & jam on all the loops with valid unroll factors in the
  /// loopnest represented by \p Lp.
  void unrollCandidates(HLLoop *Lp);

  /// Clears existing unroll candidates.
  void clearCandidates();

public:
  /// Initializes unroll factor for \p Lp.
  void initializeUnrollFactor(HLLoop *Lp);

  /// Returns unroll factor of \p Lp.
  unsigned getUnrollFactor(HLLoop *Lp);

  /// Updates unroll factor of \p Lp to \p UnrollFactor and returns the old
  /// unroll factor.
  unsigned updateUnrollFactor(HLLoop *Lp, unsigned UnrollFactor);

  /// Marks loop as unrollable.
  void throttle(HLLoop *Lp);

  /// Marks loop and all its parent loop as unrollable.
  void throttleRecursively(HLLoop *Lp);

  /// Returns true if the loop is marked unrollable.
  bool isThrottled(HLLoop *Lp);

  /// Computes the cost of the loopnest represented by \p Lp by taking into
  /// account unroll factors associated with
  unsigned computeLoopNestCost(HLLoop *Lp) const;
};

// Assigns unroll factor to outer loops using legality and profitability
// analysis.
class HIRUnrollAndJam::Analyzer final : public HLNodeVisitorBase {
  HIRUnrollAndJam &HUAJ;

public:
  Analyzer(HIRUnrollAndJam &HUAJ) : HUAJ(HUAJ) {}

  /// Performs preliminary checks to throttle loops for unroll & jam.
  void visit(HLLoop *Lp);

  /// Peforms profitability and legality checks on outer loops.
  void postVisit(HLLoop *Lp);

  /// Do nothing for instructions.
  void visit(HLInst *Inst) {}

  /// Throttle if we encounter an HLNode other than HLLoop or HLInst.
  void visit(HLNode *Node) {
    if (auto ParentLoop = Node->getLexicalParentLoop()) {
      HUAJ.throttleRecursively(ParentLoop);
    }
  }

  void postVisit(HLNode *) {}

  /// Computes and returns unroll factor for the loop using cost model. Returns
  /// 0 to indicate that unroll & jam should be throttled recursively and 1 to
  /// indicate throttling of \p HLoop only.
  unsigned computeUnrollFactorUsingCost(HLLoop *HLoop,
                                        bool HasEnablingPragma) const;

  /// Returns true if \p Lp can legally be unrolled & jammed.
  bool canLegallyUnrollAndJam(HLLoop *Lp) const;

  /// Driver function performing legality/profitability analysis on a loopnest
  /// represented by \p Lp.
  void analyze(HLLoop *Lp);
};

// Checks the legality of unroll & jam for a loop.
class LegalityChecker final : public HLNodeVisitorBase {
  DDGraph DDG;
  const HLLoop *CandidateLoop;
  unsigned LoopLevel;
  bool IsLegal;

  /// Returns true if it is legal to permute LoopLevel DV element with innermost
  /// level DV element. This is same as checking whether the two loops can be
  /// interchanged.
  bool isLegalToPermute(const DirectionVector &DV,
                        bool IsInnermostLoopDV) const;

public:
  LegalityChecker(HIRDDAnalysis &DDA, const HLLoop *Loop)
      : DDG(DDA.getGraph(Loop)), CandidateLoop(Loop),
        LoopLevel(Loop->getNestingLevel()), IsLegal(true) {}

  /// Iterates though DDRefs and checks legality of edge DVs.
  void visit(const HLDDNode *Node);

  void visit(const HLNode *Node) {}
  void postVisit(const HLNode *Node) {}

  bool isDone() const { return !IsLegal; }

  /// Driver function which checks legality of the loop.
  bool isLegal();
};
} // namespace

char HIRUnrollAndJam::ID = 0;
INITIALIZE_PASS_BEGIN(HIRUnrollAndJam, "hir-unroll-and-jam", "HIR Unroll & Jam",
                      false, false)
INITIALIZE_PASS_DEPENDENCY(HIRFrameworkWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRLoopStatisticsWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRLoopResourceWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRLoopLocalityWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRDDAnalysisWrapperPass)
INITIALIZE_PASS_END(HIRUnrollAndJam, "hir-unroll-and-jam", "HIR Unroll & Jam",
                    false, false)

FunctionPass *llvm::createHIRUnrollAndJamPass() {
  return new HIRUnrollAndJam();
}

bool LegalityChecker::isLegal() {
  HLNodeUtils::visitRange(*this, CandidateLoop->child_begin(),
                          CandidateLoop->child_end());
  return IsLegal;
}

bool LegalityChecker::isLegalToPermute(const DirectionVector &DV,
                                       bool IsInnermostLoopDV) const {
  // Legality check is the same as interchanging CandidateLoop with the
  // innermost loop so we check whether swapping the corresponding DV elements
  // yields a legal DV.

  unsigned LastLevel = DV.getLastLevel();
  assert((LastLevel >= LoopLevel) && "DV has invalid last level!");

  DVKind LoopLevelDV = DV[LoopLevel - 1];
  DVKind InnermostDV = DV[LastLevel - 1];

  // Consider edges in outer loops as permuting LoopLevel with (*) after the
  // last level DV.
  if (!IsInnermostLoopDV) {
    LastLevel++;
    InnermostDV = DVKind::ALL;
  }

  // 1. We can always permute these combinations-
  // (<, <)
  // (=, =)
  // (>, >)
  if (LoopLevelDV == InnermostDV) {
    if ((LoopLevelDV == DVKind::LT) || (LoopLevelDV == DVKind::EQ) ||
        (LoopLevelDV == DVKind::GT)) {
      return true;
    }
  }

  // 2. Check if dependence is carried by an outer loop which makes interchange
  // legal.
  if (DV.isIndepFromLevel(LoopLevel)) {
    return true;
  }

  // 3. We cannot permute outer and inner DV elements if the direction is
  // reversed in any combination after the permutation. For example (*, <)
  // yields (<, <), (=, <) and (<, >) after decomposing. The direction of (<, >)
  // gets reversed after permutation.
  if (((LoopLevelDV & DVKind::LT) && (InnermostDV & DVKind::GT)) ||
      ((LoopLevelDV & DVKind::GT) && (InnermostDV & DVKind::LT))) {
    return false;
  }

  DVKind ValidDV, InvalidDV;

  // 4. Now we check if any of the DV elements between LoopLevel to innermost
  // level preserve the direction of the DV after permutation.
  if ((LoopLevelDV == DVKind::ALL) || (InnermostDV == DVKind::ALL)) {
    // (*, =) and (=, *) can only be permuted if all intervening levels are (=).
    assert(((LoopLevelDV == DVKind::ALL && InnermostDV == DVKind::EQ) ||
            (LoopLevelDV == DVKind::EQ && InnermostDV == DVKind::ALL)) &&
           "Unexpected Direction vector!");
    ValidDV = DVKind::NONE;
    InvalidDV = DVKind::NE;
  } else {
    // At this point either one of LoopLevelDV/InnermostDV is EQ (Ex - (<, =),
    // (=, >)) or it is a composite case which decays to the former case. For
    // example, (<, <=) decays to (<, =).
    // Direction would be preserved if we find an element with the same
    // direction as LoopLevelDV/InnermostDV before finding an element with the
    // reverse direction.
    if ((LoopLevelDV & DVKind::LT) || (InnermostDV & DVKind::LT)) {
      ValidDV = DVKind::LT;
      InvalidDV = DVKind::GT;
    } else {
      ValidDV = DVKind::GT;
      InvalidDV = DVKind::LT;
    }
  }

  for (unsigned I = LoopLevel + 1; I < LastLevel; ++I) {
    if (DV[I - 1] & InvalidDV) {
      return false;
    } else if (DV[I - 1] == ValidDV) {
      return true;
    }
  }

  // All intervening elements are (or decay to) EQ so it is ok to permute.
  return true;
}

void LegalityChecker::visit(const HLDDNode *Node) {

  bool IsInnermostLoop = Node->getLexicalParentLoop()->isInnermost();

  for (auto RefIt = Node->ddref_begin(), E = Node->ddref_end(); RefIt != E;
       ++RefIt) {

    if ((*RefIt)->isTerminalRef()) {
      // Ignore edges for temps which are not livein to candidate loop.
      if (!CandidateLoop->isLiveIn((*RefIt)->getSymbase())) {
        continue;
      }
    }

    for (auto EdgeIt = DDG.outgoing_edges_begin(*RefIt),
              EE = DDG.outgoing_edges_end(*RefIt);
         EdgeIt != EE; ++EdgeIt) {
      const DDEdge *Edge = *EdgeIt;

      auto SinkNode = Edge->getSink()->getHLDDNode();

      if (!isLegalToPermute(
              Edge->getDV(),
              (IsInnermostLoop || SinkNode->getParentLoop()->isInnermost()))) {
        IsLegal = false;
        return;
      }
    }
  }
}

bool HIRUnrollAndJam::isUninitialized(HLLoop *Lp) const {
  for (auto &LoopInfo : LoopNestUFInfo[Lp->getNestingLevel() - 1]) {
    if (LoopInfo.first == Lp) {
      return false;
    }
  }

  return true;
}

void HIRUnrollAndJam::initializeUnrollFactor(HLLoop *Lp) {
  assert(isUninitialized(Lp) && "Attempt to reinitialize loop!");
  LoopNestUFInfo[Lp->getNestingLevel() - 1].emplace_back(
      Lp, Lp->isInnermost() ? 1 : MaxUnrollFactor);
}

unsigned HIRUnrollAndJam::getOrUpdateUnrollFactor(HLLoop *Lp,
                                                  unsigned UnrollFactor,
                                                  bool Update) {
  assert((!Update || (UnrollFactor <= MaxUnrollFactor)) &&
         "Invalid unroll factor!");

  auto Level = Lp->getNestingLevel();

  for (auto &LoopInfo : LoopNestUFInfo[Level - 1]) {
    if (LoopInfo.first == Lp) {
      if (!Update) {
        return LoopInfo.second;

      } else {
        assert(((UnrollFactor < 2) || (UnrollFactor <= LoopInfo.second)) &&
               "Unroll factor can only be refined downwards!");
        unsigned OldFactor = (LoopInfo.second == 0);

        if (!OldFactor) {
          LoopInfo.second = UnrollFactor;
        }
        return OldFactor;
      }
    }
  }

  llvm_unreachable("Loop not found in loop tree!");
}

unsigned HIRUnrollAndJam::getUnrollFactor(HLLoop *Lp) {
  return getOrUpdateUnrollFactor(Lp, 0, false);
}

unsigned HIRUnrollAndJam::updateUnrollFactor(HLLoop *Lp,
                                             unsigned UnrollFactor) {
  if (UnrollFactor > 1) {
    HaveUnrollCandidates = true;
  }

  return getOrUpdateUnrollFactor(Lp, UnrollFactor, true);
}

bool HIRUnrollAndJam::isThrottled(HLLoop *Lp) {
  unsigned UF = getUnrollFactor(Lp);
  return (UF <= 1);
}

void HIRUnrollAndJam::throttle(HLLoop *Lp) { updateUnrollFactor(Lp, 1); }

void HIRUnrollAndJam::throttleRecursively(HLLoop *Lp) {
  while (!updateUnrollFactor(Lp, 0) && (Lp = Lp->getParentLoop())) {
  }
}

void HIRUnrollAndJam::Analyzer::visit(HLLoop *Lp) {

  HUAJ.initializeUnrollFactor(Lp);

  if (!Lp->isDo()) {
    DEBUG(dbgs() << "Skipping unroll & jam of non-DO loop!\n");
    HUAJ.throttleRecursively(Lp);
    return;
  }

  // TODO: What is the right behavior for vectorizable loops?
  if (Lp->isVecLoop()) {
    DEBUG(dbgs() << "Skipping unroll & jam of vectorizable loop!\n");
    HUAJ.throttleRecursively(Lp);
    return;
  }

  auto &LS = HUAJ.HLS->getSelfLoopStatistics(Lp);

  // Cannot unroll loop if it has calls with noduplicate attribute.
  if (LS.hasCallsWithNoDuplicate()) {
    DEBUG(dbgs() << "Skipping unroll & jam of loopnest containing call(s) with "
                    "NoDuplicate attribute !\n");
    HUAJ.throttleRecursively(Lp);
    return;
  }

  if (!Lp->isInnermost()) {
    if (!Lp->isNormalized()) {
      DEBUG(dbgs() << "Skipping unroll & jam of non-normalized loop!\n");
      HUAJ.throttle(Lp);
      return;

    } else if (Lp->hasUnrollAndJamDisablingPragma()) {
      DEBUG(dbgs() << "Skipping unroll & jam of pragma disabled loop!\n");
      HUAJ.throttle(Lp);
      return;
    }
  } else if (Lp->hasUnrollEnablingPragma()) {
    // TODO: Check this for all loops when we have unroll & jam metadata.
    DEBUG(dbgs()
          << "Skipping unroll & jam as innermost loop has unroll pragma!\n");
    HUAJ.throttleRecursively(Lp);
    return;
  }

  // Throttle unroll of outer loop whose inner loop's bounds varies within the
  // outer loop, as they cannot be fused.
  if (Lp->getParentLoop()) {
    for (auto RefIt = Lp->ddref_begin(), E = Lp->ddref_end(); RefIt != E;
         ++RefIt) {

      auto CE = (*RefIt)->getSingleCanonExpr();

      if (unsigned DefLevel = CE->getDefinedAtLevel()) {
        DEBUG(
            dbgs() << "Skipping unroll & jam for loopnest as it is illegal!\n");
        HUAJ.throttleRecursively(Lp->getParentLoopAtLevel(DefLevel));
      }

      for (auto IV = CE->iv_begin(), IVE = CE->iv_end(); IV != IVE; ++IV) {
        if (CE->getIVConstCoeff(IV) != 0) {
          DEBUG(dbgs() << "Skipping unroll & jam for loop as it is illegal!\n");
          HUAJ.throttle(Lp->getParentLoopAtLevel(CE->getLevel(IV)));
        }
      }
    }
  }
}

unsigned HIRUnrollAndJam::computeLoopNestCost(HLLoop *Lp) const {
  unsigned Cost = HLR->getSelfLoopResource(Lp).getTotalCost();

  if (Lp->isInnermost()) {
    return Cost;
  }

  bool ChildrenFound = false;

  // Immediate children appear in a contiguous chunk in the next level of
  // LoopNestUFInfo.
  for (auto &ChildLoopInfo : LoopNestUFInfo[Lp->getNestingLevel()]) {
    auto ChildLp = ChildLoopInfo.first;

    if (ChildLp->getParentLoop() != Lp) {
      if (!ChildrenFound) {
        // Haven't encountered any children yet, keep looking.
        continue;
      } else {
        break;
      }
    }
    ChildrenFound = true;

    unsigned UnrollFactor = ChildLoopInfo.second ? ChildLoopInfo.second : 1;

    Cost += (UnrollFactor * computeLoopNestCost(ChildLp));
  }

  assert(ChildrenFound && "No children found for non-innermost loop!");

  return Cost;
}

unsigned HIRUnrollAndJam::Analyzer::computeUnrollFactorUsingCost(
    HLLoop *Lp, bool HasEnablingPragma) const {
  unsigned LoopCost = HUAJ.HLR->getSelfLoopResource(Lp).getTotalCost();

  if (LoopCost > MaxOuterLoopCost) {
    DEBUG(dbgs() << "Skipping unroll & jam of loop as the loop body cost "
                    "exceeds threshold!\n");
    return 0;
  }

  unsigned LoopNestCost = HUAJ.computeLoopNestCost(Lp);

  if ((2 * LoopNestCost) > MaxUnrolledLoopNestCost) {
    DEBUG(dbgs() << "Skipping unroll & jam of loop as the unrolled loop body "
                    "cost exceeds threshold!\n");
    return 0;
  }

  uint64_t TC;
  bool IsConstTC = Lp->isConstTripLoop(&TC);
  unsigned UnrollFactor;

  if (HasEnablingPragma) {
    // TODO: fix this when frontend implements unroll & jam pragma.
    UnrollFactor = Lp->getUnrollPragmaCount();

    if (!UnrollFactor) {
      UnrollFactor = MaxUnrollFactor;
    } else if (UnrollFactor == 1) {
      DEBUG(dbgs() << "Skipping unroll & jam as pragma count is set to 1!\n");
      return 0;
    }

    if (IsConstTC) {
      if (TC < 3) {
        DEBUG(dbgs() << "Skipping unroll & jam of pragma enabled loop as trip "
                        "count is too small!\n");
        return 1;
      }

      if (TC <= UnrollFactor) {
        UnrollFactor = TC / 2;
      }
    }

    if ((UnrollFactor * LoopNestCost) > MaxUnrolledLoopNestCost) {
      // This it to avoid encountering unroll factor of 1 in the while loop
      // below when using pragma count. For example if the pragma unroll factor
      // is 3, we get 1 on dividing by 2.
      UnrollFactor = PowerOf2Floor(UnrollFactor);
    }

  } else {
    if ((IsConstTC || (TC = Lp->getMaxTripCountEstimate())) &&
        (TC < MinTripCountThreshold)) {
      DEBUG(dbgs() << "Skipping unroll & jam of small trip count loop!\n");
      return 1;
    }
    UnrollFactor = MaxUnrollFactor;
  }

  while ((UnrollFactor * LoopNestCost) > MaxUnrolledLoopNestCost) {
    UnrollFactor /= 2;
  }

  assert(UnrollFactor >= 2 && "Unexpected unroll factor!");

  return UnrollFactor;
}

bool HIRUnrollAndJam::Analyzer::canLegallyUnrollAndJam(HLLoop *Lp) const {
  // TODO: use a smaller unroll factor if allowed by the distance vector.
  LegalityChecker LC(*HUAJ.DDA, Lp);

  return LC.isLegal();
}

void HIRUnrollAndJam::Analyzer::postVisit(HLLoop *Lp) {

  if (Lp->isInnermost() || HUAJ.isThrottled(Lp)) {
    return;
  }

  bool HasEnablingPragma = Lp->hasUnrollAndJamEnablingPragma();

  unsigned UnrollFactor = computeUnrollFactorUsingCost(Lp, HasEnablingPragma);

  if (!UnrollFactor) {
    HUAJ.throttleRecursively(Lp);
    return;
  } else if (UnrollFactor == 1) {
    HUAJ.throttle(Lp);
    return;
  }

  if (!HasEnablingPragma &&
      // TODO: refine unroll factor using extra cache lines accessed by
      // unrolling?
      !HUAJ.HLA->hasTemporalLocality(Lp, UnrollFactor - 1)) {
    DEBUG(
        dbgs()
        << "Skipping unroll & jam as loop does not have temporal locality!\n");
    HUAJ.throttle(Lp);
    return;
  }

  if (!canLegallyUnrollAndJam(Lp)) {
    DEBUG(dbgs() << "Skipping unroll & jam for loop as it is illegal!\n");
    HUAJ.throttle(Lp);
    return;
  }

  HUAJ.updateUnrollFactor(Lp, UnrollFactor);
}

void HIRUnrollAndJam::Analyzer::analyze(HLLoop *Lp) {
  HLNodeUtils::visit(*this, Lp);
}

void HIRUnrollAndJam::sanitizeOptions() {

  // Set a sane unroll factor.
  if (MaxUnrollFactor < 2) {
    MaxUnrollFactor = 2;

  } else if (MaxUnrollFactor > AbsoluteMaxUnrollFactor) {
    MaxUnrollFactor = AbsoluteMaxUnrollFactor;

  } else if (!isPowerOf2_32(MaxUnrollFactor)) {
    MaxUnrollFactor = DefaultMaxUnrollFactor;
  }

  // Set a sane minimum trip threshold.
  unsigned MinExpectedThreshold = (2 * MaxUnrollFactor);

  if (MinTripCountThreshold < MinExpectedThreshold) {
    MinTripCountThreshold = MinExpectedThreshold;
  }
}

void HIRUnrollAndJam::clearCandidates() {
  for (auto &UFInfo : LoopNestUFInfo) {
    UFInfo.clear();
  }

  HaveUnrollCandidates = false;
}

void HIRUnrollAndJam::replaceLoops(LoopMapTy &LoopMap) {

  for (auto &LoopPair : LoopMap) {
    unsigned LoopLevel = LoopPair.second->getNestingLevel();
    bool Found = false;

    for (auto &UFInfo : LoopNestUFInfo[LoopLevel - 1]) {
      if (UFInfo.first == LoopPair.first) {
        UFInfo.first = LoopPair.second;
        Found = true;
        break;
      }
    }
    (void)Found;
    assert(Found && "Inner loop not found!");
  }
}

void HIRUnrollAndJam::unrollCandidates(HLLoop *Lp) {
  if (!HaveUnrollCandidates) {
    return;
  }

  // Set gen code as we will be performing unroll & jam on at least one loop in
  // this loopnest.
  Lp->getParentRegion()->setGenCode();

  for (auto &LoopsPerLevel : LoopNestUFInfo) {
    for (auto &LoopUFPair : LoopsPerLevel) {
      if (LoopUFPair.second > 1) {
        LoopMapTy LoopMap;

        unrollLoopImpl(LoopUFPair.first, LoopUFPair.second, &LoopMap);
        replaceLoops(LoopMap);
        LoopsUnrolledAndJammed++;
      }
    }
  }
}

bool HIRUnrollAndJam::runOnFunction(Function &F) {
  if (DisableHIRUnrollAndJam || skipFunction(F)) {
    return false;
  }

  auto HIRF = &getAnalysis<HIRFrameworkWrapperPass>().getHIR();
  HLS = &getAnalysis<HIRLoopStatisticsWrapperPass>().getHLS();
  HLR = &getAnalysis<HIRLoopResourceWrapperPass>().getHLR();
  HLA = &getAnalysis<HIRLoopLocalityWrapperPass>().getHLL();
  DDA = &getAnalysis<HIRDDAnalysisWrapperPass>().getDDA();

  sanitizeOptions();

  SmallVector<HLLoop *, 16> OutermostLoops;

  HIRF->getHLNodeUtils().gatherOutermostLoops(OutermostLoops);

  Analyzer AY(*this);

  for (auto Loop : OutermostLoops) {
    AY.analyze(Loop);
    unrollCandidates(Loop);
    clearCandidates();
  }

  return false;
}

namespace {

typedef SmallVector<unsigned, 8> TempBlobIndexVecTy;
typedef std::pair<unsigned, TempBlobIndexVecTy> TempBlobIndexMap;

// Stores the mapping of temps in outer loops to renamed temps in each unrolled
// iteration.
typedef SmallVector<TempBlobIndexMap, 6> TempRenamingMapTy;

// Updates CanonExprs for unroll/unroll & jam.
class CanonExprUpdater final : public HLNodeVisitorBase {
private:
  unsigned Level;
  unsigned UnrollFactor;
  unsigned UnrollCnt;
  bool CreateNewLvalTemps;
  bool RenameTemps;
  TempRenamingMapTy &TempRenamingMap;

  void processRegDDRef(RegDDRef *RegDD);
  void processCanonExpr(CanonExpr *CExpr);

public:
  CanonExprUpdater(unsigned Level, unsigned UF,
                   TempRenamingMapTy &TempRenamingMap)
      : Level(Level), UnrollFactor(UF), UnrollCnt(-1),
        CreateNewLvalTemps(false), RenameTemps(false),
        TempRenamingMap(TempRenamingMap) {}

  unsigned getUnrollFactor() const { return UnrollFactor; }
  void setUnrollCount(unsigned Count) { UnrollCnt = Count; }

  void setCreateNewLvalTemps(bool Flag) { CreateNewLvalTemps = Flag; }

  void renameTemps(bool Flag) { RenameTemps = Flag; }

  /// No processing needed for Goto
  void visit(HLGoto *Goto){};
  /// No processing needed for Label
  void visit(HLLabel *Label){};
  void visit(HLDDNode *Node);
  void visit(HLNode *Node) {
    llvm_unreachable(" Node not supported for unrolling.");
  };
  void postVisit(HLNode *Node) {}

  void createLvalTempMapping(RegDDRef *LvalRef);
};

struct UnrollInfo {
  CanonExprUpdater CEUpdater;
  LoopMapTy *LoopMap;
  HLLabel *ExitLabel;
  bool NeedRemainderLoop;

  TempRenamingMapTy TempRenamingMap;

  UnrollInfo(unsigned LoopLevel, unsigned UnrollFactor, LoopMapTy *LoopMap,
             HLLabel *ExitLabel, bool NeedRemainderLoop)
      : CEUpdater(LoopLevel, UnrollFactor, TempRenamingMap), LoopMap(LoopMap),
        ExitLabel(ExitLabel), NeedRemainderLoop(NeedRemainderLoop) {}
};

} // namespace

void CanonExprUpdater::visit(HLDDNode *Node) {
  assert((UnrollCnt < UnrollFactor) && "Invalid unroll count!");

  for (auto Iter = Node->ddref_begin(), End = Node->ddref_end(); Iter != End;
       ++Iter) {
    processRegDDRef(*Iter);
  }
}

void CanonExprUpdater::createLvalTempMapping(RegDDRef *Ref) {
  if (!CreateNewLvalTemps) {
    return;
  }

  if (!Ref->isTerminalRef() || !Ref->isLval() || Ref->isFakeLval()) {
    return;
  }

  unsigned OldTempIndex =
      Ref->isSelfBlob()
          ? Ref->getSelfBlobIndex()
          : Ref->getBlobUtils().findTempBlobIndex(Ref->getSymbase());

  auto TempIt = TempRenamingMap.end();

  for (auto It = TempRenamingMap.begin(), E = TempRenamingMap.end(); It != E;
       ++It) {
    if (It->first == OldTempIndex) {
      if (It->second.size() > UnrollCnt) {
        // Temp has been renamed already for the current unrolled iteration. We
        // have found another temp definition. We should keep using the existing
        // mapping.
        return;
      }
      TempIt = It;
      break;
    }
  }

  unsigned NewTempIndex =
      Ref->getHLDDNode()->getHLNodeUtils().createAndReplaceTemp(Ref);

  if (TempIt != TempRenamingMap.end()) {
    TempIt->second.push_back(NewTempIndex);
  } else {
    TempRenamingMap.emplace_back(OldTempIndex,
                                 TempBlobIndexVecTy(1, NewTempIndex));
  }
}

void CanonExprUpdater::processRegDDRef(RegDDRef *Ref) {

  createLvalTempMapping(Ref);

  if (RenameTemps) {
    for (auto &TempEntry : TempRenamingMap) {
      unsigned OldTempIndex = TempEntry.first;

      if (TempEntry.second.size() > UnrollCnt) {
        unsigned NewTempIndex = TempEntry.second[UnrollCnt];

        Ref->replaceTempBlob(OldTempIndex, NewTempIndex);
      }
    }
  }

  for (auto Iter = Ref->canon_begin(), End = Ref->canon_end(); Iter != End;
       ++Iter) {
    processCanonExpr(*Iter);
  }
}

/// Processes CanonExpr to modify IV to:
/// IV*UF + (Original IVCoeff)*UnrollCnt.
void CanonExprUpdater::processCanonExpr(CanonExpr *CExpr) {
  if (UnrollCnt) {
    CExpr->shift(Level, UnrollCnt);
  }

  CExpr->multiplyIVByConstant(Level, UnrollFactor);
  CExpr->simplify(true);
}

static void patchIntermediateBottomTest(HLIf *BottomTest, HLLabel *ExitLabel) {

  auto PredIter = BottomTest->pred_begin();
  auto FirstChild = BottomTest->getFirstThenChild();

  auto Goto = cast<HLGoto>(FirstChild);

  // Invert predicate and make it jump to ExitLabel.
  BottomTest->invertPredicate(PredIter);
  Goto->setTargetLabel(ExitLabel);
}

static void createUnrolledNodeRange(HLNode *FirstNode, HLNode *LastNode,
                                    HLContainerTy &NodeRange, UnrollInfo &UInfo,
                                    bool IsInnermostLoop) {
  assert(NodeRange.empty() && "Empty node range expected!");

  HLNode *CurFirstChild = nullptr;
  HLNode *CurLastChild = nullptr;

  unsigned UnrollFactor = UInfo.CEUpdater.getUnrollFactor();
  unsigned UnrollTrip =
      UInfo.NeedRemainderLoop ? UnrollFactor : UnrollFactor - 1;

  // We need to create new mapping for lval temps in outer loops.
  UInfo.CEUpdater.setCreateNewLvalTemps(!IsInnermostLoop);
  UInfo.CEUpdater.renameTemps(true);

  for (unsigned UnrollCnt = 0; UnrollCnt < UnrollTrip; ++UnrollCnt) {
    HLNodeUtils::cloneSequence(&NodeRange, FirstNode, LastNode);

    CurFirstChild = (UnrollCnt == 0)
                        ? &(NodeRange.front())
                        : &*(std::next(CurLastChild->getIterator()));
    CurLastChild = &(NodeRange.back());

    UInfo.CEUpdater.setUnrollCount(UnrollCnt);

    if (UnrollCnt == (UnrollFactor - 1)) {
      // No need to rename in the last unrolled iteration.
      // This preserves liveouts of the top level loop.
      UInfo.CEUpdater.setCreateNewLvalTemps(false);
      UInfo.CEUpdater.renameTemps(false);
    }

    HLNodeUtils::visitRange(UInfo.CEUpdater, CurFirstChild, CurLastChild);

    if (UInfo.ExitLabel) {
      patchIntermediateBottomTest(cast<HLIf>(CurLastChild), UInfo.ExitLabel);
    }
  }

  // Reuse original nodes for the last unrolled iteration.
  if (!UInfo.NeedRemainderLoop) {
    UInfo.CEUpdater.setUnrollCount(UnrollTrip);

    // No need to rename in the last unrolled iteration.
    // This preserves liveouts of the top level loop.
    UInfo.CEUpdater.setCreateNewLvalTemps(false);
    UInfo.CEUpdater.renameTemps(false);

    HLNodeUtils::visitRange(UInfo.CEUpdater, FirstNode->getIterator(),
                            std::next(LastNode->getIterator()));

    HLNodeUtils::remove(&NodeRange, FirstNode->getIterator(),
                        std::next(LastNode->getIterator()));
  }
}

static HLNode *getLastNodeInRange(HLNode *FirstNode) {
  HLNode *LastNode = FirstNode;

  for (HLNode *NextNode = FirstNode; (NextNode && !isa<HLLoop>(NextNode));
       NextNode = NextNode->getNextNode()) {
    LastNode = NextNode;
  }

  return LastNode;
}

static void addRenamedTempsAsLiveinLiveout(HLLoop *Loop,
                                           TempRenamingMapTy &TempRenamingMap) {
  auto &BU = Loop->getBlobUtils();

  for (auto &TempEntry : TempRenamingMap) {
    unsigned OldSymbase = BU.getTempBlobSymbase(TempEntry.first);

    if (Loop->isLiveIn(OldSymbase)) {
      for (unsigned RenamedTempBlob : TempEntry.second) {
        Loop->addLiveInTemp(BU.getTempBlobSymbase(RenamedTempBlob));
      }
    }

    if (Loop->isLiveOut(OldSymbase)) {
      for (unsigned RenamedTempBlob : TempEntry.second) {
        Loop->addLiveOutTemp(BU.getTempBlobSymbase(RenamedTempBlob));
      }
    }
  }
}

static void unrollLoopRecursive(HLLoop *OrigLoop, HLLoop *NewLoop,
                                UnrollInfo &UInfo, bool IsTopLoop) {
  HLContainerTy NodeRange;

  if (!IsTopLoop) {
    // Unroll preheader/postexit for non top level loops.
    if (OrigLoop->hasPreheader()) {
      createUnrolledNodeRange(OrigLoop->getFirstPreheaderNode(),
                              OrigLoop->getLastPreheaderNode(), NodeRange,
                              UInfo, false);
      HLNodeUtils::insertAsFirstPreheaderNodes(NewLoop, &NodeRange);
    }

    if (OrigLoop->hasPostexit()) {
      createUnrolledNodeRange(OrigLoop->getFirstPostexitNode(),
                              OrigLoop->getLastPostexitNode(), NodeRange, UInfo,
                              false);
      HLNodeUtils::insertAsFirstPostexitNodes(NewLoop, &NodeRange);
    }

    addRenamedTempsAsLiveinLiveout(NewLoop, UInfo.TempRenamingMap);
  }

  HLNode *CurFirstNode = OrigLoop->getFirstChild();
  bool IsInnermost = false;

  if (OrigLoop == NewLoop) {
    // Skip loop label cloning for unknown loops.
    CurFirstNode = CurFirstNode->getNextNode();
    IsInnermost = true;
  } else {
    IsInnermost = OrigLoop->isInnermost();
  }

  while (CurFirstNode) {
    // Avoid unnecessary node traversal for innermost loops as their body will
    // be handled as a single node range.
    HLNode *CurLastNode = IsInnermost ? OrigLoop->getLastChild()
                                      : getLastNodeInRange(CurFirstNode);

    // Keep pointer to next node in case this one is moved (for last unrolled
    // iteration).
    HLNode *NextFirstNode = CurLastNode->getNextNode();

    // Unroll & Jam mode
    if (auto ChildLoop = dyn_cast<HLLoop>(CurFirstNode)) {
      assert((CurFirstNode == CurLastNode) &&
             "Single node range expected for loops!");
      assert(UInfo.LoopMap && "Non-null loop map expected!");

      HLLoop *NewInnerLoop = ChildLoop->cloneEmptyLoop();
      UInfo.LoopMap->emplace_back(ChildLoop, NewInnerLoop);

      HLNodeUtils::insertAsLastChild(NewLoop, NewInnerLoop);
      unrollLoopRecursive(ChildLoop, NewInnerLoop, UInfo, false);

    } else {
      createUnrolledNodeRange(CurFirstNode, CurLastNode, NodeRange, UInfo,
                              IsInnermost);
      HLNodeUtils::insertAsLastChildren(NewLoop, &NodeRange);
    }

    CurFirstNode = NextFirstNode;
  }
}

static void unrollMainLoop(HLLoop *OrigLoop, HLLoop *MainLoop,
                           unsigned UnrollFactor, bool NeedRemainderLoop,
                           LoopMapTy *LoopMap) {

  auto &HNU = OrigLoop->getHLNodeUtils();
  HLLabel *ExitLabel = nullptr;

  // Unknown loop unrollng.
  if (OrigLoop == MainLoop) {
    assert(OrigLoop->isUnknown() && "Unknown loop expected!");
    assert(OrigLoop->isInnermost() && "Only innermost unknown loops expected!");

    // Extract postexit before adding an exit label.
    MainLoop->extractPostexit();

    // Insert exit label.
    ExitLabel = HNU.createHLLabel("loopexit");
    HLNodeUtils::insertAfter(MainLoop, ExitLabel);
  }

  UnrollInfo UInfo(OrigLoop->getNestingLevel(), UnrollFactor, LoopMap,
                   ExitLabel, NeedRemainderLoop);

  HLNode *MarkerNode = HNU.getOrCreateMarkerNode();

  // Replace loop by marker node, until we are done populating it so we can
  // insert all the nodes in one go.
  // This saves multiple topsort num recalculations.
  HLNodeUtils::replace(MainLoop, MarkerNode);

  unrollLoopRecursive(OrigLoop, MainLoop, UInfo, true);

  // Insert loop back in HIR.
  HLNodeUtils::replace(MarkerNode, MainLoop);
}

void unrollLoopImpl(HLLoop *Loop, unsigned UnrollFactor, LoopMapTy *LoopMap) {
  assert(Loop && "Loop is null!");
  assert((UnrollFactor > 1) && "Invalid unroll factor!");

  bool NeedRemainderLoop = false;
  bool IsUnknownLoop = Loop->isUnknown();
  HLLoop *MainLoop = nullptr;

  LoopOptReportBuilder &LORBuilder =
      Loop->getHLNodeUtils().getHIRFramework().getLORBuilder();

  if (IsUnknownLoop) {
    MainLoop = Loop;
    MainLoop->getParentRegion()->setGenCode();
    MainLoop->setNumExits(MainLoop->getNumExits() * UnrollFactor);

    LORBuilder(*MainLoop).addRemark(
        OptReportVerbosity::Low,
        "Unknown loop has been partially unrolled with %d factor",
        UnrollFactor);
  } else {
    // Create the unrolled main loop and setup remainder loop.
    MainLoop = HIRTransformUtils::setupMainAndRemainderLoops(
        Loop, UnrollFactor, NeedRemainderLoop, LORBuilder);
  }

  unrollMainLoop(Loop, MainLoop, UnrollFactor, NeedRemainderLoop, LoopMap);

  // If a remainder loop is not needed get rid of the OrigLoop at this point.
  if (!NeedRemainderLoop && !IsUnknownLoop) {
    // Invalidate analysis for original loopnest if remainder loop is not needed
    // since we reuse the instructions inside them.
    HIRInvalidationUtils::invalidateLoopNestBody(Loop);

    HLNodeUtils::remove(Loop);
  }
}
