//===-- CSANameLICs.cpp - Propagate DBG_VALUE to LIC names ----------------===//
//
// Copyright (C) 2017-2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file implements a pass that adds names to LICs based on DBG_VALUE
// instructions.
//
//===----------------------------------------------------------------------===//

#include "CSA.h"
#include "CSAInstrInfo.h"
#include "CSAMachineFunctionInfo.h"
#include "CSATargetMachine.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/ADT/StringMap.h"
#include "llvm/IR/DebugInfoMetadata.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

namespace llvm {
  class CSANameLICsPass : public MachineFunctionPass {
  public:
    static char ID;
    CSANameLICsPass();

    StringRef getPassName() const override {
      return "CSA: Name LICs pass";
    }

    bool runOnMachineFunction(MachineFunction &MF) override;
    void getAnalysisUsage(AnalysisUsage &AU) const override {
      AU.setPreservesAll();
      MachineFunctionPass::getAnalysisUsage(AU);
    }

  private:
    MachineFunction *MF;
    const MachineRegisterInfo *MRI;
    CSAMachineFunctionInfo *LMFI;
    const CSAInstrInfo *TII;
    StringMap<unsigned> names;

    void nameLIC(MachineInstr &MI);
    void nameTerminator(const MachineBasicBlock &MBB, const MachineOperand &MO);
  };
}

char CSANameLICsPass::ID = 0;

CSANameLICsPass::CSANameLICsPass() : MachineFunctionPass(ID) {
}


MachineFunctionPass *llvm::createCSANameLICsPass() {
  return new CSANameLICsPass();
}

bool CSANameLICsPass::runOnMachineFunction(MachineFunction &MF) {
  this->MF = &MF;
  MRI = &MF.getRegInfo();
  LMFI = MF.getInfo<CSAMachineFunctionInfo>();
  TII = static_cast<const CSAInstrInfo*>(MF.getSubtarget<CSASubtarget>().getInstrInfo());

  for (auto &MBB : MF) {
    for (auto &MI : MBB) {
      if (MI.isDebugValue())
        nameLIC(MI);
    }
    MachineBasicBlock *TBB = nullptr, *FBB = nullptr;
    SmallVector<MachineOperand, 2> cond;
    if (!TII->analyzeBranch(MBB, TBB, FBB, cond, false) && TBB && FBB) {
      nameTerminator(MBB, cond[1]);
    }
  }
  names.clear();

  this->MF = nullptr;
  return false;
}

void CSANameLICsPass::nameLIC(MachineInstr &MI) {
  if (!MI.getOperand(0).isReg())
    return;
  MI.setFlag(MachineInstr::NonSequential);
  auto name = MI.getDebugVariable()->getName();
  unsigned reg = MI.getOperand(0).getReg();
  if (TargetRegisterInfo::isPhysicalRegister(reg))
    return;
  LMFI->setLICName(reg, name);
}

void CSANameLICsPass::nameTerminator(const MachineBasicBlock &MBB,
    const MachineOperand &MO) {
  // If there's no basic block name, don't name it.
  if (MBB.getName().empty())
    return;
  if (!MO.isReg())
    return;
  unsigned reg = MO.getReg();
  if (TargetRegisterInfo::isPhysicalRegister(reg))
    return;
  // Don't try to change the name of something already set.
  if (!LMFI->getLICName(reg).empty())
    return;
  LMFI->setLICName(reg, "switch." + Twine(MBB.getName()) + ".cond");
}
