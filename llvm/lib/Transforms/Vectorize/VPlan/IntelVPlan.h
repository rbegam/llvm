#ifndef LLVM_TRANSFORMS_VECTORIZE_VPLAN_INTELVPLAN_H 
#define LLVM_TRANSFORMS_VECTORIZE_VPLAN_INTELVPLAN_H

#include "../VPlan.h" // FIXME
#include "VPInstruction.h"
#include "VPLoopInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/Support/GenericDomTreeConstruction.h"

namespace llvm {

namespace vpo {

class VPLoop;
class VPLoopInfo;

class VPOneByOneIRRecipeBase : public VPRecipeBase {
  friend class VPlanUtilsLoopVectorizer;

private:
  /// Container of IR instructions
  VPInstructionContainerTy InstContainer;

protected:
  VPOneByOneIRRecipeBase() = delete;

  VPOneByOneIRRecipeBase(unsigned char SC,
                         const BasicBlock::iterator B,
                         const BasicBlock::iterator E,
                         class VPlan *Plan) : VPRecipeBase(SC) {
    for (auto It = B; It != E; ++It) {
      VPInstructionIR *VPInst;

      VPInst = new VPInstructionIR(&*It);
      InstContainer.insert(InstContainer.end(), VPInst);
    }
  }

  /// Do the actual code generation for a single instruction.
  /// This function is to be implemented and specialized by the respective
  /// sub-class.
  virtual void transformIRInstruction(Instruction *I,
                                      struct VPTransformState &State) = 0;

public:
  typedef VPInstructionContainerTy::iterator iterator;
  typedef VPInstructionContainerTy::const_iterator const_iterator;
  
  ~VPOneByOneIRRecipeBase() {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPScalarizeOneByOneSC ||
           V->getVPRecipeID() == VPRecipeBase::VPVectorizeOneByOneSC;
  }

  bool isScalarizing() {
    return getVPRecipeID() == VPRecipeBase::VPScalarizeOneByOneSC;
  }

  /// The method which generates all new IR instructions that correspond to
  /// this VPOneByOneIRRecipeBase in the vectorized version, thereby
  /// "executing" the VPlan.
  /// VPOneByOneIRRecipeBase may either scalarize or vectorize all Instructions.
  void vectorize(struct VPTransformState &State) override {
    auto It = begin();
    auto End = end();

    for (; It != End; ++It) {
      auto Inst = cast<VPInstructionIR>(&*It);
      transformIRInstruction(Inst->getInstruction(), State);
    }
  }

  iterator begin() { return InstContainer.begin(); }
  const_iterator begin() const { return InstContainer.begin(); }

  iterator end() { return InstContainer.end(); }
  const_iterator end() const { return InstContainer.end(); }
};

class VPLoopRegion : public VPRegionBlock {
  friend class IntelVPlanUtils; 
  
private: 
  // Pointer to VPLoopInfo analysis information for this loop region
  VPLoop *VPL;

public: 

  VPLoopRegion(const std::string &Name, VPLoop *L)
      : VPRegionBlock(VPLoopRegionSC, Name), VPL(L) {}

  const VPLoop *getVPLoop() const { return VPL; }
  VPLoop *getVPLoop() { return VPL; }

  //TODO: print as LoopBase or print as VPBlockBase
  //void print(raw_ostream &OS, unsigned Depth = 0) const {
  //  VPBlocLoopBase::print(OS, Depth);
  //}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPBlockBase *B) {
    return B->getVPBlockID() == VPBlockBase::VPLoopRegionSC;
  }
}; 

class IntelVPlan : public VPlan {

private:
  VPLoopInfo *VPLInfo;

public: 
  IntelVPlan() : VPlan(), VPLInfo(nullptr) {}

  ~IntelVPlan() {
    if (VPLInfo)
      delete (VPLInfo);
  }

  VPLoopInfo *getVPLoopInfo() { return VPLInfo; }
  const VPLoopInfo *getVPLoopInfo() const { return VPLInfo; }

