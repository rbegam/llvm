//==-- VPOParoptUtils.cpp - Utilities for VPO Paropt Transforms -*- C++ -*--==//
//
// Copyright (C) 2015-2016 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive property
// of Intel Corporation and may not be disclosed, examined or reproduced in
// whole or in part without explicit written authorization from the company.
//
// Authors:
// --------
// Xinmin Tian (xinmin.tian@intel.com)
//
// Major Revisions:
// ----------------
// Nov 2015: Initial Implementation of OpenMP runtime APIs (Xinmin Tian)
//
//==------------------------------------------------------------------------==//
///
/// \file
/// This file provides a set of utilities for VPO Paropt Transformations to
/// generate OpenMP runtime API call instructions.
///
//==------------------------------------------------------------------------==//

#include "llvm/ADT/StringRef.h"
#include "llvm/IR/Module.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Intrinsics.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Metadata.h"
#include "llvm/IR/DebugLoc.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Transforms/Intel_VPO/Utils/VPOUtils.h"
#include "llvm/Transforms/Intel_VPO/Paropt/VPOParoptUtils.h"
#include <string>

#define DEBUG_TYPE "VPOParoptUtils"

using namespace llvm;
using namespace llvm::vpo;

static const unsigned StackAdjustedAlignment = 16;

// This function generates a runtime library call to __kmpc_begin(&loc, 0)
CallInst *VPOParoptUtils::genKmpcBeginCall(Function *F, Instruction *AI,
                                           StructType *IdentTy) {
  Module *M = F->getParent();
  LLVMContext &C = F->getContext();

  BasicBlock &B = F->getEntryBlock();
  BasicBlock &E = B;

  int Flags = KMP_IDENT_KMPC;

  GlobalVariable *KmpcLoc =
      genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, &B, &E);

  ConstantInt *ValueZero = ConstantInt::get(Type::getInt32Ty(C), 0);

  Constant *FnC = M->getOrInsertFunction("__kmpc_begin", Type::getVoidTy(C),
                                         PointerType::getUnqual(IdentTy),
                                         Type::getInt32Ty(C));

  Function *FnKmpcBegin = cast<Function>(FnC);

  FnKmpcBegin->setCallingConv(CallingConv::C);

  std::vector<Value *> FnKmpcBeginArgs;
  FnKmpcBeginArgs.push_back(KmpcLoc);
  FnKmpcBeginArgs.push_back(ValueZero);

  CallInst *KmpcBeginCall = CallInst::Create(FnKmpcBegin, FnKmpcBeginArgs);
  KmpcBeginCall->setCallingConv(CallingConv::C);

  return KmpcBeginCall;
}

// This function generates a runtime library call to __kmpc_end(&loc)
CallInst *VPOParoptUtils::genKmpcEndCall(Function *F, Instruction *AI,
                                         StructType *IdentTy) {
  Module *M = F->getParent();
  LLVMContext &C = F->getContext();

  BasicBlock &B = F->getEntryBlock();
  BasicBlock &E = B;

  int Flags = KMP_IDENT_KMPC;

  GlobalVariable *KmpcLoc =
      genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, &B, &E);

  Constant *FnC = M->getOrInsertFunction("__kmpc_end", Type::getVoidTy(C),
                                         PointerType::getUnqual(IdentTy));

  Function *FnKmpcEnd = cast<Function>(FnC);

  FnKmpcEnd->setCallingConv(CallingConv::C);

  std::vector<Value *> FnKmpcEndArgs;
  FnKmpcEndArgs.push_back(KmpcLoc);

  CallInst *KmpcEndCall = CallInst::Create(FnKmpcEnd, FnKmpcEndArgs);
  KmpcEndCall->setCallingConv(CallingConv::C);

  return KmpcEndCall;
}


// This function generates a runtime library call to __kmpc_ok_to_fork(&loc)
CallInst *VPOParoptUtils::genKmpcForkTest(WRegionNode *W, StructType *IdentTy, 
                                          Instruction *InsertPt) {
  BasicBlock *B = W->getEntryBBlock();
  BasicBlock *E = W->getExitBBlock();

  Function *F = B->getParent();

  Module *M = F->getParent();
  LLVMContext &C = F->getContext();

  int Flags = KMP_IDENT_KMPC;

  GlobalVariable *Loc =
      genKmpcLocfromDebugLoc(F, InsertPt, IdentTy, Flags, B, E);

  FunctionType *FnForkTestTy = FunctionType::get(
      Type::getInt32Ty(C), PointerType::getUnqual(IdentTy), false);

  Function *FnForkTest = M->getFunction("__kmpc_ok_to_fork");

  if (!FnForkTest) {
    FnForkTest = Function::Create(FnForkTestTy, GlobalValue::ExternalLinkage,
                                  "__kmpc_ok_to_fork", M);
    FnForkTest->setCallingConv(CallingConv::C);
  }

  std::vector<Value *> FnForkTestArgs;
  FnForkTestArgs.push_back(Loc);

  CallInst *ForkTestCall = CallInst::Create(FnForkTest, FnForkTestArgs, 
                                            "fork.test", InsertPt);
  ForkTestCall->setCallingConv(CallingConv::C);
  ForkTestCall->setTailCall(true);

  return ForkTestCall;
}


