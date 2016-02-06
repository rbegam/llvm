//===--------- WRegionNode.h - W-Region Graph Node --------------*- C++ -*-===//
//
//   Copyright (C) 2015 Intel Corporation. All rights reserved.
//
//   The information and source code contained herein is the exclusive
//   property of Intel Corporation. and may not be disclosed, examined
//   or reproduced in whole or in part without explicit written authorization
//   from the company.
//
//===----------------------------------------------------------------------===//
//
//   This file defines the W-Region Graph node.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_ANALYSIS_VPO_WREGIONNODE_H
#define LLVM_ANALYSIS_VPO_WREGIONNODE_H

#include "llvm/ADT/ilist.h"
#include "llvm/ADT/ilist_node.h"
#include "llvm/ADT/StringRef.h"

#include "llvm/IR/Dominators.h"
#include "llvm/Support/ErrorHandling.h"
#include "llvm/Support/FormattedStream.h"
#include "llvm/Support/raw_ostream.h"
#include "llvm/Support/Debug.h"

#include "llvm/IR/InstrTypes.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/IR/BasicBlock.h"

#include "llvm/Transforms/Intel_VPO/Utils/VPOUtils.h"
#include "llvm/Analysis/Intel_VPO/WRegionInfo/WRegionClause.h"

#include <set>
#include <unordered_map>

namespace llvm {

namespace vpo {

extern std::unordered_map<int, StringRef> WRNName;

typedef VPOSmallVectorBB WRegionBBSetTy;

class WRegionNode;

typedef iplist<WRegionNode> WRContainerTy;

/// \brief WRegion Node base class
class WRegionNode : public ilist_node<WRegionNode> {

public:

  /// Iterators to iterator over basic block set
  typedef WRegionBBSetTy::iterator bbset_iterator;
  typedef WRegionBBSetTy::const_iterator bbset_const_iterator;
  typedef WRegionBBSetTy::reverse_iterator bbset_reverse_iterator;
  typedef WRegionBBSetTy::const_reverse_iterator bbset_const_reverse_iterator;

private:
  /// \brief Make class uncopyable.
  void operator=(const WRegionNode &) = delete;

  /// Unique number associated with this WRegionNode.
  unsigned Number;

  /// ID to differentitate between concrete subclasses.
  const unsigned SubClassID;

  /// Entry and Exit BBs of this WRN
  BasicBlock    *EntryBBlock;
  BasicBlock    *ExitBBlock;

  /// Set containing all the BBs in this WRN
  WRegionBBSetTy BBlockSet;

  /// Enclosing parent of WRegionNode in CFG.
  WRegionNode *Parent;

  /// True if the WRN came from HIR; false otherwise
  bool IsFromHIR;

  /// Counter used for assigning unique numbers to WRegionNodes.
  static unsigned UniqueNum;

  /// \brief Sets the unique number associated with this WRegionNode.
  void setNextNumber() { Number = ++UniqueNum; }

  /// \brief Sets the flag to indicate if WRN came from HIR
  void setIsFromHIR(bool flag) { IsFromHIR = flag; }

  /// \brief Destroys all objects of this class. Should only be
  /// called after code gen.
  static void destroyAll();

protected:

  /// \brief constructors
  WRegionNode(unsigned SCID, BasicBlock *BB); // for LLVM IR
  WRegionNode(unsigned SCID);                 // for HIR only
  WRegionNode(WRegionNode *W);                // for both

  // copy constructor not needed (at least for now)
  // WRegionNode(const WRegionNode &WRegionNodeObj);

  /// \brief Destroys the object.
  void destroy();

  /// \brief Sets the entry(first) bblock of this region.
  void setEntryBBlock(BasicBlock *EntryBB) { EntryBBlock = EntryBB; }

  /// \brief Sets the exit(last) bblock of this region.
  void setExitBBlock(BasicBlock *ExitBB) { ExitBBlock = ExitBB; }

  /// \brief Sets the graph parent of this WRegionNode.
  void setParent(WRegionNode *P) { Parent = P; }

  // The following three functions extract clause info from intrinsics.
  // There are three such intrinsics: intel_directive_qual,  
  // intel_directive_qual_opnd, and  intel_directive_qual_opndlist.
  
  /// \brief Update WRN for clauses with no operands.
  void handleQual(int ClauseID);

  /// \brief Update WRN for clauses with one operand.
  void handleQualOpnd(int ClauseID, Value *V);

  /// \brief Update WRN for clauses with operand list.
  void handleQualOpndList(int ClauseID, IntrinsicInst* Call);

