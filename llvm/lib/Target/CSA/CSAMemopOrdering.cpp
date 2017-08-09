//===- CSAMemopOrdering.cpp - CSA Memory Operation Ordering ----*- C++ -*--===//
//
//===----------------------------------------------------------------------===//
//
// This file implements a machine function pass for the CSA target that
// ensures that memory operations occur in the correct order.
//
//===----------------------------------------------------------------------===//

#include "CSA.h"
#include "CSATargetMachine.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SmallPtrSet.h"
#include "llvm/Analysis/AliasSetTracker.h"
#include "llvm/CodeGen/MachineDominators.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/CodeGen/MachineSSAUpdater.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/CodeGen/MachineInstrBuilder.h"
#include "MachineCDG.h"

using namespace llvm;

// Flag for controlling code that deals with memory ordering.
enum OrderMemopsMode {
  // No extra code added at all for ordering.  Often incorrect.
  none = 0,

  // Linear ordering of all memops.  Dumb but should be correct.
  linear = 1,

  //  Stores inside a basic block are totally ordered.
  //  Loads ordered between the stores, but
  //  unordered with respect to each other.
  //  No reordering across basic blocks.
  wavefront = 2,
};

static cl::opt<OrderMemopsMode>
OrderMemopsType("csa-order-memops-type",
                cl::Hidden,
                cl::desc("CSA Specific: Order memory operations"),
                cl::values(clEnumVal(none,
                                     "No memory ordering. Possibly incorrect"),
                           clEnumVal(linear,
                                     "Linear ordering. Dumb but correct"),
                           clEnumVal(wavefront,
                                     "Totally ordered stores, parallel loads between stores.")),
                cl::init(OrderMemopsMode::wavefront));

//  Boolean flag.  If it is set to 0, we force "none" for memory
//  ordering.  Otherwise, we just obey the OrderMemopsType variable.
static cl::opt<int>
OrderMemops("csa-order-memops",
            cl::Hidden, cl::ZeroOrMore,
            cl::desc("CSA Specific: Disable ordering of memory operations (by setting to 0)"),
            cl::init(1));

static cl::opt<bool>
KillReadChains("csa-kill-readchains",
            cl::Hidden,
            cl::desc("CSA-specific: kill ordering chains which only link reads"),
            cl::init(false));

// The register class we are going to use for all the memory-op
// dependencies.  Technically they could be I0, but I don't know how
// happy LLVM will be with that.
const TargetRegisterClass* MemopRC = &CSA::I1RegClass;


// These values are used for tuning LLVM datastructures; correctness is not at
// stake if they are off.
// Width of vectors we are using for memory op calculations.
#define MEMDEP_VEC_WIDTH 8
// A guess at the number of memops per alias set per function.
#define MEMDEP_OPS_PER_SET 32

#define DEBUG_TYPE "csa-memop-ordering"

// TODO: memop ordering statistics?
//STATISTIC(NumInlineAsmExpansions, "Number of asm()s expanded into MIs");
//STATISTIC(NumInlineAsmInstrs,     "Number of machine instructions resulting from asm()s");

namespace {

  /* This wraps AliasSetTracker, and gives a less sophisticated interface. Its
   * advantage is that it can handle PseudoSourceValues, such as frame index
   * pointers, which do not appear in IR and are not represented by Values. The
   * intended usage is as follows:
   * 1. Populate all memory ops with add()
   * 2. Query only the total number of alias sets, or the alias set number for
   *    a given MachineMemOperand. You cannot get an underlying AliasSet from
   *    MachineAliasSetTracker.
   *
   * It is illegal to query a memory op which you have not previously add()ed.
   *
   * */
  class MachineAliasSetTracker {
    public:
      MachineAliasSetTracker(AliasAnalysis &aa, MachineFrameInfo *mfi) :
        AST(aa), MFI(mfi), isMerged(false), pseudosCounter(1) { }

      /* Use 'add' to populate the tracker with pointers. */
      void add(MachineMemOperand *mop);
      /* Query the number of effective alias sets. */
      unsigned getNumAliasSets();
      /* Query the opaque ID of the set associated with a given mem op */
      unsigned getAliasSetNumForMemop(const MachineMemOperand *mop);
      void dump() const { print(dbgs()); }
      void print(raw_ostream &OS) const;


    private:
      AliasSetTracker AST;
      MachineFrameInfo *MFI;
      bool isMerged;
      std::map<const PseudoSourceValue*, unsigned> pseudos;
      unsigned pseudosCounter;
  };

  class CSAMemopOrdering : public MachineFunctionPass {
    bool runOnMachineFunction(MachineFunction &MF) override;
    const TargetMachine *TM;
    const CSASubtarget *STI;
    const MCInstrInfo  *MII;
    const MachineLoopInfo *MLI;

