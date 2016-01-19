//===-- X86TargetFrameLowering.h - Define frame lowering for X86 -*- C++ -*-==//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This class implements X86-specific bits of TargetFrameLowering class.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_LIB_TARGET_X86_X86FRAMELOWERING_H
#define LLVM_LIB_TARGET_X86_X86FRAMELOWERING_H

#include "llvm/Target/TargetFrameLowering.h"

namespace llvm {

class MachineInstrBuilder;
class MCCFIInstruction;
class X86Subtarget;
class X86RegisterInfo;

class X86FrameLowering : public TargetFrameLowering {
public:
  X86FrameLowering(const X86Subtarget &STI, unsigned StackAlignOverride);

  // Cached subtarget predicates.

  const X86Subtarget &STI;
  const TargetInstrInfo &TII;
  const X86RegisterInfo *TRI;

  unsigned SlotSize;

  /// Is64Bit implies that x86_64 instructions are available.
  bool Is64Bit;

  bool IsLP64;

  /// True if the 64-bit frame or stack pointer should be used. True for most
  /// 64-bit targets with the exception of x32. If this is false, 32-bit
  /// instruction operands should be used to manipulate StackPtr and FramePtr.
  bool Uses64BitFramePtr;

  unsigned StackPtr;

  /// Emit a call to the target's stack probe function. This is required for all
  /// large stack allocations on Windows. The caller is required to materialize
  /// the number of bytes to probe in RAX/EAX.
  void emitStackProbeCall(MachineFunction &MF, MachineBasicBlock &MBB,
                          MachineBasicBlock::iterator MBBI, DebugLoc DL) const;

  void emitCalleeSavedFrameMoves(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MBBI,
                                 DebugLoc DL) const;

  /// emitProlog/emitEpilog - These methods insert prolog and epilog code into
  /// the function.
  void emitPrologue(MachineFunction &MF, MachineBasicBlock &MBB) const override;
  void emitEpilogue(MachineFunction &MF, MachineBasicBlock &MBB) const override;

  void adjustForSegmentedStacks(MachineFunction &MF,
                                MachineBasicBlock &PrologueMBB) const override;

  void adjustForHiPEPrologue(MachineFunction &MF,
                             MachineBasicBlock &PrologueMBB) const override;

  void determineCalleeSaves(MachineFunction &MF, BitVector &SavedRegs,
                            RegScavenger *RS = nullptr) const override;

  bool
  assignCalleeSavedSpillSlots(MachineFunction &MF,
                              const TargetRegisterInfo *TRI,
                              std::vector<CalleeSavedInfo> &CSI) const override;

  bool spillCalleeSavedRegisters(MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MI,
                                 const std::vector<CalleeSavedInfo> &CSI,
                                 const TargetRegisterInfo *TRI) const override;

  bool restoreCalleeSavedRegisters(MachineBasicBlock &MBB,
                                  MachineBasicBlock::iterator MI,
                                  const std::vector<CalleeSavedInfo> &CSI,
                                  const TargetRegisterInfo *TRI) const override;

  bool hasFP(const MachineFunction &MF) const override;
  bool hasReservedCallFrame(const MachineFunction &MF) const override;
  bool canSimplifyCallFramePseudos(const MachineFunction &MF) const override;
  bool needsFrameIndexResolution(const MachineFunction &MF) const override;

  int getFrameIndexReference(const MachineFunction &MF, int FI,
                             unsigned &FrameReg) const override;

  int getFrameIndexReferenceFromSP(const MachineFunction &MF, int FI,
                                   unsigned &FrameReg) const override;

  void eliminateCallFramePseudoInstr(MachineFunction &MF,
                                 MachineBasicBlock &MBB,
                                 MachineBasicBlock::iterator MI) const override;

  unsigned getWinEHParentFrameOffset(const MachineFunction &MF) const override;

  /// Check the instruction before/after the passed instruction. If
  /// it is an ADD/SUB/LEA instruction it is deleted argument and the
  /// stack adjustment is returned as a positive value for ADD/LEA and
  /// a negative for SUB.
  int mergeSPUpdates(MachineBasicBlock &MBB, MachineBasicBlock::iterator &MBBI,
                     bool doMergeWithPrevious) const;

  /// Emit a series of instructions to increment / decrement the stack
  /// pointer by a constant value.
  void emitSPUpdate(MachineBasicBlock &MBB, MachineBasicBlock::iterator &MBBI,
                    int64_t NumBytes, bool InEpilogue) const;

  /// Check that LEA can be used on SP in an epilogue sequence for \p MF.
  bool canUseLEAForSPInEpilogue(const MachineFunction &MF) const;

  /// Check whether or not the given \p MBB can be used as a epilogue
  /// for the target.
  /// The epilogue will be inserted before the first terminator of that block.
  /// This method is used by the shrink-wrapping pass to decide if
  /// \p MBB will be correctly handled by the target.
  bool canUseAsEpilogue(const MachineBasicBlock &MBB) const override;

#if INTEL_CUSTOMIZATION
  /// Order the symbols in the local stack.
  /// We want to place the local stack objects in some sort of sensible order.
  /// The heuristic we use is to try and pack them according to static number
  /// of uses and size in order to minimize code size.
  void orderFrameObjects(const MachineFunction &MF,
                         std::vector<int> &objectsToAllocate) const override;
#endif // INTEL_CUSTOMIZATION

#if INTEL_CUSTOMIZATION
  // Cherry picking r252266.
  /// Sets up EBP and optionally ESI based on the incoming EBP value.  Only
  /// needed for 32-bit. Used in funclet prologues and at catchret destinations.
  MachineBasicBlock::iterator
  restoreWin32EHStackPointers(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI, DebugLoc DL,
                              bool RestoreSP = false) const;
#endif // INTEL_CUSTOMIZATION

