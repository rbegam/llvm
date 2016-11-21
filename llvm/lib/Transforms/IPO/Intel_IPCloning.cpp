//===------- Intel_IPCloning.cpp - IP Cloning -*------===//
//
// Copyright (C) 2016 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive property
// of Intel Corporation and may not be disclosed, examined or reproduced in
// whole or in part without explicit written authorization from the company.
//
//===----------------------------------------------------------------------===//
// This file does perform IP Cloning.
//===----------------------------------------------------------------------===//

#include "llvm/Transforms/IPO/Intel_IPCloning.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/IR/CallSite.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/InstIterator.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/Module.h"
#include "llvm/Pass.h"
#include "llvm/Transforms/IPO.h"
#include "llvm/Transforms/Utils/Cloning.h"
using namespace llvm;

#define DEBUG_TYPE "ipcloning"

STATISTIC(NumIPCloned, "Number of functions IPCloned");

// Option to trace IP Cloning
static cl::opt<bool> IPCloningTrace("print-ip-cloning", cl::ReallyHidden);

// Maximum number of clones allowed for any routine.
static cl::opt<unsigned> IPFunctionCloningLimit("ip-function-cloning-limit",
                                   cl::init(3), cl::ReallyHidden);

// It is a mapping between formals of current function that is being processed
// for cloning and set of possible constant values that can reach from 
// call-sites to the formals.
SmallDenseMap<Value *, std::set<Constant *>> FormalConstantValues;

// List of inexact formals for the current function that is being processed
// for cloning. Inexact means that at least one non-constant will reach
// from call-sites to formal.
SmallPtrSet<Value *, 16> InexactFormals;

// Mapping between CallInst and corresponding constant argument set.
DenseMap<Instruction *, unsigned> CallInstArgumentSetIndexMap;

// All constant argument sets for a function that is currently being
// processed. Each constant argument set is mapped with unique index value. 
SmallDenseMap<unsigned,
    std::vector<std::pair<unsigned, Constant*>>> FunctionAllArgumentsSets;

// Mapping between newly cloned function and constant argument set index.
SmallDenseMap<unsigned, Function *> ArgSetIndexClonedFunctionMap;

// List of call-sites that need to be processed for cloning
std::vector<Instruction*> CurrCallList;

// List of all cloned functions
std::set<Function *> ClonedFunctionList;

// Return true if constant argument is worth considering for cloning
static bool isConstantArgWorthy(Value *Arg) {
    Value* FnArg = Arg->stripPointerCasts();
    Function *Fn = dyn_cast<Function>(FnArg);
    // Consider all non-function constants for cloning
    if (Fn == nullptr) {
      return true;
    } 
    // if it is function address, consider only if it has local definition.
    if (Fn->isDeclaration() || Fn->isIntrinsic()
        || !Fn->hasExactDefinition() || !Fn->hasLocalLinkage() ||
        Fn->hasExternalLinkage()) {
      return false;
    }
    return true;
}

// Return true if actual argument is considered for cloning
static bool isConstantArgForCloning(Value *Arg) {
  if (Constant *C = dyn_cast<Constant>(Arg)) {
    if (isa<UndefValue>(C))
      return false;

    if (isConstantArgWorthy(Arg))
      return true;
  }
  return false;
}

// Collect constant value if 'ActualV' is constant actual argument
// and save it in constant list of 'FormalV'. Otherwise, mark
// 'FormalV' as inexact.
static void collectConstantArgument(Value* FormalV, Value* ActualV) {

  if (!isConstantArgForCloning(ActualV)) {
    // Mark inexact formal
    if (!InexactFormals.count(FormalV))
      InexactFormals.insert(FormalV);

    return;
  }
  // Now, we know it is valid constant for cloning.
  Constant *C = dyn_cast<Constant>(ActualV);
  auto &ValList = FormalConstantValues[FormalV];
    
  if (!ValList.count(C))
    ValList.insert(C);
}

// Returns maximum possible number of clones based on constant-value-lists
// of formals
static unsigned getMaxClones() {
  unsigned prod = 1;
  unsigned count;
  for (auto I = FormalConstantValues.begin(), E = FormalConstantValues.end();
       I != E; ++I) {
    auto CList = I->second;
    count = CList.size();
    if (InexactFormals.count(I->first))
      count += 1;

    if (count == 0)
      count = 1;
      
    prod = prod * count;
  }
  return prod;
}