  public:
    static char ID;
    CSAMemopOrdering() : MachineFunctionPass(ID) { }
    StringRef getPassName() const override {
      return "CSA Memory Operation Ordering";
    }

    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.addRequired<ControlDependenceGraph>();
      AU.addRequired<AAResultsWrapperPass>();
      AU.addRequired<MachineLoopInfo>();
      AU.addRequired<MachineDominatorTree>();
      AU.addRequired<MachinePostDominatorTree>();
      AU.setPreservesAll();
      MachineFunctionPass::getAnalysisUsage(AU);
    }

  private:
    AliasAnalysis *AA;
    MachineAliasSetTracker *AS;
    MachineBasicBlock *entryBB;
    ControlDependenceGraph *CDG;
    const CSAInstrInfo *TII;
    MachineRegisterInfo *MRI;
    MachineDominatorTree *DT;
    MachinePostDominatorTree *PDT;

    // For each chain, maintain 4 items:
    // start:    the first vreg in its chain
    // updater:  a MachineSSAUpdater
    // uses:     a collection of all of its uses
    // readonly: a bool: are all memops reads?
    struct depchain {
      unsigned start;
      std::unique_ptr<MachineSSAUpdater> updater;
      SmallPtrSet<MachineOperand*, MEMDEP_OPS_PER_SET> uses;
      bool readonly;
    };

    typedef DenseMap<unsigned, depchain> AliasSetDepchain;
    typedef DenseMap<unsigned, unsigned> AliasSetVReg;

    void addMemoryOrderingConstraints(MachineFunction* thisMF);

    // Helper methods:

    // Update a memory instruction by setting ordering operands in-place.
    //  issued_reg is the register to define as the extra output
    //  ready_reg is the register which is the extra input
    //  If non-NULL, ready_op_num will have the operand number which uses the
    //  ready_reg stored to it.
    void order_memop_ins(
      MachineInstr& memop,
      unsigned issued_reg, unsigned ready_reg,
      unsigned *ready_op_num
    );

    // Create a dependency chain in virtual registers through the
    // basic block BB.
    //
    //   mem_in_reg is the virtual register number being used as
    //   input, i.e., the "source" for all the memory ops in this
    //   block.
    //
    //   This function returns the virtual register that is the "sink"
    //   of all the memory operations in this block.  The returned
    //   register might be the same as the source "mem_in_reg" if
    //   there are no memory operations in this block.
    //
    // This method also converts the LD/ST instructions into OLD/OST
    // instructions, as they are encountered.
    //
    // linear version of this function links all memory operations in
    // the block together in a single chain.
    //
    void convert_block_memops_linear(MachineBasicBlock& BB,
        AliasSetDepchain& depchains);

    // Wavefront version.   Same conceptual functionality as linear version,
    // but more optimized.
    //
    // Only serializes stores in a block, but allows loads to occur in
    // parallel between stores.
    void convert_block_memops_wavefront(MachineBasicBlock& BB,
        AliasSetDepchain& depchains);

    // Merge all the .i1 registers stored in "current_wavefront" into
    // a single output register.
    // Returns the output register, or "input_mem_reg" if
    // current_wavefront is empty.
    //
    // Note that this method has several side-effects:
    //  (a) It inserts the merge instructions after
    //      instruction MI in BB, or before the last terminator in the
    //      block if MI == NULL, and
    //  (b) It clears current_wavefront.
    unsigned merge_dependency_signals(MachineBasicBlock& BB,
                                      MachineInstr* MI,
                                      SmallVector<unsigned, MEMDEP_VEC_WIDTH>* current_wavefront,
                                      unsigned input_mem_reg);

    // Helper routines for discovering/associating intrinsics.
    bool isLoopDomByIntrinsic(MachineLoop *l,
                              unsigned opcode,
                              unsigned &token) const;
    bool isLoopPostDomByIntrinsic(MachineLoop *l,
                                  unsigned opcode,
                                  unsigned token) const;
    // Determine if a val is trivially derived from an ancestor vreg,
    // accounting for PHI nodes. Mov/copy transforms are not currently
    // accounted for.
    bool isDerivedFrom(unsigned val, unsigned ancestor) const;

    // Return true if the specified loop has been annotated as parallel in the
    // source code.
    bool isParallelLoop(MachineLoop* loop) const;

    // Determine if the specified basic block contains a the specified
    // operation. (An instruction with the given opcode.) Return the
    // instruction if found, or nullptr otherwise.
    const MachineInstr* isOpInBlockOp(const MachineBasicBlock& BB, unsigned op) const;

    // Traverse the PHI nodes that were inserted by the ssa updater.
    // For PHI nodes that belong to loops that have been annotated as
    // parallel, mark the incoming PHI edges that represent memory-order backedges
    // by inserting a CSA_PARALLEL_MEMDEP psuedo-op between the definition of
    // the edge and the PHI.
    void markParallelLoopBackedges(MachineFunction *thisMF,
                                   const SmallVectorImpl<MachineInstr*>& inserted_PHIs);

    // Determine whether a given instruction should be assigned ordering.
    // This is the case if it has a memory operand and if its last use and last def
    // are both %ign.
    bool should_assign_ordering(const MachineInstr& MI) const;
  };
}

