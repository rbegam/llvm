//===- LoopSPMDization.cpp - Loop SPMDization pass          ---------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This pass implements the Loop SPMDization transformation that generates multiple loops from one loop. These loops can run in parallel. Two approaches are implemented here: the cyclic approach where each loop has a stride of k and the blocking approach where each loop iterates over contiguous #iterations/NPEs iterations.
//
//===----------------------------------------------------------------------===//

#include "llvm/IR/IntrinsicInst.h"
#include "llvm/Analysis/LoopPass.h"
#include "llvm/Transforms/Scalar.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Analysis/ScalarEvolutionExpander.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/IR/PatternMatch.h"
#include "llvm/Transforms/Utils/Cloning.h"
#include "llvm/Transforms/Utils.h"
#include "llvm/Transforms/Utils/LoopUtils.h"
#include "llvm/Transforms/Utils/UnrollLoop.h"
#include "llvm/Analysis/OptimizationDiagnosticInfo.h"

#include <vector>


using namespace llvm;
#define DEBUG_TYPE "spmdization"

#define SPMD_CYCLIC 1
#define SPMD_BLOCKING 2

namespace {
  class LoopSPMDization : public LoopPass {
    LLVMContext Context;
  public:
    static char ID;
    LoopSPMDization() : LoopPass(ID) {
      initializeLoopSPMDizationPass(*PassRegistry::getPassRegistry());
    }
  private:
    int next_token;
    unsigned spmd_approach;
    Value *steptimesk; 
    Value *StepPE0;
    Value *NewInitV; 
    Value *Cond;
    Value *nbyk;
    Value *UpperBound;
    Value *LowerBound;
    Value *TripCountV;
    bool FixReductionsIfAny(Loop *L, Loop *OrigL, BasicBlock *E, BasicBlock *AfterLoop, int PE, int NPEs, std::vector<PHINode *> *Reductions, std::vector<Value *> *ReduceVarExitOrig, std::vector<Instruction *> *ReduceVarOrig, std::vector<Instruction *> *OldInst);
    void setLoopAlreadySPMDized(Loop *L);
    bool FindReductionVariables(Loop *L, std::vector<PHINode *> *Reductions, std::vector<Value *> *ReduceVarExitOrig, std::vector<Instruction *> *ReduceVarOrig);
    PHINode *getInductionVariable(Loop *L, ScalarEvolution *SE);
    bool TransformLoopInitandStep(Loop *L, ScalarEvolution *SE, int PE, int NPEs);
    bool TransformLoopInitandBound(Loop *L, ScalarEvolution *SE, int PE, int NPEs);
    bool ZeroTripCountCheck(Loop *L, ScalarEvolution *SE, int PE, int NPEs, BasicBlock *AfterLoop, std::vector<PHINode *> *Reductions, std::vector<Value *> *ReduceVarExitOrig, std::vector<Instruction *> *ReduceVarOrig, DominatorTree *DT, LoopInfo *LI); 
    bool AddParallelIntrinsicstoLoop(Loop *L, LLVMContext& context, Module *M, BasicBlock *OrigPH, BasicBlock *E);
    IntrinsicInst* detectSPMDIntrinsic(Loop *L, LoopInfo *LI, DominatorTree *DT, PostDominatorTree *PDT, int &NPEs, Value *&approach);

