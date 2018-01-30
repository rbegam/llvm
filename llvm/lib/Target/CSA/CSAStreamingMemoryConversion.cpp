//===-- CSAStreamingMemoryConversion.cpp - Streaming memory operations ----===//
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
// This file implements a pass that converts memory operations to streaming
// memory loads and stores where applicable.
//
//===----------------------------------------------------------------------===//

#include "CSA.h"
#include "CSAInstBuilder.h"
#include "CSAInstrInfo.h"
#include "CSAMachineFunctionInfo.h"
#include "CSAMatcher.h"
#include "CSATargetMachine.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/Pass.h"
#include "llvm/Support/Debug.h"

using namespace llvm;

#define DEBUG_TYPE "csa-streamem"

static cl::opt<bool> DisableMemoryConversion("csa-disable-streammem", cl::Hidden,
    cl::desc("CSA Specific: Disable streaming memory conversion"));

namespace llvm {
  class CSAStreamingMemoryConversionPass : public MachineFunctionPass {
  public:
    static char ID;
    CSAStreamingMemoryConversionPass();

    StringRef getPassName() const override {
      return "CSA Dataflow simplification pass";
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
    std::vector<MachineInstr *> to_delete;

    bool makeStreamMemOp(MachineInstr *MI);
    MachineInstr *getDefinition(const MachineOperand &MO) const;
    void getUses(const MachineOperand &MO,
        SmallVectorImpl<MachineInstr *> &uses) const;
    MachineInstr *getSingleUse(const MachineOperand &MO) const;
    bool isZero(const MachineOperand &MO) const;
    MachineOp getLength(const MachineOperand &start,
        const MachineOperand &end, bool isEqual, int64_t stride,
        MachineInstr *buildPoint) const;
  };
}

char CSAStreamingMemoryConversionPass::ID = 0;

CSAStreamingMemoryConversionPass::CSAStreamingMemoryConversionPass() : MachineFunctionPass(ID) {
}


MachineFunctionPass *llvm::createCSAStreamingMemoryConversionPass() {
  return new CSAStreamingMemoryConversionPass();
}

bool CSAStreamingMemoryConversionPass::runOnMachineFunction(MachineFunction &MF) {
  if (DisableMemoryConversion)
    return false;

  this->MF = &MF;
  MRI = &MF.getRegInfo();
  LMFI = MF.getInfo<CSAMachineFunctionInfo>();
  TII = static_cast<const CSAInstrInfo*>(MF.getSubtarget<CSASubtarget>().getInstrInfo());

  // Run several functions one at a time on the entire graph. There is probably
  // a better way of implementing this sort of strategy (like how InstCombiner
  // does its logic), but until we have a need to go a fuller InstCombiner-like
  // route, this logic will do. Note that we can't delete instructions on the
  // fly due to how iteration works, but we do clean them up after every mini
  // pass.
  bool changed = false;
  for (auto &MBB : MF) {
    for (auto &MI : MBB) {
      changed |= makeStreamMemOp(&MI);
    }
  }
  for (auto MI : to_delete)
    MI->eraseFromParent();
  to_delete.clear();

  this->MF = nullptr;
  return changed;
}

MachineInstr *CSAStreamingMemoryConversionPass::getDefinition(const MachineOperand &MO) const {
  assert(MO.isReg() && "LICs to search for can only be registers");
  return MRI->getUniqueVRegDef(MO.getReg());
}

void CSAStreamingMemoryConversionPass::getUses(const MachineOperand &MO,
    SmallVectorImpl<MachineInstr *> &uses) const {
  assert(MO.isReg() && "LICs to search for can only be registers");
  for (auto &use : MRI->use_instructions(MO.getReg())) {
    uses.push_back(&use);
  }
}

MachineInstr *CSAStreamingMemoryConversionPass::getSingleUse(
    const MachineOperand &MO) const {
  SmallVector<MachineInstr *, 4> uses;
  getUses(MO, uses);
  return uses.size() == 1 ? uses[0] : nullptr;
}

bool isImm(const MachineOperand &MO, int64_t immValue) {
  return MO.isImm() && MO.getImm() == immValue;
}

bool CSAStreamingMemoryConversionPass::isZero(const MachineOperand &MO) const {
  if (MO.isReg()) {
    const MachineInstr *def = getDefinition(MO);
    if (def->getOpcode() == CSA::MOV64 && isImm(def->getOperand(1), 0))
      return true;
  }
  return isImm(MO, 0);
}

MachineOp CSAStreamingMemoryConversionPass::getLength(
    const MachineOperand &start, const MachineOperand &end,
    bool isEqual, int64_t stride, MachineInstr *MI) const {
  if (stride < 0)
    return getLength(end, start, isEqual, -stride, MI);
  CSAInstBuilder builder(*TII);
  builder.setInsertionPoint(MI);
  if (stride != 1) {
    // Trip count = (end + isEqual - start + stride - 1) / stride
    return builder.makeOrConstantFold(*LMFI, CSA::SRL64,
        builder.makeOrConstantFold(*LMFI, CSA::SUB64,
          builder.makeOrConstantFold(*LMFI, CSA::ADD64,
            end, OpImm(stride - 1 + isEqual)), start),
        OpImm(countTrailingZeros((unsigned)stride)));
  }
  if (isZero(start) && !isEqual) {
    return end;
  }

  MachineOperand effectiveStart = start;
  if (isEqual && start.isImm()) {
    effectiveStart = MachineOperand::CreateImm(start.getImm() - 1);
  } else if (isEqual) {
    DEBUG(dbgs() << "<= bounds not handled for non-immediate starts\n");
    return nullptr;
  }

  // In the case where multiple loads/stores originate from this stream, we'll
  // find the sub we want just above us.
  MachineInstr *possible = MI->getPrevNode();
  if (possible && possible->getOpcode() == CSA::SUB64) {
    if (possible->getOperand(1).isIdenticalTo(end) &&
        possible->getOperand(2).isIdenticalTo(effectiveStart))
      return OpUse(possible->getOperand(0));
  }

  // Compute the length as end - start.
  return builder.makeOrConstantFold(*LMFI, CSA::SUB64, end, effectiveStart);
}

// This takes a load or a store controlled by a sequence operator and
// converts it into a streaming load and store. The requirements for legality
// are as follows:
// 1. The address is calculated as a strided offset, with base and stride
//    known. The stride may be limited to 1 for CSA v1. (TODO: implementation
//    not yet considered).
// 2. The length of the stride must be constant, at least in a SCEV-style
//    sense.
// 3. The input and output memory orders must consume/produce a single memory
//    order for the entire loop and not be used otherwise. This is effectively
//    saying that the input is a repeat guarded by a loop stream and the
//    output is a switch where all but the last value are ignored, but it's
//    possible that earlier optimizations do aggregation on a different level.
//
// The biggest constraint on the valid operations is the second one. For now,
// we accept only sequence operators, since calculating length is easy:
// * SEQOTNE64 0, %lic, 1  => length = %lic
// * SEQOTNE64 %lic, 0, -1 => length = %lic
// * SEQOTLTS64 0, %lic, 1 => length = %lic
// * SEQOTLTU64 0, %lic, 1 => length = %lic
// * SEQOT{NE,LTS,LTU}64 %base, %lic, 1 => length = %lic - %base
// Note that the pred output here is the %stream we consider.
//
// The source of the address computations is more complicated. The following
// patterns should be okay:
// * LD (STRIDE %stream, %base, %stride) => base = %base, stride = %stride
// * LD{X,D,R} (REPEATO %stream, %base), (SEQOT**64_index 0, %N, %stride)
// * LD{X,D,R] (REPEATO %stream, %base), (SEQOT**64_index %start, %end, %stride)

MIRMATCHER_REGS(RESULT, REPEATED, SEQ_VAL, SEQ_PRED, SEQ_FIRST, SEQ_LAST, CTL);
using namespace CSAMatch;
constexpr auto repeated_pat = mirmatch::graph(
    RESULT = repeato_N(CTL, REPEATED),
    CTL = not1(SEQ_LAST),
    (SEQ_VAL, SEQ_PRED, SEQ_FIRST, SEQ_LAST) =
      seqot(mirmatch::AnyOperand, mirmatch::AnyOperand, mirmatch::AnyOperand)
    );

bool CSAStreamingMemoryConversionPass::makeStreamMemOp(MachineInstr *MI) {
  const MachineOperand *base, *value;
  unsigned stride;
  const MachineOperand *inOrder, *outOrder, *memOrder;
  MachineInstr *stream;
  bool baseUsesStream = false;
  auto genericOpcode = TII->getGenericOpcode(MI->getOpcode());
  switch (genericOpcode) {
  case CSA::Generic::LD: case CSA::Generic::ST:
    {
      // The address here must be a STRIDE.
      bool isLoad = MI->mayLoad();
      MachineInstr *memAddr = getDefinition(MI->getOperand(isLoad ? 2 : 1));
      if (!memAddr || memAddr->getOpcode() != CSA::STRIDE64) {
        return false;
      }

      base = &memAddr->getOperand(2);
      const MachineOperand &strideOp = memAddr->getOperand(3);
      unsigned opcodeSize = TII->getLicSize(MI->getOpcode()) / 8;
      if (!strideOp.isImm()) {
        DEBUG(dbgs() << "Stride is not an immediate, cannot compute stride\n");
        return false;
      } else if (strideOp.getImm() % opcodeSize) {
        DEBUG(dbgs() << "Stride " << strideOp.getImm() <<
            " is not a multiple of opcode size\n");
        return false;
      }
      stride = strideOp.getImm() / opcodeSize;

      // The STRIDE's stream parameter defines the stream.
      // TODO: assert that we use the seq predecessor output.
      stream = getDefinition(memAddr->getOperand(1));
      memOrder = &MI->getOperand(3);
      inOrder = &MI->getOperand(4);
      outOrder = &MI->getOperand(isLoad ? 1 : 0);
      value = &MI->getOperand(isLoad ? 0 : 2);
      break;
    }
  case CSA::Generic::LDX: case CSA::Generic::STX:
  case CSA::Generic::LDD: case CSA::Generic::STD:
    {
      bool isLoad = MI->mayLoad();
      auto &baseOp = MI->getOperand(isLoad ? 2 : 1);
      auto &indexOp = MI->getOperand(isLoad ? 3 : 2);
      if (baseOp.isImm() || indexOp.isImm())
        return false;

      // The base address needs to be repeated.
      MachineInstr *memBase = getDefinition(baseOp);
      MachineInstr *memIndex = getDefinition(indexOp);
      if (!memBase)
        return false;
      auto repeat_result = mirmatch::match(repeated_pat, memBase);
      if (!repeat_result) {
        return false;
      }

      // The stream controls the base REPEAT--they should be the same
      // instruction.
      stream = MRI->getVRegDef(repeat_result.reg(SEQ_LAST));
      if (stream != memIndex) {
        return false;
      }

      switch (memIndex->getOpcode()) {
      case CSA::SEQOTNE64:
      case CSA::SEQOTLTS64:
      case CSA::SEQOTLTU64:
      case CSA::SEQOTLES64:
      case CSA::SEQOTLEU64:
        break; // These are the valid ones.
      default:
        DEBUG(dbgs() << "Candidate indexed memory store failed to have valid "
            << "stream parameter. It may yet be valid.\n");
        DEBUG(MI->print(dbgs()));
        DEBUG(dbgs() << "Failed operator: ");
        DEBUG(memIndex->print(dbgs()));
        return false;
      }

      base = &memBase->getOperand(2);
      baseUsesStream = true;
      const MachineOperand &strideOp = memIndex->getOperand(6);
      if (!strideOp.isImm()) {
        DEBUG(dbgs() << "Candidate instruction has non-constant stride.\n");
        return false;
      }
      stride = strideOp.getImm();
      if (genericOpcode != CSA::Generic::LDX &&
          genericOpcode != CSA::Generic::STX) {
        unsigned opcodeSize = TII->getLicSize(MI->getOpcode()) / 8;
        if (stride % opcodeSize) {
          DEBUG(dbgs() << "Candidate instruction has improper stride.\n");
          return false;
        }
        stride /= opcodeSize;
      }
      memOrder = &MI->getOperand(4);
      inOrder = &MI->getOperand(5);
      outOrder = &MI->getOperand(isLoad ? 1 : 0);
      value = &MI->getOperand(isLoad ? 0 : 3);
      break;
    }
  default:
    return false;
  }

  DEBUG(dbgs() << "Identified candidate for streaming memory conversion: ");
  DEBUG(MI->print(dbgs()));
  DEBUG(dbgs() << "Base: " << *base << "; stride: " << stride <<
      "; controlling stream: ");
  DEBUG(stream->print(dbgs()));
  CSAInstBuilder builder(*TII);
  builder.setInsertionPoint(MI);

  // Verify that the memory orders are properly constrained by the stream.
  MachineInstr *inSource = getDefinition(*inOrder);
  if (!inSource) {
    DEBUG(dbgs() << "Conversion failed due to bad in memory order.\n");
    return false;
  }
  auto mem_result = mirmatch::match(repeated_pat, inSource);
  if (!mem_result || MRI->getVRegDef(mem_result.reg(SEQ_LAST)) != stream) {
    DEBUG(dbgs() << "Conversion failed due to bad in memory order.\n");
    return false;
  }

  MachineInstr *outSink = getSingleUse(*outOrder);
  if (!outSink || !TII->isSwitch(outSink)) {
    DEBUG(dbgs() << "Conversion failed because out memory order is not a switch.\n");
    return false;
  }

  // The output memory order should be a switch that ignores the signal unless
  // it's the last iteration of the stream.
  MachineInstr *sinkControl = getDefinition(outSink->getOperand(2));
  if (!sinkControl) {
    DEBUG(dbgs() << "Cannot found the definition of the output order switch");
    return false;
  }

  // TODO: check that we are using the last output of the stream.
  const MachineOperand *realOutSink;
  if (outSink->getOperand(0).getReg() == CSA::IGN) {
    if (sinkControl != stream) {
      DEBUG(dbgs() << "Output memory order is not controlled by the stream\n");
      return false;
    }
    realOutSink = &outSink->getOperand(1);
  } else if (outSink->getOperand(1).getReg() == CSA::IGN) {
    // The control structure should be a NOT (stream control)
    if (outSink->getOpcode() != CSA::NOT1 ||
        getDefinition(outSink->getOperand(1)) == stream) {
      DEBUG(dbgs() << "Output memory order is not controlled by the stream\n");
      return false;
    }
    realOutSink = &outSink->getOperand(0);
  } else {
    // The output memory order is not ignored...
    DEBUG(dbgs() << "Output memory order is not controlled by the stream\n");
    return false;
  }

  // Compute the length of the stream from the stream parameter.
  const MachineOperand &seqStart = stream->getOperand(4);
  const MachineOperand &seqEnd = stream->getOperand(5);
  const MachineOperand &seqStep = stream->getOperand(6);
  if (!seqStep.isImm()) {
    DEBUG(dbgs() << "Sequence step is not an immediate\n");
    return false;
  }
  bool isEqual = false;
  switch (TII->getGenericOpcode(stream->getOpcode())) {
  case CSA::Generic::SEQOTNE:
  case CSA::Generic::SEQOTLT:
    isEqual = false;
    break;
  case CSA::Generic::SEQOTLE:
    isEqual = true;
    break;
  default:
    DEBUG(dbgs() << "Stream operand is of unknown form.\n");
    return false;
  }
  const MachineOp length =
    getLength(seqStart, seqEnd, isEqual, seqStep.getImm(), stream);
  if (!length) {
    DEBUG(dbgs() << "Stream operand is of unknown form.\n");
    return false;
  }

  if (baseUsesStream) {
    if (seqStep.getImm() < 0) {
      DEBUG(dbgs() << "Base using stream needs to have an incrementing step\n");
      return false;
    }
    if (!isZero(seqStart)) {
      unsigned loadBase = LMFI->allocateLIC(&CSA::CI64RegClass);
      auto baseForStream = builder.makeInstruction(CSA::SLADD64,
          OpRegDef(loadBase), seqStart,
          OpImm(countTrailingZeros(TII->getLicSize(MI->getOpcode()) / 8)),
          *base);
      base = &baseForStream->getOperand(0);
    }
  }

  DEBUG(dbgs() << "No reason to disqualify the memory operation found, converting\n");

  // Actually build the new instruction now.
  unsigned opcode = TII->adjustOpcode(MI->getOpcode(),
      MI->mayLoad() ? CSA::Generic::SLD : CSA::Generic::SST);
  builder.makeInstruction(opcode,
    OpIf(MI->mayLoad(), OpDef(*value)), // Value (for load)
    *realOutSink, // Output memory order
    OpUse(*base), // Address
    length, // Length
    OpImm(stride), // Stride
    OpIf(!MI->mayLoad(), OpUse(*value)), // Value (for store)
    *memOrder, // Memory ordering
    inSource->getOperand(2)); // Input memory order

  // Delete the old instruction. Also delete the old output switch, since we
  // added a second definition of its input. Dead instruction elimination should
  // handle the rest.
  to_delete.push_back(MI);
  to_delete.push_back(outSink);

  return true;
}
