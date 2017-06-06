//===-- Nios2DelUselessJMP.cpp - Nios2 DelJmp -------------------------------===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// Simple pass to fills delay slots with useful instructions.
//
//===----------------------------------------------------------------------===//

#include "Nios2.h"

#include "Nios2TargetMachine.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/Statistic.h"

using namespace llvm;

#define DEBUG_TYPE "del-jmp"

STATISTIC(NumDelJmp, "Number of useless jmp deleted");

static cl::opt<bool> EnableDelJmp(
  "enable-nios2-del-useless-jmp",
  cl::init(true),
  cl::desc("Delete useless jmp instructions: jmp 0."),
  cl::Hidden);

namespace {
  struct DelJmp : public MachineFunctionPass {
    static char ID;
    DelJmp(TargetMachine &tm)
      : MachineFunctionPass(ID) { }

    StringRef getPassName() const override { return "Nios2 Del Useless jmp"; }
//virtual const char *getPassName() const {
//      return "Nios2 Del Useless jmp";
//    }

    bool runOnMachineBasicBlock(MachineBasicBlock &MBB, MachineBasicBlock &MBBN);
    bool runOnMachineFunction(MachineFunction &F) {
      bool Changed = false;
      if (EnableDelJmp) {
        MachineFunction::iterator FJ = F.begin();
        if (FJ != F.end())
          FJ++;
        if (FJ == F.end())
          return Changed;
        for (MachineFunction::iterator FI = F.begin(), FE = F.end();
             FJ != FE; ++FI, ++FJ)
          // In STL style, F.end() is the dummy BasicBlock() like '\0' in 
          //  C string. 
          // FJ is the next BasicBlock of FI; When FI range from F.begin() to 
          //  the PreviousBasicBlock of F.end() call runOnMachineBasicBlock().
          Changed |= runOnMachineBasicBlock(*FI, *FJ);
      }
      return Changed;
    }

  };
  char DelJmp::ID = 0;
} // end of anonymous namespace

bool DelJmp::
runOnMachineBasicBlock(MachineBasicBlock &MBB, MachineBasicBlock &MBBN) {
  bool Changed = false;

  MachineBasicBlock::iterator I = MBB.end();
  if (I != MBB.begin())
    I--;	// set I to the last instruction
  else
    return Changed;
    
  if (I->getOpcode() == Nios2::JMP_R1 && I->getOperand(0).getMBB() == &MBBN) { // TODO: Nios2 R2 support
    // I is the instruction of "jmp #offset=0", as follows,
    //     jmp	$BB0_3
    // $BB0_3:
    //     ld	$4, 28($sp)
    ++NumDelJmp;
    MBB.erase(I);	// delete the "JMP 0" instruction
    Changed = true;	// Notify LLVM kernel Changed
  }
  return Changed;

}

/// createNios2DelJmpPass - Returns a pass that DelJmp in Nios2 MachineFunctions
FunctionPass *llvm::createNios2DelJmpPass(Nios2TargetMachine &tm) {
  return new DelJmp(tm);
}
