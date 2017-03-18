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
      Plan->setInst2Recipe(&*It, this);
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

  void removeInstructions(VPInstructionContainerTy::iterator B,
                          VPInstructionContainerTy::iterator E) {
    InstContainer.erase(B, E);  
  }

  iterator begin() { return InstContainer.begin(); }
  const_iterator begin() const { return InstContainer.begin(); }

  iterator end() { return InstContainer.end(); }
  const_iterator end() const { return InstContainer.end(); }
};

class VPVectorizeOneByOneIRRecipe : public VPOneByOneIRRecipeBase {
  friend class VPlanUtilsLoopVectorizer;

private:
  /// Do the actual code generation for a single instruction.
  void transformIRInstruction(Instruction *I, VPTransformState &State) override;

public:
  VPVectorizeOneByOneIRRecipe(const BasicBlock::iterator B,
                              const BasicBlock::iterator E, VPlan *Plan)
      : VPOneByOneIRRecipeBase(VPVectorizeOneByOneSC, B, E, Plan) {}

  ~VPVectorizeOneByOneIRRecipe() {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPVectorizeOneByOneSC;
  }

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    auto It = begin();
    auto End = end();

    O << "Vectorize VPInstIR:";
    for (; It != End; ++It) {
      auto Inst = cast<VPInstructionIR>(&*It);
      auto IRInst = Inst->getInstruction();
      O << '\n' << *IRInst;
      if (willAlsoPackOrUnpack(IRInst))
        O << " (S->V)";
    }
  }
};

class VPBranchIfNotAllZeroRecipe : public VPConditionBitRecipeBase {
  friend class VPlanUtilsLoopVectorizer;

public:
  VPBranchIfNotAllZeroRecipe(Value *Cond, VPlan *Plan) :
      VPConditionBitRecipeBase(VPBranchIfNotAllZeroRecipeSC) {
    Instruction *I = dyn_cast<Instruction>(Cond);
    assert(I && "Expected Cond to be an instruction");
    Plan->setInst2Recipe(I, this);
    ConditionBit = Cond;
  }

  ~VPBranchIfNotAllZeroRecipe() {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPBranchIfNotAllZeroRecipeSC;
  }

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << "IfNotAllZero: ";
    O << '\n' << *ConditionBit;
  }

  void vectorize(struct VPTransformState &State) override {
    // TODO: vectorizing this recipe should involve generating some type of
    // intrinsic that can do a vector compare, since branch instructions cannot
    // simply be widened.
  }

  StringRef getName() const { return "Branch If Not All Zero Recipe"; }
};

class VPMaskGenerationRecipe : public VPRecipeBase {
  friend class VPlanUtilsLoopVectorizer;

private:
  const Value *IncomingPred;
  const Value *LoopBackedge;

public:
  VPMaskGenerationRecipe(const Value *Pred, const Value *Backedge) :
      VPRecipeBase(VPMaskGenerationRecipeSC), IncomingPred(Pred),
      LoopBackedge(Backedge) {}

  ~VPMaskGenerationRecipe() {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPMaskGenerationRecipeSC;
  }

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << "MaskGeneration: ";
    O << '\n' << *IncomingPred << " & " << *LoopBackedge;
  }

  void vectorize(struct VPTransformState &State) override {
    // TODO: vectorizing this recipe should involve generating a mask for the
    // instructions in the loop body. i.e., a phi instruction that has incoming
    // values using IncomingPred and LoopBackedge.
  }
};

class VPNonUniformConditionBitRecipe : public VPConditionBitRecipeBase {
  friend class VPlanUtilsLoopVectorizer;

private:
const VPMaskGenerationRecipe *MaskRecipe;

public:
  VPNonUniformConditionBitRecipe(const VPMaskGenerationRecipe *MR)
    : VPConditionBitRecipeBase(VPNonUniformBranchSC), MaskRecipe(MR) {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPNonUniformBranchSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void vectorize(VPTransformState &State) override {}

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << "Non-uniform branch condition: ";
    MaskRecipe->print(O);
  }

  StringRef getName() const { return "Non-Uniform Cond Bit Recipe"; };
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


/// A pure virtual class that provides the getScalarCondition() interface
/// function for accessing the scalar condition value.
class VPConditionBitRecipeWithScalar : public VPConditionBitRecipeBase {
protected:
	/// Recipe name.
  std::string Name;

public:
  VPConditionBitRecipeWithScalar(const unsigned char SC)
      : VPConditionBitRecipeBase(SC) { }

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
      return V->getVPRecipeID() == VPRecipeBase::VPUniformBranchSC
          || V->getVPRecipeID() == VPRecipeBase::VPLiveInBranchSC;
  }

  /// Return the scalar condition value
  virtual Value *getScalarCondition(void) const = 0;
};

/// A VPUniformConditionBitRecipe is a VPConditionBitRecipe which supports a
/// uniform conditional branch. 
class VPUniformConditionBitRecipe : public VPConditionBitRecipeWithScalar {
public:
  VPUniformConditionBitRecipe(Value *Cond)
    : VPConditionBitRecipeWithScalar(VPUniformBranchSC) {
    ScConditionBit = Cond;
  }

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPUniformBranchSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void vectorize(VPTransformState &State) override;

