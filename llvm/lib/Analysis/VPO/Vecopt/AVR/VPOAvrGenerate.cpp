//===----------------------------------------------------------------------===//
//
//   Copyright (C) 2015 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation. and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//   Source file:
//   ------------
//   VPOAvrGenerate.cpp -- Implements the AVR Generation Pass
//
//===----------------------------------------------------------------------===//

#include "llvm/Analysis/PostDominators.h" 
#include "llvm/Analysis/VPO/Vecopt/AVR/VPOAvrGenerate.h"
#include "llvm/Analysis/VPO/Vecopt/CandidateIdent/VPOVecCandIdentify.h"
#include "llvm/Analysis/VPO/Vecopt/Passes.h"
#include "llvm/Analysis/VPO/Vecopt/AVR/VPOAvrVistor.h"
#include "llvm/Analysis/VPO/WRegionInfo/WRegionUtils.h"

#include "llvm/Support/CommandLine.h"
#include "llvm/Analysis/LoopInfo.h"
#include "llvm/IR/Instructions.h"
#include <algorithm>


#define DEBUG_TYPE "avr-generation"

using namespace llvm;
using namespace llvm::vpo;

INITIALIZE_PASS_BEGIN(AVRGenerate, "avr-generate", "AVR Generate", false, true)
INITIALIZE_PASS_DEPENDENCY(IdentifyVectorCandidates)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(PostDominatorTree)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(HIRParser)
INITIALIZE_PASS_DEPENDENCY(HIRLocalityAnalysis)
INITIALIZE_PASS_DEPENDENCY(DDAnalysis)
INITIALIZE_PASS_END(AVRGenerate, "avr-generate", "AVR Generate", false, true)

char AVRGenerate::ID = 0;

// Abstract Layer command line options

static cl::opt<bool>AvrStressTest("avr-stress-test", cl::init(false),
  cl::desc("Construct full Avrs for stress testing"));

static cl::bits<ALOpts>DisableALOpt("disable-avr-opt",
  cl::desc("Specify abstract layer optimization to disable: "),
  cl::Hidden,
  cl::values(
    clEnumVal(ALBuild,       "Disable Abstract Layer Build"),
    clEnumVal(ALLoopOpt,     "Disable Abstract Layer Loop Opt"),
    clEnumVal(ALBranchOpt,   "Disable Abstract Layer Branch Opt"),
    clEnumVal(ALExprTreeOpt, "Disable Abstract Layer Expr Tree Opt"),
    clEnumValEnd));

static cl::opt<bool>AvrHIRTest("avr-hir-test", cl::init(false),
  cl::desc("Construct Avrs for HIR testing"));

// Pass Initialization

FunctionPass *llvm::createAVRGeneratePass() { return new AVRGenerate(); }

AVRGenerate::AVRGenerate() : FunctionPass(ID) {
 llvm::initializeAVRGeneratePass(*PassRegistry::getPassRegistry());

  setLLVMFunction(nullptr);
  setAvrFunction(nullptr);
  setAvrWrn(nullptr);
  setLoopInfo(nullptr);
  AbstractLayer.clear();

  // Set Stress Testing Level
  setStressTest(AvrStressTest);

  // Set Optimization Level
  // Default is Abstract Layer build with all optimizations enabled.
  DisableALBuild = DisableALOpt.isSet(ALBuild) ? true : false;
  DisableLoopOpt = DisableALOpt.isSet(ALLoopOpt) ? true : false;
  DisableAvrBranchOpt = DisableALOpt.isSet(ALBranchOpt) ? true : false;
  DisableAvrExprTreeOpt = DisableALOpt.isSet(ALExprTreeOpt) ? true : false;
}

void AVRGenerate::getAnalysisUsage(AnalysisUsage &AU) const
{
  AU.setPreservesAll();
  AU.addRequired<DominatorTreeWrapperPass>();
  AU.addRequired<PostDominatorTree>();
  AU.addRequired<LoopInfoWrapperPass>();
  if (!AvrHIRTest) {
    AU.addRequired<IdentifyVectorCandidates>();
  }

  // Temporary Check to prevent HIR building for LLVMIR mode
  // This required should be removed in future, since VPO driver
  // will be called from HIR. If called from HIR we dont need a required here.
  if (AvrHIRTest) {
    AU.addRequiredTransitive<HIRParser>();
    AU.addRequiredTransitive<HIRLocalityAnalysis>();
    AU.addRequiredTransitive<DDAnalysis>();
  }
}