// This function generates a call to notify the runtime system that the static 
// loop scheduling is started
//
//   call void @__kmpc_for_static_init_4(%ident_t* %loc, i32 %tid, 
//               i32 schedtype, i32* %islast,i32* %lb, i32* %ub, i32* %st, 
//               i32 inc, i32 chunk)
CallInst *VPOParoptUtils::genKmpcStaticInit(WRegionNode *W,
                                            StructType *IdentTy,
                                            Value *Tid, Value *SchedType,
                                            Value *IsLastVal, Value *LB,
                                            Value *UB, Value *ST,
                                            Value *Inc, Value *Chunk,
                                            Instruction *InsertPt) {
  BasicBlock *B = W->getEntryBBlock();
  BasicBlock *E = W->getExitBBlock();

  Function *F = B->getParent();
  Module   *M = F->getParent();

  LLVMContext &C = F->getContext();

  Type *IntTy = Type::getInt32Ty(C);

  int Flags = KMP_IDENT_KMPC;
  GlobalVariable *Loc =
      genKmpcLocfromDebugLoc(F, InsertPt, IdentTy, Flags, B, E);

  DEBUG(dbgs() << "\n---- Loop Source Location Info: " << *Loc << "\n\n");

  Type *InitParamsTy[] = {PointerType::getUnqual(IdentTy), 
                          IntTy, IntTy, PointerType::getUnqual(IntTy),
                          PointerType::getUnqual(IntTy),
                          PointerType::getUnqual(IntTy),
                          PointerType::getUnqual(IntTy), IntTy, IntTy};

  FunctionType *FnTy = FunctionType::get(Type::getVoidTy(C), 
                                         InitParamsTy, false);

  Function *FnStaticInit = M->getFunction("__kmpc_for_static_init_4");

  if (!FnStaticInit) {
    FnStaticInit = Function::Create(FnTy, GlobalValue::ExternalLinkage,
                                  "__kmpc_for_static_init_4", M);
    FnStaticInit->setCallingConv(CallingConv::C);
  }

  std::vector<Value *> FnStaticInitArgs;

  FnStaticInitArgs.push_back(Loc);
  FnStaticInitArgs.push_back(Tid);
  FnStaticInitArgs.push_back(SchedType);
  FnStaticInitArgs.push_back(IsLastVal);
  FnStaticInitArgs.push_back(LB);
  FnStaticInitArgs.push_back(UB);
  FnStaticInitArgs.push_back(ST);
  FnStaticInitArgs.push_back(Inc);
  FnStaticInitArgs.push_back(Chunk);

  CallInst *StaticInitCall = CallInst::Create(FnStaticInit, 
                                              FnStaticInitArgs, "", InsertPt);
  StaticInitCall->setCallingConv(CallingConv::C);
  StaticInitCall->setTailCall(false);

  return StaticInitCall;
}

// This function generates a call to notify the runtime system that the static 
// loop scheduling is done 
//   call void @__kmpc_for_static_fini(%ident_t* %loc, i32 %tid)
CallInst *VPOParoptUtils::genKmpcStaticFini(WRegionNode *W, 
                                            StructType *IdentTy, 
                                            Value *Tid, 
                                            Instruction *InsertPt) {
  BasicBlock *B = W->getEntryBBlock();
  BasicBlock *E = W->getExitBBlock();

  Function *F = B->getParent();
  Module *M = F->getParent();
  LLVMContext &C = F->getContext();

  int Flags = KMP_IDENT_KMPC;

  Type *IntTy = Type::getInt32Ty(C);

  GlobalVariable *Loc =
      genKmpcLocfromDebugLoc(F, InsertPt, IdentTy, Flags, B, E);
  DEBUG(dbgs() << "\n---- Loop Source Location Info: " << *Loc << "\n\n");

  Type *InitParamsTy[] = {PointerType::getUnqual(IdentTy), IntTy};

  FunctionType *FnTy = FunctionType::get(Type::getVoidTy(C),
                                         InitParamsTy, false);

  Function *FnStaticFini = M->getFunction("__kmpc_for_static_fini");

  if (!FnStaticFini) {
    FnStaticFini = Function::Create(FnTy, GlobalValue::ExternalLinkage,
                                  "__kmpc_for_static_fini", M);
    FnStaticFini->setCallingConv(CallingConv::C);
  }

  std::vector<Value *> FnStaticFiniArgs;

  FnStaticFiniArgs.push_back(Loc);
  FnStaticFiniArgs.push_back(Tid);

  CallInst *StaticFiniCall = CallInst::Create(FnStaticFini,
                                              FnStaticFiniArgs, "", InsertPt);
  StaticFiniCall->setCallingConv(CallingConv::C);
  StaticFiniCall->setTailCall(false);

  return StaticFiniCall;
}

