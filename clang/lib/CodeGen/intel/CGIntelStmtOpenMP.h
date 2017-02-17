//===--- CGIntelStmtOpenMP.h - Emit Intel Code from OpenMP Directives   ---===//
//
// Copyright (C) 2015-2017 Intel Corporation. All rights reserved.
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

#if INTEL_SPECIFIC_OPENMP
#include "../CGCXXABI.h"
#include "../CodeGenFunction.h"
#include "../CodeGenModule.h"
#include "clang/AST/Stmt.h"
#include "clang/AST/StmtOpenMP.h"
using namespace clang;
using namespace CodeGen;

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
  CodeGenFunction &CGF;
  StringRef End;
  llvm::Function *IntelDirective = nullptr;
  llvm::Function *IntelSimpleClause = nullptr;
  llvm::Function *IntelListClause = nullptr;
  llvm::LLVMContext &C;
  llvm::SmallVector<llvm::Value *, 16> Args;

  ArraySectionDataTy emitArraySectionData(const OMPArraySectionExpr *E);
  Address emitOMPArraySectionExpr(const OMPArraySectionExpr *E,
                                  ArraySectionTy &AS);

  void addArg(llvm::Value *Val);
  void addArg(StringRef Str);
  void addArg(const Expr *E);

  void emitDirective();
  void emitSimpleClause();
  void emitOpndClause();
  void emitListClause();
  void emitOMPSharedClause(const OMPSharedClause *Cl);
  void emitOMPPrivateClause(const OMPPrivateClause *Cl);
  void emitOMPLastprivateClause(const OMPLastprivateClause *Cl);
  void emitOMPLinearClause(const OMPLinearClause *Cl);
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

  static llvm::Value *emitIntelOpenMPDefaultConstructor(CodeGenModule &CGM,
                                                        const VarDecl *Private);
  static llvm::Value *emitIntelOpenMPDestructor(CodeGenModule &CGM,
                                                const VarDecl *Private);
  static llvm::Value *emitIntelOpenMPCopyConstructor(CodeGenModule &CGM,
                                                     const VarDecl *Private);
  static llvm::Value *emitIntelOpenMPCopyAssign(CodeGenModule &CGM,
                                                const VarDecl *Private,
                                                const Expr *SrcExpr,
                                                const Expr *DstExpr,
                                                const Expr *AssignOp);
public:
  OpenMPCodeOutliner(CodeGenFunction &CGF);
  ~OpenMPCodeOutliner();
  void emitOMPParallelDirective();
  void emitOMPParallelForDirective();
  void emitOMPSIMDDirective();
  void emitOMPAtomicDirective(OMPAtomicClause ClauseKind);
  void emitOMPSingleDirective();
  void emitOMPMasterDirective();
  void emitOMPCriticalDirective();
  void emitOMPOrderedDirective();
  void emitOMPTargetDirective();
  OpenMPCodeOutliner &operator<<(ArrayRef<OMPClause *> Clauses);
};

/// Base class for handling code generation inside OpenMP regions.
class CGOpenMPRegionInfo : public CodeGenFunction::CGCapturedStmtInfo {
public:
  CGOpenMPRegionInfo(CodeGenFunction::CGCapturedStmtInfo *OldCSI,
                     const OMPExecutableDirective &D)
      : CGCapturedStmtInfo(*cast<CapturedStmt>(D.getAssociatedStmt()),
                           CR_OpenMP),
        OldCSI(OldCSI), D(D) {}

  /// \brief Emit the captured statement body.
  void EmitBody(CodeGenFunction &CGF, const Stmt *S) override;

  // \brief Retrieve the value of the context parameter.
  llvm::Value *getContextValue() const override;
  void setContextValue(llvm::Value *V) override;

  /// \brief Lookup the captured field decl for a variable.
  const FieldDecl *lookup(const VarDecl *VD) const override;

  FieldDecl *getThisFieldDecl() const override;

  CodeGenFunction::CGCapturedStmtInfo *getOldCSI() const { return OldCSI; }

private:
  /// \brief CodeGen info about outer OpenMP region.
  CodeGenFunction::CGCapturedStmtInfo *OldCSI;
  const OMPExecutableDirective &D;
};

/// \brief RAII for emitting code of OpenMP constructs.
class InlinedOpenMPRegionRAII {
  CodeGenFunction &CGF;

public:
  /// \brief Constructs region for combined constructs.
  /// \param CodeGen Code generation sequence for combined directives. Includes
  /// a list of functions used for code generation of implicitly inlined
  /// regions.
  InlinedOpenMPRegionRAII(CodeGenFunction &CGF, const OMPExecutableDirective &D)
      : CGF(CGF) {
    // Start emission for the construct.
    CGF.CapturedStmtInfo = new CGOpenMPRegionInfo(CGF.CapturedStmtInfo, D);
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

#endif // INTEL_SPECIFIC_OPENMP