bool AVRGenerate::runOnFunction(Function &F)
{
  if (!AvrHIRTest)
    VC = &getAnalysis<IdentifyVectorCandidates>();
  DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
  PDT= &getAnalysis<PostDominatorTree>();
  LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();

  if (AvrHIRTest)
    HIRP = &getAnalysis<HIRParser>();

  setLLVMFunction(&F);

  // Build the base Abstract Layer representation. 
  if (!DisableALBuild) {

    buildAbstractLayer();

    DEBUG(dbgs() << "Abstract Layer:\n");
    DEBUG(this->dump(PrintType));
  }

  // Insert AVRLoops into Abstract Layer
  if (!DisableLoopOpt) {

    optimizeLoopControl();

    DEBUG(dbgs() << "Abstract Layer After Loop Formation:\n");
    DEBUG(this->dump(PrintType)); 
  }

  // Insert AVRIfs into Abstract Layer
  if (!DisableAvrBranchOpt) {

    optimizeAvrBranches();

    DEBUG(dbgs() << "Abstract Layer After If Formation:\n");
    DEBUG(this->dump(PrintType));

  }

  // Insert AVRTerminals and build expression trees into Abstract Layer
  if (!DisableAvrExprTreeOpt) {

#if 0
    optimizeAvrTree();

    DEBUG(dbgs() << "Abstract Layer After Expression Tree Formation:\n");
    DEBUG(this->dump(PrintType));
#endif
  }

  return false;
}

// Abstract Layer Visitor Classes

// AVRGenerateVistor - Generates HIR-based AL
AVR *AVRGenerate::AVRGenerateVisitor::visitInst(HLInst *I)
{
  return AVRUtilsHIR::createAVRAssignHIR(I);
}

AVR *AVRGenerate::AVRGenerateVisitor::visitLabel(HLLabel *L)
{
  return AVRUtilsHIR::createAVRLabelHIR(L);
}

AVR *AVRGenerate::AVRGenerateVisitor::visitGoto(HLGoto *G)
{
  return AVRUtilsHIR::createAVRBranchHIR(G);
}

AVR *AVRGenerate::AVRGenerateVisitor::visitLoop(HLLoop *L)
{
  AVRLoop *ALoop;

  ALoop = AVRUtils::createAVRLoop((const Loop *)nullptr);
  
  // Visit loop children
  for (auto It = L->child_begin(), E = L->child_end(); It != E; ++It) {
    AVR *ChildAVR;

    ChildAVR = visit(*It);
    AVRUtils::insertAVR(ALoop, nullptr, ChildAVR, LastChild);
  }

#if 0
  formatted_raw_ostream OS(dbgs());

  ALoop->print(OS, 1, 1);
#endif

  return ALoop;
}

AVR *AVRGenerate::AVRGenerateVisitor::visitRegion(HLRegion *R)
{
  AVRWrn *AWrn;
  
  // TODO - for now use AVRWrn to represent a region. AVR generation
  // for HIR will change once we figure out how SIMD/AUTOVEC intrinsics
  // are represented and what we consider as potential vectorization
  // candidates.
  AWrn = AVRUtils::createAVRWrn(nullptr);

  // Visit region children
  for (auto It = R->child_begin(), E = R->child_end(); It != E; ++It) {
    AVR *ChildAVR;
    ChildAVR = visit(*It);
    AVRUtils::insertAVR(AWrn, nullptr, ChildAVR, LastChild);
  }
  
  return (AVR *) AWrn;
}

AVR *AVRGenerate::AVRGenerateVisitor::visitIf(HLIf *HIf)
{
  AVRIf *AIf;

  AIf = AVRUtilsHIR::createAVRIfHIR(HIf);

  // Visit then children
  for (auto It = HIf->then_begin(), E = HIf->then_end(); It != E; ++It) {
    AVR *ChildAVR;
    ChildAVR = visit(*It);
    AVRUtils::insertAVR(AIf, nullptr, ChildAVR, LastChild,
                        ThenChild);
  }

  // Visit else children
  for (auto It = HIf->else_begin(), E = HIf->else_end(); It != E; ++It) {
    AVR *ChildAVR;
    ChildAVR = visit(*It);
    AVRUtils::insertAVR(AIf, nullptr, ChildAVR, LastChild,
                        ElseChild);
  }

  return AIf;
}

AVR *AVRGenerate::AVRGenerateVisitor::visitSwitch(HLSwitch *S)
{
  return (AVR *) nullptr;
}


