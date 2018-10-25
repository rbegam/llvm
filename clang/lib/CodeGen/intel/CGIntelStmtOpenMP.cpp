//===--- CGIntelStmtOpenMP.cpp - Emit Intel Code from OpenMP Directives ---===//
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

#include "CGIntelStmtOpenMP.h"
#include "CGOpenMPRuntime.h"
#include "CGCleanup.h"
using namespace clang;
using namespace CodeGen;
using namespace CGIntelOpenMP;

llvm::Value *
OpenMPCodeOutliner::emitIntelOpenMPDefaultConstructor(const Expr *IPriv) {

  if (!IPriv)
    return llvm::ConstantPointerNull::get(CGF.VoidPtrTy);

  auto *Private = cast<VarDecl>(cast<DeclRefExpr>(IPriv)->getDecl());
  QualType Ty = Private->getType();

  CodeGenModule &CGM = CGF.CGM;
  SmallString<256> OutName;
  llvm::raw_svector_ostream Out(OutName);
  CGM.getCXXABI().getMangleContext().mangleTypeName(Ty, Out);
  Out << ".omp.def_constr";

  if (llvm::Value *F = CGM.GetGlobalValue(OutName))
    return F;

  // Generate function that re-emits the declaration's initializer into the
  // threadprivate copy of the variable VD
  auto &Ctx = CGM.getContext();
  QualType PtrTy = Ctx.getPointerType(Ty);
  CodeGenFunction NewCGF(CGM);
  FunctionArgList Args;
  ImplicitParamDecl Dst(CGM.getContext(), /*DC=*/nullptr, SourceLocation(),
                        /*Id=*/nullptr, PtrTy, ImplicitParamDecl::Other);
  Args.push_back(&Dst);

  auto &FI = CGM.getTypes().arrangeBuiltinFunctionDeclaration(PtrTy, Args);
  auto FTy = CGM.getTypes().GetFunctionType(FI);
  auto *Fn = CGM.CreateGlobalInitOrDestructFunction(FTy, OutName, FI);
  NewCGF.StartFunction(GlobalDecl(), PtrTy, Fn, FI, Args, SourceLocation());
  auto *Init = Private->getInit();
  if (Init && !NewCGF.isTrivialInitializer(Init)) {
    CodeGenFunction::RunCleanupsScope Scope(NewCGF);
    LValue ArgLVal = NewCGF.EmitLoadOfPointerLValue(
        NewCGF.GetAddrOfLocalVar(&Dst), PtrTy->getAs<PointerType>());
    NewCGF.EmitAnyExprToMem(Init, ArgLVal.getAddress(), Ty.getQualifiers(),
                         /*IsInitializer=*/true);
    NewCGF.Builder.CreateStore(ArgLVal.getPointer(), NewCGF.ReturnValue);
  }
  NewCGF.FinishFunction();
  return Fn;
}

llvm::Value *
OpenMPCodeOutliner::emitIntelOpenMPDestructor(QualType Ty) {
  CodeGenModule &CGM = CGF.CGM;
  SmallString<256> OutName;
  llvm::raw_svector_ostream Out(OutName);
  CGM.getCXXABI().getMangleContext().mangleTypeName(Ty, Out);
  Out << ".omp.destr";

  if (llvm::Value *F = CGM.GetGlobalValue(OutName))
    return F;

  // Generate function that emits destructor call for the threadprivate copy
  // of the variable VD
  auto &Ctx = CGM.getContext();
  QualType PtrTy = Ctx.getPointerType(Ty);
  CodeGenFunction NewCGF(CGM);
  FunctionArgList Args;
  ImplicitParamDecl Dst(CGM.getContext(), /*DC=*/nullptr, SourceLocation(),
                        /*Id=*/nullptr, PtrTy, ImplicitParamDecl::Other);
  Args.push_back(&Dst);

  auto &FI = CGM.getTypes().arrangeBuiltinFunctionDeclaration(
      CGM.getContext().VoidTy, Args);
  auto FTy = CGM.getTypes().GetFunctionType(FI);
  auto *Fn = CGM.CreateGlobalInitOrDestructFunction(FTy, OutName, FI);
  NewCGF.StartFunction(GlobalDecl(), CGM.getContext().VoidTy, Fn, FI, Args,
                    SourceLocation());
  if (Ty.isDestructedType() != QualType::DK_none) {
    CodeGenFunction::RunCleanupsScope Scope(NewCGF);
    LValue ArgLVal = NewCGF.EmitLoadOfPointerLValue(
                                                 NewCGF.GetAddrOfLocalVar(&Dst),
                                                 PtrTy->getAs<PointerType>());
    NewCGF.emitDestroy(ArgLVal.getAddress(), Ty,
                    NewCGF.getDestroyer(Ty.isDestructedType()),
                    NewCGF.needsEHCleanup(Ty.isDestructedType()));
  }
  NewCGF.FinishFunction();
  return Fn;
}

llvm::Value *
OpenMPCodeOutliner::emitIntelOpenMPCopyConstructor(const Expr *IPriv) {
  if (!IPriv)
    return llvm::ConstantPointerNull::get(CGF.VoidPtrTy);

  auto *Private = cast<VarDecl>(cast<DeclRefExpr>(IPriv)->getDecl());

  CodeGenModule &CGM = CGF.CGM;
  auto &C = CGM.getContext();
  QualType Ty = Private->getType();
  QualType ElemType = Ty;
  if (Ty->isArrayType())
    ElemType = C.getBaseElementType(Ty).getNonReferenceType();

  SmallString<256> OutName;
  llvm::raw_svector_ostream Out(OutName);
  CGM.getCXXABI().getMangleContext().mangleTypeName(Ty, Out);
  Out << ".omp.copy_constr";

  if (llvm::Value *F = CGM.GetGlobalValue(OutName))
    return F;

  // Note that we should be able to optimize this to return the cctor directly
  // in cases where this is only a simple call.

  // Generate a copy constructor wrapper for the type of the firstprivate
  // variable.  Note this must also handle array types. This is similar to
  // the code in emitIntelOpenMPDefaultConstructor but the Init passed here
  // (which contains references to early outlining variables that do not exist)
  // must be processed to use the source object address passed in as a
  // parameter. Since we need to create new AST objects in the generated
  // routine we need to create a FunctionDecl to act as the DeclContext.

  IdentifierInfo *II = &CGM.getContext().Idents.get(OutName);
  FunctionDecl *FD = FunctionDecl::Create(
      C, C.getTranslationUnitDecl(), SourceLocation(), SourceLocation(), II,
      C.VoidTy, /*TInfo=*/nullptr, SC_Static);

  QualType ObjPtrTy = C.getPointerType(Ty);

  CodeGenFunction NewCGF(CGM);
  FunctionArgList Args;
  ImplicitParamDecl DstDecl(C, FD, SourceLocation(), nullptr, ObjPtrTy,
                            ImplicitParamDecl::Other);
  Args.push_back(&DstDecl);
  ImplicitParamDecl SrcDecl(C, FD, SourceLocation(), nullptr, ObjPtrTy,
                            ImplicitParamDecl::Other);
  Args.push_back(&SrcDecl);

  const CGFunctionInfo &FI =
      CGM.getTypes().arrangeBuiltinFunctionDeclaration(C.VoidTy, Args);

  llvm::FunctionType *LTy = CGM.getTypes().GetFunctionType(FI);

  llvm::Function *Fn = llvm::Function::Create(
      LTy, llvm::GlobalValue::InternalLinkage, OutName, &CGM.getModule());

  CGM.SetInternalFunctionAttributes(GlobalDecl(), Fn, FI);

  NewCGF.StartFunction(FD, C.VoidTy, Fn, FI, Args);
  auto *Init = Private->getInit();
  if (Init && !NewCGF.isTrivialInitializer(Init)) {
    CodeGenFunction::RunCleanupsScope Scope(NewCGF);
    auto *CCE = cast<CXXConstructExpr>(Init);
    DeclRefExpr SrcExpr(&SrcDecl, /*RefersToEnclosingVariableOrCapture=*/false,
                        ObjPtrTy, VK_LValue, SourceLocation());
    ImplicitCastExpr CastExpr(ImplicitCastExpr::OnStack,
                              C.getPointerType(ElemType), CK_BitCast, &SrcExpr,
                              VK_RValue);
    UnaryOperator SRC(&CastExpr, UO_Deref, ElemType, VK_LValue, OK_Ordinary,
                      SourceLocation(), /*CanOverflow=*/false);

    QualType CTy = ElemType;
    CTy.addConst();
    ImplicitCastExpr NoOpCast(ImplicitCastExpr::OnStack, CTy, CK_NoOp, &SRC,
                              VK_LValue);

    SmallVector<Expr *, 8> ConstructorArgs;
    ConstructorArgs.push_back(&NoOpCast);
    // Add possible default arguments, which start with the second arg.
    for (auto I = CCE->arg_begin() + 1, End = CCE->arg_end(); I != End; ++I)
      ConstructorArgs.push_back(const_cast<Expr *>(*I));

    CXXConstructExpr *RebuiltCCE = CXXConstructExpr::Create(
        C, Ty, CCE->getLocation(), CCE->getConstructor(), CCE->isElidable(),
        ConstructorArgs, CCE->hadMultipleCandidates(),
        CCE->isListInitialization(), CCE->isStdInitListInitialization(),
        CCE->requiresZeroInitialization(), CCE->getConstructionKind(),
        CCE->getParenOrBraceRange());

    LValue ArgLVal = NewCGF.EmitLoadOfPointerLValue(
        NewCGF.GetAddrOfLocalVar(&DstDecl), ObjPtrTy->getAs<PointerType>());
    NewCGF.EmitAnyExprToMem(RebuiltCCE, ArgLVal.getAddress(),
                         Ty.getQualifiers(), /*IsInitializer=*/true);
  }
  NewCGF.FinishFunction();

  return Fn;
}

