//===-- VPOAvr.h ------------------------------------------------*- C++ -*-===//
//
//   Copyright (C) 2015-2016 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//
///
/// \file
/// This file defines the Abstract Vector Representation (AVR) base node.
///
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_VPO_AVR_H
#define LLVM_ANALYSIS_VPO_AVR_H

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/Function.h"
#include "llvm/IR/BasicBlock.h"
#include "llvm/IR/CFG.h"
#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Analysis/Intel_VPO/Vecopt/VPOSLEV.h"

namespace llvm { // LLVM Namespace
namespace vpo {  // VPO Vectorizer Namespace

#define TabLength 2

/// Abstract Layer print verbosity levels
enum VerbosityLevel { PrintBase, PrintDataType, PrintAvrType, PrintAvrDecomp, PrintNumber };
/// Assignment LHS/RHS enumeration
enum AssignOperand { RightHand, LeftHand };

class AVRLoop;
class AVRPredicate;

/// \brief Abstract Vector Representation Node base class
///
/// This represents a node of the vectorizer AVR. It is used to represent
/// the incoming LLVM IR or incoming LoopOpt HIR.
///
/// This class (hierarchy) disallows creating objects on stack.
/// Objects are created/destroyed using AVRUtils friend class.
class AVR : public ilist_node<AVR> {

private:
  /// \brief Make class uncopyable.
  void operator=(const AVR &) = delete;

  /// SubClassID - AVR Subclass Identifier
  const unsigned char SubClassID;

  /// Parent - Lexical parent of this AVR
  AVR *Parent;

  /// GlobalNumber - A global number used for assigning unique ID to each avr
  static unsigned GlobalNumber;

  /// Number - Unique ID for AVR node.
  unsigned Number;

  /// Slev - SIMD lane evolution classification of this AVR node.
  SLEV Slev;

  /// Predicate - The AVR node that is the predicate masking this one.
  AVRPredicate* Predicate;

  /// \brief Destroys all objects of this class. Only called after Vectorizer
  /// phase code generation.
  static void destroyAll();

protected:
  AVR(unsigned SCID);
  AVR(const AVR &AVRObj);
  virtual ~AVR() {}

  /// \brief Destroys the object.
  void destroy();

  /// Sets unique ID for this AVR Node.
  void setNumber();

  /// \brief Sets the lexical parent of this AVR.
  void setParent(AVR *ParentNode) { Parent = ParentNode; }

  /// \brief Sets the predicate for this AVR.
  void setPredicate(AVRPredicate *P) { Predicate = P; }

  /// Only this utility class should be used to modify/delete AVR nodes.
  friend class AVRUtils;

  /// \brief Utility function for printing only known SLEVs.
  void printSLEV(formatted_raw_ostream &OS) const {
    if (getSLEV().isBOTTOM())
      return;
    getSLEV().printValue(OS);
    OS << " ";
  }

public:
  /// Virtual Clone Method
  virtual AVR *clone() const = 0;

  /// \brief Dumps AvrNode.
  void dump() const;

  /// \brief Dumps Avr Node at verbosity Level.
  void dump(VerbosityLevel VLevel) const;

  /// \brief Virtual print method. Derived classes should implement this
  /// routine.
  virtual void print(formatted_raw_ostream &OS, unsigned Depth,
                     VerbosityLevel VLevel) const = 0;

  /// \brief Virtual shallow-print method. Default implementation is to call the
  /// print() method (which is shallow for most nodes). Nodes containing other
  /// nodes should reimplement to just print the node itself without their
  /// contained nodes. 
  virtual void shallowPrint(formatted_raw_ostream &OS) const {

    print(OS, 0, PrintNumber);
  }

  /// \brief Returns a StringRef for the type name of this node.
  virtual StringRef getAvrTypeName() const = 0;

  /// \brief Returns the value name of this node.
  /// The string will be w.r.t to underlying IR.
  virtual std::string getAvrValueName() const { return "ANON"; };

  /// \brief Returns the Avr nodes's unique ID number
  unsigned getNumber() const { return Number; }

  /// \brief Returns the Avr nodes's SLEV data.
  SLEV getSLEV() const { return Slev; }

  /// \brief Returns the Avr nodes's predicating Avr node.
  AVRPredicate* getPredicate() const { return Predicate; }

  /// \brief Code generation for AVR.
  virtual void codeGen();

  /// \brief Returns the immediate lexical parent of the AVR.
  AVR *getParent() const { return Parent; }

  /// \brief Returns the parent loop of this node, if one exists.
  AVRLoop *getParentLoop() const;

  /// \brief Returns the strictly lexical parent loop of this node, if one
  /// exists.
  /// AVR nodes which are part of preheader or postexit will have different
  /// parent.
  AVRLoop *getLexicalParentLoop() const;

  /// \brief Return an ID for the concrete type of this object.
  ///
  /// This is used to implement the classof, etc. checks in LLVM and should't
  /// be used for any other purpose.
  unsigned getAVRID() const { return SubClassID; }

// AvrKind subclass enumeration
#include "llvm/Analysis/Intel_VPO/Vecopt/VPOAvrKinds.h"
};

} // End VPO Vectorizer Namspace

/// \brief Traits for iplist<AVR>
///
/// See ilist_traits<Instruction> in BasicBlock.h for details
template <>
struct ilist_traits<vpo::AVR> : public ilist_default_traits<vpo::AVR> {

  static vpo::AVR *createNode(const vpo::AVR &) {
    llvm_unreachable("AVR should be explicitly created via AVRUtils"
                     "class");

    return nullptr;
  }
  static void deleteNode(vpo::AVR *) {}
};

namespace vpo { // VPO Vectorizer Namespace

typedef iplist<AVR> AVRContainerTy;

// TODO: Remove this.
extern AVRContainerTy AVRFunctions;

} // End VPO Vectorizer Namespace
} // End LLVM Namespace

#endif // LLVM_ANALYSIS_VPO_AVR_H