  // Below are virtual functions to get/set clause information of the WRN.
  // These routines should never be called; calling them indicates intention
  // to access clause info for a WRN that does not allow such clause (eg, a 
  // parallel construct does not take a collapse clause). These virtual 
  // functions defined in the base class will all emit an error message.
  // Note: The return stmt in the getters below prevent compiler warnings
  //       when building the compiler.
  void errorClause(StringRef ClauseName) const;
  void errorClause(int ClauseID) const;

  virtual void setAligned(AlignedClause *A)  {errorClause(QUAL_OMP_ALIGNED);  }
  virtual AlignedClause *getAligned()  const {errorClause(QUAL_OMP_ALIGNED);
                                              return nullptr;                 }
  virtual void setCollapse(int N)            {errorClause(QUAL_OMP_COLLAPSE); }
  virtual int getCollapse()            const {errorClause(QUAL_OMP_COLLAPSE);
                                              return 0;                       }
  virtual void setCopyin(CopyinClause *C)    {errorClause(QUAL_OMP_COPYIN);   }
  virtual CopyinClause *getCopyin()    const {errorClause(QUAL_OMP_COPYIN);
                                              return nullptr;                 }
  virtual void setDefault(WRNDefaultKind T)  {errorClause("DEFAULT");         }
  virtual WRNDefaultKind getDefault()  const {errorClause("DEFAULT");
                                              return WRNDefaultAbsent;        }
  virtual void setFpriv(FirstprivateClause *F)
                          {errorClause(QUAL_OMP_FIRSTPRIVATE);                }
  virtual FirstprivateClause *getFpriv()const
                          {errorClause(QUAL_OMP_FIRSTPRIVATE); return nullptr;}

  virtual void setIf(EXPR E)                 {errorClause(QUAL_OMP_IF);       }
  virtual EXPR getIf()                 const {errorClause(QUAL_OMP_IF);
                                              return nullptr;                 }
  virtual void setLpriv(LastprivateClause *L)
                          {errorClause(QUAL_OMP_LASTPRIVATE);                 }
  virtual LastprivateClause *getLpriv()const
                          {errorClause(QUAL_OMP_LASTPRIVATE); return nullptr; }

  virtual void setLinear(LinearClause *L)    {errorClause(QUAL_OMP_LINEAR);   }
  virtual LinearClause *getLinear()    const {errorClause(QUAL_OMP_LINEAR);
                                              return nullptr;                 }
  virtual void setNumThreads(EXPR E)       {errorClause(QUAL_OMP_NUM_THREADS);}
  virtual EXPR getNumThreads()       const {errorClause(QUAL_OMP_NUM_THREADS);
                                            return nullptr;                   }
  virtual void setPriv(PrivateClause *P)     {errorClause(QUAL_OMP_PRIVATE);  }
  virtual PrivateClause *getPriv()     const {errorClause(QUAL_OMP_PRIVATE);  
                                              return nullptr;                 }
  virtual void setProcBind(WRNProcBindKind P){errorClause("PROC_BIND");       }
  virtual WRNProcBindKind setProcBind()const {errorClause("PROC_BIND");       
                                              return WRNProcBindAbsent;       }
  virtual void setRed(ReductionClause *R)    {errorClause("REDUCTION");       }
  virtual ReductionClause *getRed()    const {errorClause("REDUCTION");       
                                              return nullptr;}
  virtual void setSafelen(int N)             {errorClause(QUAL_OMP_SAFELEN);  }
  virtual int getSafelen()             const {errorClause(QUAL_OMP_SAFELEN);  
                                              return 0;                       }
  virtual void setShared(SharedClause *S)    {errorClause(QUAL_OMP_SHARED);   }
  virtual SharedClause *getShared()    const {errorClause(QUAL_OMP_SHARED);
                                              return nullptr;                 }
  virtual void setSimdlen(int N)             {errorClause(QUAL_OMP_SIMDLEN);  }
  virtual int getSimdlen()             const {errorClause(QUAL_OMP_SIMDLEN);
                                              return 0;                       }
  // TODO: complete the list as we implement more WRN kinds
  

  /// Only these classes are allowed to create/modify/delete WRegionNode.
  friend class WRegionUtils;
  friend class WRegionCollection;  //temporary

public:
  
#if 0
  // WRegionNodes are destroyed in bulk using
  // WRegionUtils::destroyAll(). iplist<> tries to
  // access and destroy the nodes if we don't clear them out here.
  virtual ~WRegionNode() { Children.clearAndLeakNodesUnsafely(); }
#else
  virtual ~WRegionNode() {}
#endif

  // Virtual Clone Method
  // virtual WRegionNode *clone() const = 0;

  /// \brief Returns the unique number associated with this WRegionNode.
  unsigned getNumber() const { return Number; }

  /// \brief Returns the flag that indicates if WRN came from HIR
  bool getIsFromHIR() const { return IsFromHIR; }

  /// \brief Dumps WRegionNode.
  void dump() const;

