//===- InlineReport.cpp - Inline report ------------ ---------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file implements the mechanics of the inlining report.
//
//===----------------------------------------------------------------------===//

#ifdef INTEL_CUSTOMIZATION

#include "llvm/Transforms/IPO/InlineReport.h"
#include "llvm/Transforms/IPO/InlinerPass.h"
#include "llvm/IR/Module.h"
#include "llvm/Support/raw_ostream.h"

using namespace llvm;
using namespace InlineReportTypes;

//
// The functions below implement the printing of the inlining report 
//

// The reasons that a call site is inlined or not inlinined fall into 
// several categories.  These are indicated by the InlPtrType for each 
// reason.
//
// The simplest category is those reasons which are absolute: we inlined 
// or didn't inline the call site exactly because of this.  In this case, 
// printing a simple text string suffices to describe why the call site 
// was or was not inlined.  These reasons have the InlPrtType InlPtrSimple. 
//
// Sometimes, however, the real reason a call site was or was not inlined 
// is because the values of the cost and the threshold for that call site.
// In these cases, if the cost <= threshold, the inlining was done, but 
// if cost > threshold it was not.  But there are often large bonuses and 
// penalities that contribute to the value of the cost and/or threshold.
// In such cases, reporting the principal reason the cost and/or threshold
// was adjusted provides a more meaningful reason than simply citing the 
// cost and threshold numbers, and we do that.  These reasons have the 
// InlPtrType InlPtrCost.
//
// Finally, there are some reasons that can't be adequately displayed by 
// either of the above two techniques.  These have the InlPtrType
// InlPrtSpecial.  The handling of them is done directly within the report 
// printing functions themselves. 

typedef enum {
  InlPrtNone,     // Used for sentinels and the generic value "InlrNoReason"
                  // No text is expected to be printed for these. 
  InlPrtSimple,   // Print only the text for the (non-)inlining reason 
  InlPrtCost,     // Print the text and cost info for the (non-)inlining reason
  InlPrtSpecial   // The function InlineReportCallSite::print needs to have
                  //   special cased code to handle it  
} InlPrtType; 

typedef struct { 
  InlPrtType Type;      // Classification of inlining reason
  const char* Message;  // Text message for inlining reason (or nullptr) 
} InlPrtRecord; 

///
/// \brief A table of entries, one for each possible (non-)inlining reason
///
const static InlPrtRecord InlineReasonText[NinlrLast + 1] = {
  // InlrFirst, 
  InlPrtNone, nullptr,
  // InlrNoReason,
  InlPrtNone, nullptr,
  // InlrAlwaysInline,
  InlPrtSimple, "Callee is always inline",
  // InlrSingleBasicBlock,
  InlPrtCost, "Callee is single basic block",
  // InlrSingleLocalCall,
  InlPrtCost, "Callee has single callsite and local linkage",
  // InlrEmptyFunction,
  InlPrtCost, "Callee is empty", 
  // InlrVectorBonus,
  InlPrtCost, "Callee has vector instructions", 
  // InlrProfitable,
  InlPrtCost, "Inlining is profitable", 
  // InlrLast, 
  InlPrtNone, nullptr, 
  // NinlrFirst, 
  InlPrtNone, nullptr, 
  // NinlrNoReason,
  InlPrtSimple, "Not tested for inlining", 
  // NinlrColdCC,
  InlPrtCost, "Callee has cold calling convention",
  // NinlrDeleted,
  InlPrtSpecial, nullptr, 
  // NinlrDuplicateCall,
  InlPrtSimple, "Callee cannot be called more than once",
  // NinlrDynamicAlloca,
  InlPrtCost, "Callee has dynamic alloca",
  // NinlrExtern,
  InlPrtSpecial, nullptr, 
  // NinlrIndirect,
  InlPrtSpecial, "Call site is indirect", 
  // NinlrIndirectBranch,
  InlPrtCost, "Callee has indirect branch",
  // NinlrBlockAddress,
  InlPrtCost, "Callee has block address", 
  // NinlrCallsFramescape,
  InlPrtCost, "Callee calls framescape", 
  // NinlrNeverInline,
  InlPrtSimple, "Callee is never inline", 
  // NinlrIntrinsic,
  InlPrtSimple,"Callee is intrinsic",
  // NinlrOuterInlining,
  InlPrtSpecial, "High outer inlining cost",
  // NinlrRecursive,
  InlPrtSimple, "Callee has recursion", 
  // NinlrReturnsTwice,
  InlPrtSimple, "Callee has returns twice instruction",
  // NinlrTooMuchStack,
  InlPrtCost, "Callee uses too much stack space",
  // NinlrVarargs,
  InlPrtSimple, "Callee is varargs",
  // NinlrMismatchedAttributes,
  InlPrtSimple, "Caller/Callee mismatched attributes",
  // NinlrMismatchedGC
  InlPrtSimple, "Caller/Callee garbage collector mismatch", 
  // NinlrMismatchedPersonality,
  InlPrtSimple, "Caller/Callee personality mismatch", 
  // NinlrNoinlineAttribute
  InlPrtSimple, "Callee has noinline attribute", 
  // NinlrNoinlineCallsite,
  InlPrtSimple, "Callsite is noinline",
  // NinlrNoReturn,
  InlPrtSimple, "Callee is noreturn",
  // NinlrOptNone,
  InlPrtSimple, "Callee is opt none", 
  // NinlrMayBeOverriden,
  InlPrtSimple, "Callee may be overriden",
  // NinlrNotPossible,
  InlPrtSimple, "Not legal to inline",
  // NinlrNotAlwaysInline,
  InlPrtSimple, "Callee is not always_inline",
  // NinlrNotProfitable,
  InlPrtCost, "Inlining is not profitable", 
  // NinlrNewlyCreated,
  InlPrtSimple, "Newly created callsite", 
  // NinlrLast 
  InlPrtNone, nullptr
}; 