llvm::Value *OpenMPCodeOutliner::emitIntelOpenMPCopyAssign(QualType Ty,
    const Expr *SrcExpr, const Expr *DstExpr, const Expr *AssignOp) {
  CodeGenModule &CGM = CGF.CGM;
  auto &C = CGM.getContext();
  QualType ElemType = Ty;
  if (Ty->isArrayType())
    ElemType = C.getBaseElementType(Ty).getNonReferenceType();

  SmallString<256> OutName;
  llvm::raw_svector_ostream Out(OutName);
  CGM.getCXXABI().getMangleContext().mangleTypeName(Ty, Out);
  Out << ".omp.copy_assign";

  if (llvm::Value *F = CGM.GetGlobalValue(OutName))
    return F;

  IdentifierInfo *II = &CGM.getContext().Idents.get(OutName);
  FunctionDecl *FD = FunctionDecl::Create(
      C, C.getTranslationUnitDecl(), SourceLocation(), SourceLocation(), II,
      C.VoidTy, /*TInfo=*/nullptr, SC_Static);

  QualType ObjPtrTy = C.getPointerType(Ty);

  CodeGenFunction NewCGF(CGM);
  FunctionArgList Args;
  ImplicitParamDecl DstDecl(C, FD, SourceLocation(), nullptr, ObjPtrTy,
                            ImplicitParamDecl::Other);
  Args.push_back(&DstDecl);
  ImplicitParamDecl SrcDecl(C, FD, SourceLocation(), nullptr, ObjPtrTy,
                            ImplicitParamDecl::Other);
  Args.push_back(&SrcDecl);

  const CGFunctionInfo &FI =
      CGM.getTypes().arrangeBuiltinFunctionDeclaration(C.VoidTy, Args);

  llvm::FunctionType *LTy = CGM.getTypes().GetFunctionType(FI);

  llvm::Function *Fn = llvm::Function::Create(
      LTy, llvm::GlobalValue::InternalLinkage, OutName, &CGM.getModule());

  CGM.SetInternalFunctionAttributes(GlobalDecl(), Fn, FI);

  NewCGF.StartFunction(FD, C.VoidTy, Fn, FI, Args);

  auto DestAddr = NewCGF.EmitLoadOfPointerLValue(
                                            NewCGF.GetAddrOfLocalVar(&DstDecl),
                                            ObjPtrTy->getAs<PointerType>())
                      .getAddress();

  auto SrcAddr = NewCGF.EmitLoadOfPointerLValue(
                                            NewCGF.GetAddrOfLocalVar(&SrcDecl),
                                            ObjPtrTy->getAs<PointerType>())
                     .getAddress();

  auto *SrcVD = cast<VarDecl>(cast<DeclRefExpr>(SrcExpr)->getDecl());
  auto *DestVD = cast<VarDecl>(cast<DeclRefExpr>(DstExpr)->getDecl());

  NewCGF.EmitOMPCopy(Ty, DestAddr, SrcAddr, DestVD, SrcVD, AssignOp);

  NewCGF.FinishFunction();
  return Fn;
}

namespace CGIntelOpenMP {
  /// Returns the base expression for an array section or an array subscript
  /// used where an array section is expected.  Optionally (if AS is not
  /// a nullptr) fills in the array section data for each dimension.
  const Expr *OpenMPCodeOutliner::getArraySectionBase(const Expr *E,
                                                      ArraySectionTy *AS) {
    const Expr *Base = E->IgnoreParenImpCasts();
    while (const auto *TempOASE = dyn_cast<OMPArraySectionExpr>(Base)) {
      if (AS)
        AS->insert(AS->begin(), emitArraySectionData(Base));
      Base = TempOASE->getBase()->IgnoreParenImpCasts();
    }
    while (const auto *TempASE = dyn_cast<ArraySubscriptExpr>(Base)) {
      if (AS)
        AS->insert(AS->begin(), emitArraySectionData(Base));
      Base = TempASE->getBase()->IgnoreParenImpCasts();
    }
    return Base;
  }

  OpenMPCodeOutliner::ArraySectionDataTy
  OpenMPCodeOutliner::emitArraySectionData(const Expr *E) {
    ArraySectionDataTy Data;
    auto &C = CGF.getContext();

    if (auto *ASE = dyn_cast<ArraySubscriptExpr>(E)) {
      auto *Index = ASE->getIdx();
      Data.LowerBound = CGF.EmitScalarConversion(
          CGF.EmitScalarExpr(Index), Index->getType(),
          C.getSizeType(), Index->getExprLoc());
      Data.Length = llvm::ConstantInt::get(CGF.SizeTy, /*V=*/1);
      Data.Stride = llvm::ConstantInt::get(CGF.SizeTy, /*V=*/1);
      return Data;
    }

    auto *OASE = cast<OMPArraySectionExpr>(E);
    if (auto *LowerBound = OASE->getLowerBound()) {
      Data.LowerBound = CGF.EmitScalarConversion(
          CGF.EmitScalarExpr(LowerBound), LowerBound->getType(),
          C.getSizeType(), LowerBound->getExprLoc());
    } else
      Data.LowerBound = llvm::ConstantInt::getNullValue(CGF.SizeTy);
    QualType BaseTy = OMPArraySectionExpr::getBaseOriginalType(
        OASE->getBase()->IgnoreParenImpCasts());
    if (auto *Length = OASE->getLength()) {
      Data.Length = CGF.EmitScalarConversion(CGF.EmitScalarExpr(Length),
                                             Length->getType(), C.getSizeType(),
                                             Length->getExprLoc());
    } else {
      llvm::APSInt ConstLength;
      if (auto *VAT = C.getAsVariableArrayType(BaseTy)) {
        Length = VAT->getSizeExpr();
        if (Length->isIntegerConstantExpr(ConstLength, C))
          Length = nullptr;
      } else {
        auto *CAT = C.getAsConstantArrayType(BaseTy);
        ConstLength = CAT->getSize();
      }
      llvm::Value *LengthVal;
      if (Length) {
        LengthVal = CGF.EmitScalarConversion(
            CGF.EmitScalarExpr(Length), Length->getType(), C.getSizeType(),
            Length->getExprLoc());
      } else {
        LengthVal =
            llvm::ConstantInt::get(CGF.SizeTy, ConstLength.getExtValue());
      }
      Data.Length = CGF.Builder.CreateSub(LengthVal, Data.LowerBound);
    }
    Data.Stride = llvm::ConstantInt::get(CGF.SizeTy, /*V=*/1);
    return Data;
  }

  /// Emit an address of the base of OMPArraySectionExpr and fills data for
  /// array sections.
  Address
  OpenMPCodeOutliner::emitOMPArraySectionExpr(const Expr *E,
                                              ArraySectionTy *AS) {
    const Expr *Base = getArraySectionBase(E, AS);
    QualType BaseTy = Base->getType();
    Address BaseAddr = CGF.EmitLValue(Base).getAddress();
    if (BaseTy->isVariablyModifiedType()) {
      for (auto &ASD : *AS) {
        if (const ArrayType *AT = BaseTy->getAsArrayTypeUnsafe()) {
          BaseTy = AT->getElementType();
          llvm::Value *Size = nullptr;
          if (auto *VAT = dyn_cast<VariableArrayType>(AT)) {
            Size = CGF.EmitScalarConversion(
                CGF.EmitScalarExpr(VAT->getSizeExpr()),
                VAT->getSizeExpr()->getType(), CGF.getContext().getSizeType(),
                SourceLocation());
          } else if (auto *CAT = dyn_cast<ConstantArrayType>(AT))
            Size = llvm::ConstantInt::get(CGF.SizeTy, CAT->getSize());
          else
            Size = llvm::ConstantPointerNull::get(CGF.VoidPtrTy);
          ASD.VLASize = Size;
        } else {
          assert((BaseTy->isPointerType()));
          BaseTy = BaseTy->getPointeeType();
          ASD.VLASize = llvm::ConstantPointerNull::get(CGF.VoidPtrTy);
        }
      }
    }
    return BaseAddr;
  }

  void OpenMPCodeOutliner::addArg(llvm::Value *Val) {
    BundleValues.push_back(Val);
  }

  void OpenMPCodeOutliner::addArg(StringRef Str) {
    BundleString = Str;
  }

  void OpenMPCodeOutliner::addArg(const Expr *E) {
    auto SavedIP = CGF.Builder.saveIP();
    setInsertPoint();
    if (isa<ArraySubscriptExpr>(E->IgnoreParenImpCasts()) ||
        E->getType()->isSpecificPlaceholderType(BuiltinType::OMPArraySection)) {
      ArraySectionTy AS;
      Address Base = emitOMPArraySectionExpr(E, &AS);
      addArg(Base.getPointer());
      addArg(llvm::ConstantInt::get(CGF.SizeTy, AS.size()));
      for (auto &V : AS) {
        assert(V.LowerBound);
        addArg(V.LowerBound);
        assert(V.Length);
        addArg(V.Length);
        assert(V.Stride);
        addArg(V.Stride);
      }
    } else {
      assert(E->isGLValue());
      addArg(CGF.EmitLValue(E).getPointer());
    }
    CGF.Builder.restoreIP(SavedIP);
  }