char CSAMemopOrdering::ID = 0;

MachineFunctionPass *llvm::createCSAMemopOrderingPass() {
  return new CSAMemopOrdering();
}

bool CSAMemopOrdering::runOnMachineFunction(MachineFunction &MF) {

  if (!OrderMemops || (OrderMemopsType <= OrderMemopsMode::none)) {
    return false;
  }

  TII = static_cast<const CSAInstrInfo*>(MF.getSubtarget().getInstrInfo());
  MRI = &MF.getRegInfo();

  AA = &getAnalysis<AAResultsWrapperPass>().getAAResults();
  CDG = &getAnalysis<ControlDependenceGraph>();

  MLI = &getAnalysis<MachineLoopInfo>();
  DT = &getAnalysis<MachineDominatorTree>();
  PDT = &getAnalysis<MachinePostDominatorTree>();

  // Find the entry block. (Surely there's an easier way to do this?)
  ControlDependenceNode *rootN = CDG->getRoot();
  if(!(rootN && *rootN->begin())) return false;
  entryBB = (*rootN->begin())->getBlock();
  assert(entryBB && "Couldn't determine this function's entry block");

  // Create the AliasSetTracker and populate with all basic blocks.
  MachineAliasSetTracker AST(*AA, &MF.getFrameInfo());
  AS = &AST;
  for (MachineBasicBlock &MB : MF) {
    for (MachineInstr &MI : MB) {
      for (MachineMemOperand *op : MI.memoperands()) {
        AS->add(op);
      }
    }
  }
  DEBUG(errs() << "AliasSets for function " << MF.getName() << ":\n");
  DEBUG(AS->dump());

  // This step should run before the main dataflow conversion because
  // it introduces extra dependencies through virtual registers than
  // the dataflow conversion must also deal with.
  addMemoryOrderingConstraints(&MF);

  return true;
}

void CSAMemopOrdering::addMemoryOrderingConstraints(MachineFunction *thisMF) {

  const unsigned MemTokenMOVOpcode = TII->getMemTokenMOVOpcode();

  AliasSetDepchain depchains;
  SmallVector<MachineInstr*, 16> inserted_PHIs;  // Phi nodes from ssa updaters

  // Initialize the maps.
  for (unsigned set=0, e=AS->getNumAliasSets(); set<e; ++set) {
    // Create the start of the chain for each alias set.
    depchains[set].start = MRI->createVirtualRegister(MemopRC);
    BuildMI(*entryBB,
        entryBB->getFirstNonPHI(),
        DebugLoc(),
        TII->get(MemTokenMOVOpcode),
        depchains[set].start).addImm(1);
    // Initialize an SSAUpdater for each set.
    depchains[set].updater.reset(new MachineSSAUpdater(*thisMF, &inserted_PHIs));
    depchains[set].updater->Initialize(depchains[set].start);
    depchains[set].updater->AddAvailableValue(entryBB, depchains[set].start);
    depchains[set].readonly = KillReadChains;
  }

  // An extra loop over all memops to determine which alias sets consist only
  // of reads.
  for (MachineBasicBlock &BB : *thisMF) {
    for (MachineInstr &MI : BB) {

      // If this instruction doesn't have memory operands, we're done.
      if (MI.memoperands_empty())
        continue;

      assert(MI.hasOneMemOperand() && "Can't handle multiple-memop ordering");
      const MachineMemOperand *mOp = *MI.memoperands_begin();

      // Use AliasAnalysis to determine which ordering chain we should be on.
      unsigned as = AS->getAliasSetNumForMemop(mOp);

      // Update the chain's readonly status.
      depchains[as].readonly &= TII->isLoad(&MI);
    }
  }

  DEBUG(errs() << "Before addMemoryOrderingConstraints");
  for (MachineBasicBlock &BB : *thisMF) {

    // Link all the memory ops in BB together.
    // Return the name of the last output register (which could be
    // mem_in_reg).
    switch (OrderMemopsType) {
    case OrderMemopsMode::wavefront:
      {
        convert_block_memops_wavefront(BB, depchains);
      }
      break;
    case OrderMemopsMode::linear:
      {
        convert_block_memops_linear(BB, depchains);
      }
      break;

      // We should never get here.
    case OrderMemopsMode::none:
    default:
      assert(0 && "Only linear and wavefront memory ordering implemented now.");

    }

    DEBUG(errs() << "After memop conversion of basic block: " << BB << "\n");
  }

  // Create a mov to consume the end of all of each chain. We'll need one in
  // each terminating basic block. (We are still thinking control flow here.)
  // Note that using RI1 register class should keep this on the SXU.  Even
  // though we allocate a separate virtual register for each one, LLVM in the
  // end is free to re-use the same physical register since the values are dead
  // after each def.
  for (MachineBasicBlock &BB : *thisMF) {
    if (!BB.isReturnBlock())
      continue;

    for (unsigned set=0, e=AS->getNumAliasSets(); set<e; ++set) {
      unsigned depchain_end = MRI->createVirtualRegister(&CSA::RI1RegClass);
      unsigned prev_chain_reg = depchains[set].start;
      MachineInstr* endMov = BuildMI(BB, BB.getFirstTerminator(), DebugLoc(), TII->get(MemTokenMOVOpcode), depchain_end).addReg(prev_chain_reg);
      depchains[set].uses.insert(endMov->operands_end()-1);
    }
  }

  // Finally, use the updater for each set to fully rewrite to SSA. This
  // includes generating PHI nodes.
  for (unsigned set=0, e=AS->getNumAliasSets(); set<e; ++set) {
    for (MachineOperand *op : depchains[set].uses) {
      // There is an exception here: RewriteUse is not smart enough to find new
      // values added by "AddAvailableValue" before a use in the same basic
      // block. (I.e., it can only find them if they are in a basic block which
      // is a strict dominator.) This is only an issue when the use is in the
      // function's entry block. Fortunately, in this case, we can be sure that
      // depchain_reg contained the right value to use.
      if(op->getParent()->getParent() == entryBB)
        continue;

      depchains[set].updater->RewriteUse(*op);
    }
  }

  // Traverse the PHI nodes that were inserted by the ssa updater.
  // For PHI nodes that belong to loops that have been annotated as
  // parallel, mark the incoming PHI edges that represent memory-order backedges
  // by inserting a CSA_PARALLEL_MEMDEP psuedo-op between the definition of
  // the edge and the PHI.
  markParallelLoopBackedges(thisMF, inserted_PHIs);
}