// This function generates OpenMP runtime __kmpc_threadprivate_cached call.
CallInst *VPOParoptUtils::genKmpcThreadPrivateCachedCall(
                   Function *F,
                   Instruction *AI,
                   StructType *IdentTy,
                   Value *Tid,
                   Value *GV,
                   Value *GVSize,
                   Value *TpvGV) {
  Module *M = F->getParent();
  LLVMContext &C = F->getContext();

  BasicBlock &B = F->getEntryBlock();
  BasicBlock &E = B;

  int Flags = KMP_IDENT_KMPC;

  GlobalVariable *KmpcLoc =
      genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, &B, &E);

  SmallVector<Value *, 6> FnGetTpvArgs;
  FnGetTpvArgs.push_back(KmpcLoc);
  FnGetTpvArgs.push_back(Tid);
  FnGetTpvArgs.push_back(GV);
  FnGetTpvArgs.push_back(GVSize);
  FnGetTpvArgs.push_back(TpvGV);

  auto ReturnTy = Type::getInt8PtrTy(C);
  return genCall(M, "__kmpc_threadprivate_cached", ReturnTy, FnGetTpvArgs);
}

// This function generates a runtime library call to get global OpenMP thread
// ID - __kmpc_global_thread_num(&loc)
CallInst *VPOParoptUtils::genKmpcGlobalThreadNumCall(Function *F,
                                                     Instruction *AI,
                                                     StructType *IdentTy) {
  Module *M = F->getParent();
  LLVMContext &C = F->getContext();

  BasicBlock &B = F->getEntryBlock();
  BasicBlock &E = B;

  int Flags = KMP_IDENT_KMPC;

  GlobalVariable *KmpcLoc =
      genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, &B, &E);

  FunctionType *FnGetTidTy = FunctionType::get(
      Type::getInt32Ty(C), PointerType::getUnqual(IdentTy), false);

  Function *FnGetTid = M->getFunction("__kmpc_global_thread_num");

  if (!FnGetTid) {
    FnGetTid = Function::Create(FnGetTidTy, GlobalValue::ExternalLinkage,
                                "__kmpc_global_thread_num", M);
    FnGetTid->setCallingConv(CallingConv::C);
  }

  std::vector<Value *> FnGetTidArgs;
  FnGetTidArgs.push_back(KmpcLoc);

  CallInst *GetTidCall = CallInst::Create(FnGetTid, FnGetTidArgs, "tid.val");
  GetTidCall->setCallingConv(CallingConv::C);
  GetTidCall->setTailCall(true);

  return GetTidCall;
}