  void OpenMPCodeOutliner::getApplicableDirectives(
                            SmallVector<DirectiveIntrinsicSet *, 4> &Dirs) {
    // This is likely to become complicated but for now if there are more
    // than one directive we place the clause on each if clause is allowed
    // there.
    if (Directives.size() == 1) {
      Dirs.push_back(&Directives[0]);
      return;
    }
    for (auto &D : Directives) {
      if (CurrentClauseKind == OMPC_unknown && isOpenMPLoopDirective(D.DKind)) {
        // This is the normalized iteration variable.  Just place it on the
        // first loop directive and return.
        Dirs.push_back(&D);
        return;
      }
      if (isAllowedClauseForDirective(D.DKind, CurrentClauseKind))
        Dirs.push_back(&D);
    }
  }

  void OpenMPCodeOutliner::startDirectiveIntrinsicSet(StringRef Begin,
                                                      StringRef End,
                                                      OpenMPDirectiveKind K) {
    assert(BundleValues.empty());
    DirectiveIntrinsicSet D(End, K);
    llvm::OperandBundleDef B(Begin, BundleValues);
    D.OpBundles.push_back(B);
    D.Intrins.push_back(llvm::Intrinsic::intel_directive);
    Directives.push_back(D);
  }

  void OpenMPCodeOutliner::emitDirective(DirectiveIntrinsicSet &D,
                                         StringRef Name) {
    assert(BundleValues.empty());
    llvm::OperandBundleDef B(Name, BundleValues);
    D.OpBundles.push_back(B);
    D.Intrins.push_back(llvm::Intrinsic::intel_directive);
    clearBundleTemps();
  }

  void OpenMPCodeOutliner::emitClause() {
    SmallVector<DirectiveIntrinsicSet *, 4> DRefs;
    getApplicableDirectives(DRefs);
    for (auto *D : DRefs) {
      llvm::OperandBundleDef B(BundleString, BundleValues);
      D->OpBundles.push_back(B);
    }
    clearBundleTemps();
    CurrentClauseKind = OMPC_unknown;
  }

  void OpenMPCodeOutliner::emitImplicit(Expr *E, ImplicitClauseKind K) {
    switch (K) {
    case ICK_private:
      CurrentClauseKind = OMPC_private;
      addArg("QUAL.OMP.PRIVATE");
      break;
    case ICK_specified_firstprivate:
    case ICK_firstprivate:
      CurrentClauseKind = OMPC_firstprivate;
      addArg("QUAL.OMP.FIRSTPRIVATE");
      break;
    case ICK_shared:
      CurrentClauseKind = OMPC_shared;
      addArg("QUAL.OMP.SHARED");
      break;
    case ICK_map_tofrom:
      CurrentClauseKind = OMPC_map;
      addArg("QUAL.OMP.MAP.TOFROM");
      break;
    case ICK_normalized_iv:
      CurrentClauseKind = OMPC_unknown;
      addArg("QUAL.OMP.NORMALIZED.IV");
      break;
    case ICK_normalized_ub:
      CurrentClauseKind = OMPC_unknown;
      addArg("QUAL.OMP.NORMALIZED.UB");
      break;
    default:
      llvm_unreachable("Clause not allowed");
    }
    ClauseEmissionHelper CEH(*this);
    addArg(E);
    CurrentClauseKind = OMPC_unknown;
  }

  void OpenMPCodeOutliner::emitImplicit(const VarDecl *VD,
                                        ImplicitClauseKind K) {
    // ICK_unknown is used when we do not want a variable to appear in any
    // clause list, so just return when we see it.
    if (K == ICK_unknown)
      return;

    if (!OMPLateOutlineLexicalScope::isCapturedVar(CGF, VD)) {
      // We don't want this DeclRefExpr to generate entries in the Def/Ref
      // lists, so temporarily save and null the CapturedStmtInfo.
      auto savedCSI = CGF.CapturedStmtInfo;
      CGF.CapturedStmtInfo = nullptr;

      DeclRefExpr DRE(const_cast<VarDecl *>(VD),
                      /*RefersToEnclosingVariableOrCapture=*/false,
                      VD->getType().getNonReferenceType(), VK_LValue,
                      SourceLocation());
      emitImplicit(&DRE, K);

      CGF.CapturedStmtInfo = savedCSI;
    }
  }

  bool OpenMPCodeOutliner::isUnspecifiedImplicit(const VarDecl *V) {
    if (ImplicitMap.find(V) == ImplicitMap.end())
      return false;
    return ImplicitMap[V] != ICK_specified_firstprivate;
  }

  bool OpenMPCodeOutliner::isImplicit(const VarDecl *V) {
    return ImplicitMap.find(V) != ImplicitMap.end();
  }

  bool OpenMPCodeOutliner::isExplicit(const VarDecl *V) {
    return ExplicitRefs.find(V) != ExplicitRefs.end();
  }

  void OpenMPCodeOutliner::addImplicitClauses() {
    if (!isOpenMPLoopDirective(CurrentDirectiveKind) &&
        !isOpenMPParallelDirective(CurrentDirectiveKind) &&
        CurrentDirectiveKind != OMPD_task &&
        CurrentDirectiveKind != OMPD_target &&
        CurrentDirectiveKind != OMPD_teams)
      return;

    // Add clause for implicit use of the 'this' pointer.
    if (Directive.hasAssociatedStmt() &&
        isAllowedClauseForDirective(CurrentDirectiveKind, OMPC_shared)) {
      // Catch the use of the this pointer for a captured lambda field.
      if (llvm::Value *TPV = getThisPointerValue()) {
        CurrentClauseKind = OMPC_shared;
        ClauseEmissionHelper CEH(*this);
        addArg("QUAL.OMP.SHARED");
        addArg(TPV);
        CurrentClauseKind = OMPC_unknown;
      }
      if (const Stmt *AS = Directive.getAssociatedStmt()) {
        auto CS = cast<CapturedStmt>(AS);
        for (auto &C : CS->captures()) {
          if (!C.capturesThis())
            continue;

          CurrentClauseKind = OMPC_shared;
          ClauseEmissionHelper CEH(*this);
          addArg("QUAL.OMP.SHARED");
          addArg(CGF.LoadCXXThis());
          CurrentClauseKind = OMPC_unknown;
          break;
        }
      }
    }

    for (const auto *VD : VarRefs) {
      if (isExplicit(VD))
        continue;
      if (isImplicit(VD)) {
        emitImplicit(VD, ImplicitMap[VD]);
        continue;
      }
      // These are not treated like normal variables and should produce only
      // NORMALIZED.[IV|UB] on their specific loop.  No clauses should be
      // added to outer regions.
      if (VD->getName() == ".omp.iv" || VD->getName() == ".omp.ub")
        continue;
      if (VarDefs.find(VD) != VarDefs.end()) {
        // Defined in the region: private
        emitImplicit(VD, ICK_private);
      } else if (CurrentDirectiveKind == OMPD_target) {
        if (!VD->getType()->isScalarType() ||
            Directive.hasClausesOfKind<OMPDefaultmapClause>())
          emitImplicit(VD, ICK_map_tofrom);
        else
          emitImplicit(VD, ICK_firstprivate);
      } else if (isAllowedClauseForDirective(CurrentDirectiveKind,
                                             OMPC_shared)) {
        // Referenced but not defined in the region: shared
        emitImplicit(VD, ICK_shared);
      }
    }
  }

  void OpenMPCodeOutliner::addRefsToOuter() {
    if (CGF.CapturedStmtInfo) {
      for (const auto *VD : VarDefs) {
        if (isUnspecifiedImplicit(VD))
          continue;
        CGF.CapturedStmtInfo->recordVariableDefinition(VD);
      }
      for (const auto *VD : VarRefs) {
        if (isUnspecifiedImplicit(VD))
          continue;
        CGF.CapturedStmtInfo->recordVariableReference(VD);
      }
      for (const auto *VD : ExplicitRefs) {
        CGF.CapturedStmtInfo->recordVariableReference(VD);
      }
    }
  }