    bool runOnLoop(Loop *L, LPPassManager &) override {

      // Skip SPMDization if optnone is set; this makes it possible to use
      // things like OptBisect with SPMDization.
      if (skipLoop(L)) return false;
      if (MDNode *LoopID = L->getLoopID())
        if(GetUnrollMetadata(LoopID, "llvm.loop.spmd.disable")) {
          return true;
        }

      LoopInfo *LI = &getAnalysis<LoopInfoWrapperPass>().getLoopInfo();
      DominatorTree *DT = &getAnalysis<DominatorTreeWrapperPass>().getDomTree();
      PostDominatorTree *PDT = &getAnalysis<PostDominatorTreeWrapperPass>().getPostDomTree();
      ScalarEvolution *SE = &getAnalysis<ScalarEvolutionWrapperPass>().getSE();
      
      LLVMContext& context = L->getHeader()->getContext();
      Function *F = L->getHeader()->getParent();
      Module *M = F->getParent() ;
      OptimizationRemarkEmitter ORE(F);
    
      
      ValueToValueMapTy VMap;
      BasicBlock *OrigPH = L->getLoopPreheader();
      Loop *OrigL = L;
      spmd_approach = 0;
      int NPEs;
      Value *approachV = nullptr;
      IntrinsicInst* found_spmd = detectSPMDIntrinsic(L, LI, DT, PDT, NPEs, approachV);
      if (found_spmd) {
        if(ConstantExpr *expr = dyn_cast<ConstantExpr>(approachV)) {
          if (expr->getOpcode() == Instruction::GetElementPtr) {
            GlobalVariable *glob_arg = dyn_cast<GlobalVariable>(expr->getOperand(0));
            if (glob_arg and glob_arg->isConstant() and glob_arg->getInitializer()) {
              // Unlike C, Fortran string has no null byte at the end
              StringRef user_approach;
              if(dyn_cast<ConstantDataArray>(glob_arg->getInitializer())->isCString())
                user_approach = dyn_cast<ConstantDataArray>(glob_arg->getInitializer())->getAsCString();
              else
                user_approach = dyn_cast<ConstantDataArray>(glob_arg->getInitializer())->getAsString();
           
              if(user_approach.compare_lower("cyclic")==0) {
                spmd_approach = SPMD_CYCLIC;
              }
              else if(user_approach.compare_lower("blocked")==0 || user_approach.compare_lower("blocking")==0 || user_approach.compare_lower("block")==0) {
                spmd_approach = SPMD_BLOCKING;
              }
              else if(user_approach.compare_lower("hybrid")==0) {
                errs() << "\n";
                errs().changeColor(raw_ostream::BLUE, true);
                errs() << "!! WARNING: Hybrid Approach of SPMD is not supported yet !!";
                errs().resetColor();
                return false;
              }
              else {
                errs() << "\n";
                errs().changeColor(raw_ostream::BLUE, true);
                errs() << "!! WARNING: BAD CSA SPMD INTRINSIC !!";
                errs().resetColor();
                errs() << " Second argument should be Cyclic, Blocked, Blocking, or Hybrid.\n"
                  "This call will be ignored.\n\n";
                return false;
              }
            }
            else {
              errs() << "\n";
              errs().changeColor(raw_ostream::BLUE, true);
              errs() << "!! WARNING: BAD CSA SPMD INTRINSIC !!";
              errs().resetColor();
              return false;
            }
          }     
          else {
            errs() << "\n";
            errs().changeColor(raw_ostream::BLUE, true);
            errs() << "!! WARNING: BAD CSA SPMD INTRINSIC !!";
            errs().resetColor();
            return false;
          }
        }
        else {
          errs() << "\n";
          errs().changeColor(raw_ostream::BLUE, true);
          errs() << "!! WARNING: BAD CSA SPMD INTRINSIC !!";
          errs().resetColor();
          return false;
        }
        
        if(!L->getExitBlock()) {
          /*ORE.emit(
            OptimizationRemarkMissed(DEBUG_TYPE, "Unstructured Code",
            L->getStartLoc(), L->getHeader())
            << "Unable to perform loop SPMDization as directed by the pragma"
            "because loop body has unstructured code.");
          */
          errs() << "\n";
          errs().changeColor(raw_ostream::BLUE, true);
          errs() << "!! WARNING: COULD NOT PERFORM SPMDization !!\n";
          errs().resetColor();
          errs() << R"help(The SPMDization loop body has unstructured code.

Branches to or from an OpenMP structured block are illegal

)help";
          return false;
        }
        ORE.emit(
                 OptimizationRemark(DEBUG_TYPE, "",
                                    L->getStartLoc(), L->getHeader())
                 << "Performed loop SPMDization as directed by the pragma.");
        
        //Fix me: We assume a maximum of 16 reductions in the loop
        std::vector<PHINode *> Reductions(16);
        std::vector<Value *> ReduceVarExitOrig(16);
        std::vector<Instruction *> ReduceVarOrig(16);
        //there is OldInst foreach reduction variable
        std::vector<Instruction *> OldInsts(16);
        FindReductionVariables(L, &Reductions, &ReduceVarExitOrig, &ReduceVarOrig);
        if(spmd_approach == SPMD_CYCLIC) {
          if(!TransformLoopInitandStep(L, SE, 0, NPEs)) {
            return false;
          } 
        }
        else if(spmd_approach == SPMD_BLOCKING) { 
          const DataLayout &DL = L->getHeader()->getModule()->getDataLayout();
          SCEVExpander Expander(*SE, DL, "loop-SPMDization");
          BranchInst *PreHeaderBR = cast<BranchInst>(L->getLoopPreheader()->getTerminator());
          const SCEV *BECountSC = SE->getBackedgeTakenCount(L);

          // Sometimes SCEV can't figure out the backedge taken count; bail and
          // print a warning if that happens.
          if (isa<SCEVCouldNotCompute>(BECountSC)) {
            errs() << "\n";
            errs().changeColor(raw_ostream::BLUE, true);
            errs() << "!! WARNING: COULD NOT PERFORM SPMDization !!";
            errs().resetColor();
            errs() << R"help(

We were unable to determine an expression for the trip count of a loop for which
blocking SPMDization was requested. Please simplify the loop control logic or
try a different SPMDization strategy instead.

)help";
            return false;
          }

          const SCEV *TripCountSC =
            SE->getAddExpr(BECountSC, SE->getConstant(BECountSC->getType(), 1));
          TripCountV = Expander.expandCodeFor(TripCountSC, 
                                              TripCountSC->getType(),    
                                              PreHeaderBR);
     
          IRBuilder<> BPR(L->getLoopPreheader()->getTerminator());
          Value *nbykmightzero = BPR.CreateUDiv(TripCountV,
                                                ConstantInt::get(BECountSC->getType(), NPEs), 
                                                ".nbyk");
          auto *IsZero = BPR.CreateICmpEQ(nbykmightzero, ConstantInt::get(BECountSC->getType(), 0));
          // If n by k is zero (there will be loops with zero trip count), each loop will run at most one iteration
          nbyk = BPR.CreateSelect(IsZero, ConstantInt::get(BECountSC->getType(), 1), nbykmightzero);
          
          TransformLoopInitandBound(L, SE, 0, NPEs);
        }
     
        setLoopAlreadySPMDized(L);
        
        BasicBlock *PH = SplitBlock(OrigPH, OrigPH->getTerminator(), DT, LI);
        PH->setName(L->getHeader()->getName() + ".ph");
        BasicBlock *OrigE = L->getExitBlock();
        
        Instruction *i = dyn_cast<Instruction>(OrigE->begin());
        BasicBlock *E = SplitBlock(OrigE, i, DT, LI);
        OrigE->setName(L->getHeader()->getName() + ".e");
        BasicBlock *AfterLoop = E;
        