// This function collects path, file name, line, column information for
// generating kmpc_location struct needed for OpenMP runtime library
GlobalVariable *
VPOParoptUtils::genKmpcLocfromDebugLoc(Function *F, Instruction *AI,
                                       StructType *IdentTy, int Flags,
                                       BasicBlock *BS, BasicBlock *BE) {
  Module *M = F->getParent();
  LLVMContext &C = F->getContext();
  std::string LocString;

  StringRef Path("");
  StringRef File("unknown");
  StringRef FnName("unknown");
  unsigned SLine = 0;
  unsigned ELine = 0;

  int VpoEmitSourceLocation = 1;

  for (int K = 0; K < 2; ++K) {
    BasicBlock::iterator I = (K == 0) ? BS->begin() : BE->begin();
    if (Instruction *Inst = dyn_cast<Instruction>(&*I)) {
      if (DILocation *Loc = Inst->getDebugLoc()) {
        if (K == 0) {
          Path = Loc->getDirectory();
          File = Loc->getFilename();
          FnName = Loc->getScope()->getSubprogram()->getName();
          SLine = Loc->getLine();
        } else {
          ELine = Loc->getLine();
        }
      }
    }
  }

  // Source location string for OpenMP runtime library call
  // LocString = ";pathfilename;routinename;sline;eline;;"
  switch (VpoEmitSourceLocation) {

  case 1:
    LocString = (";unknown;" + FnName + ";" + Twine(SLine) + ";" +
                 Twine(ELine) + ";;\00")
                    .str();
    break;

  case 2:
    LocString = (";" + Path + "/" + File + ";" + FnName + ";" + Twine(SLine) +
                 ";" + Twine(ELine) + ";;\00")
                    .str();
    break;

  case 0:
  default:
    LocString = ";unknown;unknown;0;0;;\00";
    break;
  }

  // Constant Definitions
  ConstantInt *ValueZero = ConstantInt::get(Type::getInt32Ty(C), 0);
  ConstantInt *ValueFlags = ConstantInt::get(Type::getInt32Ty(C), Flags);

  // Define the type of loc string. It is an array of i8/char type.
  ArrayType *LocStringTy = ArrayType::get(Type::getInt8Ty(C), LocString.size());

  // Create a global variable containing the loc string. Example:
  // @.source.0.0.9 = private unnamed_addr constant [22 x i8]
  // c";unknown;unknown;0;0;;"
  Constant *LocStringInit = ConstantDataArray::getString(C, LocString, false);
  GlobalVariable *LocStringVar = new GlobalVariable(
      *M, LocStringTy, true, GlobalValue::PrivateLinkage, LocStringInit,
      ".source." + Twine(SLine) + "." + Twine(ELine));
  // Allows merging of variables with same content.
  LocStringVar->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  DEBUG(dbgs() << "\nSource Location Info: " << LocString << "\n");

  // Get a pointer to the global variable containing loc string.
  Constant *Zeros[] = {ValueZero, ValueZero};
  Constant *LocStringPtr =
      ConstantExpr::getGetElementPtr(LocStringTy, LocStringVar, Zeros);

  // We now have values of all loc struct elements.
  // IdentTy:       {i32,   i32,    i32,    i32,    i8*         }
  // Loc struct:    {0,     Flags,  0,      0,      LocStringPtr}
  // So, we finally create a global variable to hold the struct. Example:
  // @.kmpc_loc.0.0.10 = private unnamed_addr constant { i32, i32, i32, i32, i8*
  // } { i32 0, i32 2, i32 0, i32 0, i8* getelementptr inbounds ([22 x i8], [22
  // x i8]* @.source.0.0.9, i32 0, i32 0) }
  Constant *StructInit = ConstantStruct::get(
      IdentTy, {ValueZero, ValueFlags, ValueZero, ValueZero, LocStringPtr});
  GlobalVariable *KmpcLoc = new GlobalVariable(
      *M, IdentTy, true, GlobalValue::PrivateLinkage, StructInit,
      ".kmpc_loc." + Twine(SLine) + "." + Twine(ELine));
  // Allows merging of variables with same content.
  KmpcLoc->setUnnamedAddr(GlobalValue::UnnamedAddr::Global);

  return KmpcLoc;
}

// Generate source location information for Explicit barrier
GlobalVariable *VPOParoptUtils::genKmpcLocforExplicitBarrier(
    Function *F, Instruction *AI, StructType *IdentTy, BasicBlock *BB) {
  int Flags = KMP_IDENT_KMPC | KMP_IDENT_BARRIER_EXPL; // bits 0x2 | 0x20

#if 0
  if (VPOParopt_openmp_dvsm)
    flags |= KMP_IDENT_CLOMP;  // bit 0x4
#endif

  GlobalVariable *KmpcLoc =
      VPOParoptUtils::genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, BB, BB);
  return KmpcLoc;
}

// Generate source location information for Implicit barrier
GlobalVariable *VPOParoptUtils::genKmpcLocforImplicitBarrier(
    WRegionNode *W, Function *F, Instruction *AI, StructType *IdentTy,
    BasicBlock *BB) {
  int Flags = 0;

  switch (W->getWRegionKindID()) {

  case WRegionNode::WRNParallelLoop:
  case WRegionNode::WRNWksLoop:
    Flags = KMP_IDENT_BARRIER_IMPL_FOR;
    break;

  case WRegionNode::WRNParallelSections:
  case WRegionNode::WRNSections:
    Flags = KMP_IDENT_BARRIER_IMPL_SECTIONS;
    break;

  case WRegionNode::WRNTask:
  case WRegionNode::WRNTaskloop:
    break;

  case WRegionNode::WRNSingle:
    Flags = KMP_IDENT_BARRIER_IMPL_SINGLE;
    break;

  default:
    Flags = KMP_IDENT_BARRIER_IMPL;
    break;
  }

  Flags |= KMP_IDENT_KMPC; // bit 0x2

#if 0
  if (PAROPT_openmp_dvsm)
    Flags |= KMP_IDENT_CLOMP;  // bit 0x4
#endif

  GlobalVariable *KmpcLoc =
      VPOParoptUtils::genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, BB, BB);
  return KmpcLoc;
}