//
// Member functions for class InlineReportCallSite
//

InlineReportCallSite::~InlineReportCallSite(void) {
  while (!Children.empty()) {
    InlineReportCallSite* cs = Children.back();
    Children.pop_back();
    delete cs;
  }
}

InlineReportCallSite* InlineReportCallSite::copyBase(
  const InlineReportCallSite& Base, Instruction* NI)
{
  InlineReportCallSite* NewCS = new InlineReportCallSite(Base.Callee, 
    Base.IsInlined, Base.Reason, Base.M, nullptr, NI); 
  NewCS->IsInlined = Base.IsInlined; 
  NewCS->InlineCost = Base.InlineCost; 
  NewCS->OuterInlineCost = Base.OuterInlineCost; 
  NewCS->InlineThreshold = Base.InlineThreshold; 
  NewCS->Line = Base.Line; 
  NewCS->Col = Base.Col; 
  NewCS->Children.clear(); 
  return NewCS; 
} 

InlineReportCallSite* InlineReportCallSite::cloneBase(
  const ValueToValueMapTy& IIMap) { 
  if (IsInlined) { 
    InlineReportCallSite* IRCSk = copyBase(*this, nullptr); 
    return IRCSk; 
  } 
  const Value* oldCall = this->getCall(); 
  if (oldCall == nullptr) { 
    return nullptr; 
  } 
  ValueToValueMapTy::const_iterator VMI = IIMap.find(oldCall);
  if (VMI == IIMap.end()) { 
    return nullptr; 
  } 
  WeakVH newCall = VMI->second; 
  Instruction* NI = cast<Instruction>(newCall);
  InlineReportCallSite* IRCSk = copyBase(*this, NI); 
  return IRCSk; 
} 

///
/// \brief Print 'indentCount' indentations 
///
static void printIndentCount(unsigned indentCount) {
  for (unsigned J = 0; J < indentCount; ++J) {
    llvm::errs() << "   "; 
  } 
} 

///
/// \brief Print a simple message 
///
/// message: The message being printed 
/// indentCount: The number of indentations before printing the message
/// level: The level N from '-inline-report=N'
///
static void printSimpleMessage(const char* Message, unsigned IndentCount, 
  unsigned Level, bool IsInline) {
  if (Level & InlineReportOptions::Reasons) {
    if (Level & InlineReportOptions::SameLine) { 
      llvm::errs() << " "; 
    } 
    else { 
      llvm::errs() << "\n"; 
      printIndentCount(IndentCount + 1);
    } 
    llvm::errs() << (IsInline ? "<<" : "[["); 
    llvm::errs() << Message;
    llvm::errs() << (IsInline ? ">>" : "]]"); 
    llvm::errs() << "\n"; 
  } 
  else { 
      llvm::errs() << "\n"; 
  } 
} 