  void setVPLoopInfo(VPLoopInfo *VPLI) { VPLInfo = VPLI; }
};

/// A VPUniformConditionBitRecipe is a VPConditionBitRecipe which supports a
/// uniform conditional branch. 
class VPUniformConditionBitRecipe : public VPConditionBitRecipeBase {
public:
  VPUniformConditionBitRecipe(Value *Cond)
    : VPConditionBitRecipeBase(VPUniformBranchSC) {
    ScConditionBit = Cond;
  }

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPUniformBranchSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void vectorize(VPTransformState &State) override;

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << "Uniform Branch condition. Output: ";
    if (ConditionBit) {
      O << *ConditionBit;
    } else {
      O << "NULL";
    }

    O << " Input: ";
    if (ScConditionBit) {
      O << *ScConditionBit;
    } else {
      O << "NULL";
    }
  }

  StringRef getName() const { return "Cond Bit Recipe"; };

private:
  Value *ScConditionBit;
};

/// A VPLiveInConditionBitRecipe is a recipe for a Condition operand of 
/// a uniform conditional branch. The Condition is defined outside the loop.
class VPLiveInConditionBitRecipe : public VPConditionBitRecipeBase {
public:
  VPLiveInConditionBitRecipe(Value *Cond)
    : VPConditionBitRecipeBase(VPLiveInBranchSC) {
    ConditionBit = Cond;
  }

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPLiveInBranchSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void vectorize(VPTransformState &State) override {}

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << "Live-in Branch condition: " << ConditionBit;
  }

  StringRef getName() const { return "Live-in cond Bit Recipe"; };
};

/// The IntelVPlanUtils class provides interfaces for the construction and
/// manipulation of a VPlan.
class IntelVPlanUtils : public VPlanUtils {

public:
  IntelVPlanUtils(IntelVPlan *Plan) : VPlanUtils(Plan) {}

  /// Creates a new VPScalarizeOneByOneRecipe or VPVectorizeOneByOneRecipe based
  /// on the isScalarizing parameter respectively.
  // TODO: VPlan is passed in the original interface. Why is this necessary if
  // VPlanUtils already has a copy?
  vpo::VPOneByOneIRRecipeBase *createOneByOneRecipe(const BasicBlock::iterator B,
                                                    const BasicBlock::iterator E,
                                                    // VPlan *Plan,
                                                    bool isScalarizing);

  /// Creates a new VPUniformConditionBitRecipe.
  VPUniformConditionBitRecipe *
  createUniformConditionBitRecipe(Value *Cond) {
    return new VPUniformConditionBitRecipe(Cond);
  }

  /// Creates a new VPLiveInConditionBitRecipe.
  VPLiveInConditionBitRecipe *
  createLiveInConditionBitRecipe(Value *Cond) {
    return new VPLiveInConditionBitRecipe(Cond);
  }

  /// Returns true if the edge FromBlock->ToBlock is a back-edge.
  static bool isBackEdge(const VPBlockBase *FromBlock,
                         const VPBlockBase *ToBlock) {
      assert(FromBlock->getParent() == ToBlock->getParent());
      // A back-edge has to be within a loop region
      const VPLoopRegion *LoopRegion = dyn_cast<VPLoopRegion>(FromBlock->getParent());
      if (! LoopRegion) {
          return false;
      }
      // A back-edge is latch->header
      const VPLoop *Loop = LoopRegion->getVPLoop();
      return (Loop->contains(FromBlock) && Loop->contains(ToBlock)
              && Loop->isLoopLatch(FromBlock)
              && (ToBlock == Loop->getHeader()));
  }


  /// Create a new, empty VPLoop, with no blocks.
  VPLoopRegion *createLoop(VPLoop *VPL) {
    VPLoopRegion *Loop = new VPLoopRegion(createUniqueName("loop"), VPL);
    setReplicator(Loop, false /*IsReplicator*/);
    return Loop;
  }