// Avr Branch Optimization: if-formation.
/// \brief AVRBranchOptVisitor class is a specialized visitor which walks the
/// Abstract Layer and idenitifies conditional AvrBranch nodes which can be
/// transformed to AvrIf nodes.  
///
/// This visitor constructs a vector of objects of type CandidateIf. A
/// CandidateIf simply contains a pointer to the cond-branch along with
/// pointers to then and else blocks which the brnach jumps to. CandidateIf
/// objects are consumed in the tranformation phase of Avr Branch optimization.
///
class AVRBranchOptVisitor {

  typedef SmallVector<CandidateIf *, 16> CandidateIfTy;
  typedef CandidateIfTy::reverse_iterator reverse_iterator;
  typedef CandidateIfTy::const_reverse_iterator const_reverse_iterator;

private:

  /// AL - Abstract Layer to optimize.
  AVRGenerate *AL;

  /// CandidateIfs - Vector of CandidateIfs identified by this visitor.
  CandidateIfTy CandidateIfs; 

  /// \brief Returns a CandidateIf if ABranch can be represnted as an
  /// AVRIf. 
  CandidateIf *generateAvrIfCandidate(AVRBranchIR *ABranch);

  /// \brief Returns the CandidateIf which lexically first branches to ALabel
  /// for short ciruits. Returns null if ALabel is not part of a sc-chain.
  CandidateIf *identifyShortCircuitParent(AVRLabelIR *ALabel);

  /// \brief Returns an AvrBlock (range of avrs specified by a begin and end
  /// avr) that represents the given BBlock.
  AvrBlock *findIfChildrenBlock(BasicBlock *BBlock);

  /// \brief Returns true if ThenChildren and ElseChildren contain
  /// supported control-flow for AvrIf optimization 
  bool isSupportedAvrIfChildren(AvrBlock *ThenChildren, AvrBlock *ElseChidlren);

public:

  AVRBranchOptVisitor(AVRGenerate *AbstractLayer) : AL (AbstractLayer) {}

  /// Visit Functions
  void visit(AVR* ANode) {}
  void visit(AVRBranch *ABranch);
  void postVisit(AVR* ANode) {}
  bool isDone() { return false; }
  bool skipRecursion(AVR *ANode) { return false; }

  /// \brief Return number of candidate ifs identified.
  unsigned getNumberOfCandidates() { return CandidateIfs.size(); }

  /// \brief Returns true if CandidateIfs is empty
  bool isEmpty() { return CandidateIfs.empty(); }

  // The If transformation will be bottom up. Only define reverse
  // iterators.
  reverse_iterator rbegin() { return CandidateIfs.rbegin(); }
  reverse_iterator rend() { return CandidateIfs.rend(); }
  const_reverse_iterator rbegin() const { return CandidateIfs.rbegin(); }
  const_reverse_iterator rend() const { return CandidateIfs.rend(); }

};

CandidateIf *AVRBranchOptVisitor::identifyShortCircuitParent(AVRLabelIR *AvrLabel) {

  // Search for short circuit in else-block
  auto Itr = std::find_if(CandidateIfs.begin(), CandidateIfs.end(),
   [AvrLabel](CandidateIf *obj) -> bool {
     if (obj->hasElseBlock()) {
       return  obj->getElseBegin() == AvrLabel; 
     }
     return false;
   });

  if (Itr != CandidateIfs.end()) {
    return *Itr;
  }

  return nullptr;
}

bool AVRBranchOptVisitor::isSupportedAvrIfChildren(AvrBlock *ThenChildren,
                                                   AvrBlock *ElseChildren) {

  if (!ThenChildren || !ElseChildren)
    return false;

  if (AVRBranch *ThenTerm = dyn_cast<AVRBranch>(ThenChildren->getEnd())) {

    if (AVRBranch *ElseTerm = dyn_cast<AVRBranch>(ElseChildren->getEnd())) {
 
      if (!ThenTerm->isConditional() && !ElseTerm->isConditional())
        return true;

      // TODO: Check Successors and support more compilcated if structures.
    }
  }

  return false;
}

AvrBlock *AVRBranchOptVisitor::findIfChildrenBlock(BasicBlock *BBlock) {

  if (!BBlock)
    return nullptr;

  // Search AL for AVRLabel generated for this BB.
  auto Itr = AL->AvrLabels.find(BBlock);
  if (Itr != AL->AvrLabels.end()) {

    AVRLabelIR *ChildrenBegin = Itr->second;
    AVR *ChildrenEnd = ChildrenBegin->getTerminator();

    assert(ChildrenBegin && ChildrenEnd && "Malformed If-children block!");

    return new AvrBlock(ChildrenBegin, ChildrenEnd);
  }

  // Unable to find Avr Label for given BBlock.
  assert(0 && "Avr Label for BB not found in abstract layer!");
  DEBUG(dbgs() << "AbstractLayer: compare-opt failed. Missing BB Label in AL!\n");

  return nullptr;
}