///
/// \brief Print the inlining cost and threshold values
///
void InlineReportCallSite::printCostAndThreshold(void) {
  llvm::errs() << " (" << getInlineCost();
  if (getIsInlined()) { 
    llvm::errs() << "<="; 
  } 
  else { 
    llvm::errs() << ">"; 
  } 
  llvm::errs() << getInlineThreshold() << ")";
} 

///
/// \brief Print the outer inlining cost and threshold values
///
void InlineReportCallSite::printOuterCostAndThreshold(void) {
  llvm::errs() << " (" << getOuterInlineCost() << ">" << getInlineCost() 
    << ">" << getInlineThreshold() << ")"; 
} 

///
/// \brief Print the callee name, and if non-zero, the line and column 
/// number of the call site 
///
void InlineReportCallSite::printCalleeNameModuleLineCol(unsigned Level) 
{
  if (getCallee() != nullptr) { 
    llvm::errs() << getCallee()->getName(); 
  } 
  if (Level & InlineReportOptions::File) { 
    llvm::errs() << " " << M->getModuleIdentifier();   
  }  
  if ((Level & InlineReportOptions::LineCol) && (Line != 0 || Col != 0)) { 
    llvm::errs() << " (" << Line << "," << Col << ")"; 
  } 
} 

///
/// \brief Print a representation of the inlining instance.
///
/// indentCount: The number of indentations to print
/// level: The level N from '-inline-report=N'
///
void InlineReportCallSite::print(unsigned IndentCount, unsigned Level) {
  assert(InlineReasonText[getReason()].Type != InlPrtNone); 
  printIndentCount(IndentCount);
  if (getIsInlined()) { 
    llvm::errs() << "-> INLINE: "; 
    printCalleeNameModuleLineCol(Level); 
    if (InlineReasonText[getReason()].Type == InlPrtCost) { 
      printCostAndThreshold();
    } 
    printSimpleMessage(InlineReasonText[getReason()].Message, 
      IndentCount, Level, true);  
  } 
  else { 
    if (InlineReasonText[getReason()].Type == InlPrtSpecial) { 
      switch (getReason()) { 
      case NinlrDeleted:
        llvm::errs() << "-> DELETE: ";
        printCalleeNameModuleLineCol(Level); 
        llvm::errs() << "\n";
        break; 
      case NinlrExtern:
        llvm::errs() << "-> EXTERN: ";
        printCalleeNameModuleLineCol(Level); 
        llvm::errs() << "\n";
        break;
      case NinlrIndirect: 
        llvm::errs() << "-> INDIRECT: ";
        printCalleeNameModuleLineCol(Level); 
        printSimpleMessage(InlineReasonText[getReason()].Message, 
          IndentCount, Level, false);
        break; 
      case NinlrOuterInlining:
        printCalleeNameModuleLineCol(Level); 
        printOuterCostAndThreshold();
        printSimpleMessage(InlineReasonText[getReason()].Message, 
          IndentCount, Level, false);
        break; 
      default: 
        assert(0); 
      }
    } 
    else { 
      llvm::errs() << "-> "; 
      printCalleeNameModuleLineCol(Level);
      if (InlineReasonText[getReason()].Type == InlPrtCost) { 
        printCostAndThreshold();
      } 
      printSimpleMessage(InlineReasonText[getReason()].Message, 
        IndentCount, Level, false);
    } 
  } 
} 

//
// Member functions for class InlineReportFunction
//

InlineReportFunction::~InlineReportFunction(void) {
  while (!CallSites.empty()) {
    InlineReportCallSite* CS = CallSites.back();
    CallSites.pop_back();
    delete CS;
  }
}

// 
// Member functions for class InlineReport
//