// Generates a critical section around the middle BasicBlocks of `W`
// by emitting calls to `__kmpc_critical` before `BeginInst`, and
// `__kmpc_end_critical` after `EndInst`.
bool VPOParoptUtils::genKmpcCriticalSection(WRegionNode *W, StructType *IdentTy,
                                            AllocaInst *TidPtr,
                                            const StringRef &LockNameSuffix) {
  assert(W != nullptr && "WRegionNode is null.");
  assert(IdentTy != nullptr && "IdentTy is null.");
  assert(TidPtr != nullptr && "TidPtr is null.");

  assert(W->getBBSetSize() >= 3 &&
         "Critical Node is expected to have at least 3 BBlocks.");

  // W should have entry and exit BBlocks with the directive
  // intrinsic calls, and some middle BBlocks. We intend on inserting the
  // critical calls at the places marked below:
  //
  //    EntryBB:
  //      call void @llvm.intel.directive(metadata !"DIR.OMP.CRITICAL")
  //      call void @llvm.intel.directive(metadata !"DIR.QUAL.LIST.END")
  // +------< begin critical >
  // |    br label %BB1
  // |
  // |  BB1:
  // |    ...
  // |  ...
  // |    br label %ExitBB
  // |
  // |  ExitBB:
  // |    call void @llvm.intel.directive(metadata !"DIR.OMP.END.CRITICAL")
  // |    call void @llvm.intel.directive(metadata !"DIR.QUAL.LIST.END")
  // +------< end critical >
  //      br label %..

  BasicBlock *EntryBB = W->getEntryBBlock();
  BasicBlock *ExitBB = W->getExitBBlock();
  assert(EntryBB->size() >= 3 && "Entry BBlock has invalid size.");
  assert(ExitBB->size() >= 3 && "Exit BBlock has invalid size.");

  // BeginInst: `br label %BB1` (EntryBB) in the above example.
  Instruction *BeginInst = &(*(EntryBB->rbegin()));
  // EndInst: `call void @llvm.intel.directive(metadata !"DIR.QUAL.LIST.END")`
  // (ExitBB) in the above example.
  Instruction *EndInst = &(*(++(ExitBB->rbegin())));

  assert(BeginInst != nullptr && "BeginInst is null.");
  assert(EndInst != nullptr && "EndInst is null.");

  return genKmpcCriticalSection(W, IdentTy, TidPtr, BeginInst, EndInst,
                                LockNameSuffix);
}

// Wraps the above function for case when the caller does not provide a lock
// name suffix, and uses a default lock name suffix.
bool VPOParoptUtils::genKmpcCriticalSection(WRegionNode *W, StructType *IdentTy,
                                            AllocaInst *TidPtr) {
  return genKmpcCriticalSection(W, IdentTy, TidPtr, "");
}

// Generates a KMPC call to IntrinsicName with Tid obtained using TidPtr.
CallInst *
VPOParoptUtils::genKmpcCallWithTid(WRegionNode *W, StructType *IdentTy,
                                   AllocaInst *TidPtr, Instruction *InsertPt,
                                   StringRef IntrinsicName, Type *ReturnTy,
                                   ArrayRef<Value *> Args) {
  assert(W != nullptr && "WRegionNode is null.");
  assert(IdentTy != nullptr && "IdentTy is null.");
  assert(InsertPt != nullptr && "InsertPt is null.");
  assert(!IntrinsicName.empty() && "IntrinsicName is empty.");
  assert(TidPtr != nullptr && "TidPtr is null.");

  // The KMPC call is of form:
  //     __kmpc_atomic_<type>(loc, tid, args).
  // We have the Intrinsic name, its return type and other function args. The
  // loc argument is obtained using the IdentTy struct inside genKmpcCall. But
  // we need a valid Tid, which we can load from memory using TidPtr.
  LoadInst *LoadTid = new LoadInst(TidPtr, "my.tid", InsertPt);
  LoadTid->setAlignment(4);

  // Now bundle all the function arguments together.
  SmallVector<Value*, 3> FnArgs = {LoadTid};
  FnArgs.append(Args.begin(), Args.end());

  // And then try to generate the KMPC call.
  return VPOParoptUtils::genKmpcCall(W, IdentTy, InsertPt, IntrinsicName,
                                     ReturnTy, FnArgs);
}

// This function generates a call to query the current thread if it is a master
// thread. Or, generates a call to end_master callfor the team of threads.
//   %master = call @__kmpc_master(%ident_t* %loc, i32 %tid)
//      or
//   call void @__kmpc_end_master(%ident_t* %loc, i32 %tid)
CallInst *VPOParoptUtils::genKmpcMasterOrEndMasterCall(WRegionNode *W,
                            StructType *IdentTy, Value *Tid,
                            Instruction *InsertPt, bool IsMasterStart) {

  BasicBlock  *B = W->getEntryBBlock();
  Function    *F = B->getParent();
  LLVMContext &C = F->getContext();

  Type *RetTy = nullptr;
  StringRef FnName;

  if (IsMasterStart) {
    FnName = "__kmpc_master";
    RetTy = Type::getInt32Ty(C);
  }
  else {
    FnName = "__kmpc_end_master";
    RetTy = Type::getVoidTy(C);
  }

  LoadInst *LoadTid = new LoadInst(Tid, "my.tid", InsertPt);
  LoadTid->setAlignment(4);

  // Now bundle all the function arguments together.
  SmallVector<Value *, 3> FnArgs = {LoadTid};

  CallInst *MasterOrEndCall = VPOParoptUtils::genKmpcCall(W,
                                IdentTy, InsertPt, FnName, RetTy, FnArgs);
  return MasterOrEndCall;
}

