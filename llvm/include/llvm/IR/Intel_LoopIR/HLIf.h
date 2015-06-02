//===--------------- HLIf.h - High level IR if node -------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//
//
// This file defines the HLIf node.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_IR_INTEL_LOOPIR_HLIF_H
#define LLVM_IR_INTEL_LOOPIR_HLIF_H

#include "llvm/IR/Intel_LoopIR/HLDDNode.h"
#include "llvm/IR/InstrTypes.h"

namespace llvm {

namespace loopopt {

class HLLoop;

/// \brief High level node representing a conditional branch
///
/// Sample HLIf-
/// If( (Op1 Pred1 Op2) AND (Op3 Pred2 Op4) )
class HLIf : public HLDDNode {
public:
  typedef std::vector<CmpInst::Predicate> PredicateTy;
  typedef std::vector<unsigned> ConjunctionTy;
  typedef HLContainerTy ChildNodeTy;

  /// Iterators to iterate over predicates
  typedef PredicateTy::iterator pred_iterator;
  typedef PredicateTy::const_iterator const_pred_iterator;
  typedef PredicateTy::reverse_iterator reverse_pred_iterator;
  typedef PredicateTy::const_reverse_iterator const_reverse_pred_iterator;

  /// Iterators to iterate over then/else children nodes
  typedef ChildNodeTy::iterator then_iterator;
  typedef ChildNodeTy::const_iterator const_then_iterator;
  typedef ChildNodeTy::reverse_iterator reverse_then_iterator;
  typedef ChildNodeTy::const_reverse_iterator const_reverse_then_iterator;

  typedef then_iterator else_iterator;
  typedef const_then_iterator const_else_iterator;
  typedef reverse_then_iterator reverse_else_iterator;
  typedef const_reverse_then_iterator const_reverse_else_iterator;

private:
  /// HLIf should contain two operands(DDRefs) per predicate unless the
  /// predicate is FCMP_TRUE or FCMP_FALSE in which case they can be null.
  /// It should contain at least one predicate. Predicates are joined using the
  /// implicit AND conjunction.
  PredicateTy Predicates;
  /// Contains both then and else children, in that order.
  /// Having a single container allows for more efficient and cleaner
  /// implementation of insert(Before/After) and remove(Before/After).
  ChildNodeTy Children;
  /// Iterator pointing to the begining of else children.
  ChildNodeTy::iterator ElseBegin;

protected:
  HLIf(CmpInst::Predicate FirstPred, RegDDRef *Ref1, RegDDRef *Ref2);

  /// HLNodes are destroyed in bulk using HLNodeUtils::destroyAll(). iplist<>
  /// tries to
  /// access and destroy the nodes if we don't clear them out here.
  ~HLIf() { Children.clearAndLeakNodesUnsafely(); }

  /// \brief Copy constructor used by cloning.
  HLIf(const HLIf &HLIfObj, GotoContainerTy *GotoList, LabelMapTy *LabelMap);

  friend class HLNodeUtils;
  /// Required to access setParent() for loop's ztt.
  friend class HLLoop;

  /// Implements getNumOperands() functionality.
  unsigned getNumOperandsInternal() const;

  /// \brief Initializes some of the members to bring the object in a sane
  /// state.
  void initialize();

  /// \brief Returns the offset of the LHS/RHS DDRef associated with this
  /// predicate.
  unsigned getPredicateOperandDDRefOffset(pred_iterator PredI,
                                          bool IsLHS) const;
  unsigned getPredicateOperandDDRefOffset(const_pred_iterator PredI,
                                          bool IsLHS) const;

  /// \brief Clone Implementation
  /// This function populates the GotoList with Goto branching within the
  /// cloned If and LabelMap with Old and New Labels. Returns a cloned If.
  HLIf *cloneImpl(GotoContainerTy *GotoList,
                  LabelMapTy *LabelMap) const override;

public:
  /// \brief Prints HLIf.
  virtual void print(formatted_raw_ostream &OS, unsigned Depth) const override;

  /// \brief Returns the underlying type of if.
  Type *getLLVMType() const;

  /// Predicate iterator methods
  pred_iterator pred_begin() { return Predicates.begin(); }
  const_pred_iterator pred_begin() const { return Predicates.begin(); }
  pred_iterator pred_end() { return Predicates.end(); }
  const_pred_iterator pred_end() const { return Predicates.end(); }

  reverse_pred_iterator pred_rbegin() { return Predicates.rbegin(); }
  const_reverse_pred_iterator pred_rbegin() const {
    return Predicates.rbegin();
  }
  reverse_pred_iterator pred_rend() { return Predicates.rend(); }
  const_reverse_pred_iterator pred_rend() const { return Predicates.rend(); }

  /// \brief Returns the number of predicates associated with this if.
  unsigned getNumPredicates() const { return Predicates.size(); }

