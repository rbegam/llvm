//===---- CGLoopInfo.cpp - LLVM CodeGen for loop metadata -*- C++ -*-------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//

#include "CGLoopInfo.h"
#include "clang/AST/ASTContext.h"
#include "clang/AST/Attr.h"
#include "clang/Sema/LoopHint.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
using namespace clang::CodeGen;
using namespace llvm;

static MDNode *createMetadata(LLVMContext &Ctx, const LoopAttributes &Attrs,
                              const llvm::DebugLoc &StartLoc,
                              const llvm::DebugLoc &EndLoc) {

  if (!Attrs.IsParallel && Attrs.VectorizeWidth == 0 &&
      Attrs.InterleaveCount == 0 && Attrs.UnrollCount == 0 &&
      Attrs.VectorizeEnable == LoopAttributes::Unspecified &&
      Attrs.UnrollEnable == LoopAttributes::Unspecified &&
      Attrs.DistributeEnable == LoopAttributes::Unspecified &&
#if INTEL_CUSTOMIZATION
      !Attrs.LoopCoalesceEnable &&
      Attrs.LoopCoalesceCount == 0 && Attrs.IICount == 0 &&
      Attrs.MaxConcurrencyCount == 0 && Attrs.IVDepCount == 0 &&
      !Attrs.IVDepEnable && !Attrs.IVDepHLSEnable &&
      !Attrs.IVDepHLSIntelEnable && !Attrs.IVDepLoop && !Attrs.IVDepBack &&
      !Attrs.NoFusionEnable &&
#endif // INTEL_CUSTOMIZATION
      !StartLoc && !EndLoc)
    return nullptr;

  SmallVector<Metadata *, 4> Args;
  // Reserve operand 0 for loop id self reference.
  auto TempNode = MDNode::getTemporary(Ctx, None);
  Args.push_back(TempNode.get());

  // If we have a valid start debug location for the loop, add it.
  if (StartLoc) {
    Args.push_back(StartLoc.getAsMDNode());

    // If we also have a valid end debug location for the loop, add it.
    if (EndLoc)
      Args.push_back(EndLoc.getAsMDNode());
  }

  // Setting vectorize.width
  if (Attrs.VectorizeWidth > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.vectorize.width"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.VectorizeWidth))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }

  // Setting interleave.count
  if (Attrs.InterleaveCount > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.interleave.count"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.InterleaveCount))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }

#if INTEL_CUSTOMIZATION
  // Setting II count
  if (Attrs.IICount > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.ii.count"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.IICount))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting max_concurrency count
  if (Attrs.MaxConcurrencyCount > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.max_concurrency.count"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.MaxConcurrencyCount))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting loop_coalesce count
  if (Attrs.LoopCoalesceCount > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.coalesce.count"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.LoopCoalesceCount))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting loop_coalesce
  if (Attrs.LoopCoalesceEnable) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.coalesce.enable")};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting ivdep safelen count
  if (Attrs.IVDepCount > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.ivdep.safelen"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.IVDepCount))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting ivdep
  if (Attrs.IVDepHLSEnable || Attrs.IVDepHLSIntelEnable) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.ivdep.enable")};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  if (Attrs.IVDepEnable ||  Attrs.IVDepHLSIntelEnable) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.vectorize.ivdep_back")};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting nofusion
  if (Attrs.NoFusionEnable) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.nofusion.enable")};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  if (Attrs.IVDepLoop) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.vectorize.ivdep_loop")};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  if (Attrs.IVDepBack) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.vectorize.ivdep_back")};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
#endif // INTEL_CUSTOMIZATION

  // Setting interleave.count
  if (Attrs.UnrollCount > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.unroll.count"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.UnrollCount))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }

  // Setting vectorize.enable
  if (Attrs.VectorizeEnable != LoopAttributes::Unspecified) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.vectorize.enable"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt1Ty(Ctx), (Attrs.VectorizeEnable ==
                                                   LoopAttributes::Enable)))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }

  // Setting unroll.full or unroll.disable
  if (Attrs.UnrollEnable != LoopAttributes::Unspecified) {
    std::string Name;
    if (Attrs.UnrollEnable == LoopAttributes::Enable)
      Name = "llvm.loop.unroll.enable";
    else if (Attrs.UnrollEnable == LoopAttributes::Full)
      Name = "llvm.loop.unroll.full";
    else
      Name = "llvm.loop.unroll.disable";
    Metadata *Vals[] = {MDString::get(Ctx, Name)};
    Args.push_back(MDNode::get(Ctx, Vals));
  }

  if (Attrs.DistributeEnable != LoopAttributes::Unspecified) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.distribute.enable"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt1Ty(Ctx), (Attrs.DistributeEnable ==
                                                   LoopAttributes::Enable)))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }

  // Set the first operand to itself.
  MDNode *LoopID = MDNode::get(Ctx, Args);
  LoopID->replaceOperandWith(0, LoopID);
  return LoopID;
}