CandidateIf *AVRBranchOptVisitor::generateAvrIfCandidate(AVRBranchIR *ABranch) {

  if (!ABranch->isConditional() || ABranch->isBottomTest())
    return nullptr;

  BasicBlock *ThenBBlock = ABranch->getThenBBlock();
  BasicBlock *ElseBBlock = ABranch->getElseBBlock();
  AvrBlock *ThenChildren = nullptr, *ElseChildren = nullptr;
  CandidateIf *ShortCircuitParent = nullptr;
  AVRBranch *ShortCircuitBr = nullptr;

  if (ThenBBlock) {
    ThenChildren = findIfChildrenBlock(ThenBBlock);

    if (!ThenChildren)
      return nullptr;
  }

  if (ElseBBlock) {
    ElseChildren = findIfChildrenBlock(ElseBBlock);

    if (!ElseChildren)
      return nullptr;

    // Is Short Circuit?
    AVRLabelIR *ElseLabel = cast<AVRLabelIR>(ElseChildren->getBegin());
    ShortCircuitParent = identifyShortCircuitParent(ElseLabel);

    if (ShortCircuitParent) {

      AVRLabelIR *TargetLabel = cast<AVRLabelIR>(ShortCircuitParent->getElseBegin());
      ShortCircuitBr = AVRUtils::createAVRBranch(TargetLabel);
    }
  }

  // Current support only allows ThenChildren Terminator and ElseChildren
  // Terminator to branch to common label
  if (!isSupportedAvrIfChildren(ThenChildren, ElseChildren))
    return nullptr;

  return new CandidateIf(ABranch, ThenChildren, ElseChildren, ShortCircuitParent,
                         ShortCircuitBr);
}

void AVRBranchOptVisitor::visit(AVRBranch *ABranch) {

  // TODO: Convert optimzation to fully IR-independent opt.
  if (AVRBranchIR *AvrBranchIR = dyn_cast<AVRBranchIR>(ABranch)) {

    CandidateIf *CandidateIf = generateAvrIfCandidate(AvrBranchIR);

    if (CandidateIf) {
      CandidateIfs.push_back(CandidateIf);
    }
  }
}

void AVRGenerate::buildAbstractLayer()
{
  // Temporary AL construction mechanism. HIR based AL will be constructed
  // via incoming HIR-based WRN graph once available.
  if (AvrHIRTest) {

    AVRGenerateVisitor AG;

    // Walk the HIR and build WRGraph based on HIR
    WRContainerTy *WRGraph = WRegionUtils::buildWRGraphFromHIR();
    DEBUG(errs() << "WRGraph #nodes= " << WRGraph->size() << "\n");
    for (auto I=WRGraph->begin(), E = WRGraph->end(); I != E; ++I) {
      DEBUG(I->dump());
    }

    // TBD: Using WRN nodes directly for now. This needs to be changed
    // to depend on identify vector candidates. We also need to create
    // AVRLoop variants for LLVM/HIR variants ans use these going
    // forward.
    for (auto I=WRGraph->begin(), E = WRGraph->end(); I != E; ++I) {
      DEBUG(errs() << "Starting AVR gen for \n");
      DEBUG(I->dump());
      AVRWrn *AWrn;
      AVR *Avr;
      WRNVecLoopNode *WVecNode;

      WVecNode = dyn_cast<WRNVecLoopNode>(I);
      
      if (!WVecNode) {
        continue;
      }

      // Create an AVRWrn and insert AVR for contained loop as child
      AWrn = AVRUtils::createAVRWrn(WVecNode);
      Avr = AG.visit(WVecNode->getHLLoop());
      AVRUtils::insertAVR(AWrn, nullptr, Avr, FirstChild);

      AbstractLayer.push_back(AWrn);
    }

    // We have generated AL from HIR, do not invoke LLVM IR AL opts
    if (!AbstractLayer.empty()) {
      DisableLoopOpt = true;
      DisableAvrBranchOpt = true;
    }

  }
  else if (ScalarStressTest) {

     DEBUG(dbgs() << "\nAVR: Generating AVRs for whole function.\n");

    // Build complete AVR node representation for function in stress testing mode
    buildAvrsForFunction();
  }
  else {

    DEBUG(dbgs() << "\nAVR: Generating AVRs for vector candidates.\n");

    // Build AVR node representation for incoming vector candidates
    buildAvrsForVectorCandidates();
  }
}