// This function generates calls to guard the single thread execution for the
// single/end single region.
//
//   call single = @__kmpc_single(%ident_t* %loc, i32 %tid)
//      or
//   call void @__kmpc_end_single(%ident_t* %loc, i32 %tid)
CallInst *VPOParoptUtils::genKmpcSingleOrEndSingleCall(WRegionNode *W,
                            StructType *IdentTy, Value *Tid,
                            Instruction *InsertPt, bool IsSingleStart) {

  BasicBlock  *B = W->getEntryBBlock();
  Function    *F = B->getParent();
  LLVMContext &C = F->getContext();

  Type *RetTy = nullptr;
  StringRef FnName;

  if (IsSingleStart) {
    FnName = "__kmpc_single";
    RetTy = Type::getInt32Ty(C);
  }
  else {
    FnName = "__kmpc_end_single";
    RetTy = Type::getVoidTy(C);
  }

  LoadInst *LoadTid = new LoadInst(Tid, "my.tid", InsertPt);
  LoadTid->setAlignment(4);

  // Now bundle all the function arguments together.
  SmallVector<Value *, 3> FnArgs = {LoadTid};

  CallInst *SingleOrEndCall = VPOParoptUtils::genKmpcCall(W,
                                IdentTy, InsertPt, FnName, RetTy, FnArgs);
  return SingleOrEndCall;
}

// This function generates calls to guard the ordered thread execution for the
// ordered/end ordered region.
//
//   call void @__kmpc_ordered(%ident_t* %loc, i32 %tid)
//      or
//   call void @__kmpc_end_ordered(%ident_t* %loc, i32 %tid)
CallInst *VPOParoptUtils::genKmpcOrderedOrEndOrderedCall(WRegionNode *W,
                            StructType *IdentTy, Value *Tid,
                            Instruction *InsertPt, bool IsOrderedStart) {

  BasicBlock  *B = W->getEntryBBlock();
  Function    *F = B->getParent();
  LLVMContext &C = F->getContext();

  Type *RetTy = Type::getVoidTy(C);

  StringRef FnName;

  if (IsOrderedStart)
    FnName = "__kmpc_ordered";
  else
    FnName = "__kmpc_end_ordered";

  LoadInst *LoadTid = new LoadInst(Tid, "my.tid", InsertPt);
  LoadTid->setAlignment(4);

  // Now bundle all the function arguments together.
  SmallVector<Value *, 3> FnArgs = {LoadTid};

  CallInst *OrderedOrEndCall = VPOParoptUtils::genKmpcCall(W,
                                 IdentTy, InsertPt, FnName, RetTy, FnArgs);
  return OrderedOrEndCall;
}

// Private helper methods for generation of a KMPC call.

// Generates KMPC calls to the intrinsic `IntrinsicName`.
CallInst *VPOParoptUtils::genKmpcCall(WRegionNode *W, StructType *IdentTy,
                                      Instruction *InsertPt,
                                      StringRef IntrinsicName, Type *ReturnTy,
                                      ArrayRef<Value *> Args) {
  assert(W != nullptr && "WRegionNode is null.");
  assert(IdentTy != nullptr && "IdentTy is null.");
  assert(InsertPt != nullptr && "InsertPt is null.");
  assert(!IntrinsicName.empty() && "IntrinsicName is empty.");

  // Obtain Loc info
  BasicBlock *B = W->getEntryBBlock();
  BasicBlock *E = W->getExitBBlock();

  Function *F = B->getParent();
  Module *M = F->getParent();

  int Flags = KMP_IDENT_KMPC;

  // Before emitting the KMPC call, we need the Loc information.
  GlobalVariable *Loc =
      genKmpcLocfromDebugLoc(F, InsertPt, IdentTy, Flags, B, E);
  DEBUG(dbgs() << __FUNCTION__ << ": Loc: " << *Loc << "\n");

  // At this point, we have all the function args: loc + incoming Args. We bind
  // them together as FnArgs.
  SmallVector<Value *, 9> FnArgs = {Loc};
  FnArgs.append(Args.begin(), Args.end());

  // Next, for return type, we use ReturnType if it is not null, otherwise we
  // use VoidTy.
  LLVMContext &C = F->getContext();
  ReturnTy = (ReturnTy != nullptr) ? ReturnTy : Type::getVoidTy(C);

  return genCall(M, IntrinsicName, ReturnTy, FnArgs);
}