// Returns minimum number of clones needed based on constant-value-lists
// of formals
static unsigned getMinClones() {
  unsigned prod = 1;
  unsigned count;
  for (auto I = FormalConstantValues.begin(), E = FormalConstantValues.end();
       I != E; ++I) {
    auto CList = I->second;
    count = CList.size();
    if (InexactFormals.count(I->first))
      count += 1;

    if (prod < count)
      prod = count;
  }
  return prod;
}

// Look at all callsites of 'F' and collect all constant values
// of formals. Return true if use of 'F' is noticed as non-call. 
static bool analyzeAllCallsOfFunction(Function &F) {
  bool FunctionAddressTaken = false;

  for (User *UR : F.users()) {
    // Ignore if use of function is not a call
    if (!isa<CallInst>(UR)) {
      FunctionAddressTaken = true;
      continue;
    }
    CallSite CS = CallSite(UR);
    Function *Callee = CS.getCalledFunction();
    if (Callee != &F) {
      FunctionAddressTaken = true;
      continue;
    }

    // Collect constant values for each formal
    CurrCallList.push_back(CS.getInstruction()); 
    CallSite::arg_iterator CAI = CS.arg_begin();
    for (Function::arg_iterator AI = F.arg_begin(), E = F.arg_end();
         AI != E; ++AI, ++CAI) {
      collectConstantArgument(&*AI, *CAI);
    }
  }
  return FunctionAddressTaken;
}

// Create argument set for callsite 'CS' of  'F' and save it in 
// 'ConstantArgsSet'
//
static void createConstantArgumentsSet(CallSite CS,  Function &F,
         std::vector<std::pair<unsigned, Constant *>>& ConstantArgsSet) {
  
  unsigned position = 0;
  CallSite::arg_iterator CAI = CS.arg_begin();
  for (Function::arg_iterator AI = F.arg_begin(), E = F.arg_end();
       AI != E; ++AI, ++CAI, position++) {
    if (isConstantArgForCloning(*CAI)) {
      Constant *C = dyn_cast<Constant>(*CAI);
      ConstantArgsSet.push_back(std::make_pair(position, C)); 
    }
  }
}

// For given constant argument set 'ConstantArgs', it returns index
// of the constant argument set in "FunctionAllArgumentsSets".
//
static unsigned getConstantArgumentsSetIndex(
        std::vector<std::pair<unsigned, Constant *>>& ConstantArgs) {
  auto I = FunctionAllArgumentsSets.begin();
  auto E = FunctionAllArgumentsSets.end();
  unsigned index = 0;
  for(; I != E; I++) {
    if (I->second == ConstantArgs) {
      return I->first;
    }
    index++;
  }
  auto &CArgs = FunctionAllArgumentsSets[index];
  std::copy(ConstantArgs.begin(), ConstantArgs.end(),
            std::back_inserter(CArgs));
  return index;
}

// Clear all maps and sets
//
static void clearAllMaps(void) {
    CallInstArgumentSetIndexMap.clear();
    FunctionAllArgumentsSets.clear();
    ArgSetIndexClonedFunctionMap.clear();
    FormalConstantValues.clear();
    InexactFormals.clear(); 
    CurrCallList.clear();
}

// Heuristics to enable cloning for 'F'. Currently, it returns true always.
//
static bool isFunctionWorthyForCloning(Function &F) {
  // May need to add some heuristics like size of routine etc
  //
  return true;
}

// Returns true if cloning is skipped for 'F'.
//
static bool skipAnalyzeCallsOfFunction(Function &F) {
  if (F.isDeclaration() || F.isIntrinsic() || !F.hasExactDefinition() ||
      F.use_empty())
    return true;

   // Skip cloning analysis if it is cloned routine.
   if (ClonedFunctionList.count(&F))
     return true;

  // Allow  all routines for now
  if (!F.hasLocalLinkage())
    return true;

  if (!isFunctionWorthyForCloning(F))
    return true;

  return false;
}

// Dump constant values collected for each formal of 'F'
//
static void dumpFormalsConstants(Function &F) {
  unsigned position = 0;
  for (Function::arg_iterator AI = F.arg_begin(), E = F.arg_end();
       AI != E; ++AI, position++) {

     auto CList = FormalConstantValues[&*AI];
     errs() <<  "         Formal_" << position << ":";
     if (InexactFormals.count(&*AI))
       errs() << "  (Inexact)  \n";
     else
       errs() << "  (Exact)  \n";
     
     // Dump list of constants
     for (auto I = CList.begin(), E = CList.end(); I != E; I++) {
       errs() << "                  " << *(*(&*I)) << "\n";
     }
  }
  errs() << "\n\n";
}