InlineReportFunction* InlineReport::addFunction(Function* F, Module* M) {
  if (Level == 0) { 
    return nullptr; 
  } 
  if (F == nullptr) { 
    return nullptr; 
  } 
  InlineReportFunctionMap::const_iterator MapIt = IRFunctionMap.find(F); 
  if (MapIt != IRFunctionMap.end()) { 
    makeCurrent(M, F); 
    return MapIt->second; 
  } 
  InlineReportFunction* IRF = new InlineReportFunction(F);
  IRFunctionMap.insert(std::make_pair(F, IRF)); 
  return IRF; 
} 

InlineReportCallSite* InlineReport::addCallSite(Function* F, CallSite* CS, 
  Module* M) {
  if (Level == 0) { 
     return nullptr; 
  }
  if (F == nullptr) { 
     return nullptr; 
  } 
  Instruction *I = CS->getInstruction(); 
  DebugLoc DLoc = CS->getInstruction()->getDebugLoc();
  InlineReportCallSite* IRCS = new InlineReportCallSite(
    CS->getCalledFunction(), false, NinlrNoReason, M, &DLoc, I); 
  InlineReportFunctionMap::const_iterator MapIt = IRFunctionMap.find(F);
  assert(MapIt != IRFunctionMap.end()); 
  InlineReportFunction* IRF = MapIt->second;  
  IRF->addCallSite(IRCS); 
  IRInstructionCallSiteMap.insert(std::make_pair(I, IRCS)); 
  addCallback(I); 
  return IRCS;
} 

void InlineReport::setDead(Function* F) { 
  if (Level == 0) {
    return; 
  }
  InlineReportFunctionMap::const_iterator MapIt = IRFunctionMap.find(F);
  assert(MapIt != IRFunctionMap.end());
  InlineReportFunction* INR = MapIt->second; 
  INR->setDead(true); 
} 

void InlineReport::cloneChildren(
  const InlineReportCallSiteVector& OldCallSiteVector, 
  InlineReportCallSite* NewCallSite, ValueToValueMapTy& IIMap)
{
  assert(NewCallSite->getChildren().empty()); 
  for (unsigned I = 0, E = OldCallSiteVector.size(); I < E; ++I) { 
    //
    // Copy the old InlineReportCallSite and add it to the children of the 
    // cloned InlineReportCallSite.
    InlineReportCallSite* IRCSj = OldCallSiteVector[I]; 
    InlineReportCallSite* IRCSk = IRCSj->cloneBase(IIMap); 
    if (IRCSk == nullptr) { 
      continue; 
    } 
    NewCallSite->addChild(IRCSk); 
    //
    // We keep track of the new calls that are added added to the inline 
    // report in case they themselves will be inlined. 
    if (IRCSk->getCall() != nullptr) { 
      IRInstructionCallSiteMap.insert(std::make_pair(IRCSk->getCall(), IRCSk)); 
      addCallback(IRCSk->getCall()); 
    } 
    //
    // Recursively copy the InlineReportCallSites for the children. 
    if (IRCSj->getIsInlined()) { 
      cloneChildren(IRCSj->getChildren(), IRCSk, IIMap); 
    } 
  } 
} 

void InlineReport::inlineCallSite(Instruction* NI, InlineReportCallSite* IRCS, 
  Module* M, Function* Callee, InlineFunctionInfo& InlineInfo) { 
  if (Level == 0) { 
    return; 
  } 
  //
  // Get the inline report for the routine being inlined.  We are going 
  // to make a clone of it.
  InlineReportFunctionMap::const_iterator MapItF = IRFunctionMap.find(Callee);
  InlineReportFunction* INR = addFunction(Callee, M);
  //
  // Ensure that the report is up to date since the last call to 
  // Inliner::runOnSCC
  makeCurrent(M, Callee); 
  //
  // Create InlineReportCallSites "new calls" which appear in the inlined 
  // code.  Also, create a mapping from the "original calls" which appeared 
  // in the routine that was inlined, to the "new calls". When we clone the 
  // inline report for the routine being inlined, we need to replace the 
  // original calls with the new calls in the cloned inline report.  
  // We use 'IIMap' to do that mapping.  
  ValueToValueMapTy IIMap; 
  SmallVector<const Value*, 8>& OriginalCalls = InlineInfo.OriginalCalls; 
  SmallVector<WeakVH, 8>& NewCalls = InlineInfo.InlinedCalls; 
  for (unsigned I = 0, E = OriginalCalls.size(); I < E; ++I) { 
    IIMap.insert(std::make_pair(OriginalCalls[I], NewCalls[I])); 
  } 
  //
  // Clone the inline report INR and attach it to the inlined call site IRCS.
  // Use IIMap to map the original calls to the new calls in the cloned 
  // inline report. 
  cloneChildren(INR->getCallSites(), IRCS, IIMap); 
  // Indicate that the call has been inlined in the inline report 
  IRCS->setIsInlined(true);
  //
  // Remove the inlined instruction from the IRInstructionCallSiteMap
  InlineReportInstructionCallSiteMap::const_iterator MapIt;
  MapIt = IRInstructionCallSiteMap.find(NI);
  assert(MapIt != IRInstructionCallSiteMap.end());
  IRInstructionCallSiteMap.erase(MapIt);
  IRCS->setCall(nullptr); 
} 