AvrItr AVRGenerate::preorderTravAvrBuild(BasicBlock *BB, AvrItr InsertionPos)
{
  assert(BB && InsertionPos && "Avr preorder traversal failed!");

  auto *DomNode = DT->getNode(BB);

  // Build AVR node sequence for current basic block
  InsertionPos = generateAvrInstSeqForBB(BB, InsertionPos);
  AvrItr LastAvr(InsertionPos), ThenPos = nullptr;

  // Traverse dominator children
  for (auto I = DomNode->begin(), E = DomNode->end(); I != E; ++I) {

    auto *DomChildBB = (*I)->getBlock();

    if (AVRBranch *Branch = dyn_cast<AVRBranch>(LastAvr)) { 

      if (Branch->isConditional()) { 

	// Traverse the basic blocks in program if-then-else order.
        BranchInst *BI = cast<BranchInst>(BB->getTerminator());

        if ((DomChildBB == BI->getSuccessor(0))  &&
            // If one of the 'if' successors post-dominates the other, it is
            // better to link it after the 'if' instead of linking it as a child.
            !PDT->dominates(DomChildBB, BI->getSuccessor(1))) {

          InsertionPos = preorderTravAvrBuild(DomChildBB, InsertionPos);
	  ThenPos = InsertionPos;
          continue;
        }
        else if (DomChildBB == BI->getSuccessor(1)  &&
        !PDT->dominates(DomChildBB, BI->getSuccessor(0)) ) {

          InsertionPos = preorderTravAvrBuild(DomChildBB, ThenPos ? ThenPos : InsertionPos);
	  ThenPos = nullptr;
          continue;
        }
      }
    }

    // TODO: Properly Handle Switch statements
    InsertionPos = preorderTravAvrBuild(DomChildBB, InsertionPos);
  }

  return InsertionPos;
}

void AVRGenerate::buildAvrsForVectorCandidates()
{
  // Temporary implemtation uses vector of Vector Candidate objects to
  // build AVRs.  Will move away from usage of this object and use
  // vistor for WRN graph when available.

  for (auto I = VC->begin(), E = VC->end(); I != E; ++I) {

    AvrWrn = AVRUtils::createAVRWrn((*I)->getWrnNode());

    preorderTravAvrBuild((*I)->getEntryBBlock(), AvrWrn);
    AbstractLayer.push_back(AvrWrn);
  }
}

AvrItr AVRGenerate::generateAvrInstSeqForBB(BasicBlock *BB, AvrItr InsertionPos)
{
  AVRLabelIR *ALabel = AVRUtilsIR::createAVRLabelIR(BB);
  AVR *ACondition = nullptr, *NewNode = nullptr;

  // Add Avr label to map for downstream AL optimizations
  AvrLabels[BB] = ALabel;

  // First BB of loop, function, split is inserted as first child
  if (isa<AVRLoop>(InsertionPos) || isa<AVRFunction>(InsertionPos) ||
      isa<AVRWrn>(InsertionPos)) {
    AVRUtils::insertFirstChildAVR(InsertionPos, ALabel);
  }
  else {
    AVRUtils::insertAVRAfter(InsertionPos, ALabel);
  }

  InsertionPos = ALabel;

  for (auto I = BB->begin(), E = std::prev(BB->end()); I != E; ++I) {

    switch(I->getOpcode()) {
      case Instruction::Call:
        NewNode = AVRUtilsIR::createAVRCallIR(I);
        break;
      case Instruction::PHI:
        NewNode = AVRUtilsIR::createAVRPhiIR(I);
        break;
      case Instruction::Br:
        assert(0 && "Encountered a branch before block terminator!"); 
        NewNode = AVRUtilsIR::createAVRBranchIR(I);
        break;
      case Instruction::Ret:
        assert(0 && "Encountered a return before block terminator!"); 
        NewNode = AVRUtilsIR::createAVRReturnIR(I);
        break;
      case Instruction::ICmp:
      case Instruction::FCmp:
        ACondition = AVRUtilsIR::createAVRCompareIR(I);
	NewNode = ACondition;
        break;
      case Instruction::Select:
	// When a select is encountered, we pair it with the previous compare generated.
        assert(ACondition && "Select instruction missing compare");
        NewNode = AVRUtilsIR::createAVRSelectIR(I, cast<AVRCompare>(ACondition));

        // Reset ACondition pointer to null for any downstream compares.
        ACondition = nullptr;
        break;
      default:
        NewNode = AVRUtilsIR::createAVRAssignIR(I);
    }

    AVRUtils::insertAVRAfter(InsertionPos, NewNode);
    InsertionPos = NewNode;
  } 

  InsertionPos = generateAvrTerminator(BB, InsertionPos, ACondition);
  ALabel->setTerminator(InsertionPos);

  return InsertionPos;
}