// It analyzes all callsites of 'F' and collect all possible constant
// argument sets. All collected constant argument sets are saved in
// "FunctionAllArgumentsSets". It return false if number of constant
// argument sets exceeds "IPFunctionCloningLimit".
//
static bool collectAllConstantArgumentsSets(Function &F) {

  std::vector<std::pair<unsigned, Constant *>> ConstantArgs;
  for (unsigned i = 0, e = CurrCallList.size(); i != e; ++i) {
    Instruction* I = CurrCallList[i];
    CallSite CS = CallSite(I);

    ConstantArgs.clear();
    createConstantArgumentsSet(CS, F, ConstantArgs);
    if (ConstantArgs.size() == 0)
      continue;
    unsigned index = getConstantArgumentsSetIndex(ConstantArgs);
    CallInstArgumentSetIndexMap[CS.getInstruction()] = index;

    if (FunctionAllArgumentsSets.size() > IPFunctionCloningLimit) {
      if (IPCloningTrace)
        errs() << "     Exceeding number of argument sets limit \n";
      return false;
    }
  }
  if (FunctionAllArgumentsSets.size() == 0) {
    if (IPCloningTrace)
      errs() << "     Zero argument sets found \n";
    return false;
  }
  if (IPCloningTrace) {
    errs() << "    Number of argument sets found: ";
    errs() << FunctionAllArgumentsSets.size() << "\n";
  }

  return true;
}

// Returns true if there is a constant value in 'CArgs' at 'position'.
//
static bool isArgumentConstantAtPosition(
                std::vector<std::pair<unsigned, Constant *>> & CArgs,
                unsigned position) {
  for(auto I = CArgs.begin(), E = CArgs.end(); I != E; I++) {
    if (I->first == position)
      return true;
  }
  return false;
}

// Returns true if it is valid to set callee of callsite 'CS' to 'ClonedFn'.
// This routine makes sure that same constant argument set of 'ClonedFn'
// is passed to 'CS'.  'index' is index of constant argument set for
// 'ClonedFn'.
//
static bool okayEliminateRecursion(Function *ClonedFn, unsigned index,
                                   CallSite CS) {
  // Get constant argument set for ClonedFn. 
  auto &CArgs = FunctionAllArgumentsSets[index];

  unsigned position = 0;
  CallSite::arg_iterator CAI = CS.arg_begin();
  for (Function::arg_iterator AI = ClonedFn->arg_begin(),
       E = ClonedFn->arg_end(); AI != E; ++AI, ++CAI, position++) {

    if (!isArgumentConstantAtPosition(CArgs, position)) {
      // If argument is not constant in CArgs, then actual argument of CS 
      // should be non-constant.
      if (isConstantArgForCloning(*CAI))
        return false;
    }
    else {
      // If argument is constant in CArgs, then actual argument of CS 
      // should pass through formal.
      if ((&*AI) != (*CAI))
        return false;
    }
  }
  return true;
}

// Fix recursion callsites in cloned functions if possible.
//
//  Before cloning:
//     spec_qsort(...) {  <- entry
//        ...
//        spec_qsort(...);  <- call
//        ...
//     }
//
//  After cloning:
//     spec_qsort..0(...) {   <- entry
//        ...
//        spec_qsort(...);    <- call
//        ...
//     }
//
//   Fix recursion if possible:
//     spec_qsort..0(...) {   <- entry
//        ...
//        spec_qsort..0(...); <- call
//        ...
//     }
//
static void eliminateRecursionIfPossible(Function *ClonedFn,
                                   Function *OriginalFn, unsigned index) {
  for (inst_iterator II = inst_begin(ClonedFn), E = inst_end(ClonedFn);
     II != E; ++II) {
    if (!isa<CallInst>(&*II))
      continue;

    CallSite CS = CallSite(&*II);
    Function *Callee = CS.getCalledFunction();
    if (Callee == OriginalFn && 
        okayEliminateRecursion(ClonedFn, index, CS)) {
      CS.setCalledFunction(ClonedFn);

      if (IPCloningTrace)
        errs() << " Replaced Cloned call:   " << *CS.getInstruction() << "\n";
    }
  }
}

