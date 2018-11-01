//===--- CGIntelStmtOpenMP.h - Emit Intel Code from OpenMP Directives   ---===//
//
// Copyright (C) 2015-2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file contains code to emit OpenMP nodes as LLVM code.
//
//===----------------------------------------------------------------------===//

#include "../CGCXXABI.h"
#include "../CodeGenFunction.h"
#include "../CodeGenModule.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtOpenMP.h"
#include "llvm/ADT/DenseSet.h"
using namespace clang;
using namespace CodeGen;

class OMPLateOutlineLexicalScope : public CodeGenFunction::LexicalScope {
  CodeGenFunction::OMPPrivateScope Remaps;
public:
  OMPLateOutlineLexicalScope(
      CodeGenFunction &CGF, const OMPExecutableDirective &S,
      OpenMPDirectiveKind CapturedRegion = OMPD_unknown)
      : CodeGenFunction::LexicalScope(CGF, S.getSourceRange()), Remaps(CGF) {

    for (const auto *C : S.clauses()) {
      if (const auto *CPI = OMPClauseWithPreInit::get(C)) {
        if (const auto *PreInit =
                cast_or_null<DeclStmt>(CPI->getPreInitStmt())) {
          for (const auto *I : PreInit->decls()) {
            if (!I->hasAttr<OMPCaptureNoInitAttr>()) {
              CGF.EmitVarDecl(cast<VarDecl>(*I));
            } else {
              CodeGenFunction::AutoVarEmission Emission =
                  CGF.EmitAutoVarAlloca(cast<VarDecl>(*I));
              CGF.EmitAutoVarCleanups(Emission);
            }
          }
        }
      }
    }

    CGF.RemapForLateOutlining(S, Remaps);
    (void)Remaps.Privatize();
  }

  static bool isCapturedVar(CodeGenFunction &CGF, const VarDecl *VD) {
    return CGF.LambdaCaptureFields.lookup(VD) ||
           (CGF.CapturedStmtInfo && CGF.CapturedStmtInfo->lookup(VD)) ||
           (CGF.CurCodeDecl && isa<BlockDecl>(CGF.CurCodeDecl));
  }

};

namespace CGIntelOpenMP {

enum OMPAtomicClause {
  OMP_read,
  OMP_write,
  OMP_update,
  OMP_capture,
  OMP_read_seq_cst,
  OMP_write_seq_cst,
  OMP_update_seq_cst,
  OMP_capture_seq_cst,
};
/// Class used for emission of Intel-specific intrinsics for OpenMP code.
class OpenMPCodeOutliner {
  struct ArraySectionDataTy final {
    llvm::Value *LowerBound = nullptr;
    llvm::Value *Length = nullptr;
    llvm::Value *Stride = nullptr;
    llvm::Value *VLASize = nullptr;
  };
  typedef llvm::SmallVector<ArraySectionDataTy, 4> ArraySectionTy;

  // Used temporarily to build a bundle.
  OpenMPClauseKind CurrentClauseKind;
  StringRef BundleString;
  SmallVector<llvm::Value*, 8> BundleValues;
  void clearBundleTemps() { BundleString = ""; BundleValues.clear(); }

  struct DirectiveIntrinsicSet final {
    OpenMPDirectiveKind DKind;
    SmallVector<llvm::OperandBundleDef, 8> OpBundles;
    SmallVector<llvm::Intrinsic::ID, 8> Intrins;
    StringRef End;
    llvm::CallInst *CallEntry;
    DirectiveIntrinsicSet(StringRef E, OpenMPDirectiveKind K)
          : DKind(K), End(E), CallEntry(nullptr) {}
    void clear() { OpBundles.clear(); Intrins.clear(); }
  };
  SmallVector<DirectiveIntrinsicSet, 4> Directives;
  CodeGenFunction &CGF;
  llvm::LLVMContext &C;

  // Save and restore the TerminateLandingPad.
  CodeGenFunction::OMPTerminateLandingPadHandler TLPH;

  // For region entry/exit implementation
  llvm::Function *RegionEntryDirective = nullptr;
  llvm::Function *RegionExitDirective = nullptr;