void
CSAMemopOrdering::convert_block_memops_wavefront(MachineBasicBlock& BB,
    AliasSetDepchain& depchains) {
  DEBUG(errs() << "Wavefront memory ordering for block " << BB << "\n");

  // Save the latest evolution of each alias set's memory chain here.
  AliasSetVReg depchain_reg;
  // Also save a wavefront of load output signals per alias set.
  DenseMap<unsigned, SmallVector<unsigned, MEMDEP_VEC_WIDTH> > wavefront;

  for (MachineInstr& MI : BB) {
    DEBUG(errs() << "Found instruction: " << MI << "\n");

    // If this instruction shouldn't be ordered, we're done.
    if (not should_assign_ordering(MI)) continue;

    assert(MI.hasOneMemOperand() && "Can't handle multiple-memop ordering");
    const MachineMemOperand *mOp = *MI.memoperands_begin();

    // Use AliasAnalysis to determine which ordering chain we should be on.
    unsigned as = AS->getAliasSetNumForMemop(mOp);

    // If this chain consists only of readonly access, then it is unnecessary.
    // This is a stronger requirement than is necessary.
    if (depchains[as].readonly) continue;

    // Create a new vreg which will be written to as the next link of the
    // chain.
    unsigned next_mem_reg = MRI->createVirtualRegister(MemopRC);

    // If the previous link is the first into this block, we use the vreg which
    // the updater was initialized with. The use will be saved, and then the
    // updater will fix it up later. Otherwise, just use an output created in
    // this block, which won't need updating.
    if (!depchain_reg[as])
      depchain_reg[as] = depchains[as].start;

    bool is_load = TII->isLoad(&MI);
    if (is_load) {
      // Just a load. Build up the set of load outputs that we depend on.
      wavefront[as].push_back(next_mem_reg);
    }
    else {
      // This is a store or atomic instruction.  If there were any loads in the
      // last interval, merge all their outputs into one output, and change the
      // latest source.
      depchain_reg[as] = merge_dependency_signals(BB,
          &MI,
          &wavefront[as],
          depchain_reg[as]);

      // Update the SSA updater.
      depchains[as].updater->AddAvailableValue(&BB, depchain_reg[as]);

      // We have merged/consumed all pending load outputs.
      assert(wavefront[as].size() == 0);
    }

    unsigned newOpNum;
    order_memop_ins(MI, next_mem_reg, depchain_reg[as], &newOpNum);

    // If the instruction uses a value coming into the block, then it will need
    // to be fixed by MachineSSAUpdater later. Save the operand to the list to
    // do this later.
    MachineOperand *newOp = &MI.getOperand(newOpNum);
    assert(newOp && newOp->isReg() && newOp->isUse());
    if (newOp->getReg() == depchains[as].start) {
      depchains[as].uses.insert(newOp);
    }

    if (!is_load) {
      // Advance the chain.
      depchain_reg[as] = next_mem_reg;

      // Update the SSA updater.
      depchains[as].updater->AddAvailableValue(&BB, next_mem_reg);
    }
  }

  for (auto &pair : wavefront) {
    unsigned as = pair.first;
    unsigned next_mem_reg = depchain_reg[as];
    assert(next_mem_reg);

    // Sink any loads at the end of the block to the end of the block.
    next_mem_reg = merge_dependency_signals(BB,
        NULL,
        &pair.second,
        depchain_reg[as]);
    depchain_reg[as] = next_mem_reg;

    // Update the SSA updater.
    depchains[as].updater->AddAvailableValue(&BB, next_mem_reg);
  }
}