  void OpenMPCodeOutliner::addExplicit(const Expr *E) {
    if (isa<ArraySubscriptExpr>(E->IgnoreParenImpCasts()) ||
        E->getType()->isSpecificPlaceholderType(BuiltinType::OMPArraySection))
      E = getArraySectionBase(E, /*AS=*/nullptr);
    addExplicit(cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl()));
  }

  void OpenMPCodeOutliner::emitOMPSharedClause(const OMPSharedClause *Cl) {
    for (auto *E : Cl->varlists()) {
      // Shared fields (or fields generated for lambda captures) are not
      // emitted since they are correctly handled through the shared this
      // pointer.
      if (const auto *ME = dyn_cast<MemberExpr>(E)) {
        if (isa<CXXThisExpr>(ME->getBase()))
          continue;
      } else if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
        if (DRE->refersToEnclosingVariableOrCapture())
          continue;
      }
      ClauseEmissionHelper CEH(*this);
      addArg("QUAL.OMP.SHARED");
      auto *PVD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
      addExplicit(PVD);
      addArg(E);
    }
  }

  void OpenMPCodeOutliner::emitOMPPrivateClause(const OMPPrivateClause *Cl) {
    auto IPriv = Cl->private_copies().begin();
    for (auto *E : Cl->varlists()) {
      auto *PVD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
      addExplicit(PVD);
      auto *Private = cast<VarDecl>(cast<DeclRefExpr>(*IPriv)->getDecl());
      auto *Init = Private->getInit();
      ClauseEmissionHelper CEH(*this);
      if (Init || Private->getType().isDestructedType())
        addArg("QUAL.OMP.PRIVATE:NONPOD");
      else
        addArg("QUAL.OMP.PRIVATE");
      addArg(E);
      if (Init || Private->getType().isDestructedType()) {
        addArg(emitIntelOpenMPDefaultConstructor(*IPriv));
        addArg(emitIntelOpenMPDestructor(Private->getType()));
      }
      ++IPriv;
    }
  }
  void OpenMPCodeOutliner::emitOMPLastprivateClause(
                                     const OMPLastprivateClause *Cl) {
    auto IPriv = Cl->private_copies().begin();
    auto ISrcExpr = Cl->source_exprs().begin();
    auto IDestExpr = Cl->destination_exprs().begin();
    auto IAssignOp = Cl->assignment_ops().begin();
    for (auto *E : Cl->varlists()) {
      auto *PVD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
      addExplicit(PVD);
      bool IsPODType = E->getType().isPODType(CGF.getContext());
      ClauseEmissionHelper CEH(*this);
      if (!IsPODType)
        addArg("QUAL.OMP.LASTPRIVATE:NONPOD");
      else if (Cl->isConditional())
        addArg("QUAL.OMP.LASTPRIVATE:CONDITIONAL");
      else
        addArg("QUAL.OMP.LASTPRIVATE");
      addArg(E);
      if (!IsPODType) {
        addArg(emitIntelOpenMPDefaultConstructor(*IPriv));
        addArg(emitIntelOpenMPCopyAssign(E->getType(), *ISrcExpr, *IDestExpr,
                                         *IAssignOp));
        addArg(emitIntelOpenMPDestructor(E->getType()));
      }
      ++IPriv;
      ++ISrcExpr;
      ++IDestExpr;
      ++IAssignOp;
    }
  }
  void OpenMPCodeOutliner::emitOMPLinearClause(const OMPLinearClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.LINEAR");
    for (auto *E : Cl->varlists())
      addArg(E);
    addArg(Cl->getStep() ? CGF.EmitScalarExpr(Cl->getStep())
                         : CGF.Builder.getInt32(1));
  }

  template <typename RedClause>
  void OpenMPCodeOutliner::emitOMPReductionClauseCommon(const RedClause *Cl,
                                                        StringRef QualName) {
    OverloadedOperatorKind OOK =
        Cl->getNameInfo().getName().getCXXOverloadedOperator();
    auto I = Cl->reduction_ops().begin();
    for (auto *E : Cl->varlists()) {
      addExplicit(E);
      assert(isa<BinaryOperator>((*I)->IgnoreImpCasts()));
      SmallString<64> Op;
      Op += "QUAL.OMP.";
      Op += QualName;
      Op += ".";
      switch (OOK) {
      case OO_Plus:
        Op += "ADD";
        break;
      case OO_Minus:
        Op += "SUB";
        break;
      case OO_Star:
        Op += "MUL";
        break;
      case OO_Amp:
        Op += "BAND";
        break;
      case OO_Pipe:
        Op += "BOR";
        break;
      case OO_Caret:
        Op += "BXOR";
        break;
      case OO_AmpAmp:
        Op += "AND";
        break;
      case OO_PipePipe:
        Op += "OR";
        break;
      case OO_New:
      case OO_Delete:
      case OO_Array_New:
      case OO_Array_Delete:
      case OO_Slash:
      case OO_Percent:
      case OO_Tilde:
      case OO_Exclaim:
      case OO_Equal:
      case OO_Less:
      case OO_Greater:
      case OO_LessEqual:
      case OO_GreaterEqual:
      case OO_Spaceship:
      case OO_PlusEqual:
      case OO_MinusEqual:
      case OO_StarEqual:
      case OO_SlashEqual:
      case OO_PercentEqual:
      case OO_CaretEqual:
      case OO_AmpEqual:
      case OO_PipeEqual:
      case OO_LessLess:
      case OO_GreaterGreater:
      case OO_LessLessEqual:
      case OO_GreaterGreaterEqual:
      case OO_EqualEqual:
      case OO_ExclaimEqual:
      case OO_PlusPlus:
      case OO_MinusMinus:
      case OO_Comma:
      case OO_ArrowStar:
      case OO_Arrow:
      case OO_Call:
      case OO_Subscript:
      case OO_Conditional:
      case OO_Coawait:
      case NUM_OVERLOADED_OPERATORS:
        llvm_unreachable("Unexpected reduction identifier");
      case OO_None:
        if (auto II = Cl->getNameInfo().getName().getAsIdentifierInfo()) {
          if (II->isStr("max"))
            Op += "MAX";
          else if (II->isStr("min"))
            Op += "MIN";
          QualType ElemType = E->getType();
          if (ElemType->isArrayType())
            ElemType = CGF.CGM.getContext().getBaseElementType(ElemType)
                .getNonReferenceType();
          if (ElemType->isVectorType())
            ElemType = ElemType->getAs<VectorType>()->getElementType();
          if (ElemType->isUnsignedIntegerType())
            Op += ":UNSIGNED";
        }
        break;
      }
      if (isa<ArraySubscriptExpr>(E->IgnoreParenImpCasts()) ||
          E->getType()->isSpecificPlaceholderType(BuiltinType::OMPArraySection))
        Op += ":ARRSECT";
      ClauseEmissionHelper CEH(*this);
      addArg(Op);
      addArg(E);
      ++I;
    }
  }

  void OpenMPCodeOutliner::emitOMPReductionClause(
                                      const OMPReductionClause *Cl) {
    emitOMPReductionClauseCommon(Cl, "REDUCTION");
  }

  void OpenMPCodeOutliner::emitOMPTaskReductionClause(
      const OMPTaskReductionClause *Cl) {
    emitOMPReductionClauseCommon(Cl, "REDUCTION");
  }

  void
  OpenMPCodeOutliner::emitOMPInReductionClause(const OMPInReductionClause *Cl) {
    emitOMPReductionClauseCommon(Cl, "INREDUCTION");
  }

  void OpenMPCodeOutliner::emitOMPOrderedClause(const OMPOrderedClause *C) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.ORDERED");
    if (auto *E = C->getNumForLoops())
      addArg(CGF.EmitScalarExpr(E));
    else
      addArg(CGF.Builder.getInt32(0));
  }

  void OpenMPCodeOutliner::emitOMPScheduleClause(const OMPScheduleClause *C) {
    int DefaultChunkSize = 0;
    SmallString<64> SchedString;
    switch (C->getScheduleKind()) {
    case OMPC_SCHEDULE_static:
      SchedString = "QUAL.OMP.SCHEDULE.STATIC";
      break;
    case OMPC_SCHEDULE_dynamic:
      DefaultChunkSize = 1;
      SchedString = "QUAL.OMP.SCHEDULE.DYNAMIC";
      break;
    case OMPC_SCHEDULE_guided:
      DefaultChunkSize = 1;
      SchedString = "QUAL.OMP.SCHEDULE.GUIDED";
      break;
    case OMPC_SCHEDULE_auto:
      SchedString = "QUAL.OMP.SCHEDULE.AUTO";
      break;
    case OMPC_SCHEDULE_runtime:
      SchedString = "QUAL.OMP.SCHEDULE.RUNTIME";
      break;
    case OMPC_SCHEDULE_unknown:
      llvm_unreachable("Unknown schedule clause");
    }

    SmallString<64> Modifiers;
    for (int Count = 0; Count < 2; ++Count) {
      SmallString<64> LocalModifier;
      auto Mod = Count == 0 ? C->getFirstScheduleModifier()
                            : C->getSecondScheduleModifier();
      switch (Mod) {
      case OMPC_SCHEDULE_MODIFIER_monotonic:
        LocalModifier = "MONOTONIC";
        break;
      case OMPC_SCHEDULE_MODIFIER_nonmonotonic:
        LocalModifier = "NONMONOTONIC";
        break;
      case OMPC_SCHEDULE_MODIFIER_simd:
        LocalModifier = "SIMD";
        break;
      case OMPC_SCHEDULE_MODIFIER_last:
      case OMPC_SCHEDULE_MODIFIER_unknown:
        break;
      }
      if (!LocalModifier.empty()) {
        if (!Modifiers.empty())
          Modifiers += ".";
        Modifiers += LocalModifier;
      }
    }
    if (!Modifiers.empty()) {
      SchedString += ':';
      SchedString += Modifiers;
    }
    ClauseEmissionHelper CEH(*this);
    addArg(SchedString);
    if (auto *E = C->getChunkSize())
      addArg(CGF.EmitScalarExpr(E));
    else
      addArg(CGF.Builder.getInt32(DefaultChunkSize));
  }

  void OpenMPCodeOutliner::emitOMPFirstprivateClause(
                                  const OMPFirstprivateClause *Cl) {
    if (Cl->isImplicit()) {
      if (CurrentDirectiveKind == OMPD_target ||
          CurrentDirectiveKind == OMPD_task) {
        // Only insert this on topmost part of combined directives.
        for (const auto *E : Cl->varlists()) {
          if (const auto *DRE = dyn_cast<DeclRefExpr>(E)) {
            ImplicitMap.insert(std::make_pair(cast<VarDecl>(DRE->getDecl()),
                                              ICK_specified_firstprivate));
          }
        }
      }
      return;
    }
    auto *IPriv = Cl->private_copies().begin();
    for (auto *E : Cl->varlists()) {
      ClauseEmissionHelper CEH(*this);
      auto *PVD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
      addExplicit(PVD);
      bool IsPODType = E->getType().isPODType(CGF.getContext());
      if (!IsPODType)
        addArg("QUAL.OMP.FIRSTPRIVATE:NONPOD");
      else
        addArg("QUAL.OMP.FIRSTPRIVATE");
      addArg(E);
      if (!IsPODType) {
        addArg(emitIntelOpenMPCopyConstructor(*IPriv));
        addArg(emitIntelOpenMPDestructor(E->getType()));
      }
      ++IPriv;
    }
  }

  void OpenMPCodeOutliner::emitOMPCopyinClause(const OMPCopyinClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.COPYIN");
    for (auto *E : Cl->varlists()) {
      if (!E->getType().isPODType(CGF.getContext()))
        CGF.CGM.ErrorUnsupported(E, "non-POD copyin variable");
      auto *PVD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
      addExplicit(PVD);
      addArg(E);
    }
  }

  void OpenMPCodeOutliner::emitOMPIfClause(const OMPIfClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.IF");
    addArg(CGF.EmitScalarExpr(Cl->getCondition()));
  }

  void OpenMPCodeOutliner::emitOMPNumThreadsClause(
                                    const OMPNumThreadsClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.NUM_THREADS");
    addArg(CGF.EmitScalarExpr(Cl->getNumThreads()));
  }

  void OpenMPCodeOutliner::emitOMPDefaultClause(const OMPDefaultClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    switch (Cl->getDefaultKind()) {
    case OMPC_DEFAULT_none:
      addArg("QUAL.OMP.DEFAULT.NONE");
      break;
    case OMPC_DEFAULT_shared:
      addArg("QUAL.OMP.DEFAULT.SHARED");
      break;
    case OMPC_DEFAULT_unknown:
      llvm_unreachable("Unknown default clause");
    }
  }

  void OpenMPCodeOutliner::emitOMPProcBindClause(const OMPProcBindClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    switch (Cl->getProcBindKind()) {
    case OMPC_PROC_BIND_master:
      addArg("QUAL.OMP.PROCBIND.MASTER");
      break;
    case OMPC_PROC_BIND_close:
      addArg("QUAL.OMP.PROCBIND.CLOSE");
      break;
    case OMPC_PROC_BIND_spread:
      addArg("QUAL.OMP.PROCBIND.SPREAD");
      break;
    case OMPC_PROC_BIND_unknown:
      llvm_unreachable("Unknown proc_bind clause");
    }
  }

  void OpenMPCodeOutliner::emitOMPSafelenClause(const OMPSafelenClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.SAFELEN");
    addArg(CGF.EmitScalarExpr(Cl->getSafelen()));
  }

  void OpenMPCodeOutliner::emitOMPSimdlenClause(const OMPSimdlenClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.SIMDLEN");
    addArg(CGF.EmitScalarExpr(Cl->getSimdlen()));
  }

  void OpenMPCodeOutliner::emitOMPCollapseClause(const OMPCollapseClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.COLLAPSE");
    addArg(CGF.EmitScalarExpr(Cl->getNumForLoops()));
  }

  void OpenMPCodeOutliner::emitOMPAlignedClause(const OMPAlignedClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.ALIGNED");
    for (auto *E : Cl->varlists())
      addArg(E);
    addArg(Cl->getAlignment() ? CGF.EmitScalarExpr(Cl->getAlignment())
                              : CGF.Builder.getInt32(0));
  }

  void
  OpenMPCodeOutliner::emitOMPGrainsizeClause(const OMPGrainsizeClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.GRAINSIZE");
    addArg(CGF.EmitScalarExpr(Cl->getGrainsize()));
  }

  void OpenMPCodeOutliner::emitOMPNumTasksClause(const OMPNumTasksClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.NUM_TASKS");
    addArg(CGF.EmitScalarExpr(Cl->getNumTasks()));
  }

  void OpenMPCodeOutliner::emitOMPPriorityClause(const OMPPriorityClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.PRIORITY");
    addArg(CGF.EmitScalarExpr(Cl->getPriority()));
  }

  void OpenMPCodeOutliner::emitOMPFinalClause(const OMPFinalClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.FINAL");
    addArg(CGF.EmitScalarExpr(Cl->getCondition()));
  }

  void OpenMPCodeOutliner::emitOMPNogroupClause(const OMPNogroupClause *) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.NOGROUP");
  }

  void OpenMPCodeOutliner::emitOMPMergeableClause(const OMPMergeableClause *) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.MERGEABLE");
  }

  void OpenMPCodeOutliner::emitOMPUntiedClause(const OMPUntiedClause *) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.UNTIED");
  }

  void OpenMPCodeOutliner::emitOMPDependClause(const OMPDependClause *Cl) {
    auto DepKind = Cl->getDependencyKind();

    if (DepKind == OMPC_DEPEND_source) {
      ClauseEmissionHelper CEH(*this);
      addArg("QUAL.OMP.DEPEND.SOURCE");
      return;
    }

    if (DepKind == OMPC_DEPEND_sink) {
      ClauseEmissionHelper CEH(*this);
      addArg("QUAL.OMP.DEPEND.SINK");
      for (unsigned I = 0, E = Cl->getNumLoops(); I < E; ++I)
        addArg(CGF.EmitScalarExpr(Cl->getLoopData(I)));
      return;
    }

    SmallString<64> Op;
    for (auto *E : Cl->varlists()) {
      switch (DepKind) {
      case OMPC_DEPEND_in:
        Op = "QUAL.OMP.DEPEND.IN";
        break;
      case OMPC_DEPEND_out:
        Op = "QUAL.OMP.DEPEND.OUT";
        break;
      case OMPC_DEPEND_inout:
        Op = "QUAL.OMP.DEPEND.INOUT";
        break;
      default:
        llvm_unreachable("Unknown depend clause");
      }
      if (E->getType()->isSpecificPlaceholderType(BuiltinType::OMPArraySection))
        Op += ":ARRSECT";
      ClauseEmissionHelper CEH(*this);
      addArg(Op);
      addArg(E);
    }
  }

  void OpenMPCodeOutliner::emitOMPDeviceClause(const OMPDeviceClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.DEVICE");
    addArg(CGF.EmitScalarExpr(Cl->getDevice()));
  }

  void
  OpenMPCodeOutliner::emitOMPIsDevicePtrClause(const OMPIsDevicePtrClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.IS_DEVICE_PTR");
    for (auto *E : Cl->varlists()) {
      auto *PVD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
      addExplicit(PVD);
      addArg(E);
    }
  }

  void
  OpenMPCodeOutliner::emitOMPDefaultmapClause(const OMPDefaultmapClause *Cl) {
    SmallString<64> DefaultmapString;
    switch (Cl->getDefaultmapModifier()) {
    case OMPC_DEFAULTMAP_MODIFIER_tofrom:
      DefaultmapString += "QUAL.OMP.DEFAULTMAP.TOFROM";
      break;
    case OMPC_DEFAULTMAP_MODIFIER_last:
    case OMPC_DEFAULTMAP_MODIFIER_unknown:
      llvm_unreachable("Unknown defaultmap modifier");
    }
    switch (Cl->getDefaultmapKind()) {
    case OMPC_DEFAULTMAP_scalar:
      DefaultmapString += ".SCALAR";
      break;
    case OMPC_DEFAULTMAP_unknown:
      llvm_unreachable("Unknown defaultmap kind");
    }
    ClauseEmissionHelper CEH(*this);
    addArg(DefaultmapString);
  }

  void OpenMPCodeOutliner::emitOMPNowaitClause(const OMPNowaitClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.NOWAIT");
  }

  void OpenMPCodeOutliner::emitOMPUseDevicePtrClause(
      const OMPUseDevicePtrClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.USE_DEVICE_PTR");
    for (auto *E : Cl->varlists())
      addArg(E);
  }

  void OpenMPCodeOutliner::emitOMPToClause(const OMPToClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.TO");
    for (auto *E : Cl->varlists())
      addArg(E);
  }

  void OpenMPCodeOutliner::emitOMPFromClause(const OMPFromClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.FROM");
    for (auto *E : Cl->varlists())
      addArg(E);
  }

  void OpenMPCodeOutliner::emitOMPNumTeamsClause(const OMPNumTeamsClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.NUM_TEAMS");
    addArg(CGF.EmitScalarExpr(Cl->getNumTeams()));
  }

  void OpenMPCodeOutliner::emitOMPThreadLimitClause(
                                              const OMPThreadLimitClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.THREAD_LIMIT");
    addArg(CGF.EmitScalarExpr(Cl->getThreadLimit()));
  }

  void OpenMPCodeOutliner::emitOMPDistScheduleClause(
                                             const OMPDistScheduleClause *Cl) {
    int DefaultChunkSize = 0;
    SmallString<64> SchedString;
    switch (Cl->getDistScheduleKind()) {
    case OMPC_DIST_SCHEDULE_static:
      SchedString = "QUAL.OMP.DIST_SCHEDULE.STATIC";
      break;
    case OMPC_DIST_SCHEDULE_unknown:
      llvm_unreachable("Unknown schedule clause");
    }

    ClauseEmissionHelper CEH(*this);
    addArg(SchedString);
    if (auto *E = Cl->getChunkSize())
      addArg(CGF.EmitScalarExpr(E));
    else
      addArg(CGF.Builder.getInt32(DefaultChunkSize));
  }

  void OpenMPCodeOutliner::emitOMPFlushClause(const OMPFlushClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.FLUSH");
    for (auto *E : Cl->varlists())
      addArg(E);
  }

  void
  OpenMPCodeOutliner::emitOMPCopyprivateClause(const OMPCopyprivateClause *Cl) {
    auto ISrcExpr = Cl->source_exprs().begin();
    auto IDestExpr = Cl->destination_exprs().begin();
    auto IAssignOp = Cl->assignment_ops().begin();
    for (auto *E : Cl->varlists()) {
      ClauseEmissionHelper CEH(*this);
      auto *PVD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
      addExplicit(PVD);
      bool IsPODType = E->getType().isPODType(CGF.getContext());
      if (IsPODType)
        addArg("QUAL.OMP.COPYPRIVATE");
      else
        addArg("QUAL.OMP.COPYPRIVATE:NONPOD");
      addArg(E);
      if (!IsPODType) {
        addArg(emitIntelOpenMPCopyAssign(E->getType(), *ISrcExpr, *IDestExpr,
                                         *IAssignOp));
      }
      ++ISrcExpr;
      ++IDestExpr;
      ++IAssignOp;
    }
  }

  void OpenMPCodeOutliner::emitOMPHintClause(const OMPHintClause *Cl) {
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.HINT");
    addArg(CGF.EmitScalarExpr(Cl->getHint()));
  }

  void OpenMPCodeOutliner::emitOMPReadClause(const OMPReadClause *) {}
  void OpenMPCodeOutliner::emitOMPWriteClause(const OMPWriteClause *) {}
  void OpenMPCodeOutliner::emitOMPUpdateClause(const OMPUpdateClause *) {}
  void OpenMPCodeOutliner::emitOMPCaptureClause(const OMPCaptureClause *) {}
  void OpenMPCodeOutliner::emitOMPSeqCstClause(const OMPSeqCstClause *) {}
  void OpenMPCodeOutliner::emitOMPThreadsClause(const OMPThreadsClause *) {}
  void OpenMPCodeOutliner::emitOMPSIMDClause(const OMPSIMDClause *) {}
  void OpenMPCodeOutliner::emitOMPUnifiedAddressClause(
      const OMPUnifiedAddressClause *) {}
  void OpenMPCodeOutliner::emitOMPUnifiedSharedMemoryClause(
      const OMPUnifiedSharedMemoryClause *) {}
  void OpenMPCodeOutliner::emitOMPReverseOffloadClause(
      const OMPReverseOffloadClause *) {}
  void OpenMPCodeOutliner::emitOMPDynamicAllocatorsClause(
      const OMPDynamicAllocatorsClause *) {}

  void OpenMPCodeOutliner::addFenceCalls(bool IsBegin) {
    switch (Directive.getDirectiveKind()) {
    case OMPD_atomic:
    case OMPD_critical:
    case OMPD_single:
    case OMPD_master:
      if (IsBegin)
        CGF.Builder.CreateFence(llvm::AtomicOrdering::Acquire);
      else
       CGF.Builder.CreateFence(llvm::AtomicOrdering::Release);
      break;
    case OMPD_barrier:
    case OMPD_taskwait:
      if (IsBegin)
       CGF.Builder.CreateFence(llvm::AtomicOrdering::AcquireRelease);
      break;
    default:
      break;
    }
  }

  OpenMPCodeOutliner::OpenMPCodeOutliner(CodeGenFunction &CGF,
                                         const OMPExecutableDirective &D,
                                         OpenMPDirectiveKind Kind)
      : CGF(CGF), C(CGF.CGM.getLLVMContext()), TLPH(CGF), Directive(D),
        CurrentDirectiveKind(Kind) {
    // Set an attribute indicating that the routine may have OpenMP directives
    // (represented with llvm intrinsics) in the LLVM IR
    CGF.CurFn->addFnAttr("may-have-openmp-directive", "true");

    if (CurrentDirectiveKind == OMPD_unknown)
      CurrentDirectiveKind = D.getDirectiveKind();

    RegionEntryDirective = CGF.CGM.getIntrinsic(
                               llvm::Intrinsic::directive_region_entry);
    RegionExitDirective = CGF.CGM.getIntrinsic(
                               llvm::Intrinsic::directive_region_exit);

    // Create a marker call at the start of the region.  The values generated
    // from clauses must be inserted before this point.
    SmallVector<llvm::Value*, 1> CallArgs;
    MarkerInstruction = CGF.Builder.CreateCall(RegionEntryDirective, CallArgs);

    if (isOpenMPLoopDirective(CurrentDirectiveKind)) {
      auto *LoopDir = dyn_cast<OMPLoopDirective>(&D);
      for (auto *E : LoopDir->counters()) {
        auto *PVD = cast<VarDecl>(cast<DeclRefExpr>(E)->getDecl());
        if (isOpenMPSimdDirective(CurrentDirectiveKind))
          ImplicitMap.insert(std::make_pair(PVD, ICK_unknown));
        else
          ImplicitMap.insert(std::make_pair(PVD, ICK_private));
      }
      auto IVExpr = cast<DeclRefExpr>(LoopDir->getIterationVariable());
      auto IVDecl = cast<VarDecl>(IVExpr->getDecl());
      ImplicitMap.insert(std::make_pair(IVDecl, ICK_normalized_iv));
      auto UBExpr =
          cast<DeclRefExpr>(CurrentDirectiveKind == OMPD_simd
                                ? LoopDir->getLateOutlineUpperBoundVariable()
                                : LoopDir->getUpperBoundVariable());
      auto UBDecl = cast<VarDecl>(UBExpr->getDecl());
      ImplicitMap.insert(std::make_pair(UBDecl, ICK_normalized_ub));
      if (isOpenMPWorksharingDirective(CurrentDirectiveKind) ||
          isOpenMPTaskLoopDirective(CurrentDirectiveKind) ||
          isOpenMPDistributeDirective(CurrentDirectiveKind)) {
        auto LBExpr = cast<DeclRefExpr>(LoopDir->getLowerBoundVariable());
        auto LBDecl = cast<VarDecl>(LBExpr->getDecl());
        ImplicitMap.insert(std::make_pair(LBDecl, ICK_firstprivate));
      }
    }
    addFenceCalls(/*IsBegin=*/true);
  }

  OpenMPCodeOutliner::~OpenMPCodeOutliner() {

    addFenceCalls(/*IsBegin=*/false);

    addImplicitClauses();

    // Insert the start directives.
    auto EndIP = CGF.Builder.saveIP();
    setInsertPoint();

    for (auto &D : Directives) {
      D.CallEntry =
          CGF.Builder.CreateCall(RegionEntryDirective, {}, D.OpBundles);
      D.clear();
      // Place the end directive in place of the start.
      emitDirective(D, D.End);
    }
    CGF.Builder.restoreIP(EndIP);

    // Insert the end directives.
    for (auto I = Directives.rbegin(), E = Directives.rend(); I != E; ++I)
      CGF.Builder.CreateCall(RegionExitDirective, {I->CallEntry}, I->OpBundles);
    MarkerInstruction->eraseFromParent();

    addRefsToOuter();
  }

  void OpenMPCodeOutliner::emitOMPParallelDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.PARALLEL", "DIR.OMP.END.PARALLEL");
  }
  void OpenMPCodeOutliner::emitOMPParallelForDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.PARALLEL.LOOP",
                               "DIR.OMP.END.PARALLEL.LOOP");
  }
  void OpenMPCodeOutliner::emitOMPSIMDDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.SIMD", "DIR.OMP.END.SIMD");
  }

  void OpenMPCodeOutliner::emitOMPForDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.LOOP", "DIR.OMP.END.LOOP");
  }

  void OpenMPCodeOutliner::emitOMPForSimdDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.LOOP", "DIR.OMP.END.LOOP", OMPD_for);
    startDirectiveIntrinsicSet("DIR.OMP.SIMD", "DIR.OMP.END.SIMD", OMPD_simd);
  }

  void OpenMPCodeOutliner::emitOMPParallelForSimdDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.PARALLEL.LOOP",
                               "DIR.OMP.END.PARALLEL.LOOP", OMPD_parallel_for);
    startDirectiveIntrinsicSet("DIR.OMP.SIMD", "DIR.OMP.END.SIMD", OMPD_simd);
  }

  void OpenMPCodeOutliner::emitOMPTaskLoopDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TASKLOOP", "DIR.OMP.END.TASKLOOP",
                               OMPD_taskloop);
  }

  void OpenMPCodeOutliner::emitOMPTaskLoopSimdDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TASKLOOP", "DIR.OMP.END.TASKLOOP",
                               OMPD_taskloop);
    startDirectiveIntrinsicSet("DIR.OMP.SIMD", "DIR.OMP.END.SIMD", OMPD_simd);
  }

  void OpenMPCodeOutliner::emitOMPAtomicDirective(OMPAtomicClause ClauseKind) {
    startDirectiveIntrinsicSet("DIR.OMP.ATOMIC", "DIR.OMP.END.ATOMIC");

    StringRef Op = "QUAL.OMP.UPDATE";
    switch (ClauseKind) {
    case OMP_read:
      Op = "QUAL.OMP.READ";
      break;
    case OMP_write:
      Op = "QUAL.OMP.WRITE";
      break;
    case OMP_update:
      break;
    case OMP_capture:
      Op = "QUAL.OMP.CAPTURE";
      break;
    case OMP_read_seq_cst:
      Op = "QUAL.OMP.READ.SEQ_CST";
      break;
    case OMP_write_seq_cst:
      Op = "QUAL.OMP.WRITE.SEQ_CST";
      break;
    case OMP_update_seq_cst:
      Op = "QUAL.OMP.UPDATE.SEQ_CST";
      break;
    case OMP_capture_seq_cst:
      Op = "QUAL.OMP.CAPTURE.SEQ_CST";
      break;
    }
    ClauseEmissionHelper CEH(*this);
    addArg(Op);
  }
  void OpenMPCodeOutliner::emitOMPSingleDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.SINGLE", "DIR.OMP.END.SINGLE");
  }
  void OpenMPCodeOutliner::emitOMPMasterDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.MASTER", "DIR.OMP.END.MASTER");
  }
  void OpenMPCodeOutliner::emitOMPCriticalDirective(const StringRef Name) {
    startDirectiveIntrinsicSet("DIR.OMP.CRITICAL", "DIR.OMP.END.CRITICAL");
    if (!Name.empty()) {
      ClauseEmissionHelper CEH(*this);
      addArg("QUAL.OMP.NAME");
      addArg(llvm::ConstantDataArray::getString(C, Name, /*AddNull=*/false));
    }
  }
  void OpenMPCodeOutliner::emitOMPOrderedDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.ORDERED", "DIR.OMP.END.ORDERED");
  }
  void OpenMPCodeOutliner::emitOMPTargetDirective(int OffloadEntryIndex) {
    startDirectiveIntrinsicSet("DIR.OMP.TARGET", "DIR.OMP.END.TARGET");

    // Add operand bundle for the offload entry index.
    ClauseEmissionHelper CEH(*this);
    addArg("QUAL.OMP.OFFLOAD.ENTRY.IDX");
    addArg(CGF.Builder.getInt32(OffloadEntryIndex));
  }
  void OpenMPCodeOutliner::emitOMPTargetDataDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TARGET.DATA",
                               "DIR.OMP.END.TARGET.DATA");
  }
  void OpenMPCodeOutliner::emitOMPTargetUpdateDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TARGET.UPDATE",
                               "DIR.OMP.END.TARGET.UPDATE");
  }
  void OpenMPCodeOutliner::emitOMPTargetEnterDataDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TARGET.ENTER.DATA",
                               "DIR.OMP.END.TARGET.ENTER.DATA");
  }
  void OpenMPCodeOutliner::emitOMPTargetExitDataDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TARGET.EXIT.DATA",
                               "DIR.OMP.END.TARGET.EXIT.DATA");
  }
  void OpenMPCodeOutliner::emitOMPTaskDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TASK", "DIR.OMP.END.TASK");
  }
  void OpenMPCodeOutliner::emitOMPTaskGroupDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TASKGROUP", "DIR.OMP.END.TASKGROUP");
  }
  void OpenMPCodeOutliner::emitOMPTaskWaitDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TASKWAIT", "DIR.OMP.END.TASKWAIT");
  }
  void OpenMPCodeOutliner::emitOMPTaskYieldDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TASKYIELD", "DIR.OMP.END.TASKYIELD");
  }
  void OpenMPCodeOutliner::emitOMPBarrierDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.BARRIER", "DIR.OMP.END.BARRIER");
  }
  void OpenMPCodeOutliner::emitOMPFlushDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.FLUSH", "DIR.OMP.END.FLUSH");
  }
  void OpenMPCodeOutliner::emitOMPTeamsDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.TEAMS", "DIR.OMP.END.TEAMS");
  }
  void OpenMPCodeOutliner::emitOMPDistributeDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.DISTRIBUTE", "DIR.OMP.END.DISTRIBUTE");
  }
  void OpenMPCodeOutliner::emitOMPDistributeParallelForDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.DISTRIBUTE.PARLOOP",
                               "DIR.OMP.END.DISTRIBUTE.PARLOOP");
  }
  void OpenMPCodeOutliner::emitOMPDistributeParallelForSimdDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.DISTRIBUTE.PARLOOP",
                               "DIR.OMP.END.DISTRIBUTE.PARLOOP",
                               OMPD_distribute_parallel_for);
    startDirectiveIntrinsicSet("DIR.OMP.SIMD", "DIR.OMP.END.SIMD", OMPD_simd);
  }
  void OpenMPCodeOutliner::emitOMPDistributeSimdDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.DISTRIBUTE", "DIR.OMP.END.DISTRIBUTE",
                               OMPD_distribute_simd);
    startDirectiveIntrinsicSet("DIR.OMP.SIMD", "DIR.OMP.END.SIMD", OMPD_simd);
  }
  void OpenMPCodeOutliner::emitOMPSectionsDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.SECTIONS", "DIR.OMP.END.SECTIONS");
  }
  void OpenMPCodeOutliner::emitOMPSectionDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.SECTION", "DIR.OMP.END.SECTION");
  }
  void OpenMPCodeOutliner::emitOMPParallelSectionsDirective() {
    startDirectiveIntrinsicSet("DIR.OMP.PARALLEL.SECTIONS",
                               "DIR.OMP.END.PARALLEL.SECTIONS");
  }

  static StringRef getCancelQualString(OpenMPDirectiveKind Kind) {
    switch (Kind) {
    case OMPD_parallel:
      return "PARALLEL";
    case OMPD_sections:
      return "SECTIONS";
    case OMPD_for:
      return "LOOP";
    case OMPD_taskgroup:
      return "TASKGROUP";
    default:
      llvm_unreachable("Unexpected cancel region type");
    }
  }

  void OpenMPCodeOutliner::emitOMPCancelDirective(OpenMPDirectiveKind Kind) {
    startDirectiveIntrinsicSet("DIR.OMP.CANCEL", "DIR.OMP.END.CANCEL");
    SmallString<32> Qual;
    Qual = "QUAL.OMP.CANCEL.";
    Qual += getCancelQualString(Kind);
    ClauseEmissionHelper CEH(*this);
    addArg(Qual);
  }
  void OpenMPCodeOutliner::emitOMPCancellationPointDirective(
      OpenMPDirectiveKind Kind) {
    startDirectiveIntrinsicSet("DIR.OMP.CANCELLATION.POINT",
                               "DIR.OMP.END.CANCELLATION.POINT");
    SmallString<32> Qual;
    Qual = "QUAL.OMP.CANCEL.";
    Qual += getCancelQualString(Kind);
    ClauseEmissionHelper CEH(*this);
    addArg(Qual);
  }

  OpenMPCodeOutliner &OpenMPCodeOutliner::
  operator<<(ArrayRef<OMPClause *> Clauses) {
    for (auto *C : Clauses) {
      CurrentClauseKind = C->getClauseKind();
      if (!isAllowedClauseForDirective(CurrentDirectiveKind, CurrentClauseKind))
        continue;
      switch (CurrentClauseKind) {
#define OPENMP_CLAUSE(Name, Class)                                             \
  case OMPC_##Name:                                                            \
    emit##Class(cast<Class>(C));                                               \
    break;
#include "clang/Basic/OpenMPKinds.def"
      case OMPC_uniform:
      case OMPC_threadprivate:
      case OMPC_unknown:
        llvm_unreachable("Clause not allowed");
      }
    }
    CurrentClauseKind = OMPC_unknown;
    return *this;
  }

  /// \brief Emit the captured statement body.
  void CGOpenMPRegionInfo::EmitBody(CodeGenFunction &CGF, const Stmt *S) {
    if (!CGF.HaveInsertPoint())
      return;
    auto *CS = cast<CapturedStmt>(S);
    CGF.EmitStmt(CS->getCapturedStmt());
  }

  // \brief Retrieve the value of the context parameter.
  llvm::Value *CGOpenMPRegionInfo::getContextValue() const {
    if (OldCSI)
      return OldCSI->getContextValue();
    llvm_unreachable("No context value for inlined OpenMP region");
  }
  void CGOpenMPRegionInfo::setContextValue(llvm::Value *V) {
    if (OldCSI) {
      OldCSI->setContextValue(V);
      return;
    }
    llvm_unreachable("No context value for inlined OpenMP region");
  }
  /// \brief Lookup the captured field decl for a variable.
  const FieldDecl *CGOpenMPRegionInfo::lookup(
                                   const VarDecl *VD) const {
    if (OldCSI)
      return OldCSI->lookup(VD);
    // If there is no outer outlined region,no need to lookup in a list of
    // captured variables, we can use the original one.
    return nullptr;
  }
  FieldDecl *CGOpenMPRegionInfo::getThisFieldDecl() const {
    if (OldCSI)
      return OldCSI->getThisFieldDecl();
    return nullptr;
  }

} // namespace