void InlineReport::setReasonIsInlined(const CallSite& CS, InlineReason Reason) {
  if (Level == 0) { 
    return; 
  } 
  assert(IsInlinedReason(Reason)); 
  Instruction* NI = CS.getInstruction(); 
  InlineReportInstructionCallSiteMap::const_iterator 
    MapIt = IRInstructionCallSiteMap.find(NI);
  assert(MapIt != IRInstructionCallSiteMap.end()); 
  InlineReportCallSite* IRCS = MapIt->second; 
  IRCS->setReason(Reason); 
}

void InlineReport::setReasonIsInlined(const CallSite& CS, 
  const InlineCost& IC) {
  if (Level == 0) { 
    return; 
  } 
  InlineReason Reason = IC.getInlineReason(); 
  assert(IsInlinedReason(Reason)); 
  Instruction* NI = CS.getInstruction(); 
  InlineReportInstructionCallSiteMap::const_iterator
    MapIt = IRInstructionCallSiteMap.find(NI);
  assert(MapIt != IRInstructionCallSiteMap.end());
  InlineReportCallSite* IRCS = MapIt->second; 
  IRCS->setReason(IC.getInlineReason()); 
  IRCS->setInlineCost(IC.getCost()); 
  IRCS->setInlineThreshold(IC.getCost() + IC.getCostDelta()); 
}

void InlineReport::setReasonNotInlined(const CallSite& CS, 
  InlineReason Reason) {
  if (Level == 0) { 
    return; 
  } 
  assert(IsNotInlinedReason(Reason)); 
  Instruction* NI = CS.getInstruction(); 
  InlineReportInstructionCallSiteMap::const_iterator
    MapIt = IRInstructionCallSiteMap.find(NI);
  assert(MapIt != IRInstructionCallSiteMap.end());
  InlineReportCallSite* IRCS = MapIt->second; 
  IRCS->setReason(Reason); 
}

void InlineReport::setReasonNotInlined(const CallSite& CS, 
  const InlineCost& IC) {
  if (Level == 0) { 
    return; 
  } 
  InlineReason Reason = IC.getInlineReason();
  assert(IsNotInlinedReason(Reason)); 
  Instruction* NI = CS.getInstruction(); 
  InlineReportInstructionCallSiteMap::const_iterator
    MapIt = IRInstructionCallSiteMap.find(NI);
  assert(MapIt != IRInstructionCallSiteMap.end());
  InlineReportCallSite* IRCS = MapIt->second; 
  IRCS->setReason(Reason); 
  IRCS->setInlineCost(IC.getCost()); 
  IRCS->setInlineThreshold(IC.getCost() + IC.getCostDelta()); 
}

void InlineReport::setReasonNotInlined(const CallSite& CS, 
  const InlineCost& IC, int TotalSecondaryCost) {
  if (Level == 0) { 
    return; 
  } 
  InlineReason Reason = IC.getInlineReason();
  assert(Reason == NinlrOuterInlining); 
  setReasonNotInlined(CS, IC); 
  Instruction* NI = CS.getInstruction(); 
  InlineReportInstructionCallSiteMap::const_iterator
    MapIt = IRInstructionCallSiteMap.find(NI);
  assert(MapIt != IRInstructionCallSiteMap.end());
  InlineReportCallSite* IRCS = MapIt->second; 
  IRCS->setOuterInlineCost(TotalSecondaryCost);
} 