// Genetates a CallInst for a function with name `FnName`.
CallInst *VPOParoptUtils::genCall(Module *M, StringRef FnName, Type *ReturnTy,
                                  ArrayRef<Value *> FnArgs) {
  assert(M != nullptr && "Module is null.");
  assert(!FnName.empty() && "Function name is empty.");
  assert(FunctionType::isValidReturnType(ReturnTy) && "Invalid Return Type");

  // Before creating a call to the function, we first need to insert the
  // function prototype of the intrinsic into the Module's symbol table. To
  // create the prototype, we need the function name, Types of all the function
  // params, and the return type. We already have the intrinsic name. We now
  // obtain the function param types from FnArgs.
  SmallVector<Type *, 9> ParamTypes;
  for (Value *Arg : FnArgs) {
    Type *ArgType = Arg->getType();
    assert(FunctionType::isValidArgumentType(ArgType) && "Invalid Argument.");
    ParamTypes.insert(ParamTypes.end(), ArgType);
  }

  // Now we create the function type with param and return type.
  FunctionType *FnTy = FunctionType::get(ReturnTy, ParamTypes, false);

  // Now we try to insert the function prototype into the module symbol table.
  // But if it already exists, we just use the existing one.
  Constant *FnC = M->getOrInsertFunction(FnName, FnTy);
  Function *Fn = cast<Function>(FnC);
  assert(Fn != nullptr && "Function Declaration is null.");

  // We now  have the function declaration. Now generate a call to it.
  CallInst *FnCall = CallInst::Create(Fn, FnArgs);
  assert(FnCall != nullptr && "Failed to generate Function Call");

  FnCall->setCallingConv(CallingConv::C);
  FnCall->setTailCall(false);
  DEBUG(dbgs() << __FUNCTION__ << ": Function call: " << *FnCall << "\n");

  return FnCall;
}

// Private helper methods for generation of a critical section.

// Creates a prefix for the name of the lock var to be used in KMPC critical
// calls.
SmallString<64> VPOParoptUtils::getKmpcCriticalLockNamePrefix(WRegionNode *W) {

  assert(W != nullptr && "WRegionNode is null.");

  if (isa<WRNAtomicNode>(W))
    return SmallString<64>("_kmpc_atomic_");

  // For Critical, the lock name is determined based on OS and architecture.
  BasicBlock *BB = W->getEntryBBlock();
  assert(BB && "WRegionNode has no Entry BB.");
  Function *F = BB->getParent();
  assert(F != nullptr && "BB has no parent Function");
  Module *M = F->getParent();
  assert(M != nullptr && "Function has no parent Module");

  Triple TargetTriple(M->getTargetTriple());

  if (TargetTriple.isOSWindows()) {
    Triple::ArchType Arch = TargetTriple.getArch();
    if (Arch == Triple::x86)
      return SmallString<64>("_$vcomp$critsect$");

    if (Arch == Triple::x86_64)
      return SmallString<64>("$vcomp$critsect$");

    // TODO: Check if we need to check for other architectures here.
  }

  // TODO: Check if we need to check for Architectre/ OS types here.
  return SmallString<64>(".gomp_critical_user_");
}


// Returns the lock variable to be used in KMPC critical calls.
GlobalVariable *
VPOParoptUtils::genKmpcCriticalLockVar(WRegionNode *W,
                                       const StringRef &LockNameSuffix) {

  assert(W != nullptr && "WRegionNode is null.");

  // We first get the lock name prefix for the lock var based on the target.
  SmallString<64> LockName = getKmpcCriticalLockNamePrefix(W);
  LockName += LockNameSuffix;
  LockName += ".var";

  DEBUG(dbgs() << __FUNCTION__ << ": Lock name:" << LockName << ".\n");

  // Now, the type for lock variable is an array of eight 32-bit integers.
  BasicBlock *BB = W->getEntryBBlock();
  assert(BB && "WRegionNode has no Entry BB.");
  Function *F = BB->getParent();
  Module *M = F->getParent();
  LLVMContext &C = M->getContext();

  ArrayType *LockVarTy = ArrayType::get(Type::getInt32Ty(C), 8);

  // See if a lock object already exists, if so, reuse it.
  GlobalVariable *GV =
      M->getGlobalVariable(LockName);
  if (GV != nullptr) {
    DEBUG(dbgs() << __FUNCTION__ << ": Reusing existig lock var: " << *GV
                 << ".\n");

    assert(GV->getType()->getContainedType(0) == LockVarTy &&
           "Lock variable name conflicts with an existing variable.");
    return GV;
  }

  // Otherwise, Create a new lock object. CommonLinkage is used so that multiple
  // lock variables with the same name (across modules) get merged into a single
  // one at link time.
  GV = new GlobalVariable(*M, LockVarTy, false, GlobalValue::CommonLinkage,
                          ConstantAggregateZero::get(LockVarTy), LockName);

  assert(GV != nullptr && "Unable to generate Kmpc critical lock var.");
  DEBUG(dbgs() << __FUNCTION__ << ": Lock var generated: " << *GV << ".\n");

  return GV;
}

