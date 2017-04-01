//===--- HIRScalarReplArray.h -Loop Scalar Replacement --- --*- C++ -*---===//
//
// Copyright (C) 2015-2016 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===--------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_SCALARREPL_ARRAY_H
#define LLVM_TRANSFORMS_INTEL_LOOPTRANSFORMS_SCALARREPL_ARRAY_H

#include "llvm/Pass.h"
#include "llvm/Transforms/Intel_LoopTransforms/HIRTransformPass.h"
#include "llvm/Transforms/Intel_LoopTransforms/Utils/DDRefGrouping.h"

namespace llvm {
class Function;

namespace loopopt {
class DDGraph;
class HIRDDAnalysis;
class HIRLocalityAnalysis;
struct LoopStatistics;

namespace scalarreplarray {

class HIRScalarReplArray;

// RefTuple has a (MemRef, TmpId, TmpRef) tuple, where:
// - MemRef: a MemRef RegDDRef*;
// - TmpId:  the dependence-distance to the 1st MemRef item in a group;
// - TmpRef: a Tmp RegDDRef * that can later replace MemRef in scalar repl;
class RefTuple {
  RegDDRef *MemRef = nullptr;
  int64_t TmpId;
  RegDDRef *TmpRef = nullptr;

public:
  RefTuple(RegDDRef *InitRef) : MemRef(InitRef), TmpId(-1), TmpRef(nullptr) {}

  // Getters + Setters
  RegDDRef *getMemRef(void) const { return MemRef; }
  void setMemRef(RegDDRef *Ref) { MemRef = Ref; }
  int64_t getTmpId(void) const { return TmpId; }
  void setTmpId(int64_t Id) { TmpId = Id; }
  RegDDRef *getTmpRef(void) const { return TmpRef; }
  void setTmpRef(RegDDRef *Ref) { TmpRef = Ref; }

#ifndef NDEBUG
  void print(bool NewLine = false) const;
#endif
};

typedef DDRefGrouping::RefGroupTy<const RegDDRef> RefGroupTy;

// MemRefGroup has a vector of RefTuple (RefTupleVec), a vector of TmpRef
// (TmpV), and some supporting data
//
// It collects relevant MemRefs having the same BaseCE and Symbase from a loop.
// This class also serves the basis for scalar-repl analysis and transformation.
//
struct MemRefGroup {
  SmallVector<RefTuple, 8> RefTupleVec;
  SmallVector<RegDDRef *, 8> TmpV;

  bool HasRWGap;

  HIRScalarReplArray *HSRA = nullptr;
  unsigned Symbase;
  CanonExpr *BaseCE = nullptr;

  HLLoop *Lp = nullptr;
  DDGraph &DDG;
  unsigned MaxDepDist, NumLoads, NumStores, LoopLevel;
  bool IsLegal, IsProfitable, IsPostChecksOk, IsSuitable;

  RefTuple *MaxIdxLoadRT = nullptr, *MinIdxStoreRT = nullptr;

  MemRefGroup(RefGroupTy &Group, HIRScalarReplArray *HSRA, DDGraph &DDG);

  // Getters + Setters
  bool isSuitable(void) const { return IsSuitable; }
  void setSuitable(bool NewFlag) { IsSuitable = NewFlag; }

  void setMaxDepDist(unsigned MDD) { MaxDepDist = MDD; }
  unsigned getMaxDepDist(void) const { return MaxDepDist; }

  unsigned getNumTemps(void) const { return MaxDepDist + 1; }

  bool isLoadOnly(void) { return (NumStores == 0); }
  unsigned getNumStores(void) const { return NumStores; }

  // Only have Stores, and NO MemRef gap
  bool isCompleteStoreOnly(void);

  // Check if the group has store refs whose largest DepDist is greater than or
  // equal to a given Dist.
  bool hasStoreDepDistGreaterEqualOne(void) const;

  // Insert a given RegDDRef into RefTupleVec
  void insert(RegDDRef *Ref) { RefTupleVec.push_back(RefTuple(Ref)); }

  unsigned getSize(void) const { return RefTupleVec.size(); }