// It does actual cloning and fixes recursion calls if possible.
//
static void cloneFunction(void) {
  for (unsigned I = 0, E = CurrCallList.size(); I != E; ++I) {
    ValueToValueMapTy VMap;
    Instruction* CallInst = CurrCallList[I];
    CallSite CS = CallSite(CallInst);

    // Skip callsite if  no constant argument set is collected.
    if (CallInstArgumentSetIndexMap.find(CS.getInstruction()) ==
        CallInstArgumentSetIndexMap.end()) {
      continue;
    }
    Function* SrcFn = CS.getCalledFunction();

    // Get cloned function for constant argument set if it is already there
    unsigned index = CallInstArgumentSetIndexMap[CS.getInstruction()];
    Function* NewFn = ArgSetIndexClonedFunctionMap[index];

    // Create new clone if it is not there for constant argument set
    if (NewFn == nullptr) {
      NewFn = CloneFunction(SrcFn, VMap);
      ArgSetIndexClonedFunctionMap[index] = NewFn;
      ClonedFunctionList.insert(NewFn);
    }

    CS.setCalledFunction(NewFn);
    NumIPCloned++;
    eliminateRecursionIfPossible(NewFn, SrcFn, index);

    if (IPCloningTrace)
      errs() << " Cloned call:   " << *CS.getInstruction() << "\n";
  }
}

// Main routine to analyze all calls and clone functions if profitable.
//
static bool analysisCallsCloneFunctions(Module &M) {
  bool FunctionAddressTaken;

  if (IPCloningTrace)
    errs() << " Enter IP cloning \n";
  
  ClonedFunctionList.clear();

  for (Function &F : M) {

    if (skipAnalyzeCallsOfFunction(F)) {
      if (IPCloningTrace)
        errs() << " Skipping " << F.getName() << "\n";
      continue;
    }

    clearAllMaps(); 

    if (IPCloningTrace)
      errs() << " Cloning Analysis for:  " <<  F.getName() << "\n";

    FunctionAddressTaken = analyzeAllCallsOfFunction(F);

    // It is okay to enable cloning for address taken routines but
    // disable it for now.
    if (FunctionAddressTaken) {
      if (IPCloningTrace)
        errs() << " Skipping address taken " << F.getName() << "\n";
      continue;
    }

    
    if (FormalConstantValues.size() == 0 || CurrCallList.size() == 0) {
      if (IPCloningTrace)
        errs() << " Skipping non-candidate " << F.getName() << "\n";
      continue;
    }

    if (IPCloningTrace)
      dumpFormalsConstants(F);

    unsigned MaxClones = getMaxClones(); 
    unsigned MinClones = getMinClones(); 

    if (IPCloningTrace) {
      errs() << " Max clones:  " << MaxClones << "\n";
      errs() << " Min clones:  " << MinClones << "\n";
    }

    if (MaxClones <= 1 || MinClones > IPFunctionCloningLimit) {
      if (IPCloningTrace)
        errs() << " Skipping not worthy candidate " << F.getName() << "\n";
      continue;
    }

    if (!collectAllConstantArgumentsSets(F)) {
      if (IPCloningTrace)
        errs() << " Skipping not profitable candidate " << F.getName() << "\n";
      continue;
    }

    cloneFunction();
  }

  if (IPCloningTrace)
    errs() << " Total clones:  " << NumIPCloned << "\n";

  if (NumIPCloned != 0)
    return true; 

  return false;
}

static bool runIPCloning(Module &M) {
  bool Change = false;

  Change = analysisCallsCloneFunctions(M);
  clearAllMaps(); 

  return Change;
}

namespace {

struct IPCloningLegacyPass : public ModulePass {
  static char ID; // Pass identification, replacement for typeid
  IPCloningLegacyPass() : ModulePass(ID) {
    initializeIPCloningLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addPreserved<WholeProgramWrapperPass>();
  }

  bool runOnModule(Module &M) override {
    if (skipModule(M))
      return false;

    return runIPCloning(M);
  }
};
}

char IPCloningLegacyPass::ID = 0;
INITIALIZE_PASS(IPCloningLegacyPass, "ip-cloning", "IP Cloning", false, false)


ModulePass *llvm::createIPCloningLegacyPass() {
  return new IPCloningLegacyPass();
}

PreservedAnalyses IPCloningPass::run(Module &M,
                                          ModuleAnalysisManager &AM) {
  if (runIPCloning(M))
    return PreservedAnalyses::none();
  return PreservedAnalyses::all();
}