void InlineReport::printOptionValues(void) const {
  llvm::errs() << "Option Values:\n"; 
  llvm::errs() << "  inline-threshold: " << InlineLimit << "\n";    
  llvm::errs() << "  inlinehint-threshold: " << HintThreshold << "\n";    
  llvm::errs() << "  cold-threshold: " << ColdThreshold << "\n";    
  llvm::errs() << "\n"; 
} 

///
/// \brief Print the callsites in the 'Vector' 
///
/// indentCount: The number of indentations to print
/// level: The level N from '-inline-report=N'
///
static void printInlineReportCallSiteVector(
  const InlineReportCallSiteVector& Vector,
  unsigned IndentCount, unsigned Level) {
  for (unsigned I = 0, E = Vector.size(); I < E; ++I) {
    Vector[I]->print(IndentCount, Level); 
    printInlineReportCallSiteVector(Vector[I]->getChildren(), IndentCount + 1,
      Level);
  } 
} 

void InlineReportFunction::print(unsigned Level) const { 
  if (Level == 0) { 
    return;
  } 
  printInlineReportCallSiteVector(CallSites, 1, Level); 
} 

void InlineReport::print(void) const {
  if (Level == 0) { 
    return;
  } 
  llvm::errs() << "---- Begin Inlining Report ----\n"; 
  printOptionValues();  
  InlineReportFunctionMap::const_iterator Mit, E; 
  for (Mit = IRFunctionMap.begin(), E = IRFunctionMap.end(); Mit != E; ++Mit) { 
    Function* F = Mit->first;
    InlineReportFunction* IRF = Mit->second;
    if (IRF->getDead()) {
      llvm::errs() << "DEAD STATIC FUNC: " << F->getName() << "\n\n";
    }
    else { 
      llvm::errs() << "COMPILE FUNC: " << F->getName() << "\n";
      InlineReportFunction* IRF = Mit->second;
      IRF->print(Level); 
      llvm::errs() << "\n"; 
    } 
  } 
  llvm::errs() << "---- End Inlining Report ------\n"; 
}

void InlineReportCallSite::loadCallsToMap(std::map<Instruction*, bool>& LMap) {
  Instruction* NI = getCall();
  if (NI != nullptr) { 
    LMap.insert(std::make_pair(NI, true)); 
  } 
  for (unsigned I = 0, E = Children.size(); I < E; ++I) { 
    Children[I]->loadCallsToMap(LMap); 
  } 
} 

#ifndef NDEBUG
bool InlineReport::validateFunction(Function* F) { 
  llvm::errs() << "Validating " << F->getName() << "\n"; 
  bool ReturnValue = true;
  InlineReportFunctionMap::const_iterator MapIt;
  MapIt = IRFunctionMap.find(F); 
  if (MapIt == IRFunctionMap.end()) { 
    return false;
  } 
  InlineReportFunction* IRF = MapIt->second; 
  IRF->print(Level);
  std::map<Instruction*, bool> OriginalCalls; 
  const InlineReportCallSiteVector& Vec = IRF->getCallSites(); 
  for (unsigned I = 0, E = Vec.size(); I < E; ++I) { 
    Vec[I]->loadCallsToMap(OriginalCalls); 
  } 
  for (Function::iterator BB = F->begin(), E = F->end(); BB != E; ++BB) { 
    for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) { 
      CallSite CS(cast<Value>(I));
      if (!CS) { 
        continue; 
      } 
      Instruction* NI = CS.getInstruction(); 
      std::map<Instruction*, bool>::const_iterator MapIt;
      MapIt = OriginalCalls.find(NI); 
      if (MapIt == OriginalCalls.end()) { 
        ReturnValue = false; 
        llvm::errs() << "Cannot find " << NI << "\n";
        NI->dump(); 
      } 
    } 
  } 
  llvm::errs() << "Done Validating " << F->getName() << "\n";
  return ReturnValue;
} 