  SmallVector<RefTuple, 8> &getRefTupleVec(void) { return RefTupleVec; }
  SmallVector<RegDDRef *, 8> &getTmpV(void) { return TmpV; };

  RefTuple *getMaxIdxLoadRT(void) const { return MaxIdxLoadRT; }
  RefTuple *getMinIdxStoreRT(void) const { return MinIdxStoreRT; }

  // Obtain the 1st available RefTuple * by distance
  const RefTuple *getByDist(unsigned Dist) const;

  // Does a given RegDDRef* physically belong to MRG?
  // (use direct pointer comparison)
  bool belongs(RegDDRef *Ref) const;

  // Inside the loop's body, within the MRG, mark if there is 1 MemRef(R) that
  // needs to generate a load right before the MemRef.
  //
  // Mark Max-index load with MIN TOPO#: may find if #Loads >0
  // E.g. .., A[i+3](.), A[i+4](R) .. A[i+4](R) ...
  //                     ^max_index load with MinTOPO#
  //
  // - examine all MemRef(s)(R) whose DepDist is MaxDepDist
  // - if not empty:
  //   . mark the one with MIN TOPO# as MaxLoad
  //
  void markMaxLoad(void);

  // Mark min-index store with MAX TOPO#: must find if #Stores >0
  // E.g. A[i](W), A[i](W), A[i](W), A[i+1](.) ...
  //                        ^min_index store with MaxTOPO#
  //
  // - identify the MinDD (DD of 1st store)
  // - examine all MemRef(s)(W) whose DD is MinDD
  // - if not empty:
  //   . mark the one with MAX TOPO# as MinStore
  //
  void markMinStore(void);

  // Identify any missing MemRef (GAP), result is in RWGap vector
  void identifyGaps(SmallVectorImpl<bool> &RWGap);

  // Analyze the MRG, return true if the MRG is suitable for scalar repl
  //
  // Analysis:
  // -Count #Loads, #Stores
  // -Legal test
  // -Profit test
  // -Check and set MaxDepDist
  // -Post Checks
  //
  bool analyze(HLLoop *Lp, DDGraph &DDG);

  // A MRG is legal IF&F each DDEdge is legal:
  // - for each valid DDEdge, Refs on both ends belong to the same MRG;
  bool isLegal(DDGraph &DDG) const;

  // each valid DDEdge, both ends of the Edge must be in MRG
  template <bool IsIncoming> bool areDDEdgesInSameMRG(DDGraph &DDG) const;

  // A MRG is profitable IF&F it has at least 1 non anti-dependence DDEdge
  //
  // This is further simplified as: a MRG is NOT profitable IF&F
  // - it has only 2 MemRefs (1 load, 1 store)
  // (and)
  // - MaxLoad and MinStore both exist
  //
  // Since MinStore exists with 1 store, no need to check it.
  //
  bool isProfitable(void) const {
    return !((NumLoads == 1) && (NumStores == 1) && MaxIdxLoadRT) && hasReuse();
  }

  // A MemRefGroup has reuse if the group's MaxDepDist is smaller than the
  // loop's trip count.
  // TODO: this is conservative: it treats partial reuse as no reuse.
  // (May need to fine tune it.)
  bool hasReuse(void) const;

  // Collect 1st ref (load or store) whose DistTo1stRef <MaxDD (for load)
  // or >1 (for store).
  //
  // Test the Ref:
  // can every loop-level IV be merged or replaced by its BoundCE?
  bool doPostCheckOnRef(const HLLoop *Lp, bool IsLoad);

  // Check the group's Max DepDist exist and is within bound:
  // - MaxDepDist must be < Threshold
  // (and)
  // - Set MaxDepDist
  //
  void checkAndSetMaxDepDist(void);

  // -postCheck on Loads if applicable
  // -postCheck on Stores if applicable
  // -postCheck on max DepDist:checkAndSetMaxDepDist
  bool doPostChecks(const HLLoop *Lp);