  // Used to insert instructions outside the region.
  llvm::Instruction *MarkerInstruction = nullptr;

  class ClauseEmissionHelper final {
    llvm::IRBuilderBase::InsertPoint SavedIP;
    OpenMPCodeOutliner &O;
    bool EmitClause;

  public:
    ClauseEmissionHelper(OpenMPCodeOutliner &O, bool EmitClause = true)
        : O(O), EmitClause(EmitClause) {
      SavedIP = O.CGF.Builder.saveIP();
      O.setInsertPoint();
    }
    ~ClauseEmissionHelper() {
      O.CGF.Builder.restoreIP(SavedIP);
      if (EmitClause)
        O.emitClause();
    }
  };
  const OMPExecutableDirective &Directive;

  const Expr *getArraySectionBase(const Expr *E, ArraySectionTy *AS);
  ArraySectionDataTy emitArraySectionData(const Expr *E);
  Address emitOMPArraySectionExpr(const Expr *E, ArraySectionTy *AS);

  void addArg(llvm::Value *Val);
  void addArg(StringRef Str);
  void addArg(const Expr *E);

  void addFenceCalls(bool IsBegin);
  void getApplicableDirectives(SmallVector<DirectiveIntrinsicSet *, 4> &Dirs);
  void startDirectiveIntrinsicSet(StringRef B, StringRef E,
                                  OpenMPDirectiveKind K = OMPD_unknown);
  void emitDirective(DirectiveIntrinsicSet &D, StringRef Name);
  void emitClause();
  void emitOMPSharedClause(const OMPSharedClause *Cl);
  void emitOMPPrivateClause(const OMPPrivateClause *Cl);
  void emitOMPLastprivateClause(const OMPLastprivateClause *Cl);
  void emitOMPLinearClause(const OMPLinearClause *Cl);
  template <typename RedClause>
  void emitOMPReductionClauseCommon(const RedClause *Cl, StringRef QualName);
  void emitOMPReductionClause(const OMPReductionClause *Cl);
  void emitOMPOrderedClause(const OMPOrderedClause *C);
  void emitOMPMapClause(const OMPMapClause *Cl);
  void emitOMPScheduleClause(const OMPScheduleClause *C);
  void emitOMPFirstprivateClause(const OMPFirstprivateClause *Cl);
  void emitOMPCopyinClause(const OMPCopyinClause *Cl);
  void emitOMPIfClause(const OMPIfClause *Cl);
  void emitOMPNumThreadsClause(const OMPNumThreadsClause *Cl);
  void emitOMPDefaultClause(const OMPDefaultClause *Cl);
  void emitOMPProcBindClause(const OMPProcBindClause *Cl);
  void emitOMPSafelenClause(const OMPSafelenClause *Cl);
  void emitOMPSimdlenClause(const OMPSimdlenClause *Cl);
  void emitOMPCollapseClause(const OMPCollapseClause *Cl);
  void emitOMPAlignedClause(const OMPAlignedClause *Cl);
  void emitOMPFinalClause(const OMPFinalClause *);
  void emitOMPCopyprivateClause(const OMPCopyprivateClause *);
  void emitOMPNowaitClause(const OMPNowaitClause *);
  void emitOMPUntiedClause(const OMPUntiedClause *);
  void emitOMPMergeableClause(const OMPMergeableClause *);
  void emitOMPFlushClause(const OMPFlushClause *);
  void emitOMPReadClause(const OMPReadClause *);
  void emitOMPWriteClause(const OMPWriteClause *);
  void emitOMPUpdateClause(const OMPUpdateClause *);
  void emitOMPCaptureClause(const OMPCaptureClause *);
  void emitOMPSeqCstClause(const OMPSeqCstClause *);
  void emitOMPDependClause(const OMPDependClause *);
  void emitOMPDeviceClause(const OMPDeviceClause *);
  void emitOMPThreadsClause(const OMPThreadsClause *);
  void emitOMPSIMDClause(const OMPSIMDClause *);
  void emitOMPNumTeamsClause(const OMPNumTeamsClause *);
  void emitOMPThreadLimitClause(const OMPThreadLimitClause *);
  void emitOMPPriorityClause(const OMPPriorityClause *);
  void emitOMPGrainsizeClause(const OMPGrainsizeClause *);
  void emitOMPNogroupClause(const OMPNogroupClause *);
  void emitOMPNumTasksClause(const OMPNumTasksClause *);
  void emitOMPHintClause(const OMPHintClause *);
  void emitOMPDistScheduleClause(const OMPDistScheduleClause *);
  void emitOMPDefaultmapClause(const OMPDefaultmapClause *);
  void emitOMPToClause(const OMPToClause *);
  void emitOMPFromClause(const OMPFromClause *);
  void emitOMPUseDevicePtrClause(const OMPUseDevicePtrClause *);
  void emitOMPIsDevicePtrClause(const OMPIsDevicePtrClause *);
  void emitOMPTaskReductionClause(const OMPTaskReductionClause *);
  void emitOMPInReductionClause(const OMPInReductionClause *);
  void emitOMPUnifiedAddressClause(const OMPUnifiedAddressClause *);
  void emitOMPUnifiedSharedMemoryClause(const OMPUnifiedSharedMemoryClause *);
  void emitOMPReverseOffloadClause(const OMPReverseOffloadClause *);
  void emitOMPDynamicAllocatorsClause(const OMPDynamicAllocatorsClause *);