LoopAttributes::LoopAttributes(bool IsParallel)
#if INTEL_CUSTOMIZATION
    : IsParallel(IsParallel), LoopCoalesceEnable(false),
      LoopCoalesceCount(0), IICount(0), MaxConcurrencyCount(0),
      IVDepEnable(false), IVDepHLSEnable(false), IVDepHLSIntelEnable(false),
      IVDepCount(0), NoFusionEnable(false), IVDepLoop(false), IVDepBack(false),
      VectorizeEnable(LoopAttributes::Unspecified),
#endif // INTEL_CUSTOMIZATION
      UnrollEnable(LoopAttributes::Unspecified), VectorizeWidth(0),
      InterleaveCount(0), UnrollCount(0),
      DistributeEnable(LoopAttributes::Unspecified) {}

void LoopAttributes::clear() {
  IsParallel = false;
#if INTEL_CUSTOMIZATION
  LoopCoalesceEnable = false;
  LoopCoalesceCount = 0;
  IICount = 0;
  MaxConcurrencyCount = 0;
  IVDepEnable = false;
  IVDepHLSEnable = false;
  IVDepHLSIntelEnable = false;
  IVDepCount = 0;
  NoFusionEnable = false;
  IVDepLoop = false;
  IVDepBack = false;
#endif // INTEL_CUSTOMIZATION
  VectorizeWidth = 0;
  InterleaveCount = 0;
  UnrollCount = 0;
  VectorizeEnable = LoopAttributes::Unspecified;
  UnrollEnable = LoopAttributes::Unspecified;
  DistributeEnable = LoopAttributes::Unspecified;
}

LoopInfo::LoopInfo(BasicBlock *Header, const LoopAttributes &Attrs,
                   const llvm::DebugLoc &StartLoc, const llvm::DebugLoc &EndLoc)
    : LoopID(nullptr), Header(Header), Attrs(Attrs) {
  LoopID = createMetadata(Header->getContext(), Attrs, StartLoc, EndLoc);
}

void LoopInfoStack::push(BasicBlock *Header, const llvm::DebugLoc &StartLoc,
                         const llvm::DebugLoc &EndLoc) {
  Active.push_back(LoopInfo(Header, StagedAttrs, StartLoc, EndLoc));
  // Clear the attributes so nested loops do not inherit them.
  StagedAttrs.clear();
}