  // handle Temp(s):
  // - create all needed temps and store them into TmpV vector
  // - associate each MemRef with its matching Tmp
  //
  // E.g.
  // [BEFORE]
  // RTV: {(A[i], -1, null), (A[i+4], -1, null)}
  // TmpV:{}
  //
  // AFTER:
  // RTV: {(A[i], 0, t0), (A[i+4], 4, t4)}
  // TmpV: {t0, t1, t2, t3, t4}
  //
  void handleTemps(void);

  // Generate temp-rotation code
  // E.g. with temps in [t0 .. tN]
  // Temp-Rotation code looks like: t0=t1; t1=t2; ...; tN-1=tN;
  //
  void generateTempRotation(HLLoop *Lp);

  // Generate Loads (from MemRef into its matching Tmp) when needed
  //
  // Note:
  // a load is needed if a non-max_index MemRef[i+r](R) exists in a
  // loop's
  // body.
  //
  // a load is also needed even if a MemRef[i+r](R) doesn't exist in
  // a loop's
  // body (GAP), provided r is [0 .. max_index).
  //
  // E.g.
  // i: 0, 100, 1
  // |  B[i] = A[i] + A[i+4];
  //
  // Though reads on A[i+1] .. A[i+3] are not explicitly available
  // in the loop's
  // body, we still need to initialize t1=A[i+1],t2=A[i+2],t3=A[i+3]
  // for i = LB,
  // to ensure those temps are properly initialized before rotation.
  //
  // and mark each Temp as LiveIn to the loop
  //
  void generateLoadToTmps(HLLoop *Lp, SmallVectorImpl<bool> &RWGap);

  // Generate a load from MemRef to TmpRef code
  // E.g. t1 = A[i+1];
  void generateLoadWithMemRef(HLLoop *Lp, RegDDRef *MemRef, unsigned Index,
                              RegDDRef *TmpRef, bool IndepMemRef,
                              CanonExpr *LBCE);

  bool verify(void);

#ifndef NDEBUG
  // E.g.: {A[i].R, A[i+1].W, ... } 3W:2R
  void print(bool NewLine = true);
  void printRefTupleVec(bool NewLine = false);
  void printTmpVec(bool NewLine = false);
#endif
};

class HIRScalarReplArray : public HIRTransformPass {
  friend MemRefGroup;

  HIRDDAnalysis *HDDA = nullptr;
  HIRLocalityAnalysis *HLA = nullptr;
  HIRLoopStatistics *HLS = nullptr;
  unsigned LoopLevel;

  SmallVector<MemRefGroup, 8> MRGVec;

  HLNodeUtils *HNU = nullptr;
  DDRefUtils *DDRU = nullptr;
  CanonExprUtils *CEU = nullptr;
  bool Is32Bit; // Check if target is a 32b or 64b platform
  unsigned ScalarReplArrayMaxDepDist;

public:
  static char ID;

  HIRScalarReplArray(void);

  bool doInitialization(Module &M) override;

  bool runOnFunction(Function &F) override;

  void setupEnvForLoop(const HLLoop *Lp);

  // return true if there is at least 1 MRG suitable for scalar repl.
  bool doAnalysis(HLLoop *Lp);

  // Check Lp's conditions:
  // - Multiple exits;
  // - Skip if the loop has been vectorized
  // - Run a statistics on Loop and check it has no: goto/call
  bool doPreliminaryChecks(const HLLoop *Lp);

  // Collect relevant MemRefs with the same Symbase and BaseCE
  // by calling HIRLocalityAnalysis::populateTemporalLocalityGroups(.)
  bool doCollection(HLLoop *Lp, DDGraph &DDG);

  // Check if a group formed by HIRLocalityAnalysis is valid:
  //
  // the group:
  // - is not a single-entry group;
  //
  // Check only 1 occurrence: a group is suitable
  // - if it has a loop-level IV
  // - unless %blob is NonLinear
  // - negative IVCoeff is ok (we will just reverse the order in the group)
  // - check Blob: reject any Ref with a valid IVBlob
  //  (TODO: allow a group if the IVBlob is known to be positive or negative.
  //         need to adjust HasNegIVCoeff in such case.)
  //
  // Check each occurrence (of MemRef):
  // - has no volatile
  // - not inside any HLIf/HLSwitch/.
  //
  bool isValid(RefGroupTy &Group, bool &HasNegIVCoeff);