  llvm::Value *emitIntelOpenMPDefaultConstructor(const Expr *IPriv);
  llvm::Value *emitIntelOpenMPDestructor(QualType Ty);
  llvm::Value *emitIntelOpenMPCopyConstructor(const Expr *IPriv);
  llvm::Value *emitIntelOpenMPCopyAssign(QualType Ty,
                                         const Expr *SrcExpr,
                                         const Expr *DstExpr,
                                         const Expr *AssignOp);

  bool isUnspecifiedImplicit(const VarDecl *);
  bool isImplicit(const VarDecl *);
  bool isExplicit(const VarDecl *);
  void addImplicitClauses();
  void addRefsToOuter();

  enum ImplicitClauseKind {
    ICK_private,
    ICK_firstprivate,
    ICK_shared,
    ICK_map_tofrom,
    ICK_normalized_iv,
    ICK_normalized_ub,
    // A firstprivate specified with an implicit OMPFirstprivateClause.
    ICK_specified_firstprivate,
    ICK_unknown
  };

  llvm::MapVector<const VarDecl *, ImplicitClauseKind> ImplicitMap;
  llvm::DenseSet<const VarDecl *> ExplicitRefs;
  llvm::DenseSet<const VarDecl *> VarDefs;
  llvm::SmallSetVector<const VarDecl *, 32> VarRefs;
  llvm::Value *ThisPointerValue = nullptr;
  llvm::StringMap<llvm::MDNode *> MDNodes;

public:
  OpenMPCodeOutliner(CodeGenFunction &CGF, const OMPExecutableDirective &D);
  ~OpenMPCodeOutliner();
  void emitOMPParallelDirective();
  void emitOMPParallelForDirective();
  void emitOMPSIMDDirective();
  void emitOMPForDirective();
  void emitOMPParallelForSimdDirective();
  void emitOMPAtomicDirective(OMPAtomicClause ClauseKind);
  void emitOMPSingleDirective();
  void emitOMPMasterDirective();
  void emitOMPCriticalDirective(const StringRef Name);
  void emitOMPOrderedDirective();
  void emitOMPTargetDirective();
  void emitOMPTargetDataDirective();
  void emitOMPTargetUpdateDirective();
  void emitOMPTargetEnterDataDirective();
  void emitOMPTargetExitDataDirective();
  void emitOMPTaskLoopDirective();
  void emitOMPTaskLoopSimdDirective();
  void emitOMPTaskDirective();
  void emitOMPTaskGroupDirective();
  void emitOMPTaskWaitDirective();
  void emitOMPTaskYieldDirective();
  void emitOMPBarrierDirective();
  void emitOMPFlushDirective();
  void emitOMPTeamsDirective();
  void emitOMPDistributeDirective();
  void emitOMPDistributeParallelForDirective();
  void emitOMPDistributeParallelForSimdDirective();
  void emitOMPSectionsDirective();
  void emitOMPSectionDirective();
  void emitOMPParallelSectionsDirective();
  void emitOMPCancelDirective(OpenMPDirectiveKind Kind);
  void emitOMPCancellationPointDirective(OpenMPDirectiveKind Kind);
  OpenMPCodeOutliner &operator<<(ArrayRef<OMPClause *> Clauses);
  void emitImplicit(Expr *E, ImplicitClauseKind K);
  void emitImplicit(const VarDecl *VD, ImplicitClauseKind K);
  void addVariableDef(const VarDecl *VD) { VarDefs.insert(VD); }
  void addVariableRef(const VarDecl *VD) { VarRefs.insert(VD); }
  void setThisPointerValue(llvm::Value *ThisValue) {
    ThisPointerValue = ThisValue;
  }
  llvm::Value *getThisPointerValue() { return ThisPointerValue; }
  void addExplicit(const Expr *E);
  void addExplicit(const VarDecl *VD) { ExplicitRefs.insert(VD); }
  void setInsertPoint() { CGF.Builder.SetInsertPoint(MarkerInstruction); }
  void addMetadata(StringRef Kind, llvm::MDNode *N) { MDNodes[Kind] = N; }
};

/// Base class for handling code generation inside OpenMP regions.
class CGOpenMPRegionInfo : public CodeGenFunction::CGCapturedStmtInfo {
public:
  CGOpenMPRegionInfo(CodeGenFunction::CGCapturedStmtInfo *OldCSI,
                     OpenMPCodeOutliner &O,
                     const OMPExecutableDirective &D)
      : CGCapturedStmtInfo(*cast<CapturedStmt>(D.getAssociatedStmt()),
                           CR_OpenMP),
        OldCSI(OldCSI), Outliner(O), D(D) {}