        //Add CSA parallel intrinsics:
        AddParallelIntrinsicstoLoop(L, context, M, OrigPH, E);
        SmallVector<Value *, 128> NewReducedValues;//should be equal to NPEs
        for(int PE = 1; PE < NPEs; PE++) {
          SmallVector<BasicBlock *, 8> NewLoopBlocks;
          BasicBlock *Exit = L->getExitBlock();
          //clone the exit block, to be attached to the cloned loop
          BasicBlock *NewE = CloneBasicBlock(Exit, VMap, ".PE" + std::to_string(PE), F);
          VMap[Exit] = NewE;
          
          Loop *NewLoop =
            cloneLoopWithPreheader(PH, OrigPH, L, VMap,
                                   ".PE" + std::to_string(PE), LI, 
                                   DT, NewLoopBlocks);
          NewLoopBlocks.push_back(NewE);
          remapInstructionsInBlocks(NewLoopBlocks, VMap);
          // Update LoopInfo.
          if(OrigL->getParentLoop())
            OrigL->getParentLoop()->addBasicBlockToLoop(NewE, *LI);
          // Add DominatorTree node, update to correct IDom.
          DT->addNewBlock(NewE, NewLoop->getLoopPreheader());
          
          Instruction *ExitTerm = Exit->getTerminator();
          BranchInst::Create(NewLoop->getLoopPreheader(), Exit);
          ExitTerm->eraseFromParent();
          
          if(spmd_approach == SPMD_CYCLIC) 
            TransformLoopInitandStep(NewLoop, SE, 1, NPEs);
          else if(spmd_approach == SPMD_BLOCKING) 
            TransformLoopInitandBound(NewLoop, SE, PE, NPEs);
       
          ZeroTripCountCheck(NewLoop, SE, PE, NPEs, AfterLoop, &Reductions, &ReduceVarExitOrig, &ReduceVarOrig, DT, LI);
          //This assumes -ffp-contract=fast is set
          bool success_p = FixReductionsIfAny(NewLoop, OrigL, E, AfterLoop, PE, NPEs, &Reductions, &ReduceVarExitOrig, &ReduceVarOrig, &OldInsts); 
          if(!success_p)
            return false;
          L = NewLoop;
          setLoopAlreadySPMDized(L);       
        }
        //Fix missed Phi operands in AfterLoop
        BasicBlock::iterator bi, bie; 
        for (bi = AfterLoop->begin(), bie = AfterLoop->end();  (bi != bie); ++bi) {
          PHINode *RedPhi = dyn_cast<PHINode>(&*bi);
          if(!RedPhi)
            continue;
          Value *RedV;
          if(RedPhi->getBasicBlockIndex(L->getExitBlock()) == -1) 
            //Afterloop did not have a phi node
            RedPhi->setIncomingBlock(0, L->getExitBlock());
          
          RedV = RedPhi->getIncomingValueForBlock(L->getExitBlock());
          for (auto it = pred_begin(AfterLoop), et = pred_end(AfterLoop); it != et; ++it) {
            BasicBlock* predecessor = *it;
            if(RedPhi->getBasicBlockIndex(predecessor) == -1) {
              RedPhi->addIncoming(RedV, predecessor);
            }
          }
        }
      }
      return true;
    }
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      //getLoopAnalysisUsage(AU);
      AU.addRequired<DominatorTreeWrapperPass>();
      AU.addPreserved<DominatorTreeWrapperPass>();
      AU.addRequired<PostDominatorTreeWrapperPass>();
      AU.addRequired<LoopInfoWrapperPass>();
      AU.addPreserved<LoopInfoWrapperPass>();
      AU.addRequired<ScalarEvolutionWrapperPass>();
      AU.addRequired<AAResultsWrapperPass>();
      AU.addRequiredID(LoopSimplifyID);
      AU.addRequiredID(LCSSAID);
    }
  };
}
 
char LoopSPMDization::ID = 0;

INITIALIZE_PASS_BEGIN(LoopSPMDization, DEBUG_TYPE, "Loop SPMDization", false, false)
INITIALIZE_PASS_DEPENDENCY(LoopInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopAccessLegacyAnalysis)
INITIALIZE_PASS_DEPENDENCY(LoopPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ScalarEvolutionWrapperPass)
INITIALIZE_PASS_DEPENDENCY(LoopSimplify)
INITIALIZE_PASS_END(LoopSPMDization, DEBUG_TYPE, "Loop SPMDization", false, false)

Pass *llvm::createLoopSPMDizationPass() {
  return new LoopSPMDization();
}

void LoopSPMDization::setLoopAlreadySPMDized(Loop *L) {
  // Add SPMDization(disable) metadata to disable future SPMDization.
  SmallVector<Metadata *, 4> MDs;
  // Reserve first location for self reference to the LoopID metadata node.
  MDs.push_back(nullptr);
  
  LLVMContext &Context = L->getHeader()->getContext();
  SmallVector<Metadata *, 1> DisableOperands;
  DisableOperands.push_back(MDString::get(Context, "llvm.loop.spmd.disable"));
  MDNode *DisableNode = MDNode::get(Context, DisableOperands);
  MDs.push_back(DisableNode);
  
  MDNode *NewLoopID = MDNode::get(Context, MDs);
  // Set operand 0 to refer to the loop id itself.
  NewLoopID->replaceOperandWith(0, NewLoopID);
  L->setLoopID(NewLoopID);
  return;
}       
       
bool LoopSPMDization::FindReductionVariables(Loop *L, std::vector<PHINode *> *Reductions, std::vector<Value *> *ReduceVarExitOrig, std::vector<Instruction *> *ReduceVarOrig) {
  int r = 0;
  for (Instruction &I : *L->getHeader()) {
    PHINode *Phi = dyn_cast<PHINode>(&I);
    if (!Phi)
      continue;
    RecurrenceDescriptor RedDes;
    if (RecurrenceDescriptor::isReductionPHI(Phi, L, RedDes)) {
      (*Reductions)[r] = Phi; 
      Value *ReduceVar;
      PHINode *Phiop = Phi;
      PHINode *redoperation;
      if (Phi->getIncomingBlock(0) == L->getLoopPreheader()) {
        ReduceVar = dyn_cast<Value>(Phi->getIncomingValue(1));
        redoperation = dyn_cast<PHINode>(Phiop->getIncomingValue(1));
        (*ReduceVarOrig)[r]= dyn_cast<Instruction>(Phiop->getIncomingValue(1));
      }
      else {
        ReduceVar = dyn_cast<Value>(Phi->getIncomingValue(0));
        redoperation = dyn_cast<PHINode>(Phiop->getIncomingValue(0));
        (*ReduceVarOrig)[r]= dyn_cast<Instruction>(Phiop->getIncomingValue(0));
      }
      while(redoperation) {
        Phiop = redoperation;
        //We could choose 0 or 1 values but we test both to avoid cyclic Phis
        redoperation = dyn_cast<PHINode>(dyn_cast<Instruction>(Phiop->getIncomingValue(0)));
        if(redoperation) { 
          redoperation = dyn_cast<PHINode>(dyn_cast<Instruction>(Phiop->getIncomingValue(1)));
          (*ReduceVarOrig)[r]= dyn_cast<Instruction>(Phiop->getIncomingValue(1));
        }
        else
          (*ReduceVarOrig)[r]= dyn_cast<Instruction>(Phiop->getIncomingValue(0));
      }
      BasicBlock::iterator i, ie; 
      for (i = L->getExitBlock()->begin(), ie = L->getExitBlock()->end();  (i != ie); ++i) {
        PHINode *PhiExit = dyn_cast<PHINode>(&*i);
        if (!PhiExit)
          continue;
        Instruction *ReduceVarExit = dyn_cast<Instruction>(PhiExit->getIncomingValue(0));
        if(dyn_cast<Value>(ReduceVarExit) == dyn_cast<Value>(ReduceVar)) {
          
          (*ReduceVarExitOrig)[r] = dyn_cast<Value>(PhiExit);
        }
      }
      r++;
    }
  }
  return true;
}