  // Check the given RegDDRef*, on any loop-level matching CE:
  // - any negative IvCoeff?
  // - any valid IvBlob?
  //
  // - TODO:
  //   if the IvBlob is known to be positive or negative, combine it with the
  //   sign on IvCoeff to decide whether the CE has overall negative factor.
  //
  bool checkIV(const RegDDRef *MemRef, bool &HasNegIVCoeff) const;

  // Insert a MemRefGroup & MRG into MRGVec
  void insert(MemRefGroup &MRG) { MRGVec.emplace_back(MRG); }

  void doTransform(HLLoop *Lp);

  // Do ScalarRepl transformation on potentially multiple suitable groups
  //
  // E.g. if we have suitable groups according to the following table, actions
  // will be different on 32b or 64b platforms.
  //
  // Default GPRLimit: 3 for 32b, and 6 for 64b
  //
  // ---------------------------------------------------------------------
  // |Suitable Group |MaxDepDist|Act(32b) GPRsUsed  | Act(64b) GPRsUsed  |
  // ---------------------------------------------------------------------
  // |A[]            |2         |  YES     2        | YES       2        |
  // ---------------------------------------------------------------------
  // |B[]            |3         |  NO      2        | YES       5        |
  // ---------------------------------------------------------------------
  // |C[]            |2         |  YES     4        | NO        5        |
  // ---------------------------------------------------------------------
  //
  // Prepare for ScalarRepl transformation:
  // - Handle (create and assign) Temps
  // - Mark MaxLoad index
  // - Mark MinStore index
  // - Identify Gaps (if any)
  //
  // Transform the loop on a given group:
  // -doInLoopProc:
  //  . generate out-standing load/store (if MaxLoad or MinStore) is available
  //   . replace any load/store in Lp with its matching tmp
  // -doPreLoopProc: generate loads (if needed)
  // -doPostLoopProc: generate stores (if needed)
  void doTransform(HLLoop *Lp, MemRefGroup &MRG);

  // Pre-loop processing:
  // Generate Loads (load from A[i] into its matching Tmp) when needed
  // [GEN]
  // i. generate a load for any unique MemRef[i+r](R) in MRG
  //  (where r in [0..MaxDD))
  // ii.generate a load for any unique MemRef[i+r](R) missing from MRG
  //  (where r in [0..MaxDD))
  //
  // - simplify the load since IV is replaced by LBCE;
  // - mark the Temp as Loop's LiveIn;
  //
  void doPreLoopProc(HLLoop *Lp, MemRefGroup &MRG,
                     SmallVectorImpl<bool> &RWGap);

  // Post process the loop: generate store(s) when needed
  // - generate a store-from-temp for any out-standing (non-min-dd) store;
  // - simplify the store since IV is replaced by UBCE;
  // - mark the Temp as Loop's LiveOut;
  //
  void doPostLoopProc(HLLoop *Lp, MemRefGroup &MRG);

  // In-loop process: handle each relevant MemRef:
  //  . generate a load HLInst if MaxIdxLoadRT is available;
  //  . generate store HLInst if MinIdxStoreRT is available;
  //  . do replacement of each relevant MemRef with its matching temp;
  //- generate temp rotation code;
  //
  void doInLoopProc(HLLoop *Lp, MemRefGroup &MRG);

  // Utility Functions
  bool handleCmdlineArgs(Function &F);
  void releaseMemory(void) override;
  void clearWorkingSetMemory(void);
  void getAnalysisUsage(AnalysisUsage &AU) const;

  // check quota and implicitly update quota if available
  bool checkAndUpdateQuota(MemRefGroup &MRG, unsigned &NumGPRsUsed) const;

  // Replace a given MemRef with a TmpDDRef
  //(e.g. A[i] becomes t0, A[i+2] becomes t2, etc.)
  void replaceMemRefWithTmp(RegDDRef *MemRef, RegDDRef *TmpRef);

#ifndef NDEBUG
  void print(void);
  void printRefGroupTy(RefGroupTy &Group, bool PrintNewLine = true);
#endif
};
}
}
}

#endif
