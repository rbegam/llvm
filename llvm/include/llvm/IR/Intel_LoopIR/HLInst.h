//===------------- HLInst.h - High level IR node ----------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the HLInst node.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_INTEL_LOOPIR_HLINST_H
#define LLVM_IR_INTEL_LOOPIR_HLINST_H

#include "llvm/IR/Instruction.h"

#include "llvm/IR/Intel_LoopIR/HLDDNode.h"

namespace llvm {

class BasicBlock;

namespace loopopt {

class RegDDRef;

/// \brief High level node representing a LLVM instruction
class HLInst : public HLDDNode {
private:
  const Instruction *Inst;
  HLInst *SafeRednSucc;

protected:
  explicit HLInst(Instruction *In);
  ~HLInst() {}

  /// \brief Copy constructor used by cloning.
  HLInst(const HLInst &HLInstObj);

  friend class HLNodeUtils;

  /// \brief Implements getNumOperands() functionality.
  unsigned getNumOperandsInternal() const;

  /// \brief Implements isInPreheader*()/isInPostexit*() functionality.
  bool isInPreheaderPostexitImpl(bool Preheader) const;

  /// \brief Initializes some of the members to bring the object in a sane
  /// state.
  void initialize();

public:
  /// \brief Prints HLInst.
  virtual void print(formatted_raw_ostream &OS, unsigned Depth) const override;

  /// \brief Returns the underlying Instruction.
  const Instruction *getLLVMInstruction() const { return Inst; }
  /// \brief Returns true if this node is part of safe reduction chain.
  bool isSafeRedn() const { return SafeRednSucc != 0; };

  /// \brief Returns the safe reduction successor of this node in the chain.
  HLInst *getSafeRednSucc() const { return SafeRednSucc; };
  void setSafeRednSucc(HLInst *Succ) { SafeRednSucc = Succ; }

  /// \brief Returns true if the underlying instruction has an lval.
  bool hasLval() const;
  /// \brief Returns true if the underlying instruction has a single rval.
  bool hasRval() const;

  const Value *getOperandValue(unsigned OperandNum);

  /// \brief Returns the DDRef associated with the Nth operand (starting with
  /// 0).
  RegDDRef *getOperandDDRef(unsigned OperandNum);
  const RegDDRef *getOperandDDRef(unsigned OperandNum) const;
  /// \brief Sets the DDRef associated with the Nth operand (starting with 0).
  void setOperandDDRef(RegDDRef *Ref, unsigned OperandNum);
  /// \brief Removes and returns the DDRef associated with the Nth operand
  /// (starting with 0).
  RegDDRef *removeOperandDDRef(unsigned OperandNum);

  /// \brief Returns the lval DDRef of this node.
  RegDDRef *getLvalDDRef();
  const RegDDRef *getLvalDDRef() const;
  /// \brief Sets the lval DDRef of this node.
  void setLvalDDRef(RegDDRef *RDDRef);
  /// \brief Removes and returns the lval DDRef of this node.
  RegDDRef *removeLvalDDRef();

  /// \brief Returns the single rval DDRef of this node.
  RegDDRef *getRvalDDRef();
  const RegDDRef *getRvalDDRef() const;
  /// \brief Sets the single rval DDRef of this node.
  void setRvalDDRef(RegDDRef *Ref);
  /// \brief Removes and returns the single rval DDRef of this node.
  RegDDRef *removeRvalDDRef();

  /// \brief Adds an extra RegDDRef which does not correspond to lval or any
  /// operand. This DDRef is not used for code generation but might be used for
  /// exposing DD edges. TODO: more on this later...
  void addFakeDDRef(RegDDRef *RDDRef);

  /// \brief Removes a previously inserted fake DDRef.
  void removeFakeDDRef(RegDDRef *RDDRef);

  /// Operand DDRef iterator methods
  ddref_iterator op_ddref_begin() { return RegDDRefs.begin(); }
  const_ddref_iterator op_ddref_begin() const { return RegDDRefs.begin(); }
  ddref_iterator op_ddref_end() { return RegDDRefs.begin() + getNumOperands(); }
  const_ddref_iterator op_ddref_end() const {
    return RegDDRefs.begin() + getNumOperands();
  }

  reverse_ddref_iterator op_ddref_rbegin() {
    return RegDDRefs.rend() - getNumOperands();
  }
  const_reverse_ddref_iterator op_ddref_rbegin() const {
    return RegDDRefs.rend() - getNumOperands();
  }
  reverse_ddref_iterator op_ddref_rend() { return RegDDRefs.rend(); }
  const_reverse_ddref_iterator op_ddref_rend() const {
    return RegDDRefs.rend();
  }

  /// Fake DDRef iterator methods
  ddref_iterator fake_ddref_begin() { return op_ddref_end(); }
  const_ddref_iterator fake_ddref_begin() const { return op_ddref_end(); }
  ddref_iterator fake_ddref_end() { return RegDDRefs.end(); }
  const_ddref_iterator fake_ddref_end() const { return RegDDRefs.end(); }

  reverse_ddref_iterator fake_ddref_rbegin() { return RegDDRefs.rbegin(); }
  const_reverse_ddref_iterator fake_ddref_rbegin() const {
    return RegDDRefs.rbegin();
  }
  reverse_ddref_iterator fake_ddref_rend() { return op_ddref_rbegin(); }
  const_reverse_ddref_iterator fake_ddref_rend() const {
    return op_ddref_rbegin();
  }

  /// \brief Method for supporting type inquiry through isa, cast, and dyn_cast.
  static bool classof(const HLNode *Node) {
    return Node->getHLNodeID() == HLNode::HLInstVal;
  }

  /// clone() - Create a copy of 'this' HLInst that is identical in all
  /// ways except the following:
  ///   * The HLInst has no parent
  ///   * Safe Reduction Successor is set to nullptr
  HLInst *clone() const override;

  /// \brief Returns the number of operands this HLInst is supposed to have.
  /// If lval is present, it becomes the 0th operand.
  unsigned getNumOperands() const override;

  /// \brief Returns true if this is in a loop's preheader.
  bool isInPreheader() const;
  /// \brief Returns true if this is in a loop's postexit.
  bool isInPostexit() const;
  /// \brief Returns true if this is in a loop's preheader or postexit.
  bool isInPreheaderOrPostexit() const;
};

} // End namespace loopopt

} // End namespace llvm

#endif