// Calculate the identity element of the reduction operation 
// TODO: make it a more exhaustive set
Value *find_reduction_identity(PHINode *Phi, Instruction *Op) {
  Value *Ident;
  Type *Ty = Phi->getType();
  switch (Op->getOpcode()) {
  case Instruction::Add:
  case Instruction::FAdd:
  case Instruction::Sub:
  case Instruction:: FSub:
    {
      Ident =  Constant::getNullValue(Ty);
      break;
    }
  case Instruction::Or:
  case Instruction::Xor:{
    Ident =  Constant::getNullValue(Ty);
    break;
  }
  case Instruction::Mul:{
    Ident = ConstantInt::get(Ty, 1);
    break;
  }
  case Instruction::FMul:{
    Ident = ConstantFP::get(Ty, 1);
    break;
  }
  case Instruction::And:{
    Ident =  Constant::getAllOnesValue(Ty);
    break;
  }
  default:{
    // Doesn't have an identity.
    errs() << "\n";
    errs().changeColor(raw_ostream::BLUE, true);
    errs() << "!! ERROR: COULD NOT PERFORM SPMDization !!\n";
    errs().resetColor();
    errs() << R"help(
                Failed to find the identity element of the reduction operation.

                )help";
    Ident =  nullptr;
    break;
  }
  }
  return Ident;
}

//Handling of reductions
bool LoopSPMDization::FixReductionsIfAny(Loop *L, Loop *OrigL, BasicBlock *E, BasicBlock *AfterLoop, int PE, int NPEs, std::vector<PHINode *> *Reductions, std::vector<Value *> *ReduceVarExitOrig, std::vector<Instruction *> *ReduceVarOrig, std::vector<Instruction *> *OldInsts) {
  BasicBlock *pred_AfterLoop;
  pred_AfterLoop = L->getExitBlock();
  for (Instruction &I : *L->getHeader()) {
    PHINode *Phi = dyn_cast<PHINode>(&I);
    if (!Phi)
      continue;
    RecurrenceDescriptor RedDes;
    unsigned r = 0;
    if (RecurrenceDescriptor::isReductionPHI(Phi, L, RedDes)) {
      Instruction *ReduceVar;
      if (Phi->getIncomingBlock(0) == L->getLoopPreheader()) {
        ReduceVar = dyn_cast<Instruction>(Phi->getIncomingValue(1));
        // initialize the reduction on PE!=0 to identity
        Value *Ident = find_reduction_identity(Phi, (*ReduceVarOrig)[r]);
        if(Ident)
          Phi->setIncomingValue(0, Ident);
        else
          return false;
      }
      else { 
        ReduceVar = dyn_cast<Instruction>(Phi->getIncomingValue(0));
        // initialize the reduction on PE!=0 to identity
        Value *Ident = find_reduction_identity(Phi, (*ReduceVarOrig)[r]);
        if(Ident)
          Phi->setIncomingValue(1, Ident);
        else
          return false;
      }
      BasicBlock::iterator i, ie;
      for (i = AfterLoop->begin(), ie = AfterLoop->end();  (i != ie); ++i) {
        PHINode *PhiExit = dyn_cast<PHINode>(&*i);
        Instruction *NewInstPhi; 
        PHINode *NewPhi;
        Instruction *ReduceVarExit;
        IRBuilder<> B(AfterLoop->getFirstNonPHI());
        bool found_p = false;
        //look for use of the reduced value
        if (!PhiExit) {
          for(unsigned m = 0; m < i->getNumOperands(); m++) {
            if(i->getOperand(m) == (*ReduceVarExitOrig)[r]) {
              ReduceVarExit = dyn_cast<Instruction>(i->getOperand(m));
              if(PE == 1) {
                PhiExit = B.CreatePHI(ReduceVar->getType(), 1, Phi->getName() + "orig");
                PhiExit->addIncoming((*ReduceVarExitOrig)[r], pred_AfterLoop);
                i->setOperand(m, PhiExit);
              }
              NewPhi = B.CreatePHI(ReduceVar->getType(), 1, Phi->getName() + "red");
              NewPhi->addIncoming(ReduceVar, pred_AfterLoop);
              NewInstPhi = dyn_cast<Instruction>(NewPhi);
              found_p = true;
            }
          }
          
        }
        else {// There is an actual Phi node for the reduction var
          if(PhiExit->getNumIncomingValues() >= 2)// == 2) zero trip change
            ReduceVarExit = dyn_cast<Instruction>(PhiExit->getIncomingValue(1));
          else
            ReduceVarExit = dyn_cast<Instruction>(PhiExit->getIncomingValue(0));
          if(ReduceVarExit) { 
            if(dyn_cast<Value>(ReduceVarExit) == (*ReduceVarExitOrig)[r]) {
              NewInstPhi = PhiExit->clone();
              NewPhi = dyn_cast<PHINode>(NewInstPhi);
              
              if(PhiExit->getNumIncomingValues() >= 2) // ==2
                NewPhi->setIncomingValue(1, ReduceVar);
              else
                NewPhi->setIncomingValue(0, ReduceVar);
             
              AfterLoop->getInstList().insert(B.GetInsertPoint(), NewInstPhi);
              found_p = true;
            } 
          }
        }
        // AfterLoop does not contain a use or a phi of use
        BasicBlock::iterator II = i;
        if(!found_p && II == (--(AfterLoop->end())) ) {
          if(PE == 1) {
            PhiExit = B.CreatePHI(ReduceVar->getType(), 1, Phi->getName() + "orig");
            PhiExit->addIncoming((*ReduceVarExitOrig)[r], pred_AfterLoop);
          }
          NewPhi = B.CreatePHI(ReduceVar->getType(), 1, Phi->getName() + "red");
          NewPhi->addIncoming(ReduceVar, pred_AfterLoop);
          NewInstPhi = dyn_cast<Instruction>(NewPhi);
          found_p = true;
        }
        if (found_p) {
          // Handling of the new branches related to the zero trip count
          BasicBlock* BB = AfterLoop;
          for (auto it = pred_begin(BB), et = pred_end(BB); it != et; ++it) {
            BasicBlock* predecessor = *it;
            {// this is the predecessor coming from the zero trip count gard block
              if(NewPhi->getBasicBlockIndex(predecessor) == -1 && predecessor != pred_AfterLoop) {
                Value *Ident = find_reduction_identity(NewPhi, (*ReduceVarOrig)[r]);
                if(Ident)
                  NewPhi->addIncoming(Ident, predecessor);
                else
                  return false;
              }
            }
          } 
          //Phi corresponding to first cloned loop is already there
          if(PE == 1) {
            (*OldInsts)[r] = PhiExit;
            B.SetInsertPoint(AfterLoop->getFirstNonPHI());
          }
          else
            B.SetInsertPoint((*OldInsts)[r]->getNextNode());
          Instruction *NewInst = (*ReduceVarOrig)[r]->clone();
          (*OldInsts)[r]->replaceAllUsesWith(NewInst);
          (*ReduceVarExitOrig)[r]->replaceUsesOutsideBlock(NewInst, AfterLoop);
         
          NewInst->setOperand(1, dyn_cast<Value>((*OldInsts)[r]));
          NewInst->setOperand(0, dyn_cast<Value>(NewInstPhi));
          AfterLoop->getInstList().insert(B.GetInsertPoint(), NewInst);
          (*OldInsts)[r] = NewInst;
          break;
        }
      }
      r++;  
    }//end for r
  }
  return true;
}