  /// \brief Emit the captured statement body.
  void EmitBody(CodeGenFunction &CGF, const Stmt *S) override;

  // \brief Retrieve the value of the context parameter.
  llvm::Value *getContextValue() const override;
  void setContextValue(llvm::Value *V) override;

  /// \brief Lookup the captured field decl for a variable.
  const FieldDecl *lookup(const VarDecl *VD) const override;

  FieldDecl *getThisFieldDecl() const override;

  CodeGenFunction::CGCapturedStmtInfo *getOldCSI() const { return OldCSI; }

  void recordVariableDefinition(const VarDecl *VD) {
    Outliner.addVariableDef(VD);
  }
  void recordVariableReference(const VarDecl *VD) {
    Outliner.addVariableRef(VD);
  }
  void recordThisPointerReference(llvm::Value *ThisValue) {
    Outliner.setThisPointerValue(ThisValue);
  }
  bool isLateOutlinedRegion() { return true; }

private:
  /// \brief CodeGen info about outer OpenMP region.
  CodeGenFunction::CGCapturedStmtInfo *OldCSI;
  OpenMPCodeOutliner &Outliner;
  const OMPExecutableDirective &D;
};

/// \brief RAII for emitting code of OpenMP constructs.
class InlinedOpenMPRegionRAII {
  CodeGenFunction &CGF;
  OpenMPCodeOutliner &Outliner;
  const OMPExecutableDirective &Dir;
public:
  /// \brief Constructs region for combined constructs.
  /// \param CodeGen Code generation sequence for combined directives. Includes
  /// a list of functions used for code generation of implicitly inlined
  /// regions.
  InlinedOpenMPRegionRAII(CodeGenFunction &CGF, OpenMPCodeOutliner &O,
                          const OMPExecutableDirective &D)
      : CGF(CGF), Outliner(O), Dir(D) {
    // Start emission for the construct.
    CGF.CapturedStmtInfo = new CGOpenMPRegionInfo(CGF.CapturedStmtInfo,
                                                  Outliner, D);
  }
  ~InlinedOpenMPRegionRAII() {
    // Restore original CapturedStmtInfo only if we're done with code emission.
    auto *OldCSI =
        static_cast<CGOpenMPRegionInfo *>(CGF.CapturedStmtInfo)->getOldCSI();
    delete CGF.CapturedStmtInfo;
    CGF.CapturedStmtInfo = OldCSI;
  }
};

} // namespace
