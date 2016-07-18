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

// This function generates a runtime library call to __kmpc_begin(&loc, 0)
CallInst *VPOParoptUtils::genKmpcBeginCall(Function *F, Instruction *AI,
                                           StructType *IdentTy) {
  Module *M = F->getParent();
  LLVMContext &C = F->getContext();

  BasicBlock &B = F->getEntryBlock();
  BasicBlock &E = B;

  int Flags = KMP_IDENT_KMPC;

  AllocaInst *KmpcLoc = genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, &B, &E);

  ConstantInt *ValueZero = ConstantInt::get(Type::getInt32Ty(C), 0);

  Constant *FnC = M->getOrInsertFunction("__kmpc_begin", Type::getVoidTy(C),
                                         PointerType::getUnqual(IdentTy),
                                         Type::getInt32Ty(C), NULL);

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

  AllocaInst *KmpcLoc = genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, &B, &E);

  Constant *FnC = M->getOrInsertFunction("__kmpc_end", Type::getVoidTy(C),
                                         PointerType::getUnqual(IdentTy), NULL);

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

  AllocaInst *Loc = genKmpcLocfromDebugLoc(F, InsertPt, IdentTy, Flags, B, E);

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
  AllocaInst *Loc = genKmpcLocfromDebugLoc(F, InsertPt, IdentTy, Flags, B, E);

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

  AllocaInst *Loc = genKmpcLocfromDebugLoc(F, InsertPt, IdentTy, Flags, B, E);
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

  AllocaInst *KmpcLoc = genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, &B, &E);

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
AllocaInst *VPOParoptUtils::genKmpcLocfromDebugLoc(Function *F, Instruction *AI,
                                                   StructType *IdentTy,
                                                   int Flags, BasicBlock *BS,
                                                   BasicBlock *BE) {
  Module *M = F->getParent();
  LLVMContext &C = F->getContext();
  std::string KmpLoc;

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
  // KmpLoc = ";pathfilename;routinename;sline;eline;;"
  switch (VpoEmitSourceLocation) {

  case 0:
    KmpLoc = ";unknown;unknown;0;0;;\00";
    break;

  case 1:
    KmpLoc = ";unknown;" + FnName.str() + ";" + std::to_string(SLine) + ";" +
             std::to_string(ELine) + ";;\00";
    break;

  case 2:
    KmpLoc = ";" + Path.str() + "/" + File.str() + ";" + FnName.str() + ";" +
             std::to_string(SLine) + ";" + std::to_string(ELine) + ";;\00";
    break;
  default:
    KmpLoc = ";unknown;unknown;0;0;;\00";
    break;
  }

  StringRef Loc = StringRef(KmpLoc);

  // Type Definitions
  ArrayType *LocStrTy = ArrayType::get(Type::getInt8Ty(C), Loc.str().size());

  // String Constant Definitions
  Constant *LocStrDef = ConstantDataArray::getString(C, Loc.str(), false);

  // Global Variable Definitions
  Constant *VarLoc = new GlobalVariable(
      *M, LocStrTy, false, GlobalValue::PrivateLinkage, LocStrDef,
      ".KmpcLoc." + std::to_string(SLine) + '.' + std::to_string(ELine));

  // Constant Definitions
  ConstantInt *ValueZero = ConstantInt::get(Type::getInt32Ty(C), 0);
  ConstantInt *ValueOne = ConstantInt::get(Type::getInt32Ty(C), 1);
  ConstantInt *ValueFour = ConstantInt::get(Type::getInt32Ty(C), 4);

  ConstantInt *ValueFlags = ConstantInt::get(Type::getInt32Ty(C), Flags);

  DEBUG(dbgs() << "\nSource Location Info: " << Loc << "\n");

  Constant *Zeros[] = {ValueZero, ValueZero};
  Constant *LocStrRef = ConstantExpr::getGetElementPtr(LocStrTy, VarLoc, Zeros);

  // Global Variable Definitions
  // VarLoc->setInitializer(LocStrRef);

  AllocaInst *KmpcLoc = new AllocaInst(IdentTy, "loc.addr." + 
                                       std::to_string(SLine) + "." + 
                                       std::to_string(ELine), AI);
  KmpcLoc->setAlignment(8);

  GetElementPtrInst *FlagsPtr = GetElementPtrInst::Create(
      IdentTy, KmpcLoc, {ValueZero, ValueOne},
      "flags." + std::to_string(SLine) + "." + std::to_string(ELine), AI);

  StoreInst *InitFlags = new StoreInst(ValueFlags, FlagsPtr, false, AI);

  InitFlags->setAlignment(4);

  GetElementPtrInst *PSrcPtr = GetElementPtrInst::Create(
      IdentTy, KmpcLoc, {ValueZero, ValueFour},
      "psource." + std::to_string(SLine) + "." + std::to_string(ELine), AI);

  StoreInst *InitPsource = new StoreInst(LocStrRef, PSrcPtr, false, AI);
  InitPsource->setAlignment(8);
  return KmpcLoc;
}

// Generate source location information for Explicit barrier
AllocaInst *VPOParoptUtils::genKmpcLocforExplicitBarrier(Function *F,
                                                         Instruction *AI,
                                                         StructType *IdentTy,
                                                         BasicBlock *BB) {
  int Flags = KMP_IDENT_KMPC | KMP_IDENT_BARRIER_EXPL; // bits 0x2 | 0x20

#if 0
  if (VPOParopt_openmp_dvsm)
    flags |= KMP_IDENT_CLOMP;  // bit 0x4
#endif

  AllocaInst *KmpcLoc =
      VPOParoptUtils::genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, BB, BB);
  return KmpcLoc;
}

// Generate source location information for Implicit barrier
AllocaInst *VPOParoptUtils::genKmpcLocforImplicitBarrier(WRegionNode *W,
                                                         Function *F,
                                                         Instruction *AI,
                                                         StructType *IdentTy,
                                                         BasicBlock *BB) {
  int Flags = 0;

  switch (W->getWRegionKindID()) {

  case WRegionNode::WRNParallelLoop:
  case WRegionNode::WRNWksLoop:
    Flags = KMP_IDENT_BARRIER_IMPL_FOR;
    break;

  case WRegionNode::WRNParallelSections:
  case WRegionNode::WRNWksSections:
    Flags = KMP_IDENT_BARRIER_IMPL_SECTIONS;
    break;

  case WRegionNode::WRNTask:
  case WRegionNode::WRNTaskLoop:
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

  AllocaInst *KmpcLoc =
      VPOParoptUtils::genKmpcLocfromDebugLoc(F, AI, IdentTy, Flags, BB, BB);
  return KmpcLoc;
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

// Private Helpers

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
  AllocaInst *Loc = genKmpcLocfromDebugLoc(F, InsertPt, IdentTy, Flags, B, E);
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

  Type *RetTy = NULL;
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

  Type *RetTy = NULL;
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