void CSAMemopOrdering::convert_block_memops_linear(MachineBasicBlock& BB,
    AliasSetDepchain& depchains) {
  // Save the latest evolution of each alias set's memory chain here.
  AliasSetVReg depchain_reg;

  for (MachineInstr& MI : BB) {
    DEBUG(errs() << "Found instruction: " << MI << "\n");

    // If this instruction shouldn't be ordered, we're done.
    if (not should_assign_ordering(MI)) continue;

    assert(MI.hasOneMemOperand() && "Can't handle multiple-memop ordering");
    const MachineMemOperand *mOp = *MI.memoperands_begin();

    // Use AliasAnalysis to determine which ordering chain we should be on.
    unsigned as = AS->getAliasSetNumForMemop(mOp);

    // If this chain consists only of readonly access, then it is unnecessary.
    // This is a stronger requirement than is necessary.
    if (depchains[as].readonly) continue;

    // Create a new vreg which will be written to as the next link of the
    // chain.
    unsigned next_mem_reg = MRI->createVirtualRegister(MemopRC);

    // If the previous link is the first into this block, we use the vreg which
    // the updater was initialized with. The use will be saved, and then the
    // updater will fix it up later. Otherwise, just use an output created in
    // this block, which won't need updating.
    if (!depchain_reg[as])
      depchain_reg[as] = depchains[as].start;

    // Hook this instruction into the chain, connecting the previous and next
    // values. The operand number using the old value is saved to newOpNum.
    unsigned newOpNum;
    order_memop_ins(MI, next_mem_reg, depchain_reg[as], &newOpNum);

    // If the instruction uses a value coming into the block, then it will need
    // to be fixed by MachineSSAUpdater later. Save the operand to the list to
    // do this later.
    MachineOperand *newOp = &MI.getOperand(newOpNum);
    assert(newOp && newOp->isReg() && newOp->isUse());
    if (newOp->getReg() == depchains[as].start) {
      depchains[as].uses.insert(newOp);
    }

    // Advance the chain.
    depchain_reg[as] = next_mem_reg;

    // Update the SSA updater, advising it on the latest value in this
    // evolution coming out of this BB.
    depchains[as].updater->AddAvailableValue(&BB, next_mem_reg);
  }
}

unsigned CSAMemopOrdering::merge_dependency_signals(MachineBasicBlock& BB,
                                                  MachineInstr* MI,
                                                  SmallVector<unsigned, MEMDEP_VEC_WIDTH>* current_wavefront,
                                                  unsigned input_mem_reg) {

  if (current_wavefront->size() > 0) {
    DEBUG(errs() << "Merging dependency signals from " << current_wavefront->size() << " register " << "\n");

    // BFS-like algorithm for merging the registers together.
    // Merge consecutive pairs of dependency signals together,
    // and push the output into "next_level".
    SmallVector<unsigned, MEMDEP_VEC_WIDTH> tmp_buffer;
    SmallVector<unsigned, MEMDEP_VEC_WIDTH>* current_level;
    SmallVector<unsigned, MEMDEP_VEC_WIDTH>* next_level;

    current_level = current_wavefront;
    next_level = &tmp_buffer;

    while (current_level->size() > 1) {
      assert(next_level->size() == 0);
      for (unsigned i = 0; i < current_level->size(); i+=2) {
        // Merge current_level[i] and current_level[i+1] into
        // next_level[i/2]
        if ((i+1) < current_level->size()) {

          // Even case: we have a pair to merge.  Create a virtual
          // register + instruction to do the merge.
          unsigned next_out_reg = MRI->createVirtualRegister(MemopRC);
          MachineInstr* new_inst;
          if (MI) {
            new_inst = BuildMI(*MI->getParent(),
                               MI,
                               MI->getDebugLoc(),
                               TII->get(CSA::MERGE1),
                               next_out_reg).addImm(0).addReg((*current_level)[i]).addReg((*current_level)[i+1]);
          }
          else {
            // Adding a merge at the end of the block.
            new_inst = BuildMI(BB,
                               BB.getFirstTerminator(),
                               DebugLoc(),
                               TII->get(CSA::MERGE1),
                               next_out_reg).addImm(0).addReg((*current_level)[i]).addReg((*current_level)[i+1]);
          }
          DEBUG(errs() << "Inserted dependecy merge instruction " << *new_inst << "\n");
          next_level->push_back(next_out_reg);
        }
        else {
          // In an odd case, just pass register through to next level.
          next_level->push_back((*current_level)[i]);
        }
      }

      // Swap next and current.
      std::swap(current_level, next_level);
      next_level->clear();

      DEBUG(errs() << "Current level size is now " << current_level->size() << "\n");
      DEBUG(errs() << "Next level size is now " << next_level->size() << "\n");
    }

    assert(current_level->size() == 1);
    unsigned ans = (*current_level)[0];

    // Clear both vectors, just to be certain.
    current_level->clear();
    next_level->clear();

    return ans;
  }
  else {
    return input_mem_reg;
  }
}

