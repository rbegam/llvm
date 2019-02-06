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
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Metadata.h"
using namespace clang::CodeGen;
using namespace llvm;

static MDNode *createMetadata(LLVMContext &Ctx, const LoopAttributes &Attrs,
                              const llvm::DebugLoc &StartLoc,
                              const llvm::DebugLoc &EndLoc, MDNode *&AccGroup) {

  if (!Attrs.IsParallel && Attrs.VectorizeWidth == 0 &&
#if INTEL_CUSTOMIZATION
      !Attrs.LoopCoalesceEnable &&
      Attrs.LoopCoalesceCount == 0 && Attrs.IICount == 0 &&
      Attrs.MaxConcurrencyCount == 0 && Attrs.IVDepCount == 0 &&
      Attrs.IIAtMost == 0 && Attrs.IIAtLeast == 0 &&
      Attrs.SpeculatedIterations == -1 &&
      !Attrs.MinIIAtTargetFmaxEnable && !Attrs.DisableLoopPipeliningEnable &&
      Attrs.ForceHyperoptEnable == LoopAttributes::Unspecified &&
      !Attrs.IVDepEnable && !Attrs.IVDepHLSEnable &&
      !Attrs.IVDepHLSIntelEnable && !Attrs.IVDepLoop && !Attrs.IVDepBack &&
      Attrs.FusionEnable == LoopAttributes::Unspecified &&
      !Attrs.VectorizeAlwaysEnable && Attrs.LoopCount.size() == 0 &&
      Attrs.LoopCountMin == 0 && Attrs.LoopCountMax == 0 &&
      Attrs.LoopCountAvg == 0 &&
#endif // INTEL_CUSTOMIZATION
      Attrs.InterleaveCount == 0 && Attrs.UnrollCount == 0 &&
      Attrs.UnrollAndJamCount == 0 && !Attrs.PipelineDisabled &&
      Attrs.PipelineInitiationInterval == 0 &&
      Attrs.VectorizeEnable == LoopAttributes::Unspecified &&
      Attrs.UnrollEnable == LoopAttributes::Unspecified &&
      Attrs.UnrollAndJamEnable == LoopAttributes::Unspecified &&
      Attrs.DistributeEnable == LoopAttributes::Unspecified && !StartLoc &&
      !EndLoc)
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
  if (Attrs.IIAtMost > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.intel.ii.at.most.count"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.IIAtMost))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  if (Attrs.IIAtLeast > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.intel.ii.at.least.count"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.IIAtLeast))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  if (Attrs.SpeculatedIterations >= 0) {
    Metadata *Vals[] = {
        MDString::get(Ctx, "llvm.loop.intel.speculated.iterations.count"),
        ConstantAsMetadata::get(ConstantInt::get(Type::getInt32Ty(Ctx),
                                                 Attrs.SpeculatedIterations))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  if (Attrs.MinIIAtTargetFmaxEnable) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.intel.min.ii.at.target.fmax")};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  if (Attrs.DisableLoopPipeliningEnable) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.intel.pipelining.disable")};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  if (Attrs.ForceHyperoptEnable != LoopAttributes::Unspecified) {
    Metadata *Vals[] = {
        MDString::get(Ctx, Attrs.ForceHyperoptEnable == LoopAttributes::Enable
                               ? "llvm.loop.intel.hyperopt"
                               : "llvm.loop.intel.nohyperopt")};
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
  // Setting fusion.enable or fusion.disable
  if (Attrs.FusionEnable != LoopAttributes::Unspecified) {
    Metadata *Vals[] = {
        MDString::get(Ctx, Attrs.FusionEnable == LoopAttributes::Enable
                               ? "llvm.loop.fusion.enable"
                               : "llvm.loop.fusion.disable")};
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
  // Setting vector always
  if (Attrs.VectorizeAlwaysEnable) {
    Metadata *Vals[] = {MDString::get(Ctx,
                                  "llvm.loop.vectorize.ignore_profitability")};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting loop_count count
  if (Attrs.LoopCount.size() > 0) {
    llvm::SmallVector<llvm::Metadata *, 4> Vals;
    Vals.push_back(MDString::get(Ctx, "llvm.loop.intel.loopcount"));
    for (auto LoopCount : Attrs.LoopCount) {
      Vals.push_back(ConstantAsMetadata::get(
          ConstantInt::get(Type::getInt32Ty(Ctx), LoopCount)));
    }
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting loop_count min
  if (Attrs.LoopCountMin > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.intel.loopcount_minimum"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.LoopCountMin))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting loop_count max
  if (Attrs.LoopCountMax > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.intel.loopcount_maximum"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.LoopCountMax))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
  // Setting loop_count avg
  if (Attrs.LoopCountAvg > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.intel.loopcount_average"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.LoopCountAvg))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }
#endif // INTEL_CUSTOMIZATION

  // Setting unroll.count
  if (Attrs.UnrollCount > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.unroll.count"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.UnrollCount))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }

  // Setting unroll_and_jam.count
  if (Attrs.UnrollAndJamCount > 0) {
    Metadata *Vals[] = {MDString::get(Ctx, "llvm.loop.unroll_and_jam.count"),
                        ConstantAsMetadata::get(ConstantInt::get(
                            Type::getInt32Ty(Ctx), Attrs.UnrollAndJamCount))};
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

  // Setting unroll_and_jam.full or unroll_and_jam.disable
  if (Attrs.UnrollAndJamEnable != LoopAttributes::Unspecified) {
    std::string Name;
    if (Attrs.UnrollAndJamEnable == LoopAttributes::Enable)
      Name = "llvm.loop.unroll_and_jam.enable";
    else if (Attrs.UnrollAndJamEnable == LoopAttributes::Full)
      Name = "llvm.loop.unroll_and_jam.full";
    else
      Name = "llvm.loop.unroll_and_jam.disable";
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

  if (Attrs.IsParallel) {
    AccGroup = MDNode::getDistinct(Ctx, {});
    Args.push_back(MDNode::get(
        Ctx, {MDString::get(Ctx, "llvm.loop.parallel_accesses"), AccGroup}));
  }

  if (Attrs.PipelineDisabled) {
    Metadata *Vals[] = {
        MDString::get(Ctx, "llvm.loop.pipeline.disable"),
        ConstantAsMetadata::get(ConstantInt::get(
            Type::getInt1Ty(Ctx), (Attrs.PipelineDisabled == true)))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }

  if (Attrs.PipelineInitiationInterval > 0) {
    Metadata *Vals[] = {
        MDString::get(Ctx, "llvm.loop.pipeline.initiationinterval"),
        ConstantAsMetadata::get(ConstantInt::get(
            Type::getInt32Ty(Ctx), Attrs.PipelineInitiationInterval))};
    Args.push_back(MDNode::get(Ctx, Vals));
  }

  // Set the first operand to itself.
  MDNode *LoopID = MDNode::get(Ctx, Args);
  LoopID->replaceOperandWith(0, LoopID);
  return LoopID;
}

LoopAttributes::LoopAttributes(bool IsParallel)
    : IsParallel(IsParallel), VectorizeEnable(LoopAttributes::Unspecified),
#if INTEL_CUSTOMIZATION
      LoopCoalesceEnable(false), LoopCoalesceCount(0), IICount(0),
      MaxConcurrencyCount(0), IVDepEnable(false), IVDepHLSEnable(false),
      IVDepHLSIntelEnable(false), IVDepCount(0),
      IIAtMost(0), IIAtLeast(0), SpeculatedIterations(-1),
      MinIIAtTargetFmaxEnable(false), DisableLoopPipeliningEnable(false),
      ForceHyperoptEnable(LoopAttributes::Unspecified),
      FusionEnable(LoopAttributes::Unspecified), IVDepLoop(false),
      IVDepBack(false), VectorizeAlwaysEnable(false),
      LoopCountMin(0), LoopCountMax(0), LoopCountAvg(0),
#endif // INTEL_CUSTOMIZATION
      UnrollEnable(LoopAttributes::Unspecified),
      UnrollAndJamEnable(LoopAttributes::Unspecified), VectorizeWidth(0),
      InterleaveCount(0), UnrollCount(0), UnrollAndJamCount(0),
      DistributeEnable(LoopAttributes::Unspecified), PipelineDisabled(false),
      PipelineInitiationInterval(0) {}

void LoopAttributes::clear() {
  IsParallel = false;
#if INTEL_CUSTOMIZATION
  LoopCoalesceEnable = false;
  LoopCoalesceCount = 0;
  IICount = 0;
  IIAtMost = 0;
  IIAtLeast = 0;
  SpeculatedIterations = -1;
  MinIIAtTargetFmaxEnable = false;
  DisableLoopPipeliningEnable = false;
  MaxConcurrencyCount = 0;
  IVDepEnable = false;
  IVDepHLSEnable = false;
  IVDepHLSIntelEnable = false;
  IVDepCount = 0;
  ForceHyperoptEnable = LoopAttributes::Unspecified;
  FusionEnable = LoopAttributes::Unspecified;
  IVDepLoop = false;
  IVDepBack = false;
  VectorizeAlwaysEnable = false;
  LoopCount.clear();
  LoopCountMin = 0;
  LoopCountMax = 0;
  LoopCountAvg = 0;
#endif // INTEL_CUSTOMIZATION
  VectorizeWidth = 0;
  InterleaveCount = 0;
  UnrollCount = 0;
  UnrollAndJamCount = 0;
  VectorizeEnable = LoopAttributes::Unspecified;
  UnrollEnable = LoopAttributes::Unspecified;
  UnrollAndJamEnable = LoopAttributes::Unspecified;
  DistributeEnable = LoopAttributes::Unspecified;
  PipelineDisabled = false;
  PipelineInitiationInterval = 0;
}

LoopInfo::LoopInfo(BasicBlock *Header, const LoopAttributes &Attrs,
                   const llvm::DebugLoc &StartLoc, const llvm::DebugLoc &EndLoc)
    : LoopID(nullptr), Header(Header), Attrs(Attrs) {
  LoopID =
      createMetadata(Header->getContext(), Attrs, StartLoc, EndLoc, AccGroup);
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
      case LoopHintAttr::UnrollAndJam:
        setUnrollAndJamState(LoopAttributes::Disable);
        break;
      case LoopHintAttr::Distribute:
        setDistributeState(false);
        break;
      case LoopHintAttr::PipelineDisabled:
        setPipelineDisabled(true);
        break;
#if INTEL_CUSTOMIZATION
      case LoopHintAttr::Fusion:
        setFusionEnable(false);
        break;
      case LoopHintAttr::ForceHyperopt:
        setForceHyperoptEnable(false);
        break;
      case LoopHintAttr::II:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLS:
      case LoopHintAttr::IVDepHLSIntel:
      case LoopHintAttr::LoopCoalesce:
      case LoopHintAttr::MaxConcurrency:
      case LoopHintAttr::IIAtMost:
      case LoopHintAttr::IIAtLeast:
      case LoopHintAttr::MinIIAtFmax:
      case LoopHintAttr::SpeculatedIterations:
      case LoopHintAttr::DisableLoopPipelining:
      case LoopHintAttr::VectorizeAlways:
      case LoopHintAttr::LoopCount:
      case LoopHintAttr::LoopCountMax:
      case LoopHintAttr::LoopCountMin:
      case LoopHintAttr::LoopCountAvg:
#endif // INTEL_CUSTOMIZATION
      case LoopHintAttr::UnrollCount:
      case LoopHintAttr::UnrollAndJamCount:
      case LoopHintAttr::VectorizeWidth:
      case LoopHintAttr::InterleaveCount:
      case LoopHintAttr::PipelineInitiationInterval:
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
      case LoopHintAttr::UnrollAndJam:
        setUnrollAndJamState(LoopAttributes::Enable);
        break;
      case LoopHintAttr::Distribute:
        setDistributeState(true);
        break;
#if INTEL_CUSTOMIZATION
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
      case LoopHintAttr::MinIIAtFmax:
        setMinIIAtTargetFmaxEnable();
        break;
      case LoopHintAttr::DisableLoopPipelining:
        setDisableLoopPipeliningEnable();
        break;
      case LoopHintAttr::ForceHyperopt:
        setForceHyperoptEnable(true);
        break;
      case LoopHintAttr::Fusion:
        setFusionEnable(true);
        break;
      case LoopHintAttr::VectorizeAlways:
        setVectorizeAlwaysEnable();
        break;
      case LoopHintAttr::II:
      case LoopHintAttr::MaxConcurrency:
      case LoopHintAttr::IIAtMost:
      case LoopHintAttr::IIAtLeast:
      case LoopHintAttr::SpeculatedIterations:
      case LoopHintAttr::LoopCount:
      case LoopHintAttr::LoopCountMin:
      case LoopHintAttr::LoopCountMax:
      case LoopHintAttr::LoopCountAvg:
#endif // INTEL_CUSTOMIZATION
      case LoopHintAttr::UnrollCount:
      case LoopHintAttr::UnrollAndJamCount:
      case LoopHintAttr::VectorizeWidth:
      case LoopHintAttr::InterleaveCount:
      case LoopHintAttr::PipelineDisabled:
      case LoopHintAttr::PipelineInitiationInterval:
        llvm_unreachable("Options cannot enabled.");
        break;
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
#if INTEL_CUSTOMIZATION
      case LoopHintAttr::II:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLS:
      case LoopHintAttr::IVDepHLSIntel:
      case LoopHintAttr::LoopCoalesce:
      case LoopHintAttr::MaxConcurrency:
      case LoopHintAttr::IIAtMost:
      case LoopHintAttr::IIAtLeast:
      case LoopHintAttr::MinIIAtFmax:
      case LoopHintAttr::SpeculatedIterations:
      case LoopHintAttr::DisableLoopPipelining:
      case LoopHintAttr::ForceHyperopt:
      case LoopHintAttr::Fusion:
      case LoopHintAttr::VectorizeAlways:
      case LoopHintAttr::LoopCount:
      case LoopHintAttr::LoopCountMin:
      case LoopHintAttr::LoopCountMax:
      case LoopHintAttr::LoopCountAvg:
#endif // INTEL_CUSTOMIZATION
      case LoopHintAttr::Unroll:
      case LoopHintAttr::UnrollAndJam:
      case LoopHintAttr::UnrollCount:
      case LoopHintAttr::UnrollAndJamCount:
      case LoopHintAttr::VectorizeWidth:
      case LoopHintAttr::InterleaveCount:
      case LoopHintAttr::Distribute:
      case LoopHintAttr::PipelineDisabled:
      case LoopHintAttr::PipelineInitiationInterval:
        llvm_unreachable("Options cannot be used to assume mem safety.");
        break;
      }
      break;
    case LoopHintAttr::Full:
      switch (Option) {
      case LoopHintAttr::Unroll:
        setUnrollState(LoopAttributes::Full);
        break;
      case LoopHintAttr::UnrollAndJam:
        setUnrollAndJamState(LoopAttributes::Full);
        break;
#if INTEL_CUSTOMIZATION
      case LoopHintAttr::IVDepHLS:
        // Handled with IntelIVDepArrayHandler.
        break;
      case LoopHintAttr::II:
      case LoopHintAttr::LoopCoalesce:
      case LoopHintAttr::MaxConcurrency:
      case LoopHintAttr::IIAtMost:
      case LoopHintAttr::IIAtLeast:
      case LoopHintAttr::MinIIAtFmax:
      case LoopHintAttr::SpeculatedIterations:
      case LoopHintAttr::DisableLoopPipelining:
      case LoopHintAttr::ForceHyperopt:
      case LoopHintAttr::Fusion:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLSIntel:
      case LoopHintAttr::VectorizeAlways:
      case LoopHintAttr::LoopCount:
      case LoopHintAttr::LoopCountMin:
      case LoopHintAttr::LoopCountMax:
      case LoopHintAttr::LoopCountAvg:
#endif // INTEL_CUSTOMIZATION
      case LoopHintAttr::Vectorize:
      case LoopHintAttr::Interleave:
      case LoopHintAttr::UnrollCount:
      case LoopHintAttr::UnrollAndJamCount:
      case LoopHintAttr::VectorizeWidth:
      case LoopHintAttr::InterleaveCount:
      case LoopHintAttr::Distribute:
      case LoopHintAttr::PipelineDisabled:
      case LoopHintAttr::PipelineInitiationInterval:
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
      case LoopHintAttr::UnrollAndJamCount:
        setUnrollAndJamCount(ValueInt);
        break;
      case LoopHintAttr::PipelineInitiationInterval:
        setPipelineInitiationInterval(ValueInt);
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
      case LoopHintAttr::IIAtMost:
        setIIAtMost(ValueInt);
        break;
      case LoopHintAttr::IIAtLeast:
        setIIAtLeast(ValueInt);
        break;
      case LoopHintAttr::SpeculatedIterations:
        setSpeculatedIterations(ValueInt);
        break;
      case LoopHintAttr::IVDepHLS:
        setIVDepCount(ValueInt);
        break;
      case LoopHintAttr::LoopCount:
        setLoopCount(ValueInt);
        break;
      case LoopHintAttr::LoopCountMin:
        setLoopCountMin(ValueInt);
        break;
      case LoopHintAttr::LoopCountMax:
        setLoopCountMax(ValueInt);
        break;
      case LoopHintAttr::LoopCountAvg:
        setLoopCountAvg(ValueInt);
        break;
      case LoopHintAttr::MinIIAtFmax:
      case LoopHintAttr::DisableLoopPipelining:
      case LoopHintAttr::ForceHyperopt:
      case LoopHintAttr::Fusion:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLSIntel:
      case LoopHintAttr::VectorizeAlways:
#endif // INTEL_CUSTOMIZATION
      case LoopHintAttr::Unroll:
      case LoopHintAttr::UnrollAndJam:
      case LoopHintAttr::Vectorize:
      case LoopHintAttr::Interleave:
      case LoopHintAttr::Distribute:
      case LoopHintAttr::PipelineDisabled:
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
      case LoopHintAttr::IIAtMost:
      case LoopHintAttr::IIAtLeast:
      case LoopHintAttr::MinIIAtFmax:
      case LoopHintAttr::SpeculatedIterations:
      case LoopHintAttr::DisableLoopPipelining:
      case LoopHintAttr::ForceHyperopt:
      case LoopHintAttr::Unroll:
      case LoopHintAttr::Vectorize:
      case LoopHintAttr::Interleave:
      case LoopHintAttr::Distribute:
      case LoopHintAttr::Fusion:
      case LoopHintAttr::UnrollAndJam:
      case LoopHintAttr::UnrollAndJamCount:
      case LoopHintAttr::IVDep:
      case LoopHintAttr::IVDepLoop:
      case LoopHintAttr::IVDepBack:
      case LoopHintAttr::IVDepHLSIntel:
      case LoopHintAttr::VectorizeAlways:
      case LoopHintAttr::LoopCount:
      case LoopHintAttr::LoopCountMax:
      case LoopHintAttr::LoopCountMin:
      case LoopHintAttr::LoopCountAvg:
      case LoopHintAttr::PipelineInitiationInterval:
      case LoopHintAttr::PipelineDisabled:
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
  if (I->mayReadOrWriteMemory()) {
    SmallVector<Metadata *, 4> AccessGroups;
    for (const LoopInfo &AL : Active) {
      // Here we assume that every loop that has an access group is parallel.
      if (MDNode *Group = AL.getAccessGroup())
        AccessGroups.push_back(Group);
    }
    MDNode *UnionMD = nullptr;
    if (AccessGroups.size() == 1)
      UnionMD = cast<MDNode>(AccessGroups[0]);
    else if (AccessGroups.size() >= 2)
      UnionMD = MDNode::get(I->getContext(), AccessGroups);
    I->setMetadata("llvm.access.group", UnionMD);
  }

  if (!hasInfo())
    return;

  const LoopInfo &L = getInfo();
  if (!L.getLoopID())
    return;

  if (I->isTerminator()) {
    for (BasicBlock *Succ : successors(I))
      if (Succ == L.getHeader()) {
        I->setMetadata(llvm::LLVMContext::MD_loop, L.getLoopID());
        break;
      }
    return;
  }
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