  /// \brief Prints WRegionNode.
  //  Actual code from derived class only
  virtual void print(formatted_raw_ostream &OS, unsigned Depth) const = 0;

  /// \brief Prints WRegionNode children.
  void printChildren(formatted_raw_ostream &OS, unsigned Depth) const;

  /// \brief Returns the predecessor bblock of this region.
  BasicBlock *getPredBBlock() const;

  /// \brief Returns the successor bblock of this region.
  BasicBlock *getSuccBBlock() const;

  /// \brief Returns the immediate enclosing parent of the WRegionNode.
  WRegionNode *getParent() const { return Parent; }

  /// Children acess methods

  /// \brief Returns true if it has children.
  bool hasChildren() const ;

  /// \brief Returns the number of children.
  unsigned getNumChildren() const ;

  /// \brief Return address of the Children container 
  WRContainerTy &getChildren() ;

  /// \brief Returns the first child if it exists, otherwise returns null.
  WRegionNode *getFirstChild();

  /// \brief Returns the last child if it exists, otherwise returns null.
  WRegionNode *getLastChild();

  /// \brief Returns an ID for the concrete type of this object.
  ///
  /// This is used to implement the classof checks in LLVM and should't
  /// be used for any other purpose.
  unsigned getWRegionKindID() const { return SubClassID; }

  /// \brief Returns the name for this WRN based on its SubClassID
  StringRef getName() const;

  // Methods for BBlockSet

  /// \brief Returns the entry(first) bblock of this region.
  BasicBlock *getEntryBBlock() const { return EntryBBlock; }

  /// \brief Returns the exit(last) bblock of this region.
  BasicBlock *getExitBBlock() const { return ExitBBlock; }

  /// Basic Block set iterator methods.
  bbset_iterator bbset_begin() { return BBlockSet.begin(); }
  bbset_const_iterator bbset_begin() const { return BBlockSet.begin(); }
  bbset_iterator bbset_end() { return BBlockSet.end(); }
  bbset_const_iterator bbset_end() const { return BBlockSet.end(); }

  bbset_reverse_iterator bbset_rbegin() { return BBlockSet.rbegin(); }
  bbset_const_reverse_iterator bbset_rbegin() const { return BBlockSet.rbegin(); }
  bbset_reverse_iterator bbset_rend() { return BBlockSet.rend(); }
  bbset_const_reverse_iterator bbset_rend() const { return BBlockSet.rend(); }

  /// \brief Returns True if BasicBlockSet is empty.
  unsigned isBBSetEmpty() const { return BBlockSet.empty(); }

  /// \brief Returns the number of BasicBlocks in BBlockSet.
  unsigned getBBSetSize() const { return BBlockSet.size(); }

  /// \brief Populates BBlockSet with BBs in the WRN from EntryBB to ExitBB.
  void populateBBSet();

  void resetBBSet() { BBlockSet.clear(); }

  // Derived Class Enumeration

  /// \brief An enumeration to keep track of the concrete subclasses of 
  /// WRegionNode
  enum WRegionNodeKind{
    // These require outlining:
    WRNParallel,
    WRNParallelLoop,
    WRNParallelSections,
    WRNTask,
    WRNTaskLoop,

    // These don't require outlining:
    WRNVecLoop,
    WRNWksLoop,
    WRNWksSections,
    WRNSection,
    WRNSingle,
    WRNMaster,
    WRNAtomic,
    WRNBarrier,
    WRNCancel,
    WRNCritical,
    WRNFlush,
    WRNOrdered,
    WRNTaskgroup
  };
}; // class WRegionNode

} // End vpo namespace


/// \brief traits for iplist<WRegionNode>
///
/// Refer to ilist_traits<Instruction> in BasicBlock.h for explanation.
template <>
struct ilist_traits<vpo::WRegionNode>
    : public ilist_default_traits<vpo::WRegionNode> {

  vpo::WRegionNode *createSentinel() const {
    return static_cast<vpo::WRegionNode *>(&Sentinel);
  }

  static void destroySentinel(vpo::WRegionNode *) {}

  vpo::WRegionNode *provideInitialHead() const { return createSentinel(); }
  vpo::WRegionNode *ensureHead(vpo::WRegionNode *) const {
    return createSentinel();
  }
  static void noteHead(vpo::WRegionNode *, vpo::WRegionNode *) {}

  static vpo::WRegionNode *createWRegionNode(const vpo::WRegionNode &) {
    llvm_unreachable("WRegionNodes should be explicitly created via" 
                     "WRegionUtils class");
    return nullptr;
  }
  static void deleteWRegionNode(vpo::WRegionNode *) {}

private:
  mutable ilist_half_node<vpo::WRegionNode> Sentinel;
};

} // End llvm namespace

#endif