void CSAMemopOrdering::order_memop_ins(
  MachineInstr& MI,
  unsigned issued_reg, unsigned ready_reg,
  unsigned *ready_op_num = nullptr
) {
  using namespace std;

  DEBUG(errs() << "We want convert this instruction.\n");
  for (unsigned i = 0; i < MI.getNumOperands(); ++i) {
    MachineOperand& MO = MI.getOperand(i);
    DEBUG(errs() << "  Operand " << i << ": " << MO << "\n");
  }
  assert(should_assign_ordering(MI));

  // Just update the last use and last def
  prev(end(MI.defs()))->ChangeToRegister(issued_reg, true);
  prev(end(MI.uses()))->ChangeToRegister(ready_reg, false);

  // ...and if requested, save the ready_op for the caller.
  if(ready_op_num != nullptr)
    *ready_op_num = MI.getNumOperands() - 1;

  DEBUG(errs() << "   Updated instruction: " << MI << "\n");

  for (unsigned i = 0; i < MI.getNumOperands(); ++i) {
    MachineOperand& MO = MI.getOperand(i);
    DEBUG(errs() << "  Operand " << i << ": " << MO << "\n");
  }
}

bool CSAMemopOrdering::isDerivedFrom(unsigned val, unsigned ancestor) const {
  if(val == ancestor)
    return true;

  assert(MRI->hasOneDef(val) && "Expecting an SSA vreg");
  for(MachineInstr &defMI : MRI->def_instructions(val)) {
    // If not a PHI, then we're not going to trace backward any further.
    if(!defMI.isPHI())
      return false;

    unsigned numPhiOperands = defMI.getNumOperands();
    for(unsigned i=1; i<numPhiOperands; i+= 2) {
      MachineOperand &phiData = defMI.getOperand(i);
      if(phiData.isReg()) {
        unsigned newAncestor = phiData.getReg();
        // Return true if any incoming PHI values are themselves descendants.
        if(isDerivedFrom(newAncestor, ancestor))
          return true;
      }
    }
  }

  return false;
}

bool CSAMemopOrdering::isLoopDomByIntrinsic(MachineLoop* loop, unsigned op,
                                            unsigned &token) const
{
  auto *cur = DT->getNode(loop->getHeader());
  for(cur = cur->getIDom(); cur; cur = cur->getIDom()) {
    MachineBasicBlock *dom = cur->getBlock();

    // Stop if we leave the required nesting level. The matching intrinsic
    // should only be in the immediately surrounding loop nest level.
    if (MLI->getLoopDepth(dom) != loop->getLoopDepth()-1)
      break;

    const MachineInstr *inst = isOpInBlockOp(*dom, op);
    if (inst!=nullptr) {
      token = inst->getOperand(0).getReg();

      DEBUG(errs() << "\t=> Maybe! Dominating intrinsic in  " << dom->getName()
          << " (BB " << dom->getNumber() << ") at depth " <<
          MLI->getLoopDepth(dom) << " suggests it. Token vreg=" << token <<
          ".\n");
      DEBUG(errs() << "Inst is " << *inst);
      assert(inst->getNumOperands()==2 && inst->getOperand(0).isReg());

      return true;
    }
  }

  DEBUG(errs() << "\t<= No parallel annotation found.\n");
  return false;
}

bool CSAMemopOrdering::isLoopPostDomByIntrinsic(MachineLoop* loop, unsigned op,
                                                unsigned token) const
{
  auto *cur = PDT->getNode(loop->getHeader());
  for(cur = cur->getIDom(); cur; cur = cur->getIDom()) {
    MachineBasicBlock *dom = cur->getBlock();

    // Stop if we leave the required nesting level. The matching intrinsic
    // should only be in the immediately surrounding loop nest level.
    if (MLI->getLoopDepth(dom) != loop->getLoopDepth()-1)
      break;

    const MachineInstr *inst = isOpInBlockOp(*dom, op);
    if (inst!=nullptr) {
      DEBUG(errs() << "Post-dom'ing inst is " << *inst);
      assert(inst->getNumOperands()==1 && inst->getOperand(0).isReg());
      unsigned candidateToken = inst->getOperand(0).getReg();
      if (isDerivedFrom(candidateToken, token)) {
        DEBUG(errs() << "\t=> Yes! Post-dominator " << dom->getName() << " (BB "
            << dom->getNumber() << ") at depth " << MLI->getLoopDepth(dom) <<
            " says so. Token vreg=" << token << ".\n");
        return true;
      } else {
        DEBUG(errs() << "\t=> There's an intrinsic, but wrong token.\n");
      }
    }
  }

  DEBUG(errs() << "\t<= No parallel annotation found.\n");
  return false;
}