// Generates a critical section around Instructions `begin` and `end`.
bool VPOParoptUtils::genKmpcCriticalSectionImpl(
    WRegionNode *W, StructType *IdentTy, AllocaInst *TidPtr,
    Instruction *BeginInst, Instruction *EndInst, GlobalVariable *LockVar) {

  assert(W != nullptr && "WRegionNode is null.");
  assert(IdentTy != nullptr && "IdentTy is null.");
  assert(TidPtr != nullptr && "TidPtr is null.");
  assert(BeginInst != nullptr && "BeginInst is null.");
  assert(EndInst != nullptr && "EndInst is null.");
  assert(LockVar != nullptr && "LockVar is null.");

  CallInst *BeginCritical =
      genKmpcCallWithTid(W, IdentTy, TidPtr, BeginInst, "__kmpc_critical",
                         nullptr, {LockVar});
  assert(BeginCritical != nullptr && "Could not call __kmpc_critical");

  if (BeginCritical == nullptr)
      return false;

  CallInst *EndCritical =
      genKmpcCallWithTid(W, IdentTy, TidPtr, EndInst, "__kmpc_end_critical",
                         nullptr, {LockVar});
  assert(EndCritical != nullptr && "Could not call __kmpc_end_critical");

  if (BeginCritical == nullptr)
      return false;

  // Now insert the calls in the IR.
  BeginCritical->insertBefore(BeginInst);
  EndCritical->insertAfter(EndInst);

  DEBUG(dbgs() << __FUNCTION__ << ": Critical Section generated.\n");
  return true;
}

// Generates a critical section around Instructions `BeginInst` and `Endinst`,
// by emitting calls to `__kmpc_critical` before `BeginInst`, and
// `__kmpc_end_critical` after `EndInst`.
bool VPOParoptUtils::genKmpcCriticalSection(WRegionNode *W, StructType *IdentTy,
                                            AllocaInst *TidPtr,
                                            Instruction *BeginInst,
                                            Instruction *EndInst,
                                            const StringRef& LockNameSuffix) {
  assert(W != nullptr && "WRegionNode is null.");
  assert(IdentTy != nullptr && "IdentTy is null.");
  assert(TidPtr != nullptr && "TidPtr is null.");
  assert(BeginInst != nullptr && "BeginInst is null.");
  assert(EndInst != nullptr && "EndInst is null.");

  // Generate the Lock object for critical section.
  GlobalVariable *Lock = genKmpcCriticalLockVar(W, LockNameSuffix);
  assert(Lock != nullptr && "Could not create critical section lock variable.");

  return genKmpcCriticalSectionImpl(W, IdentTy, TidPtr, BeginInst, EndInst,
                                    Lock);
}

// Generates a memcpy call at the end of the given basic block BB.
// The value D represents the destination while the value S represents
// the source. The size of the memcpy is the size of destination.
// The compiler will insert the typecast if the type of source or destination
// does not match with the type i8.
// One example of the output is as follows.
//   call void @llvm.memcpy.p0i8.p0i8.i32(i8* bitcast (i32* @a to i8*), i8* %2, i32 4, i32 4, i1 false)
CallInst* VPOParoptUtils::genMemcpy(Value *D, 
                                    Value *S, 
                                    const DataLayout &DL, 
                                    BasicBlock *BB)
{
  IRBuilder<> MemcpyBuilder(BB);
  MemcpyBuilder.SetInsertPoint(BB->getTerminator());

  Value *Dest, *Src, *Size;
  unsigned A;

  // The first two arguments of the memcpy expects the i8 operands.
  // The instruction bitcast is introduced if the incoming src or dest
  // operand in not in i8 type.
  if (D->getType() != 
      Type::getInt8PtrTy(BB->getParent()->getContext())) {
    Dest = MemcpyBuilder.CreatePointerCast(D, MemcpyBuilder.getInt8PtrTy());
    Src = MemcpyBuilder.CreatePointerCast(S, MemcpyBuilder.getInt8PtrTy());
  }
  else {
    Dest = D;
    Src = S;
  }

  // For 32/64 bit architecture, the size and alignment should be
  // set accordingly.
  if (DL.getIntPtrType(MemcpyBuilder.getInt8PtrTy())->getIntegerBitWidth() == 64) {
    Size = 
      MemcpyBuilder.getInt64(
            DL.getTypeAllocSize(D->getType()->getPointerElementType()));
    A = StackAdjustedAlignment;
  }
  else {
    Size = 
      MemcpyBuilder.getInt32(DL.getTypeAllocSize(D->getType()->getPointerElementType()));
    A = StackAdjustedAlignment/4;
  }

  return MemcpyBuilder.CreateMemCpy(Dest, Src, Size, A);
}