void LoopInfoStack::push(BasicBlock *Header, clang::ASTContext &Ctx,
                         ArrayRef<const clang::Attr *> Attrs,
                         const llvm::DebugLoc &StartLoc,
                         const llvm::DebugLoc &EndLoc) {

  // Identify loop hint attributes from Attrs.
  for (const auto *Attr : Attrs) {
    const LoopHintAttr *LH = dyn_cast<LoopHintAttr>(Attr);
    const OpenCLUnrollHintAttr *OpenCLHint =
        dyn_cast<OpenCLUnrollHintAttr>(Attr);

    // Skip non loop hint attributes
    if (!LH && !OpenCLHint) {
      continue;
    }

    LoopHintAttr::OptionType Option = LoopHintAttr::Unroll;
    LoopHintAttr::LoopHintState State = LoopHintAttr::Disable;
    unsigned ValueInt = 1;
    // Translate opencl_unroll_hint attribute argument to
    // equivalent LoopHintAttr enums.
    // OpenCL v2.0 s6.11.5:  
    // 0 - full unroll (no argument).
    // 1 - disable unroll.
    // other positive integer n - unroll by n.
    if (OpenCLHint) {
      ValueInt = OpenCLHint->getUnrollHint();
      if (ValueInt == 0) {
        State = LoopHintAttr::Full;
      } else if (ValueInt != 1) {
        Option = LoopHintAttr::UnrollCount;
        State = LoopHintAttr::Numeric;
      }
    } else if (LH) {
      auto *ValueExpr = LH->getValue();
      if (ValueExpr) {
        llvm::APSInt ValueAPS = ValueExpr->EvaluateKnownConstInt(Ctx);
        ValueInt = ValueAPS.getSExtValue();
      }

      Option = LH->getOption();
      State = LH->getState();
    }
    switch (State) {
    case LoopHintAttr::Disable:
      switch (Option) {
      case LoopHintAttr::Vectorize:
        // Disable vectorization by specifying a width of 1.
        setVectorizeWidth(1);
        break;
      case LoopHintAttr::Interleave:
        // Disable interleaving by speciyfing a count of 1.
        setInterleaveCount(1);
        break;
      case LoopHintAttr::Unroll:
        setUnrollState(LoopAttributes::Disable);
        break;
      case LoopHintAttr::Distribute:
        setDistributeState(false);
        break;
      case LoopHintAttr::UnrollCount:
      case LoopHintAttr::VectorizeWidth:
      case LoopHintAttr::InterleaveCount:
#if INTEL_CUSTOMIZATION
      case LoopHintAttr::II:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLS:
      case LoopHintAttr::IVDepHLSIntel:
      case LoopHintAttr::LoopCoalesce:
      case LoopHintAttr::MaxConcurrency:
      case LoopHintAttr::NoFusion:
#endif // INTEL_CUSTOMIZATION
        llvm_unreachable("Options cannot be disabled.");
        break;
      }
      break;
    case LoopHintAttr::Enable:
      switch (Option) {
      case LoopHintAttr::Vectorize:
      case LoopHintAttr::Interleave:
        setVectorizeEnable(true);
        break;
      case LoopHintAttr::Unroll:
        setUnrollState(LoopAttributes::Enable);
        break;
      case LoopHintAttr::Distribute:
        setDistributeState(true);
        break;
      case LoopHintAttr::UnrollCount:
      case LoopHintAttr::VectorizeWidth:
      case LoopHintAttr::InterleaveCount:
        llvm_unreachable("Options cannot enabled.");
        break;
#if INTEL_CUSTOMIZATION
      case LoopHintAttr::II:
      case LoopHintAttr::MaxConcurrency:
        llvm_unreachable("Options cannot enabled.");
        break;
      case LoopHintAttr::IVDep:
        setIVDepEnable();
        break;
      case LoopHintAttr::IVDepLoop:
        setIVDepLoop();
        break;
      case LoopHintAttr::IVDepBack:
        setIVDepBack();
        break;
      case LoopHintAttr::IVDepHLS:
        setIVDepHLSEnable();
        break;
      case LoopHintAttr::IVDepHLSIntel:
        setIVDepHLSIntelEnable();
        break;
      case LoopHintAttr::LoopCoalesce:
        setLoopCoalesceEnable();
        break;
      case LoopHintAttr::NoFusion:
        setNoFusionEnable();
        break;
#endif // INTEL_CUSTOMIZATION
      }
      break;
    case LoopHintAttr::AssumeSafety:
      switch (Option) {
      case LoopHintAttr::Vectorize:
      case LoopHintAttr::Interleave:
        // Apply "llvm.mem.parallel_loop_access" metadata to load/stores.
        setParallel(true);
        setVectorizeEnable(true);
        break;
      case LoopHintAttr::Unroll:
      case LoopHintAttr::UnrollCount:
      case LoopHintAttr::VectorizeWidth:
      case LoopHintAttr::InterleaveCount:
      case LoopHintAttr::Distribute:
#if INTEL_CUSTOMIZATION
      case LoopHintAttr::II:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLS:
      case LoopHintAttr::IVDepHLSIntel:
      case LoopHintAttr::LoopCoalesce:
      case LoopHintAttr::MaxConcurrency:
      case LoopHintAttr::NoFusion:
#endif // INTEL_CUSTOMIZATION
        llvm_unreachable("Options cannot be used to assume mem safety.");
        break;
      }
      break;
    case LoopHintAttr::Full:
      switch (Option) {
      case LoopHintAttr::Unroll:
        setUnrollState(LoopAttributes::Full);
        break;
#if INTEL_CUSTOMIZATION
      case LoopHintAttr::IVDepHLS:
        // Handled with IntelIVDepArrayHandler.
        break;
      case LoopHintAttr::II:
      case LoopHintAttr::LoopCoalesce:
      case LoopHintAttr::MaxConcurrency:
      case LoopHintAttr::NoFusion:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLSIntel:
#endif // INTEL_CUSTOMIZATION
      case LoopHintAttr::Vectorize:
      case LoopHintAttr::Interleave:
      case LoopHintAttr::UnrollCount:
      case LoopHintAttr::VectorizeWidth:
      case LoopHintAttr::InterleaveCount:
      case LoopHintAttr::Distribute:
        llvm_unreachable("Options cannot be used with 'full' hint.");
        break;
      }
      break;
    case LoopHintAttr::Numeric:
      switch (Option) {
      case LoopHintAttr::VectorizeWidth:
        setVectorizeWidth(ValueInt);
        break;
      case LoopHintAttr::InterleaveCount:
        setInterleaveCount(ValueInt);
        break;
      case LoopHintAttr::UnrollCount:
        setUnrollCount(ValueInt);
        break;
#if INTEL_CUSTOMIZATION
      case LoopHintAttr::LoopCoalesce:
        setLoopCoalesceCount(ValueInt);
        break;
      case LoopHintAttr::II:
        setIICount(ValueInt);
        break;
      case LoopHintAttr::MaxConcurrency:
        setMaxConcurrencyCount(ValueInt);
        break;
      case LoopHintAttr::IVDepHLS:
        setIVDepCount(ValueInt);
        break;
      case LoopHintAttr::NoFusion:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLSIntel:
#endif // INTEL_CUSTOMIZATION
      case LoopHintAttr::Unroll:
      case LoopHintAttr::Vectorize:
      case LoopHintAttr::Interleave:
      case LoopHintAttr::Distribute:
        llvm_unreachable("Options cannot be assigned a value.");
        break;
      }
      break;
#if INTEL_CUSTOMIZATION
    case LoopHintAttr::LoopExpr:
      switch (Option) {
      case LoopHintAttr::IVDepHLS:
        // Handled with IntelIVDepArrayHandler.
        break;
      case LoopHintAttr::VectorizeWidth:
      case LoopHintAttr::InterleaveCount:
      case LoopHintAttr::UnrollCount:
      case LoopHintAttr::II:
      case LoopHintAttr::LoopCoalesce:
      case LoopHintAttr::MaxConcurrency:
      case LoopHintAttr::Unroll:
      case LoopHintAttr::Vectorize:
      case LoopHintAttr::Interleave:
      case LoopHintAttr::Distribute:
      case LoopHintAttr::NoFusion:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLSIntel:
        llvm_unreachable("Options cannot be assigned a loopexpr value.");
        break;
      }
      break;
#endif // INTEL_CUSTOMIZATION
    }
  }

  /// Stage the attributes.
  push(Header, StartLoc, EndLoc);
}