AVR *AVRGenerate::findAvrConditionForBI(BasicBlock *BB, BranchInst *BI, AVR *InsertionPos) {

  AvrItr I(AvrLabels[BB]), E(InsertionPos);
  Value *BrCond = BI->getCondition();

  // Search Backwards for condition
  for (; I != E; --E) {

    const Instruction *Inst = nullptr;

    if (AVRAssignIR *Assign = dyn_cast<AVRAssignIR>(E)) {
      Inst = Assign->getLLVMInstruction();
    }
    else if (AVRPhiIR *Phi = dyn_cast<AVRPhiIR>(E)) {
      Inst = Phi->getLLVMInstruction();
    }
    else if (AVRCompareIR *Compare = dyn_cast<AVRCompareIR>(E) ) {
      Inst = Compare->getLLVMInstruction();
    }

    if (Inst == BrCond) {
      return E;
    }
  }
  return nullptr;
}


AVR *AVRGenerate::generateAvrTerminator(BasicBlock *BB, AVR *InsertionPos,
                                        AVR *ACondition) {
  auto Terminator = BB->getTerminator();

  if (BranchInst *BI = dyn_cast<BranchInst>(Terminator)) {

    if (!ACondition && BI->isConditional()) {

      // An AvrCompare was not idenitified for this branch. Search
      // AVRs for this branch's condition.
      ACondition = findAvrConditionForBI(BB, BI, InsertionPos);
      assert(ACondition && "Unable to find condition for branch!");
    }

    // Create a branch terminator 
    AVRBranchIR *ABranch = AVRUtilsIR::createAVRBranchIR(Terminator, ACondition);
    AVRUtils::insertAVRAfter(InsertionPos, ABranch);
    InsertionPos = ABranch;
  }
  else if(SwitchInst *SI = dyn_cast<SwitchInst>(Terminator)) {
    // TODO
    assert(SI && "LLVM switch not supported yet!");
  }
  else if (ReturnInst *RI = dyn_cast<ReturnInst>(Terminator)) {

    // Creat a return terminator
    AVRReturnIR *AReturn = AVRUtilsIR::createAVRReturnIR(RI);
    AVRUtils::insertAVRAfter(InsertionPos, AReturn);
    InsertionPos = AReturn;
  }
  else {
    llvm_unreachable("Unknown terminator type!");
  }

  return InsertionPos;
}

// For explicit vectorization of loops and functions, the vectorizer
// should not generate AVRFunction nodes. Building AVR for function
// is for stress testing only.
void AVRGenerate::buildAvrsForFunction()
{
  AvrFunction = AVRUtils::createAVRFunction(Func, LI);

  preorderTravAvrBuild(AvrFunction->getEntryBBlock(), AvrFunction);

  // Add generated AVRs to Abstract Layer.
  AbstractLayer.push_back(AvrFunction);
}

void AVRGenerate::optimizeLoopControl() {

  if (!isAbstractLayerEmpty()) {

    DEBUG(dbgs() << "\nInserting Avr Loops.\n");

    // AVRGenerate has created a collection of AVR sequences which represent 
    // candidate loops for vectorization. At this point these AVR sequences do not
    // have any control flow AVRs in them.
    //
    // The control flow is not added in the first build of AVR for two reasons:
    //   1. If there is an error in control flow analysis, we still want a base 
    //      set of AVRS to fall back on for vectorization.
    // 
    //   2. The algorithm for detecting loop control flow and insert nodes is 
    //      simplier when done as a post processing on exisiting AL.
    //
    // This walk will iterate through each AVR sequence (which represents a 
    // candidate loop nest) and insert AVRLoop nodes, and move the AVR nodes
    // which represent the body of the loop into AVRLoop's children, where
    // necessary.

    // TODO: Change iteration to vistor. In case of nested
    // WRN Nodes this will not properly recursively build loops
    // and link to WRN
    for (auto I = begin(), E = end(); I != E; ++I) {
      formAvrLoopNest(I);  
    }
  }
}