  /// Returns true if Block is a loop latch
  bool blockIsLoopLatch(const VPBlockBase *Block,
                        const VPLoopInfo *VPLInfo) const {

    if (const VPLoop *ParentVPL = VPLInfo->getLoopFor(Block)) {
      SmallVector<VPBlockBase *, 2> Latches;
      ParentVPL->getLoopLatches(Latches);
      return std::find(Latches.begin(), Latches.end(), Block) != Latches.end();
    }
   
    return false;
  }
};

} // End VPO Vectorizer Namespace 

// TODO: We may need this in VPlan.h/cpp eventually
typedef DomTreeNodeBase<VPBlockBase> VPDomTreeNode;

template <>
struct GraphTraits<VPDomTreeNode *>
    : public DomTreeGraphTraitsBase<VPDomTreeNode, VPDomTreeNode::iterator> {};

template <>
struct GraphTraits<const VPDomTreeNode *>
    : public DomTreeGraphTraitsBase<const VPDomTreeNode,
                                    VPDomTreeNode::const_iterator> {};

/// \brief Template specialization of the standard LLVM dominator tree utility
/// for VPBlocks.
class VPDominatorTree : public DominatorTreeBase<VPBlockBase> {

public:
  VPDominatorTree(bool isPostDom) : DominatorTreeBase<VPBlockBase>(isPostDom) {}

  virtual ~VPDominatorTree() {}
};


// From HIR POC
   