  /// Return the scalar condition value.
  Value *getScalarCondition(void) const override{
    return ScConditionBit;
  }

  /// Set name.
  void setName(std::string Name) { this->Name = Name; }

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << Name;

    // O << " Output: ";
    // if (ConditionBit) {
    //  O << *ConditionBit;
    //} else {
    //  O << "NULL";
    //}

    // O << " Input: ";
    O << ": ";
    if (ScConditionBit) {
      O << *ScConditionBit;
    } else {
      O << "NULL";
    }
  }

  StringRef getName() const { return Name; };

private:
  Value *ScConditionBit;
};


/// A VPLiveInConditionBitRecipe is a recipe for a Condition operand of 
/// a uniform conditional branch. The Condition is defined outside the loop.
class VPLiveInConditionBitRecipe : public VPConditionBitRecipeWithScalar {
public:
  VPLiveInConditionBitRecipe(Value *Cond)
    : VPConditionBitRecipeWithScalar(VPLiveInBranchSC) {
    ConditionBit = Cond;
  }

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPLiveInBranchSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void vectorize(VPTransformState &State) override {}

  /// Return the scalar condition value.
  /// NOTE: Since it is a live-in CBR, the scalar ConditionBit is re-used.
  Value *getScalarCondition(void) const override {
    return ConditionBit;
  }

  /// Set name.
  void setName(std::string Name) { this->Name = Name; }

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << Name;

    //O << " Output: ";
    O << ": ";
    if (ConditionBit) {
      O << *ConditionBit;
    } else {
      O << "NULL";
    }
  }

  StringRef getName() const { return Name; };
};

/// A VPConstantRecipe is a recipe which represents a constant in VPlan.
/// This recipe represents a scalar integer w/o any relation to the source IR.
/// The usage of this recipe is mainly beneficial when we need to argue about
/// new recipes altering the original structure of the code and introducing new
/// commands. e.g. consider the single-exit loop massaging, we need to
/// represent a new \phi with respect to new constant values and compares to 
/// those same values.
class VPConstantRecipe : public VPRecipeBase {
public:
  VPConstantRecipe(int val)
    : VPRecipeBase(VPConstantSC), val(val) {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPConstantSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void vectorize(VPTransformState &State) override {}

  Value *getValue(void) const {
    // TODO after vectorize.
    return nullptr;
  }

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << "Const " << val;
  }

  StringRef getName() const { return "Constant: " + val; };

private:
  int val;
};

/// A VPConstantRecipe is a recipe which represents a new Phi in VPlan to 
/// facilitate the alteration of VPlan from its original source coded form.
/// Currently the elements of the phi are constants in-order to generate the
/// needed \phi for the single-exit loop massaging. However, this phi can be
/// further enhanced to handle any type of value.
class VPPhiValueRecipe : public VPRecipeBase {
public:
  VPPhiValueRecipe()
    : VPRecipeBase(VPPhiValueSC), Phi(nullptr), Incoming() {}

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPLiveInBranchSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void vectorize(VPTransformState &State) override {}

  /// Return the phi value after vectorization.
  Value *getValue(void) const {
    return Phi;
  }

  /// Adds a new element to the resulting \phi.
  void addIncomingValue(VPConstantRecipe IncomingValue,
    VPBlockBase* IncomingBlock) {
    Incoming.push_back(IncomingPair(IncomingValue, IncomingBlock));
  }

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << "Phi ";
    for (auto item : Incoming) {
      O << "[";
      item.first.print(O);
      O << ", " << item.second->getName() << "] ";
    }
  }

  StringRef getName() const { return "Phi Recipe"; };

  ~VPPhiValueRecipe() {
    delete Phi;
  }

private:
  typedef std::pair<VPConstantRecipe , VPBlockBase *> IncomingPair;
  SmallVector<IncomingPair, 4> Incoming;
  Value* Phi;
};


/// A VPCmpBitRecipe is a compare recipe which represents a compare against
/// and exact value, in our case a constant value in order to support the 
/// compares needed for the cascaded ifs in the single-exit loop massaging. 
class VPCmpBitRecipe : public VPConditionBitRecipeWithScalar {
public:
  VPCmpBitRecipe(VPPhiValueRecipe* Phi, VPConstantRecipe ConstantValue)
    : VPConditionBitRecipeWithScalar(VPCmpBitSC), Phi(Phi),
    ConstantValue(ConstantValue) {
    ConditionBit = nullptr;
  }

  /// Method to support type inquiry through isa, cast, and dyn_cast.
  static inline bool classof(const VPRecipeBase *V) {
    return V->getVPRecipeID() == VPRecipeBase::VPCmpBitSC;
  }

  /// The method clones a uniform instruction that calculates condition
  /// for uniform branch.
  void vectorize(VPTransformState &State) override {
    ConditionBit = nullptr;
  }

  /// Return the scalar condition value.
  /// NOTE: Since it is a live-in CBR, the scalar ConditionBit is re-used.
  Value *getScalarCondition(void) const override {
    return ConditionBit;
  }