/* This routine should made generic and be declared somewhere as public to be used here and in csa backend (Target/CSA/CSALoopIntrinsicExpander.cpp)*/
IntrinsicInst* LoopSPMDization::detectSPMDIntrinsic(Loop *L, LoopInfo *LI, DominatorTree *DT, PostDominatorTree *PDT, int &NPEs, Value *&approach) {

  // Attempts to match a valid SPMDization entry/exit pair with an exit in a
  // given basic block.
  const auto match_pair_from_block = [=, &NPEs, &approach](
                                                           BasicBlock *BB
                                                           ) -> IntrinsicInst * {
    using namespace llvm::PatternMatch;
    
    // The block must be exactly one loop level above the loop.
    if (LI->getLoopDepth(BB) != L->getLoopDepth() - 1) return nullptr;
    
    // And it should post-dominate the loop in order to have a correct exit in
    // it.
    if (!PDT->dominates(BB, L->getHeader())) return nullptr;
    
    // Try to find an exit with a paired entry.
    for (Instruction &exit : *BB) {
      Instruction *entry = nullptr;
      uint64_t NPEs_64;
      if (
          !match(
                 &exit,
                 m_Intrinsic<Intrinsic::csa_spmdization_exit>(m_Instruction(entry))
                 ) || !match(
                             entry,
                             m_Intrinsic<Intrinsic::csa_spmdization_entry>(
                                                                           m_ConstantInt(NPEs_64), m_Value(approach)
                                                                           )
                             )
          ) continue;

      // If one is found, make sure that the entry block is also one loop level
      // above the loop and dominates the loop.
      const BasicBlock *const entry_block = entry->getParent();
      if (LI->getLoopDepth(entry_block) != L->getLoopDepth() - 1) continue;
      if (!DT->dominates(entry_block, L->getHeader())) continue;
      
      IntrinsicInst *const entry_intr = dyn_cast<IntrinsicInst>(entry);
      assert(entry_intr && "Entry intrinsic is not an intrinsic??");
      NPEs = NPEs_64;
      return entry_intr;
    }
    
    return nullptr;
  };
  
  // If there is a parent loop, only look inside of it for exits. Otherwise,
  // look through the entire function.
  if (Loop *const L_parent = L->getParentLoop()) {
    for (BasicBlock *const BB : L_parent->getBlocks()) {
      if (IntrinsicInst *const intr = match_pair_from_block(BB))
        return intr;
    }
  } else {
    for (BasicBlock &BB : *L->getHeader()->getParent()) {
      if (IntrinsicInst *const intr = match_pair_from_block(&BB))
        return intr;
    
    }
  }

  return nullptr;
}

/* This routine has been copied from LoopInterchange.cpp. It has then been modified to accomodate the type of induction variables we are insterested in handling for SPMDization  */
PHINode *LoopSPMDization::getInductionVariable(Loop *L, ScalarEvolution *SE) {
  PHINode *InnerIndexVar = L->getCanonicalInductionVariable();
  if (InnerIndexVar)
    return InnerIndexVar;
  if (L->getLoopLatch() == nullptr || L->getLoopPredecessor() == nullptr)
    return nullptr;
  for (BasicBlock::iterator I = L->getHeader()->begin(); isa<PHINode>(I); ++I) {
    PHINode *PhiVar = cast<PHINode>(I);
    Type *PhiTy = PhiVar->getType();
    if (!PhiTy->isIntegerTy() && !PhiTy->isFloatingPointTy() &&
        !PhiTy->isPointerTy())
      return nullptr;
    if (!PhiTy->isIntegerTy())
      continue;
    if(!SE->isSCEVable(PhiVar->getType()))
      continue;
    const SCEVAddRecExpr *AddRec =
      dyn_cast<SCEVAddRecExpr>(SE->getSCEV(PhiVar));
    if (!AddRec || !AddRec->isAffine())
      continue;
    const SCEV *Step = AddRec->getStepRecurrence(*SE);
    if (!isa<SCEVConstant>(Step))
      continue;
    // Found the induction variable.
    // FIXME: Handle loops with more than one induction variable. Note that,
    // currently, legality makes sure we have only one induction variable.
    return PhiVar;
  }
  return nullptr;
}

