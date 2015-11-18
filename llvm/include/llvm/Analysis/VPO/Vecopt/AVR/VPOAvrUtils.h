//===------------------------------------------------------------*- C++ -*-===//
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
//   VPOAvr.h -- Defines the utilities class for AVR nodes.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANAYSIS_VPO_AVR_UTILS_H
#define LLVM_ANAYSIS_VPO_AVR_UTILS_H

#include "llvm/Support/Compiler.h"
#include "llvm/Analysis/VPO/Vecopt/AVR/VPOAvr.h"
#include "llvm/Analysis/VPO/Vecopt/AVR/VPOAvrFunction.h"
#include "llvm/Analysis/VPO/Vecopt/AVR/VPOAvrLoop.h"
#include "llvm/Analysis/VPO/Vecopt/AVR/VPOAvrStmt.h"
#include "llvm/Analysis/VPO/Vecopt/AVR/VPOAvrIf.h"

namespace llvm { // LLVM Namespace

class LoopInfo;

namespace vpo {  // VPO Vectorizer Namespace

// Enumeration for types of Avr insertions.
enum InsertType { FirstChild, LastChild, Append, Prepend};
enum SplitType  { None, ThenChild, ElseChild};

// Avr Iterator Type
typedef AVRContainerTy::iterator AvrItr;

/// \brief This class defines the utilies for AVR nodes.
///
/// It contains functions which are used to create, modify, and destroy
/// AVR nodes.
///
class AVRUtils {

private:

  // Internal implementations of utility helper functions, not meant
  // to be called externally.

  /// \brief Internal helper function for removing and deleting avrs
  /// and sequences of avrs.
  static AVRContainerTy *removeInternal(AvrItr Begin, AvrItr End,
                                        AVRContainerTy *MoveContainer, 
                                        bool Delete);

  static void insertAVRSeq(AVR *NewParent, AVRContainerTy &ToContainer,
                           AvrItr InsertionPos, AVRContainerTy *FromContainer,
                           AvrItr Begin, AvrItr End, InsertType Itype);

public:

  // Creation Utilities

  /// \brief Returns a new AVRFunction node.
  static AVRFunction *createAVRFunction(Function *OrigF, const LoopInfo *LpInfo);

  /// \brief Returns a new AVRLoop node.
  static AVRLoop *createAVRLoop(const Loop *Lp);

  /// \brief Returns a new AVRLoop node.
  static AVRLoop *createAVRLoop(WRNVecLoopNode *WrnSimdNode);

  /// \brief Returns a new AVRWrn node.
  static AVRWrn *createAVRWrn(WRNVecLoopNode *WrnSimdNode);

  /// \brief Returns a new AVRLoop node.
  static AVRLoop *createAVRLoop();

  /// \brief Returns a new AVRExpr node.
  static AVRExpr *createAVRExpr();

  /// \brief Returns a new AVRBranch node.
  static AVRBranch *createAVRBranch(AVRLabel *Sucessor);

  // Modification Utilities


  // Insertion Utilities

  /// \brief Standard Insert Utility wrapper for AVRIf.
  static void insertAVR(AVR *Parent, AvrItr Postion, AvrItr NewAvr,
                        InsertType Itype, SplitType SType = None);

  /// \brief Inserts NewAvr node as the first child in Parent avr.
  static void insertFirstChildAVR(AVR *Parent, AvrItr NewAvr);

  /// \brief Inserts NewAvr node as the first 'Then' child of AVRIf
  static void insertFirstThenChild(AVRIf *AvrIf, AvrItr NewAvr);

  /// \brief Inserts NewAvr node as the first 'Else' child of AVRIf
  static void insertFirstElseChild(AVRIf *AvrIf, AvrItr NewAvr);

  /// \brief Inserts NewAvr node as the last child in Parent avr.
  static void insertLastChildAVR(AVR *Parent, AvrItr NewAvr);

  /// \brief Inserts NewAvr node as the last 'Then' child of AVRIf
  static void insertLastThenChild(AVRIf *AvrIf, AvrItr NewAvr);

  /// \brief Inserts NewAvr node as the last 'Else' child of AVRIf
  static void insertLastElseChild(AVRIf *AvrIf, AvrItr NewAvr);

  /// \brief Inserts an unlinked AVR node after InsertionPos in AVR list.
  static void insertAVRAfter(AvrItr InsertionPos, AVR *Node);

  /// \brief Inserts an unlinked AVR node before InsertionPos in AVR list.
  static void insertAVRBefore(AvrItr InsertionPos, AVR *Node);

  // Move Utilities

  /// \brief Moves AVR from current location to after InsertionPos
  static void moveAfter(AvrItr InsertionPos, AVR *Node);

  /// \brief Unlinks [First, Last] from their current position and inserts them
  /// at the begining of the parent loop's children.
  static void moveAsFirstChildren(AVRLoop *ALoop, AvrItr First, AvrItr Last);

  /// \brief Unlinks Node from its current location and inserts it as the
  /// first 'Then' child of AvrIf
  static void moveAsFirstThenChild(AVRIf *AvrIf, AVR *Node);

  /// \brief Unlinks Node from its current location and inserts it as the
  /// last 'Then' child of AvrIf
  static void moveAsLastThenChild(AVRIf *AvrIf, AVR *Node);

  /// \brief Unlinks Node from its current location and inserts it as the
  /// first 'Else' child of AvrIf
  static void moveAsFirstElseChild(AVRIf *AvrIf, AVR *Node);

  /// \brief Unlinks Node from its current location and inserts it as the
  /// last 'Else' child of AvrIf
  static void moveAsLastElseChild(AVRIf *AvrIf, AVR *Node);

  /// \brief Unlinks [First, Last] from its current location and inserts them
  /// at the begining of 'Then' children of AvrIf
  static void moveAsFirstThenChildren(AVRIf *AIf, AvrItr First, AvrItr Last);

  /// \brief Unlinks [First, Last] from its current location and inserts them
  /// at the begining of 'Else' children of AvrIf
  static void moveAsFirstElseChildren(AVRIf *AIf, AvrItr First, AvrItr Last);

  // Removal Utilities

  /// \brief Destroys the passed in AVR node.
  static void destroy(AVR *Node);

  /// \brief Unlinks AVR node from avr list.
  static void remove(AVR *Node);

  /// \brief Unlinks AVR nodes from Begin to End from the avr list.
  /// Returns a pointer to an AVRContainer of the removed sequence
  static void remove(AvrItr Begin, AvrItr End);

  /// \brief Unlinks the AVR nodes which are inside AvrSequence from
  /// thier parent AVRnode container. Returns a pointer to removed container.
  static void remove(AVRContainerTy &AvrSequence)
    { remove(AvrSequence.begin(), AvrSequence.end()); }

  /// \brief Unlinks Avr node from avr list and destroys it.
  static void erase(AVR *Node);

  /// \brief Unlinks [First, Last) from AVR list and destroys them.
  static void erase(AVRContainerTy::iterator First,
                    AVRContainerTy::iterator Last);

  /// \brief Replaces OldNode by an unlinked NewNode.
  static void replace(AVR *OldNode, AVR *NewNode);

  // Search Utilities

  /// \brief Returns true if the given Container contains Node as immediate child.
  /// Uses non-recursive search.
  static bool containsAvr(AVRContainerTy &Container, AVR *Node);

  /// \brief Retrun a pointer the AVRContainer which contains Avr.
  static AVRContainerTy *getAvrContainer(AVR *Avr);

};

} // End VPO Vectorizer Namespace
} // End LLVM Namespace

#endif // LLVM_ANAYSIS_VPO_AVR_UTILS_H