bool CSAMemopOrdering::isParallelLoop(MachineLoop* loop) const
{
  DEBUG(errs() << "Is Loop \"" << loop->getHeader()->getName() << "\" (BB " <<
    loop->getHeader()->getNumber() << ") at depth " <<
    loop->getLoopDepth() << " parallel?\n");

  unsigned regionToken;
  bool hasEntry = isLoopDomByIntrinsic(loop, CSA::CSA_PARALLEL_REGION_ENTRY, regionToken);
  DEBUG(errs() << "\tIs it dominated by a begin intrinsic? " <<
      (hasEntry ?  "yes" : "no") << "\n");
  if (hasEntry) {
    bool hasExit = isLoopPostDomByIntrinsic(loop, CSA::CSA_PARALLEL_REGION_EXIT, regionToken);
    DEBUG(errs() << "\tIs it post-dominated by an end intrinsic? " <<
        (hasExit ? "yes" : "no") << "\n");
    if (hasExit)
      DEBUG(errs() << "@@>> Loop \"" << loop->getHeader()->getName() << "\" (BB " <<
        loop->getHeader()->getNumber() << ") at depth " <<
        loop->getLoopDepth() << " IS parallel! <<@@\n");
    return hasExit;
  }

  return false;
}

const MachineInstr* CSAMemopOrdering::isOpInBlockOp(const MachineBasicBlock& BB,
                                                unsigned op) const {
  // Return true if the specified basic block contains an instruction with the
  // specified opcode.

  // Find a machine instruction with given opcode in this basic block.
  auto MIiter = std::find_if(BB.begin(), BB.end(),
                             [op](const MachineInstr& MI) {
                               return MI.getOpcode() == op;
                             });

  if (MIiter != BB.end()) {
    return &*MIiter;  // Found the op
  }

  return nullptr; // Didn't find the op
}

void CSAMemopOrdering::markParallelLoopBackedges(MachineFunction *thisMF,
                                                 const SmallVectorImpl<MachineInstr*>& inserted_PHIs)
{
  DEBUG(errs() << "%%% Before markParallelLoopBackedges\n");
  DEBUG(thisMF->dump());

  // Loop through the newly inserted PHI nodes, looking for the ones that are
  // in the header of a parallel loop.
  DEBUG(errs() << "%%% Inserted PHIs\n");
  for (MachineInstr* PHI : inserted_PHIs) {
    DEBUG(errs() << "    " << *PHI);

    // Get the parallel loop in which the PHI was defined, if any
    const MachineBasicBlock* BB = PHI->getParent();
    MachineLoop* phiLoop = MLI->getLoopFor(BB);
    if (! phiLoop) {
      DEBUG(errs() << "        Not in a loop\n");
      continue;  // Ignore PHIs not in a loop
    }
    else if (phiLoop->getHeader() != PHI->getParent()) {
      DEBUG(errs() << "        In a loop, but not in loop header block\n");
      continue;
    }
    else if (! isParallelLoop(phiLoop)) {
      DEBUG(errs() << "        In serial " << *phiLoop);
      continue;
    }
    else {
      DEBUG(errs() << "        In parallel " << *phiLoop);
    }

    // A PHI machine instruction has 5 or more arguments as follows:
    //  Operand 0: output
    //  Operand 1: input register 1
    //  Operand 2: Last basic block on  control flow breanch for register 1 definition
    //  Operand 3: second input register
    //  Operand 2: Last basic block on  control flow breanch for register 2 definition
    //  ...
    //  Operand 2*N-1: input register N
    //  Operand 2*N:   Last basic block on  control flow breanch for register N definition

    // We are interested in edges that come from the same loop as the PHI. Those edges are the back
    // edges that we want to label.  Because these PHIs were inserted as a result of memory
    // ordering, it no more than one edge should come from the same loop as the PHI; if more than
    // one edge *does* come from the same loop as the PHI, we'll ignore this PHI, for now.
    MachineOperand*    backedgeOperand = nullptr;
    auto operandIter = PHI->operands_begin();
    auto operandEnd  = PHI->operands_end();
    ++operandIter;   // Skip output operand
    while (operandIter != operandEnd) {
      MachineOperand& regOperand = *operandIter++;
      assert(regOperand.isReg() && regOperand.getReg());

      MachineOperand& bbOperand  = *operandIter++;
      assert(bbOperand.isMBB());

      MachineLoop* fromLoop = MLI->getLoopFor(bbOperand.getMBB());

      if (fromLoop == phiLoop) {
        if (backedgeOperand != nullptr) {
          DEBUG(errs() <<
                "%%% Ignored PHI where more than one input is from the same loop as the PHI: " <<
                *PHI);
          continue;
        }

        backedgeOperand = &regOperand;
      }
    }

    if (backedgeOperand == nullptr)
      continue;  // No back edges from same loop were detected

    // Found a back edge from same loop as the PHI. Get register
    unsigned backedgeReg = backedgeOperand->getReg();

    // Mark the back edge by inserting a .csa_parallel_memdep psuedo-op before it.
    // The new instruction is inserted on the output of the operation that originally defined
    // backedgeReg; consumers of backedgeReg are not changed. The new instruction is inserted into
    // the same BB as the operation that originally defined it, so that the SSA form does not need
    // to be adjusted.
    assert(MRI->hasOneDef(backedgeReg));
    MachineOperand&    backedgeDef      = *MRI->def_begin(backedgeReg);
    MachineInstr*      backedgeDefInstr = backedgeDef.getParent();
    MachineBasicBlock* backedgeDefBB    = backedgeDefInstr->getParent();
    unsigned           newMemdepReg     = MRI->createVirtualRegister(MemopRC);
    MRI->def_begin(backedgeReg)->ChangeToRegister(newMemdepReg, true); // Replace definition
    BuildMI(*backedgeDefBB, backedgeDefBB->getFirstTerminator(), DebugLoc(),
            TII->get(CSA::CSA_PARALLEL_MEMDEP), backedgeReg).addReg(newMemdepReg);

    // Sanity check: all channels are 0 or 1 bit wide
  } // end for each PHI

  DEBUG(errs() << "%%% After markParallelLoopBackedges\n");
  DEBUG(thisMF->dump());
}