bool LoopSPMDization::TransformLoopInitandBound(Loop *L, ScalarEvolution *SE, int PE, int NPEs) {
  PHINode *InductionPHI = getInductionVariable(L, SE);
  BasicBlock *PreHeader = L->getLoopPreheader();
  BasicBlock *Header = L->getHeader();
  BranchInst *PreHeaderBR = cast<BranchInst>(PreHeader->getTerminator());
  BasicBlock *Latch = L->getLoopLatch();
  BranchInst *LatchBR = cast<BranchInst>(Latch->getTerminator());
  if (!InductionPHI) {
    DEBUG(dbgs() << "Failed to find the loop induction variable in one of the loops marked with SPMD intrinsic \n");
    return false;
  }
  IRBuilder<> B(PreHeaderBR);
  if(LatchBR->isConditional())
    Cond = LatchBR->getCondition();
  else
    Cond = (cast<BranchInst>(Header->getTerminator()))->getCondition();
  Instruction *CondI = dyn_cast<Instruction>(Cond);
  if(PE == 0) {
    if (InductionPHI->getIncomingBlock(0) == PreHeader){
      LowerBound = InductionPHI->getIncomingValue(0);
    }
    else {
      LowerBound = InductionPHI->getIncomingValue(1);
    }
    UpperBound = CondI->getOperand(1);
    if(dyn_cast<IntegerType>(nbyk->getType())->getBitWidth() != dyn_cast<IntegerType>(LowerBound->getType())->getBitWidth())
      nbyk = B.CreateZExtOrTrunc(nbyk, LowerBound->getType(), nbyk->getName()+".trex"); 
  }
  //i = i+PE ==> i+ (k-1)n/NPEs ==> i+(k-1)*nbyNPEs
  Value *ktimesnbyk = B.CreateMul(ConstantInt::get(nbyk->getType(), PE),
                                  nbyk,
                                  InductionPHI->getName()+
                                  ".ktimesnbyk"
                                  );
  Value *kplus1 = B.CreateAdd(ConstantInt::get(nbyk->getType(), PE),
                              ConstantInt::get(nbyk->getType(), 1), 
                              InductionPHI->getName()+
                              ".kplus1");
  Value *kplus1timesnbyk = B.CreateMul(kplus1, 
                                       nbyk,
                                       InductionPHI->getName()+
                                       ".k+1xnbyk"
                                       );
  Value *kplus1timesnbyk2 = B.CreateAdd(kplus1timesnbyk, 
                                        LowerBound,
                                        InductionPHI->getName()+
                                        ".k+1xnbyk2"
                                        );
  NewInitV = B.CreateAdd(LowerBound,
                         ktimesnbyk, 
                         InductionPHI->getName()+
                         ".init");
  if (InductionPHI->getIncomingBlock(0) == PreHeader) {
    InductionPHI->setIncomingValue(0, NewInitV );
  }
  else {
    InductionPHI->setIncomingValue(1, NewInitV );
  }
  //change bound (cond)
  if(dyn_cast<IntegerType>(kplus1timesnbyk2->getType())->getBitWidth() != dyn_cast<IntegerType>(UpperBound->getType())->getBitWidth())
    kplus1timesnbyk2 = B.CreateZExtOrTrunc(kplus1timesnbyk2, UpperBound->getType(), kplus1timesnbyk2->getName()+".trex"); 
  
  // this handles the case where the loop enters with an init value equal to the bound
  CmpInst *CmpCond = dyn_cast<CmpInst>(Cond);
  if (CmpCond->getPredicate() == CmpInst::ICMP_EQ || CmpCond->getPredicate() == CmpInst::ICMP_NE) {
    if(LatchBR->getSuccessor(0) == L->getHeader())
      CmpCond->setPredicate(CmpInst::ICMP_SLT);
    else 
      CmpCond->setPredicate(CmpInst::ICMP_SGE);
  }
  
  //Case where the loop iterator is USE after the comparison. This appears in Fortran code where the loop is converted into a while loop. The iterator gets incremented/decremented after the comparison. The new upper bound needs to be decreased in order to avoid executing one iteration twice by each two workers
  bool start_tracking = false;
  bool whileloop_p = false;
  BasicBlock::iterator bi, bie; 
  for (bi = CmpCond->getParent()->begin(), bie = CmpCond->getParent()->end();  (bi != bie); ++bi) {
    Instruction *neighbor = dyn_cast<Instruction>(&*bi);
    for (auto UA = (dyn_cast<Value>(InductionPHI))->user_begin(), EA = (dyn_cast<Value>(InductionPHI))->user_end(); UA != EA;) {
      Instruction *use = cast<Instruction>(*UA++);
      if(start_tracking && neighbor == use) {
        if(use->getOpcode() == Instruction::Add) {
          whileloop_p = true;
          break;
        }
      }
    }
    if(neighbor == CmpCond)
      start_tracking = true; 
  }
  if(whileloop_p)
    kplus1timesnbyk2 = B.CreateAdd(kplus1timesnbyk2, 
                                   ConstantInt::get(kplus1timesnbyk2->getType(), -1),
                                   InductionPHI->getName()+
                                   ".k+1xnbykwhile"
                                   );
  if(PE == NPEs-1) 
    kplus1timesnbyk2 = UpperBound;
  
  CondI->setOperand(1, kplus1timesnbyk2);
  return true;
}