  /// Children iterator methods
  then_iterator then_begin() { return Children.begin(); }
  const_then_iterator then_begin() const { return Children.begin(); }
  then_iterator then_end() { return ElseBegin; }
  const_then_iterator then_end() const { return ElseBegin; }

  reverse_then_iterator then_rbegin() {
    return ChildNodeTy::reverse_iterator(ElseBegin);
  }
  const_reverse_then_iterator then_rbegin() const {
    return ChildNodeTy::const_reverse_iterator(ElseBegin);
  }
  reverse_then_iterator then_rend() { return Children.rend(); }
  const_reverse_then_iterator then_rend() const { return Children.rend(); }

  else_iterator else_begin() { return ElseBegin; }
  const_else_iterator else_begin() const { return ElseBegin; }
  else_iterator else_end() { return Children.end(); }
  const_else_iterator else_end() const { return Children.end(); }

  reverse_else_iterator else_rbegin() { return Children.rbegin(); }
  const_reverse_else_iterator else_rbegin() const { return Children.rbegin(); }

  reverse_else_iterator else_rend() {
    return ChildNodeTy::reverse_iterator(ElseBegin);
  }
  const_reverse_else_iterator else_rend() const {
    return ChildNodeTy::const_reverse_iterator(ElseBegin);
  }

  /// Children acess methods

  /// \brief Returns the first then child if it exists, otherwise
  /// returns null.
  HLNode *getFirstThenChild();
  const HLNode *getFirstThenChild() const {
    return const_cast<HLIf *>(this)->getFirstThenChild();
  }
  /// \brief Returns the last then child if it exists, otherwise
  /// returns null.
  HLNode *getLastThenChild();
  const HLNode *getLastThenChild() const {
    return const_cast<HLIf *>(this)->getLastThenChild();
  }

  /// \brief Returns the number of then children.
  unsigned getNumThenChildren() const {
    return std::distance(then_begin(), then_end());
  }
  /// \brief Returns true if it has then children.
  bool hasThenChildren() const { return (then_begin() != then_end()); }

  /// \brief Returns the first else child if it exists, otherwise returns null.
  HLNode *getFirstElseChild();
  const HLNode *getFirstElseChild() const {
    return const_cast<HLIf *>(this)->getFirstElseChild();
  }
  /// \brief Returns the last else child if it exists, otherwise returns null.
  HLNode *getLastElseChild();
  const HLNode *getLastElseChild() const {
    return const_cast<HLIf *>(this)->getLastElseChild();
  }

  /// \brief Returns the number of else children.
  unsigned getNumElseChildren() const {
    return std::distance(else_begin(), else_end());
  }
  /// \brief Returns true if it has else children.
  bool hasElseChildren() const { return (else_begin() != else_end()); }

  /// \brief Method for supporting type inquiry through isa, cast, and dyn_cast.
  static bool classof(const HLNode *Node) {
    return Node->getHLNodeID() == HLNode::HLIfVal;
  }

  /// clone() - Create a copy of 'this' HLIf that is identical in all
  /// ways except the following:
  ///   * The HLIf has no parent
  /// This method will automatically update the goto branches with new labels
  /// inside the cloned If.
  HLIf *clone() const override;

  /// \brief Returns the number of operands this HLIf is supposed to have.
  unsigned getNumOperands() const override;

  /// \brief Adds new predicate in HLIf.
  void addPredicate(CmpInst::Predicate Pred, RegDDRef *Ref1, RegDDRef *Ref2);

  /// \brief Removes the associated predicate and operand DDRefs(not destroyed).
  /// Example-
  /// Before:
  /// If((Op1 Pred1 Op2) AND (Op3 Pred2 Op4) AND (Op5 Pred3 Op6))
  ///
  /// removePredicate(SecondPredIter);
  ///
  /// After:
  /// If((Op1 Pred1 Op2) AND (Op5 Pred3 Op6))
  void removePredicate(pred_iterator PredI);

  /// \brief Returns the LHS/RHS operand DDRef of the predicate based on the
  /// IsLHS flag.
  RegDDRef *getPredicateOperandDDRef(pred_iterator PredI, bool IsLHS);
  const RegDDRef *getPredicateOperandDDRef(const_pred_iterator PredI,
                                           bool IsLHS) const;

  /// \brief Sets the LHS/RHS operand DDRef of the predicate based on the IsLHS
  /// flag.
  void setPredicateOperandDDRef(RegDDRef *Ref, pred_iterator PredI, bool IsLHS);

  /// \brief Removes and returns the LHS/RHS operand DDRef of the predicate
  /// based on the IsLHS flag.
  RegDDRef *removePredicateOperandDDRef(pred_iterator PredI, bool IsLHS);
};

} // End namespace loopopt

} // End namespace llvm

#endif