void LoopInfoStack::pop() {
  assert(!Active.empty() && "No active loops to pop");
  Active.pop_back();
}

void LoopInfoStack::InsertHelper(Instruction *I) const {
  if (!hasInfo())
    return;

  const LoopInfo &L = getInfo();
  if (!L.getLoopID())
    return;

  if (TerminatorInst *TI = dyn_cast<TerminatorInst>(I)) {
    for (unsigned i = 0, ie = TI->getNumSuccessors(); i < ie; ++i)
      if (TI->getSuccessor(i) == L.getHeader()) {
        TI->setMetadata(llvm::LLVMContext::MD_loop, L.getLoopID());
        break;
      }
    return;
  }

  if (L.getAttributes().IsParallel && I->mayReadOrWriteMemory())
    I->setMetadata("llvm.mem.parallel_loop_access", L.getLoopID());
}
#if INTEL_CUSTOMIZATION
LoopInfo::LoopInfo(llvm::MDNode *LoopID, const LoopAttributes &Attrs)
  : LoopID(LoopID), Header(0), Attrs(Attrs) { }

void LoopInfoStack::push(llvm::MDNode *LoopID, bool IsParallel) {
  assert(Active.empty() && "cannot have an active loop");
  Active.push_back(LoopInfo(LoopID, LoopAttributes(IsParallel)));
  StagedAttrs.clear();
}
#endif  // INTEL_CUSTOMIZATION