bool LoopSPMDization::TransformLoopInitandStep(Loop *L, ScalarEvolution *SE, int PE, int NPEs) {
  
  PHINode *InductionPHI = getInductionVariable(L, SE);
  BasicBlock *PreHeader = L->getLoopPreheader();
  BasicBlock *Header = L->getHeader();
  BranchInst *PreHeaderBR = cast<BranchInst>(PreHeader->getTerminator());
  BasicBlock *Latch = L->getLoopLatch();
  if (!InductionPHI) {
    errs() << "\n";
    errs().changeColor(raw_ostream::BLUE, true);
    errs() << "!! WARNING: COULD NOT PERFORM SPMDization !!\n";
    errs().resetColor();
    errs() << R"help(
Failed to find the loop induction variable.

)help";
    DEBUG(dbgs() << "Failed to find the loop induction variable \n");
    return false;
  }
  Instruction *OldInc;
  Value *InitVar;
  if (InductionPHI->getIncomingBlock(0) == PreHeader) {
    OldInc = dyn_cast<Instruction>(InductionPHI->getIncomingValue(1));
    InitVar = InductionPHI->getIncomingValue(0);
  }
  else {
    OldInc = dyn_cast<Instruction>(InductionPHI->getIncomingValue(0));
    InitVar = InductionPHI->getIncomingValue(1);
  }
  IRBuilder<> B2(OldInc);
  if(PE == 0) {
    StepPE0 = OldInc->getOperand(1);
    steptimesk = B2.CreateMul(OldInc->getOperand(1),
                              ConstantInt::get(InductionPHI->getType(), NPEs),
                              InductionPHI->getName()+
                              ".steptimesk"
                              );
  }
  Value *NewInc = B2.CreateAdd(InductionPHI,
                               steptimesk,
                               InductionPHI->getName()+".next.spmd");
  
  IRBuilder<> B(PreHeaderBR);
  Value *steptimespe = B.CreateMul(StepPE0,
                                   ConstantInt::get(InductionPHI->getType(), PE),
                                   InductionPHI->getName()+
                                   ".steptimesPE"
                                   );
  /*Value **/NewInitV = B.CreateAdd(InitVar,
                                    steptimespe,
                                    InductionPHI->getName()+
                                    ".init", dyn_cast<Instruction>(NewInc));
  if (InductionPHI->getIncomingBlock(0) == PreHeader) {
    InductionPHI->setIncomingValue(0, NewInitV );
    InductionPHI->setIncomingValue(1, NewInc );
  }
  else {
    InductionPHI->setIncomingValue(1, NewInitV );
    InductionPHI->setIncomingValue(0, NewInc );
  }
  
  BranchInst *LatchBR = cast<BranchInst>(Latch->getTerminator());
  if(LatchBR->isConditional())
    Cond = LatchBR->getCondition();
  else
    Cond = (cast<BranchInst>(Header->getTerminator()))->getCondition();
  /*Value **///Cond = LatchBR->getCondition();
  Instruction *CondI = dyn_cast<Instruction>(Cond);
  bool cond_found_p = false;
  if(CondI->getOperand(0) == dyn_cast<Value>(OldInc) || CondI->getOperand(1) == dyn_cast<Value>(OldInc))
    cond_found_p = true;
  else if(CondI->getOperand(0) == dyn_cast<Value>(InductionPHI) || CondI->getOperand(1) == dyn_cast<Value>(InductionPHI)) {
    cond_found_p = true;
    OldInc = dyn_cast<Instruction>(InductionPHI); 
    NewInc = dyn_cast<Value>(InductionPHI);
  }
  else {
    for (auto UA = (dyn_cast<Value>(OldInc))->user_begin(), EA = (dyn_cast<Value>(OldInc))->user_end(); UA != EA;) {
      Instruction *User_OldInc = cast<Instruction>(*UA++);
      if(CondI->getOperand(0) == dyn_cast<Value>(User_OldInc) || CondI->getOperand(1) == dyn_cast<Value>(User_OldInc)) {
        cond_found_p = true;
        for(unsigned m = 0; m < User_OldInc->getNumOperands(); m++) 
          if(User_OldInc->getOperand(m) == dyn_cast<Value>(OldInc)) { 
            User_OldInc->setOperand(m, NewInc);
            //OldInc->replaceAllUsesWith(NewInc);
            NewInc = User_OldInc;
            OldInc = User_OldInc;
          } 
      }
    }
  }
  if(!cond_found_p) {
    errs() << "\n";
    errs().changeColor(raw_ostream::BLUE, true);
    errs() << "!! WARNING: COULD NOT PERFORM SPMDization !!\n";
    errs().resetColor();
    errs() << R"help(
Failed to find the loop latch condition.

)help";
    return false;
  }
  else {
    Value *TripCount = CondI->getOperand(1);
    Value *IdxCmp;
    CmpInst *CmpCond = dyn_cast<CmpInst>(Cond);
    Value *NewCondOp0, *NewCondOp1;
    if(CondI->getOperand(0) == dyn_cast<Value>(OldInc)) {
      NewCondOp0 = NewInc;
      NewCondOp1 = TripCount;
    }
    else {
      NewCondOp1 = NewInc;
      NewCondOp0 = TripCount;
    }   
    if (CmpCond->getPredicate() == CmpInst::ICMP_EQ || CmpCond->getPredicate() == CmpInst::ICMP_NE) {  
      if(LatchBR->getSuccessor(0) == L->getHeader())
        IdxCmp = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_SLT, 
                                 NewCondOp0, 
                                 NewCondOp1,  
                                 Cond->getName());
    
      else 
        IdxCmp = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_SGE, 
                                 NewCondOp0, 
                                 NewCondOp1, 
                                 Cond->getName());
      ReplaceInstWithInst(CondI, dyn_cast<Instruction>(IdxCmp));
    }
    else { //in other cases, we keep the same predicate
      if(CondI->getOperand(0) == dyn_cast<Value>(OldInc)) 
        CondI->setOperand(0, NewInc);
      else
        CondI->setOperand(1, NewInc);
    }
  }
  return true;
}