bool InlineReport::validate(void) { 
  bool GlobalRv = true; 
  InlineReportFunctionMap::const_iterator MI, ME; 
  llvm::errs() << "Start Validation Pass\n"; 
  for (MI = IRFunctionMap.begin(), ME = IRFunctionMap.end(); MI != ME; ++MI) { 
    Function* F = MI->first; 
    bool LocalRv = validateFunction(F); 
    llvm::errs() << "Validated " << F->getName();
    if (LocalRv) { 
      llvm::errs() << " passed\n"; 
    }
    else { 
      llvm::errs() << " failed\n"; 
    }  
    GlobalRv &= LocalRv;
  } 
  llvm::errs() << "End Validation Pass\n"; 
  return GlobalRv; 
}
#endif // NDEBUG

void InlineReport::makeCurrent(Module* M, Function* F) {
  InlineReportFunctionMap::const_iterator MapIt = IRFunctionMap.find(F); 
  assert(MapIt != IRFunctionMap.end());
  InlineReportFunction* IRF = MapIt->second; 
  if (IRF->getCurrent()) { 
    return;
  } 
  if (F->isDeclaration()) { 
    IRF->setCurrent(true); 
    return; 
  } 
  for (Function::iterator BB = F->begin(), E = F->end(); BB != E; ++BB) { 
    for (BasicBlock::iterator I = BB->begin(), E = BB->end(); I != E; ++I) { 
      CallSite CS(cast<Value>(I));
      if (!CS) { 
        continue; 
      } 
      Instruction* NI = CS.getInstruction(); 
      InlineReportInstructionCallSiteMap::const_iterator MapItICS; 
      MapItICS = IRInstructionCallSiteMap.find(NI); 
      if (MapItICS != IRInstructionCallSiteMap.end()) { 
        continue; 
      } 
      InlineReportCallSite* IRCS = addCallSite(F, &CS, M); 
      IRCS->setReason(NinlrNewlyCreated); 
    } 
  } 
  IRF->setCurrent(true); 
} 

void InlineReport::makeAllNotCurrent(void) {
   InlineReportFunctionMap::const_iterator It, E; 
   for (It = IRFunctionMap.begin(), E = IRFunctionMap.end(); It != E; ++It) { 
     InlineReportFunction* IRF = It->second; 
     IRF->setCurrent(false); 
   } 
} 

void InlineReport::replaceFunctionWithFunction(Function* OldFunction, 
  Function* NewFunction) {
  InlineReportFunctionMap::const_iterator IrfIt; 
  if (OldFunction == NewFunction) { 
    return; 
  } 
  IrfIt = IRFunctionMap.find(OldFunction); 
  if (IrfIt == IRFunctionMap.end()) { 
    return; 
  } 
  InlineReportFunction* IRF = IrfIt->second; 
  int count = IRFunctionMap.erase(OldFunction); 
  assert(count == 1); 
  IRFunctionMap.insert(std::make_pair(NewFunction, IRF)); 
  InlineReportInstructionCallSiteMap::const_iterator IrcsIt, IrcsEnd; 
  for (IrcsIt = IRInstructionCallSiteMap.begin(), 
    IrcsEnd = IRInstructionCallSiteMap.end(); IrcsIt != IrcsEnd; ++IrcsIt) {
    InlineReportCallSite* IRCS = IrcsIt->second;
    if (IRCS->getCallee() == OldFunction) { 
      IRCS->reassignCallee(NewFunction); 
    } 
  } 
} 

InlineReportCallSite* InlineReport::getCallSite(const CallSite& CS) {
  Instruction* NI = CS.getInstruction();
  InlineReportInstructionCallSiteMap::const_iterator
    MapItC = IRInstructionCallSiteMap.find(NI);
  if (MapItC == IRInstructionCallSiteMap.end()) { 
    return nullptr; 
  } 
  return MapItC->second;
} 

InlineReport::~InlineReport(void) {
  while (!IRCallbackVector.empty()) { 
    InlineReportCallback* IRCB = IRCallbackVector.back(); 
    IRCallbackVector.pop_back(); 
    delete IRCB; 
  } 
  InlineReportFunctionMap::const_iterator FI, FE; 
  for (FI = IRFunctionMap.begin(), FE = IRFunctionMap.end(); FI != FE; ++FI) {
    delete FI->second; 
  } 
}

void InlineReport::addCallback(Value* V) { 
  InlineReportCallback* IRCB = new InlineReportCallback(V, this); 
  IRCallbackVector.push_back(IRCB); 
}

#endif // INTEL_CUSTOMIZATION