// #include "/users/grapapor/LLVM/VPlan/llvm/lib/Transforms/Vectorize/VPlan.h" 
// #include "llvm/IR/Intel_LoopIR/HLLoop.h" 
// #include "llvm/IR/Intel_LoopIR/HLInst.h" 
//   
// using namespace loopopt; 
//   
// namespace llvm { // LLVM Namespace 
// namespace vpo {  // VPO Vectorizer Namespace 
//   
//   
// // VPLoop Classes 
//   
// class VPLoop : public VPRegion { 
//   friend class VPlanHIR; 
//   
// private: 
//   const unsigned char VLID;   // Subclass identifier (for isa/dyn_cast) 
//   
// public: 
//   typedef enum { 
//     VPLoopHIRSC,   
//   } VPLoopTy; 
//   
//   VPLoop(const std::string &Name, VPLoopTy SC) 
//       : VPRegion(Name, VPLoopSC), VLID(SC) {} 
// }; 
//   
// class VPLoopHIR : public VPLoop { 
//   friend class VPlanHIR; 
//   
// private: 
//   /// Pointer to HIR loop node. 
//   HLLoop *HIRLoop; 
//   
// public: 
//   // Interface to create VPLoop from HIR loop. 
//   VPLoopHIR(const std::string &Name, HLLoop *HLoop) 
//       : VPLoop(Name, VPLoopHIRSC), HIRLoop(HLoop) {} 
//   
//   // VPLoop copy constructor. 
//   //VPLoopHIR(VPLoopHIR &VPOrigLoop); 
//   
//   virtual ~VPLoopHIR() override {} 
// }; 
//   
//   
// // VPIf Classes 
//   
// class VPIf : public VPRegion { 
//   friend class VPlanHIR; 
//   
// private: 
//   const unsigned char VIID;   // Subclass identifier (for isa/dyn_cast) 
//   
//   /// 'Then' branch chain 
//   VPChain *ThenChain = nullptr; 
//   
//   /// 'Else' branch chain. 
//   VPChain *ElseChain = nullptr; 
//   
// public: 
//   typedef enum { 
//     VPIfHIRSC,   
//   } VPIfTy; 
//   
//   VPIf(const std::string &Name, VPIfTy SC) 
//       : VPRegion(Name, VPIfSC), VIID(SC) {} 
//   
//   VPBlock *getThenChain() { return ThenChain; } 
//   VPBlock *getElseChain() { return ElseChain; } 
// }; 
//   
// class VPIfHIR : public VPIf { 
//   friend class VPlanHIR; 
//   
// private: 
//    /// Pointer to HIR If node. 
//   HLIf *HIRIf; 
//   
// public: 
//   // Interface to create AVRLoop from LLVM Loop. 
//   VPIfHIR(const std::string &Name, HLIf *HLIf) 
//       : VPIf(Name, VPIfHIRSC), HIRIf(HLIf) {} 
//   
//   // VPIf copy constructor. 
//   //VPIfHIR(VPIfHIR &VPOrigLoop); 
//   
//   virtual ~VPIfHIR() override {} 
// }; 
//   
//   
// // VPRecipes 
//   
// // Test 
// //class VPOneByOneRecipeHIR : public VPRecipe { 
// //private: 
// // 
// ////  bool IsScalarizing = false; 
// // 
// //protected: 
// //  //VPOneByOneRecipeHIR(bool IsScalarizing) : IsScalarizing(IsScalarizing) {} 
// //  VPOneByOneRecipeHIR() {} 
// // 
// //  // Do the actual vectorization work for a single instruction 
// //  // TODO 
// //  void vectorizeInstruction(HLInst *I, VPContext& Context) {}; 
// //}; 
// // 
// //typedef ilist<HLInst> HLInstList; 
// // 
// //class VPOneByOneShadowRecipeHIR : public VPOneByOneRecipeHIR { 
// //private: 
// //  HLInstList::iterator Begin; 
// //  HLInstList::iterator End; 
// // 
// //public: 
// //  VPOneByOneShadowRecipeHIR(const HLInstList::iterator &B, 
// //                            const HLInstList::iterator &E) 
// //      // const HLInstList::iterator& E, bool IsScalarizing) 
// //      : Begin(B), 
// //        End(E) {} 
// //  //: VPOneByOneRecipe(IsScalarizing), Begin(B), End(E) {} 
// // 
// //  void vectorize(VPContext& Context) override { 
// //    for (auto It = Begin; It != End; ++It) 
// //      vectorizeInstruction(&*It, Context); 
// //  } 
// // 
// //  void print(raw_ostream &O) const override { 
// //    bool First = true; 
// //    for (auto It = Begin; It != End; ++It) { 
// //      if (!First) 
// //        O << "\n"; 
// //      formatted_raw_ostream FO(O); 
// //      It->print(FO, 1);//->printAsOperand(O, true); 
// //      First = false; 
// //    } 
// //  } 
// //}; 
//   
//   
// // VPlan 
//   
// class VPlanHIR : public VPlan { 
//   
// public:  
//   
//   //TODO: Move to utils 
//   
//   VPLoopHIR *createLoopHIR(HLLoop *HLoop, bool IsReplicator = false) { 
//     VPLoopHIR *Loop = new VPLoopHIR(createUniqueName("loop"), HLoop); 
//     if (IsReplicator) 
//       setReplicator(Loop); 
//     return Loop; 
//   } 
//   
//   VPIfHIR *createIfHIR(HLIf *HIf, bool IsReplicator = false) { 
//     VPIfHIR *If = new VPIfHIR(createUniqueName("if"), HIf); 
//     if (IsReplicator) 
//       setReplicator(If); 
//     return If; 
//   } 
//   
//   void setIfThenChain(VPIf *VIf, VPChain *ThenChain) { 
//     assert(!VIf->getThenChain() && "ThenChain already set in this VPIf"); 
//     VIf->ThenChain = ThenChain; 
//   } 
//   
//   void setIfElseChain(VPIf *VIf, VPChain *ElseChain) { 
//     assert(!VIf->getElseChain() && "ElseChain already set in this VPIf"); 
//     VIf->ElseChain = ElseChain; 
//   } 
// }; 
//   

//} // End VPO Vectorizer Namespace 
}  // namespace llvm

  
#endif // LLVM_TRANSFORMS_VECTORIZE_VPLAN_INTELVPLAN_H 