bool LoopSPMDization::ZeroTripCountCheck(Loop *L, ScalarEvolution *SE, int PE, int NPEs, BasicBlock *AfterLoop, std::vector<PHINode *> *Reductions, std::vector<Value *> *ReduceVarExitOrig, std::vector<Instruction *> *ReduceVarOrig, DominatorTree *DT, LoopInfo *LI) {
  
  BasicBlock *PreHeader = L->getLoopPreheader();
  BranchInst *PreHeaderBR = cast<BranchInst>(PreHeader->getTerminator());
  BasicBlock *Latch = L->getLoopLatch();
  IRBuilder<> B(PreHeaderBR);
  BranchInst *LatchBR = cast<BranchInst>(Latch->getTerminator());
  Instruction *CondI = dyn_cast<Instruction>(Cond);
  
  Value *TripCount = CondI->getOperand(1);
  Value *IdxCmp;
  CmpInst *CmpCond = dyn_cast<CmpInst>(Cond);
  Instruction *CmpZeroTrip;
  Value *NewCondOp0, *NewCondOp1;
  if(dyn_cast<IntegerType>(NewInitV->getType())->getBitWidth() > dyn_cast<IntegerType>(TripCount->getType())->getBitWidth()){
    auto *Trunc = B.CreateTrunc(NewInitV, TripCount->getType(), NewInitV->getName()+".trunk");
    NewInitV = Trunc;  
  }
  else {
    auto *Trunc = B.CreateSExt(NewInitV, TripCount->getType(), NewInitV->getName()+".sext");
    NewInitV = Trunc;  
  }
  
  if(spmd_approach == SPMD_CYCLIC) { 
    NewCondOp1 = NewInitV;
    NewCondOp0 = TripCount;
  }
  if(spmd_approach == SPMD_BLOCKING) { 
    NewCondOp1 = ConstantInt::get(TripCountV->getType(), PE); 
    NewCondOp0 = TripCountV;
  }
   
  if (CmpCond->getPredicate() == CmpInst::ICMP_EQ || CmpCond->getPredicate() == CmpInst::ICMP_NE) {  
    if((LatchBR->getSuccessor(0) == L->getHeader() && CmpCond->getPredicate() == CmpInst::ICMP_EQ) || (LatchBR->getSuccessor(0) != L->getHeader() && CmpCond->getPredicate() == CmpInst::ICMP_NE))
      IdxCmp = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_SLT, 
                               NewCondOp0, 
                               NewCondOp1,  
                               Cond->getName());
    
    else 
      IdxCmp = CmpInst::Create(Instruction::ICmp, CmpInst::ICMP_SGE, 
                               NewCondOp0, 
                               NewCondOp1, 
                               Cond->getName());
    CmpZeroTrip = dyn_cast<Instruction>(IdxCmp);
  }
  else { //in other cases, we keep the same predicate
    CmpZeroTrip = CondI->clone();
    IdxCmp = dyn_cast<Value>(CmpZeroTrip);
    if(dyn_cast<Instruction>(IdxCmp)->getOperand(1) == TripCount){ 
      dyn_cast<Instruction>(IdxCmp)->setOperand(0, NewCondOp1); 
      dyn_cast<Instruction>(IdxCmp)->setOperand(1, NewCondOp0);
    }
    else {
      dyn_cast<Instruction>(IdxCmp)->setOperand(1, NewCondOp1);
      dyn_cast<Instruction>(IdxCmp)->setOperand(0, NewCondOp0);
    }
  }
  PreHeader->getInstList().insert(B.GetInsertPoint(), dyn_cast<Instruction>(CmpZeroTrip));
              
  //need to distringuish cases
  if(LatchBR->getSuccessor(0) == PreHeaderBR->getSuccessor(0))
    B.CreateCondBr(IdxCmp, PreHeaderBR->getSuccessor(0), AfterLoop);
  else
    B.CreateCondBr(IdxCmp, AfterLoop, PreHeaderBR->getSuccessor(0));
  
  PreHeaderBR->eraseFromParent();
  
  BasicBlock *NewPH = InsertPreheaderForLoop(L, DT, LI, true);
  // Move section entry from .e block to the  new preheader to avoid bad section placement
  IRBuilder<> BPH(NewPH->getFirstNonPHI());
  
  for (Instruction& inst : *NewPH->getSinglePredecessor())
    if (IntrinsicInst *intr_inst = dyn_cast<IntrinsicInst>(&inst))
      if (intr_inst->getIntrinsicID() == Intrinsic::csa_parallel_section_entry) {
        (&inst)->moveBefore(NewPH->getFirstNonPHI());
        break;
      }
  
  return true;
}


bool LoopSPMDization::AddParallelIntrinsicstoLoop(Loop *L, LLVMContext& context, Module *M, BasicBlock *OrigPH, BasicBlock *E) {
  Function* FIntr = Intrinsic::getDeclaration(M, Intrinsic::csa_parallel_region_entry); 
  Instruction*const header_terminator = OrigPH->getTerminator();
  Instruction*const preheader_terminator = L->getLoopPreheader()->getTerminator();
  CallInst *region_entry = IRBuilder<>{header_terminator}.CreateCall(
    FIntr, 
    ConstantInt::get(IntegerType::get(context, 32), 1), 
    "spmd_pre"
                                                                     );
  std::string RegionName = region_entry->getName();
  next_token = context.getMDKindID(RegionName) + 1000;
  region_entry->setOperand(0, ConstantInt::get(IntegerType::get(context, 32), next_token));
  CallInst *section_entry = IRBuilder<>{preheader_terminator}.CreateCall(
    Intrinsic::getDeclaration(M, Intrinsic::csa_parallel_section_entry), 
    region_entry, 
    "spmd_pse"
                                                                         );
  
  //IRBuilder<>{preheader_terminator}.CreateCall(
  //              Intrinsic::getDeclaration(M, Intrinsic::csa_parallel_loop));
  
  // The csa.parallel.region.exit intrinsic goes at the beginning of the loop exit.
  SmallVector<BasicBlock*, 2> exits;
  L->getExitBlocks(exits);
  for (BasicBlock *const exit : exits) {
    IRBuilder<>{exit->getFirstNonPHI()}.CreateCall(
      Intrinsic::getDeclaration(M, Intrinsic::csa_parallel_section_exit),
      section_entry
                                                   );
  }
  IRBuilder<>{E->getFirstNonPHI()}.CreateCall(
    Intrinsic::getDeclaration(M, Intrinsic::csa_parallel_region_exit), 
    region_entry
                                              );
  
  return true;
}