bool CSAMemopOrdering::should_assign_ordering(const MachineInstr& MI) const {
  using namespace std;
  return not MI.memoperands_empty()
    and begin(MI.defs()) != end(MI.defs()) and begin(MI.uses()) != end(MI.uses())
    and prev(end(MI.defs()))->isReg() and prev(end(MI.defs()))->getReg() == CSA::IGN
    and prev(end(MI.uses()))->isReg() and prev(end(MI.uses()))->getReg() == CSA::IGN;
}

void MachineAliasSetTracker::add(MachineMemOperand *mop) {
  if (isMerged)
    return;

  /* Handle the "normal" case where we have a Value by adding the value into
   * the real AliasSetTracker. */
  Value *v = const_cast<Value*>(mop->getValue());
  if (v) {
    AST.add(v, mop->getSize(), mop->getAAInfo());
    return;
  }

  /* Otherwise, we there is no Value, and the pointer is something like a frame
   * object. (This is the only case I've seen so far, but there are other types
   * of PseudoSourceValues.) */
  const PseudoSourceValue *pv = mop->getPseudoValue();

  /* Ask if the PseudoValue IS aliased with a Value. If it's a
   * FixedStackPseudoSourceValue, then this will consult MFI. If the answer is
   * "no", then we consider the pv to be in its own alias set and can avoid
   * giving up (by merging all of the alias sets). Note that we can't track
   * this with an actual AliasSet. "isAliased" reports whether any Values may
   * alias, so this also assumes that PseudoValues cannot alias one another. */
  if (pv && !pv->isAliased(MFI)) {
    DEBUG(errs() << "found a non-aliasing pv.\n");
    if (!pseudos[pv])
      pseudos[pv] = pseudosCounter++;
    return;
  }

  /* If we find a memop that has no Value and no PseudoValue, or if we find
   * that any PseudoValue is not in its own alias set, then we give up and
   * consider ourselves to only have one all-encompassing alias set. */
  DEBUG(errs() << "found a pv which may be aliased. smushing into one alias set.\n");
  isMerged = true;
}

unsigned MachineAliasSetTracker::getNumAliasSets(void) {
  if (isMerged)
    return 1;

  unsigned size = 0;
  for (const AliasSet &s : AST.getAliasSets()) {
    (void)s;
    size++;
  }
  size+=pseudos.size();

  return size;
}

unsigned MachineAliasSetTracker::getAliasSetNumForMemop(const MachineMemOperand *mop) {
  if (isMerged)
    return 0;

  Value *v = const_cast<Value*>(mop->getValue());
  if (mop->getValue()) {
    AliasSet *as = AST.getAliasSetForPointerIfExists(v, mop->getSize(), mop->getAAInfo());
    assert(as && "Memop must be added to MachineAliasSetTracker before querying");
    unsigned pos = 0;
    for (const AliasSet &s : AST.getAliasSets()) {
      if (&s == as)
        return pos;
      pos++;
    }
  }

  const PseudoSourceValue *pv = mop->getPseudoValue();
  unsigned pos = 0;
  for (const AliasSet &s : AST.getAliasSets()) {
    (void)s;
    pos++;
  }
  assert(pseudos[pv] && "Memop must be added to MachineAliasSetTracker before querying");
  return pos + pseudos[pv] - 1;
}

void MachineAliasSetTracker::print(raw_ostream &OS) const {
  if (isMerged) {
    OS << "[Merged]\n\n";
    return;
  }

  OS << "Non-aliasing PseudoValues: " << pseudosCounter-1 << "\n";
  OS << "Values in AliasSetTracker:\n";
  AST.print(OS);
}