  /// Print the recipe.
  void print(raw_ostream &O) const override {
    O << Name << "Cmp-Bit Exact Condition: ";
    Phi->print(O);
    O << " == ";
    ConstantValue.print(O);
  }

  StringRef getName() const { return Name; };

private:
  VPPhiValueRecipe* Phi;
  VPConstantRecipe ConstantValue;
};

/// ; IntelVPlanUtils class provides interfaces for the construction and
/// manipulation of a VPlan.
class IntelVPlanUtils : public VPlanUtils {

public:
  IntelVPlanUtils(IntelVPlan *Plan) : VPlanUtils(Plan) {}

  /// Creates a new VPScalarizeOneByOneRecipe or VPVectorizeOneByOneRecipe based
  /// on the isScalarizing parameter respectively.
  // TODO: VPlan is passed in the original interface. Why is this necessary if
  // VPlanUtils already has a copy?
  vpo::VPOneByOneIRRecipeBase *createOneByOneRecipe(
    const BasicBlock::iterator B,
    const BasicBlock::iterator E,
    // VPlan *Plan,
    bool isScalarizing);

  // TODO: This should eventually be removed and replaced with
  // createUniqueName(). The problem is that some lit tests rely on specific BB
  // names/ids that will change if createUniqueName() is used for predicate
  // recipes.
  /// Return a unique name Prefix+UniqueId
  std::string getUniqueName(const char *Prefix) {
    static int NextUid = 0;
    std::string S;
    raw_string_ostream RSO(S);
    RSO << Prefix << NextUid++;
    return RSO.str();
  }
    
  /// Creates a new recipe that represents an all zeros bypass.
  vpo::VPBranchIfNotAllZeroRecipe *createBranchIfNotAllZeroRecipe(
    Instruction *Cond);

  /// Creates a new recipe that represents generation of an i1 vector to be used
  /// as a mask.
  vpo::VPMaskGenerationRecipe *createMaskGenerationRecipe(
    const Value *Pred, const Value *Backedge);

  /// Creates a new recipe that points to an i1 vector representing a
  /// non-uniform condition.
  vpo::VPNonUniformConditionBitRecipe *createNonUniformConditionBitRecipe(
    const vpo::VPMaskGenerationRecipe *MaskRecipe);

  /// Creates a new VPUniformConditionBitRecipe.
  VPUniformConditionBitRecipe *createUniformConditionBitRecipe(Value *Cond) {
    VPUniformConditionBitRecipe *newRecipe =
        new VPUniformConditionBitRecipe(Cond);
    newRecipe->setName(getUniqueName("UniformCBR"));
    return newRecipe;
  }

  /// Creates a new VPLiveInConditionBitRecipe.
  VPLiveInConditionBitRecipe *createLiveInConditionBitRecipe(Value *Cond) {
    VPLiveInConditionBitRecipe *newRecipe =
        new VPLiveInConditionBitRecipe(Cond);
    newRecipe->setName(getUniqueName("LiveInCBR"));
    return newRecipe;
  }

  /// Create a new VPVectorizeBooleanRecipe.
  VPVectorizeBooleanRecipe *createVectorizeBooleanRecipe(Value *Cond) {
    VPVectorizeBooleanRecipe *newRecipe =
        new VPVectorizeBooleanRecipe(VPRecipeBase::VPVectorizeBooleanSC, Cond);
    newRecipe->setName(getUniqueName("VBR"));
    return newRecipe;
  }

  /// Create a new VPIfTruePredicateRecipe.
  VPIfTruePredicateRecipe *
  createIfTruePredicateRecipe(VPVectorizeBooleanRecipe *VBR,
                              VPPredicateRecipeBase *PredecessorPredicate) {
    VPIfTruePredicateRecipe *newRecipe =
        new VPIfTruePredicateRecipe(VBR, PredecessorPredicate);
    newRecipe->setName(getUniqueName("IfT"));
    return newRecipe;
  }

  /// Create a new VPIfFalsePredicateRecipe.
  VPIfFalsePredicateRecipe *
  createIfFalsePredicateRecipe(VPVectorizeBooleanRecipe *VBR,
                               VPPredicateRecipeBase *PredecessorPredicate) {
    VPIfFalsePredicateRecipe *newRecipe =
        new VPIfFalsePredicateRecipe(VBR, PredecessorPredicate);
    newRecipe->setName(getUniqueName("IfF"));
    return newRecipe;
  }

  /// Create a new VPBlockPredicateRecipe.
  VPBlockPredicateRecipe *createBlockPredicateRecipe(void) {
    VPBlockPredicateRecipe *newRecipe = new VPBlockPredicateRecipe();
    newRecipe->setName(getUniqueName("BP"));
    return newRecipe;
  }

  /// Create a new VPAllOnesPredicateRecipe
  VPAllOnesPredicateRecipe *createAllOnesPredicateRecipe(void) {
    VPAllOnesPredicateRecipe *newRecipe =
        VPAllOnesPredicateRecipe::getPredicateRecipe();
    newRecipe->setName(getUniqueName("AllOnes"));
    return newRecipe;
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