void AVRGenerate::formAvrLoopNest(AVRFunction *AvrFunction) {

  Function *Func = AvrFunction->getOrigFunction();
  const LoopInfo *LI = AvrFunction->getLoopInfo();
    
  for (auto I = Func->begin(), E = Func->end(); I != E; ++I) {

    if (!LI->isLoopHeader(I)) 
      continue;

    Loop *Lp = LI->getLoopFor(I);
    assert(Lp &&  "Loop not found for Loop Header BB!");

    BasicBlock *LoopLatchBB = Lp->getLoopLatch();
    assert(LoopLatchBB &&  "Loop Latch BB not found!");

    AVR *AvrLbl = AvrLabels[I];
    AVRLabel *AvrTermLabel = AvrLabels[LoopLatchBB];
    AVR *AvrTerm = AvrTermLabel->getTerminator();

    if (AvrLbl && AvrTerm) {

      // Mark the bottom test (Exclude it from AvrBranch Opt)
      markLoopBottomTest(AvrTermLabel);

      // Create AvrLoop
      AVRLoop *AvrLoop = AVRUtils::createAVRLoop(Lp);
 
      // Hook AVR Loop into AVR Sequence
      AVRUtils::insertAVRBefore(AvrLbl, AvrLoop);
      AVRUtils::moveAsFirstChildren(AvrLoop, AvrLbl, AvrTerm);
    }

  }
}

// AVR If insertion walks all of the conditional branches and
// attempts to generate AVRIF for them.  We need to exclude the
// conditional branch which is in the loop latch otherwise we
// incorrectly generate an AVRIF.
void AVRGenerate::markLoopBottomTest(AVRLabel *LoopLatchLabel) {

  AvrItr BottomTest(LoopLatchLabel);

  while (BottomTest) {

    if (AVRBranch *BT = dyn_cast<AVRBranch>(BottomTest)) {
      BT->setBottomTest(true);
      return;
    }
    BottomTest = std::next(BottomTest);
  }
}

void AVRGenerate::formAvrLoopNest(AVRWrn *AvrWrn) {

  const LoopInfo *LI = AvrWrn->getLoopInfo();
  AvrWrn->populateWrnBBSet();

  for (auto I = AvrWrn->wrnbbset_begin(), E = AvrWrn->wrnbbset_end();
       I != E; ++I) {

    // TODO: FIX THIS ASAP - Should not be using const_casts.
    // The BBSet build in WRN is returning const BBlocks, but the interfaces
    // for loop info cannot handle these.
    BasicBlock *LoopHeaderBB = const_cast<BasicBlock*>(*I);

    if (!LI->isLoopHeader(LoopHeaderBB)) 
      continue;

    Loop *Lp = LI->getLoopFor(LoopHeaderBB);
    assert(Lp &&  "Loop not found for Loop Header BB!");

    BasicBlock *LoopLatchBB = Lp->getLoopLatch();
    assert(LoopLatchBB &&  "Loop Latch BB not found!");

    AVR *AvrLbl = AvrLabels[LoopHeaderBB];
    AVRLabel *AvrTermLabel = AvrLabels[LoopLatchBB];
    AVR *AvrTerm = AvrTermLabel->getTerminator();

    if (AvrLbl && AvrTerm) {

      // Mark the bottom test (Exclude it from AvrBranch Opt)
       markLoopBottomTest(AvrTermLabel);
      
      // Create AvrLoop
      AVRLoop *AvrLoop = AVRUtils::createAVRLoop(Lp);
 
      // TODO: For nested WRN, this needs to only be set for
      // top-level loop of WRN.
      AvrLoop->setWrnVecLoopNode(AvrWrn->getWrnNode());

      // Hook AVR Loop into AVR Sequence
      AVRUtils::insertAVRBefore(AvrLbl, AvrLoop);
      AVRUtils::moveAsFirstChildren(AvrLoop, AvrLbl, AvrTerm);
    }
  } 

  cleanupAvrWrnNodes();
}

void AVRGenerate::formAvrLoopNest(AVR *AvrNode) {

  if (AVRWrn *AvrWrn = dyn_cast<AVRWrn>(AvrNode)) {
    formAvrLoopNest(AvrWrn);
  } 
  else if (AVRFunction *AvrFunction = dyn_cast<AVRFunction>(AvrNode)) {
    formAvrLoopNest(AvrFunction);
  }
  else {
    assert (0 && "Unexpected Avr node for Loop formation!"); 
  }
}

void AVRGenerate::cleanupAvrWrnNodes() {
  // TODO
}

//
// AVRIf nodes are formed in two steps. 
// (1) Identification/ Setup Pass (AL visit traversal)
//     Before AVRCompare nodes  can be replaced with AVRIf nodes
//     we must determine if:
//       A. AVRCompare is a candidate if. It is not part of a special
//          compare/select sequence or IV loop check.
//       B. AVRCompare is in a short circuit compare chain. Short 
//          circuits are nested ifs which share a common if block.
//          Example:
//          if (A && B) {
//            S1
//          } 
//          else {
//            S2
//          }
//
//          We would need to generate an avr equivalent of:
//          (TODO: We can generate a more effiecnt sequence)
//          if (A) {
//             if (B) { 
//               S1
//             }
//             else {
//               goto L1;
//             }
//          }
//          else {
//      L1:   S2
//          }
//
//          Each candidate if is recorded and SC-chains are marked
//          inside CandidateIF object.
//
// (2) AVRCompare replacement with AVRIf transformation.
//