static OpenMPDirectiveKind nextDirectiveKind(OpenMPDirectiveKind FullDirKind,
                                             OpenMPDirectiveKind CurrDirKind) {
  switch (FullDirKind) {
  case OMPD_target_parallel:
    // OMPD_target -> OMPD_parallel
    if (CurrDirKind == OMPD_target)
      return OMPD_parallel;
    return OMPD_unknown;

  case OMPD_target_parallel_for:
    // OMPD_target -> OMPD_parallel_for
    if (CurrDirKind == OMPD_target)
      return OMPD_parallel_for;
    return OMPD_unknown;

  case OMPD_target_parallel_for_simd:
    // OMPD_target -> OMPD_parallel_for_simd
    if (CurrDirKind == OMPD_target)
      return OMPD_parallel_for_simd;
    return OMPD_unknown;

  case OMPD_target_simd:
    // OMPD_target -> OMPD_simd
    if (CurrDirKind == OMPD_target)
      return OMPD_simd;
    return OMPD_unknown;

  case OMPD_target_teams:
    // OMPD_target -> OMPD_teams
    if (CurrDirKind == OMPD_target)
      return OMPD_teams;
    return OMPD_unknown;

  case OMPD_target_teams_distribute:
  case OMPD_teams_distribute:
    // OMPD_target -> OMPD_teams -> OMPD_distribute
    if (CurrDirKind == OMPD_target)
      return OMPD_teams;
    if (CurrDirKind == OMPD_teams)
      return OMPD_distribute;
    return OMPD_unknown;

  case OMPD_target_teams_distribute_simd:
  case OMPD_teams_distribute_simd:
    // OMPD_target -> OMPD_teams -> OMPD_distribute_simd
    if (CurrDirKind == OMPD_target)
      return OMPD_teams;
    if (CurrDirKind == OMPD_teams)
      return OMPD_distribute_simd;
    return OMPD_unknown;

  case OMPD_target_teams_distribute_parallel_for:
  case OMPD_teams_distribute_parallel_for:
    // OMPD_target -> OMPD_teams -> OMPD_distribute_parallel_for
    if (CurrDirKind == OMPD_target)
      return OMPD_teams;
    if (CurrDirKind == OMPD_teams)
      return OMPD_distribute_parallel_for;
    return OMPD_unknown;

  case OMPD_target_teams_distribute_parallel_for_simd:
  case OMPD_teams_distribute_parallel_for_simd:
    // OMPD_target -> OMPD_teams -> OMPD_distribute_parallel_for_simd
    if (CurrDirKind == OMPD_target)
      return OMPD_teams;
    if (CurrDirKind == OMPD_teams)
      return OMPD_distribute_parallel_for_simd;
    return OMPD_unknown;

  default:
    llvm_unreachable("Unhandled combined directive.");
  }
}