  /// Wraps up getting a CFI index and building a MachineInstr for it.
  void BuildCFI(MachineBasicBlock &MBB, MachineBasicBlock::iterator MBBI,
                DebugLoc DL, MCCFIInstruction CFIInst) const;

private:
  uint64_t calculateMaxStackAlign(const MachineFunction &MF) const;

#if INTEL_CUSTOMIZATION
  // Cherry picking r252210.
  /// Aligns the stack pointer by ANDing it with -MaxAlign.
  void BuildStackAlignAND(MachineBasicBlock &MBB,
                          MachineBasicBlock::iterator MBBI, DebugLoc DL,
                          unsigned Reg, uint64_t MaxAlign) const;
#else // !INTEL_CUSTOMIZATION
  /// Aligns the stack pointer by ANDing it with -MaxAlign.
  void BuildStackAlignAND(MachineBasicBlock &MBB,
                          MachineBasicBlock::iterator MBBI, DebugLoc DL,
                          uint64_t MaxAlign) const;
#endif // !INTEL_CUSTOMIZATION

  /// Make small positive stack adjustments using POPs.
  bool adjustStackWithPops(MachineBasicBlock &MBB,
                           MachineBasicBlock::iterator MBBI, DebugLoc DL,
                           int Offset) const;

  /// Adjusts the stack pointer using LEA, SUB, or ADD.
  MachineInstrBuilder BuildStackAdjustment(MachineBasicBlock &MBB,
                                           MachineBasicBlock::iterator MBBI,
                                           DebugLoc DL, int64_t Offset,
                                           bool InEpilogue) const;

#if INTEL_CUSTOMIZATION
  /// Class used by orderFrameObjects to help sort the stack objects.
  class X86FrameSortingObject {
  public:
    X86FrameSortingObject() : IsValid(false), ObjectIndex(0), ObjectSize(0),
                              ObjectAlignment(1), ObjectNumUses(0) {}
    bool IsValid;                 // true if we care about this Object.
    unsigned int ObjectIndex;     // Index of Object into MFI list.
    unsigned int ObjectSize;      // Size of Object in bytes.
    unsigned int ObjectAlignment; // Alignment of Object in bytes.
    unsigned int ObjectNumUses;   // Object static number of uses.
  };

  /// The comparison function we use for std::sort to order our local
  /// stack symbols. The current algorithm is to use an estimated
  /// "density". This takes into consideration the size and number of
  /// uses each object has in order to roughly minimize code size.
  /// So, for example, an object of size 16B that is referenced 5 times
  /// will get higher priority than 4 4B objects referenced 1 time each.
  /// It's not perfect and we may be able to squeeze a few more bytes out of
  /// it (for example : 0(esp) requires fewer bytes, symbols allocated at the
  /// fringe end can have special consideration, given their size is less
  /// important, etc.), but the algorithmic complexity grows too much to be
  /// worth the extra gains we get. This gets us pretty close.
  /// The final order leaves us with objects with highest priority going
  /// at the end of our list.
  struct X86FrameSortingAlgorithm {
    inline bool operator() (const X86FrameSortingObject& a,
                            const X86FrameSortingObject& b)
    {
      double DensityA, DensityB;

      // For consistency in our comparison, all invalid objects are placed
      // at the end. This also allows us to stop walking when we hit the
      // first invalid item after it's all sorted.
      if (!a.IsValid)
        return false;
      if (!b.IsValid)
        return true;

      DensityA = static_cast<double>(a.ObjectNumUses) /
        static_cast<double>(a.ObjectSize);
      DensityB = static_cast<double>(b.ObjectNumUses) /
        static_cast<double>(b.ObjectSize);

      // If the two densities are equal, prioritize highest alignment
      // objects. This allows for similar alignment objects
      // to be packed together (given the same density).
      // There's room for improvement here, also, since we can pack
      // similar alignment (different density) objects next to each
      // other to save padding. This will also require further
      // complexity/iterations, and the overall gain isn't worth it,
      // in general. Something to keep in mind, though.
      if (DensityA == DensityB)
        return a.ObjectAlignment < b.ObjectAlignment;

      return DensityA < DensityB;
    }
  };
#endif // INTEL_CUSTOMIZATION

#if !INTEL_CUSTOMIZATION
  // Cherry picking r252266, where this is moved to public.
  /// Sets up EBP and optionally ESI based on the incoming EBP value.  Only
  /// needed for 32-bit. Used in funclet prologues and at catchret destinations.
  MachineBasicBlock::iterator
  restoreWin32EHStackPointers(MachineBasicBlock &MBB,
                              MachineBasicBlock::iterator MBBI, DebugLoc DL,
                              bool RestoreSP = false) const;
#endif // !INTEL_CUSTOMIZATION

  unsigned getWinEHFuncletFrameSize(const MachineFunction &MF) const;
};

} // End llvm namespace

#endif