void AVRGenerate::optimizeAvrBranches() {

  // Step 1: Identify Candidates using AL visitor
  AVRBranchOptVisitor AC(this);
  AVRVisitor<AVRBranchOptVisitor>AvrBranchOpt(AC); 
  AvrBranchOpt.forwardVisitAll(this);

  if (!AC.isEmpty()) {

    DEBUG(dbgs() << "\nIdentified " << AC.getNumberOfCandidates()
                 << " candidates for AvrIf optimization\n");

    // Optimize AVRCompare: Replace AVRBranches with AVRIf and set
    // children as appropiate. Traverse bottom up.

    // Step 2: Perform Replacement.
    for (auto I = AC.rbegin(), E = AC.rend(); I != E; ++I) {

      AVRBranch *AvrBranch = (*I)->getAvrBranch();
      AVRIfIR *AvrIfIR = AVRUtilsIR::createAVRIfIR(AvrBranch);
      AVRUtils::insertAVRBefore(AvrBranch, AvrIfIR);

      // Then-Children
      if ((*I)->hasThenBlock()) {

	AVR *ThenBegin = (*I)->getThenBegin();
	AVR *ThenEnd = (*I)->getThenEnd();

        assert (ThenBegin && ThenEnd && "Malformed AvrIf then-children!");
        AVRUtils::moveAsFirstThenChildren(AvrIfIR, ThenBegin, ThenEnd);
      } 

      // Else-Children
      if ((*I)->hasElseBlock()) {

        if(!(*I)->hasShortCircuit()) {

          AVR *ElseBegin = (*I)->getElseBegin();
          AVR *ElseEnd = (*I)->getElseEnd();

          assert (ElseEnd && ElseEnd && "Malformed AvrIf else-children!");
          AVRUtils::moveAsFirstElseChildren(AvrIfIR, ElseBegin, ElseEnd);
        }
        else {

          AVRBranch *SCSuccessor = (*I)->getShortCircuitSuccessor();

          assert(SCSuccessor && "AvrIf missing short-circuit successor!");
          AVRUtils::insertFirstElseChild(AvrIfIR, SCSuccessor);
	}
      }
    }

    // Step 3: Remove conditional branches
    for (auto I = AC.rbegin(), E = AC.rend(); I != E; ++I) {
      cleanupBranchOpt(*I);
    }
  }
  else {
    DEBUG(dbgs() << "No AVRCompares identified for AvrIf transformation!\n");
  }
}

void AVRGenerate::cleanupBranchOpt(CandidateIf *CandIf) {

  AVRBranch *Branch = CandIf->getAvrBranch();

  const ALChange *OptRemoval;

  // TODO: Move the change log modifications to the AVR utilites
  // and make transparent to user.

  // OptRemoval = new ALChange(Condition, ALBranchOpt, Removal);
  // ALChangeLog.push_back(OptRemoval);

  // Remvove the condition from AL
  // AVRUtils::remove(Condition);

  OptRemoval = new ALChange(Branch, ALBranchOpt, Removal);
  ALChangeLog.push_back(OptRemoval);

  // Remove the conditional branch from AL
  AVRUtils::remove(Branch);
}

void AVRGenerate::print(raw_ostream &OS, unsigned Depth, 
                        VerbosityLevel VLevel) const {

  formatted_raw_ostream FOS(OS);

  if (AbstractLayer.empty()) {
    FOS << "No AVRs Generated!\n";
    return;
  }

  for (auto I = begin(), E = end(); I != E; ++I) {
    I->print(FOS, Depth, VLevel);
  }
}

void AVRGenerate::print(raw_ostream &OS, const Module *M) const {
  this->print(OS, 1, PrintType);
}

void AVRGenerate::dump(VerbosityLevel VLevel) const {
  formatted_raw_ostream OS(dbgs());
  this->print(OS, 1, VLevel);
}

bool AVRGenerate::codeGen() {

  if (!AbstractLayer.empty()) {
    AVR *ANode = &AbstractLayer.back();
    ANode->codeGen();
    return true;
  }

  return false;
}

void AVRGenerate::releaseMemory()
{
  AbstractLayer.clear();
  ALChangeLog.clear();

  // TODO: Free up all generated AVRs.
}