void CodeGenFunction::EmitLateOutlineOMPDirective(
    const OMPExecutableDirective &S, OpenMPDirectiveKind Kind) {
  OMPLateOutlineLexicalScope Scope(*this, S);
  OpenMPCodeOutliner Outliner(*this, S, Kind);

  OpenMPDirectiveKind CurrentDirectiveKind = Outliner.getCurrentDirectiveKind();
  switch (CurrentDirectiveKind) {
  case OMPD_parallel:
    Outliner.emitOMPParallelDirective();
    break;
  case OMPD_atomic: {
    bool IsSeqCst = S.hasClausesOfKind<OMPSeqCstClause>();
    OMPAtomicClause ClauseKind = IsSeqCst ? OMP_update_seq_cst : OMP_update;
    if (S.hasClausesOfKind<OMPReadClause>())
      ClauseKind = IsSeqCst ? OMP_read_seq_cst : OMP_read;
    else if (S.hasClausesOfKind<OMPWriteClause>())
      ClauseKind = IsSeqCst ? OMP_write_seq_cst : OMP_write;
    else if (S.hasClausesOfKind<OMPCaptureClause>())
      ClauseKind = IsSeqCst ? OMP_capture_seq_cst : OMP_capture;
    Outliner.emitOMPAtomicDirective(ClauseKind);
    break;
  }
  case OMPD_single:
    Outliner.emitOMPSingleDirective();
    break;
  case OMPD_master:
    Outliner.emitOMPMasterDirective();
    break;
  case OMPD_critical: {
    const auto &CD = cast<OMPCriticalDirective>(S);
    Outliner.emitOMPCriticalDirective(CD.getDirectiveName().getAsString());
    break;
  }
  case OMPD_ordered:
    Outliner.emitOMPOrderedDirective();
    break;
  case OMPD_target: {
    CGM.setHasTargetCode();

    // Get an index of the associated offload entry for this target directive.
    assert(CurFuncDecl && "No parent declaration for target region!");
    StringRef ParentName;
    // In case we have Ctors/Dtors we use the complete type variant to produce
    // the mangling of the device outlined kernel.
    if (const auto *D = dyn_cast<CXXConstructorDecl>(CurFuncDecl))
      ParentName = CGM.getMangledName(GlobalDecl(D, Ctor_Complete));
    else if (const auto *D = dyn_cast<CXXDestructorDecl>(CurFuncDecl))
      ParentName = CGM.getMangledName(GlobalDecl(D, Dtor_Complete));
    else
      ParentName =
          CGM.getMangledName(GlobalDecl(cast<FunctionDecl>(CurFuncDecl)));

    int Order = CGM.getOpenMPRuntime().registerTargetRegion(S, ParentName);
    assert(Order >= 0 && "No entry for the target region");

    Outliner.emitOMPTargetDirective(Order);
    break;
  }
  case OMPD_target_data:
    CGM.setHasTargetCode();
    Outliner.emitOMPTargetDataDirective();
    break;
  case OMPD_target_update:
    CGM.setHasTargetCode();
    Outliner.emitOMPTargetUpdateDirective();
    break;
  case OMPD_target_enter_data:
    CGM.setHasTargetCode();
    Outliner.emitOMPTargetEnterDataDirective();
    break;
  case OMPD_target_exit_data:
    CGM.setHasTargetCode();
    Outliner.emitOMPTargetExitDataDirective();
    break;
  case OMPD_task:
    Outliner.emitOMPTaskDirective();
    break;
  case OMPD_taskgroup:
    Outliner.emitOMPTaskGroupDirective();
    break;
  case OMPD_taskwait:
    Outliner.emitOMPTaskWaitDirective();
    break;
  case OMPD_taskyield:
    Outliner.emitOMPTaskYieldDirective();
    break;
  case OMPD_teams:
    Outliner.emitOMPTeamsDirective();
    break;
  case OMPD_barrier:
    Outliner.emitOMPBarrierDirective();
    break;
  case OMPD_flush:
    Outliner.emitOMPFlushDirective();
    break;
  case OMPD_sections:
    Outliner.emitOMPSectionsDirective();
    break;
  case OMPD_section:
    Outliner.emitOMPSectionDirective();
    break;
  case OMPD_parallel_sections:
    Outliner.emitOMPParallelSectionsDirective();
    break;
  case OMPD_cancel:
    Outliner.emitOMPCancelDirective(
        cast<OMPCancelDirective>(S).getCancelRegion());
    break;
  case OMPD_cancellation_point:
    Outliner.emitOMPCancellationPointDirective(
        cast<OMPCancellationPointDirective>(S).getCancelRegion());
    break;

  // These directives are not yet implemented.
  case OMPD_requires:
    break;

  // These directives do not create region directives.
  case OMPD_declare_target:
  case OMPD_end_declare_target:
  case OMPD_threadprivate:
  case OMPD_declare_reduction:
  case OMPD_declare_simd:
  case OMPD_unknown:
    llvm_unreachable("Wrong OpenMP directive");

  case OMPD_distribute:
  case OMPD_distribute_simd:
  case OMPD_distribute_parallel_for:
  case OMPD_distribute_parallel_for_simd:
  case OMPD_simd:
  case OMPD_for:
  case OMPD_for_simd:
  case OMPD_parallel_for:
  case OMPD_parallel_for_simd:
  case OMPD_taskloop:
  case OMPD_taskloop_simd:
    llvm_unreachable("OpenMP loops not handled here");

  case OMPD_target_parallel:
  case OMPD_target_parallel_for:
  case OMPD_target_parallel_for_simd:
  case OMPD_target_simd:
  case OMPD_target_teams:
  case OMPD_target_teams_distribute:
  case OMPD_target_teams_distribute_simd:
  case OMPD_target_teams_distribute_parallel_for:
  case OMPD_target_teams_distribute_parallel_for_simd:
  case OMPD_teams_distribute:
  case OMPD_teams_distribute_simd:
  case OMPD_teams_distribute_parallel_for:
  case OMPD_teams_distribute_parallel_for_simd:
    llvm_unreachable("Combined directives not handled here");
  }
  Outliner << S.clauses();
  if (S.hasAssociatedStmt() && S.getAssociatedStmt() != nullptr) {
    InlinedOpenMPRegionRAII Region(*this, Outliner, S);
    if (S.getDirectiveKind() != CurrentDirectiveKind) {
      // Unless we've reached the innermost directive, keep going.
      OpenMPDirectiveKind NextKind =
          nextDirectiveKind(S.getDirectiveKind(), CurrentDirectiveKind);
      switch (NextKind) {
        case OMPD_parallel:
        case OMPD_teams:
          return EmitLateOutlineOMPDirective(S, NextKind);
        case OMPD_parallel_for:
        case OMPD_parallel_for_simd:
        case OMPD_simd:
        case OMPD_distribute:
        case OMPD_distribute_simd:
        case OMPD_distribute_parallel_for:
        case OMPD_distribute_parallel_for_simd:
          return EmitLateOutlineOMPLoopDirective(cast<OMPLoopDirective>(S),
                                                 NextKind);
        case OMPD_unknown:
          // This is the innermost directive, fallthrough.
          break;
        default:
          llvm_unreachable("Unexpected next directive kind.");
      }
    }
    const Stmt *CapturedStmt = S.getInnermostCapturedStmt();
    CapturedStmtInfo->EmitBody(*this, CapturedStmt);
  }
}

void CodeGenFunction::RemapForLateOutlining(const OMPExecutableDirective &D,
                                            OMPPrivateScope &PrivScope) {
  SmallVector<const Expr *, 5> RemapVars;
  for (const auto *C : D.getClausesOfKind<OMPPrivateClause>())
     for (const auto *Ref : C->varlists())
       RemapVars.push_back(Ref);
  for (const auto *C : D.getClausesOfKind<OMPFirstprivateClause>())
     for (const auto *Ref : C->varlists())
       RemapVars.push_back(Ref);
  for (const auto *C : D.getClausesOfKind<OMPLastprivateClause>())
     for (const auto *Ref : C->varlists())
       RemapVars.push_back(Ref);
  for (const auto *C : D.getClausesOfKind<OMPReductionClause>())
     for (const auto *Ref : C->varlists())
       RemapVars.push_back(Ref);

  for (const auto *Ref : RemapVars) {
    if (auto *DRE = dyn_cast<DeclRefExpr>(Ref->IgnoreParenImpCasts())) {
      if (auto *VD = dyn_cast<VarDecl>(DRE->getDecl())) {
        if (isa<OMPCapturedExprDecl>(VD)) {
          PrivScope.addPrivateNoTemps(VD, [this, VD]() -> Address {
            return EmitLValue(VD->getAnyInitializer()).getAddress();
          });
        }
      }
    }
  }
}

static void emitLateOutlineDirective(CodeGenFunction &CGF,
                                     const RegionCodeGenTy &CodeGen) {
  if (!CGF.HaveInsertPoint())
    return;
  CGF.EHStack.pushTerminate();
  CodeGen(CGF);
  CGF.EHStack.popTerminate();
}

void CodeGenFunction::EmitLateOutlineOMPLoopDirective(
    const OMPLoopDirective &S, OpenMPDirectiveKind Kind) {
  OMPLateOutlineLexicalScope Scope(*this, S, Kind);
  auto &&CodeGen = [&S,Kind](CodeGenFunction &CGF, PrePostActionTy &) {
    CGF.EmitLateOutlineOMPLoop(S, Kind);
  };
  emitLateOutlineDirective(*this, CodeGen);
}
