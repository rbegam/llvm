//===- AddSubReassociate.h - Reassociate add/sub expressions ----*- C++ -*-===//
//
// Copyright (C) 2018 - 2019 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_TRANSFORMS_SCALAR_ADDSUBREASSOCIATE_H
#define LLVM_TRANSFORMS_SCALAR_ADDSUBREASSOCIATE_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/Hashing.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/ADT/SmallSet.h"
#include "llvm/ADT/SmallVector.h"
#include "llvm/Analysis/ScalarEvolution.h"
#include "llvm/Analysis/ScalarEvolutionExpressions.h"
#include "llvm/IR/PassManager.h"
#include <memory>
#include <unordered_set>

namespace llvm {

template <typename T> struct HashIt {
  size_t operator()(const T &Obj) const { return Obj.getHash(); }
};

namespace intel_addsubreassoc {

// Maximum distance between two Values.
constexpr auto MAX_DISTANCE = LONG_MAX;

// This represents the associative instruction that applies to this leaf.
class AssocOpcodeData {
private:
  unsigned Opcode;
  Constant *Const;

public:
  AssocOpcodeData(const Instruction *I);
  AssocOpcodeData(unsigned AddSubOpcode) {
    assert((AddSubOpcode == Instruction::Add ||
            AddSubOpcode == Instruction::Sub) &&
           "Expected Add or Sub.");
    Opcode = AddSubOpcode;
    Const = nullptr;
  }
  unsigned getOpcode() const { return Opcode; }
  Constant *getConst() const { return Const; }
  hash_code getHash() const { return hash_combine(Opcode, Const); }
  bool operator==(const AssocOpcodeData &Data2) const {
    return Opcode == Data2.Opcode && Const == Data2.Const;
  }
  bool operator!=(const AssocOpcodeData &Data2) const {
    return !(*this == Data2);
  }
  // Comparator used for sorting.
  bool operator<(const AssocOpcodeData &Data2) const {
    if (Opcode != Data2.Opcode)
      return Opcode < Data2.Opcode;
    if (Const != Data2.Const)
      return Const < Data2.Const;
    return false;
  }
  // For debugging.
  void dump() const;
};

using AssocDataTy = SmallVector<AssocOpcodeData, 1>;

class OpcodeData {
  friend class AddSubReassociate;

private:
  unsigned Opcode;
  // The Unary associative opcodes that apply to the leaf.
  AssocDataTy AssocOpcodeVec;

public:
  OpcodeData() : Opcode(0) {}
  OpcodeData(unsigned Opcode) : Opcode(Opcode) {}
  // The Add/Sub opcode of the leaf.
  unsigned getOpcode() const { return Opcode; }
  AssocDataTy::const_iterator begin() const { return AssocOpcodeVec.begin(); }
  AssocDataTy::const_iterator end() const { return AssocOpcodeVec.end(); }
  hash_code getHash() const {
    hash_code Hash = hash_combine(Opcode);
    for (const auto &Data : AssocOpcodeVec)
      Hash = hash_combine(Hash, Data.getHash());
    return Hash;
  }
  bool isUndef() const { return Opcode == 0; }
  // Compare only the canonicalized +/- opcode.
  bool hasSameAddSubOpcode(const OpcodeData &OD2) const {
    return Opcode == OD2.Opcode;
  }
  // Compare the whole opcode.
  bool operator==(const OpcodeData &OD2) const {
    return Opcode == OD2.Opcode && AssocOpcodeVec == OD2.AssocOpcodeVec;
  }
  bool operator!=(const OpcodeData &OD2) const { return !(*this == OD2); }
  // Comparator used for sorting.
  bool operator<(const OpcodeData &OD2) const {
    if (Opcode != OD2.Opcode)
      return Opcode < OD2.Opcode;
    if (AssocOpcodeVec.size() != OD2.AssocOpcodeVec.size())
      return AssocOpcodeVec.size() < OD2.AssocOpcodeVec.size();
    for (size_t I = 0, E = AssocOpcodeVec.size(); I != E; ++I)
      if (AssocOpcodeVec[I] != OD2.AssocOpcodeVec[I])
        return AssocOpcodeVec[I] < OD2.AssocOpcodeVec[I];
    return false;
  }
  OpcodeData getFlipped() const;
  void appendAssocInstr(Instruction *I) {
    AssocOpcodeVec.push_back(AssocOpcodeData(I));
  }
  // For debugging.
  void dump() const;
};

struct LeafUserPair {
  friend class Tree;
  friend class AddSubReassociate;

private:
  // The tree leaf.
  Value *Leaf;
  // The trunk node that 'Leaf' is attached to.
  Instruction *User;
  // The canonicalized opcode that corresponds to the 'Leaf'.
  // This includes associative instructions like '<< 4'.
  OpcodeData Opcode;

public:
  LeafUserPair(Value *L, Instruction *U, const OpcodeData &Opcode)
      : Leaf(L), User(U), Opcode(Opcode) {}

  Value *getLeaf() const { return Leaf; }
  Instruction *getUser() const { return User; }
  const OpcodeData &getOpcodeData() const { return Opcode; }

  void appendAssocInstruction(Instruction *I) { Opcode.appendAssocInstr(I); }

  hash_code getHash() const {
    // TODO: Check if this hashing is not leading to conflicts.
    hash_code Hash(0);
    // A leaf is uniquely identified by the Leaf and its User within a tree.
    // However, hashing is used to compare nodes across trees. Therefore we
    // need to use all three: Leaf, User and Opcode.
    Hash = hash_combine(Hash, Leaf, User, Opcode.getHash());
    return Hash;
  }
  // Debug print.
  void dump(const unsigned Padding) const {
    dbgs().indent(Padding) << "Leaf ";
    Opcode.dump();
    dbgs() << *Leaf << "\n";
    dbgs().indent(Padding) << "   User: ";
    if (User)
      dbgs() << *User << "\n";
    else
      dbgs() << "NULL" << "\n";
  }
  // The Leaf and User uniquely identify the LeafUserPair.
  bool operator==(const LeafUserPair &Pair2) const {
    return Leaf == Pair2.Leaf && User == Pair2.User;
  }
};

using LUSetTy = DenseSet<LeafUserPair>;
using LUPairVecTy = SmallVector<LeafUserPair, 16>;

// The expression tree.
class Tree {
private:
  const DataLayout &DL;
  // Unique tree identifier. Used only for debugging.
  int Id = 0;
  // The root instruction of the tree.
  Instruction *Root = nullptr;
  // A vector of all the leaves and their corresponding users.
  // NOTE: Multiple identical leaves are allowed.
  // Main data structure for leaves and their users.
  LUPairVecTy LUVec;
  // Set to true if this tree contains shared leaves candidates.
  // This is used to avoid searching through the leaves of a tree.
  bool HasSharedLeafCandidate = false;
  // Number of shared leaves became part of a trunk. In other words,
  // that many leaves have been unshared during tree constructions.
  int SharedLeavesCount = 0;
  // True if at least one leaf has associated associative instruction.
  bool hasAssocInstr = false;

public:
  Tree(const DataLayout &DL) : DL(DL) {
    static int IdCnt;
    Id = IdCnt++;
  }
  int getId() const { return Id; }
  // Update the root of the tree.
  void setRoot(Instruction *R);
  // Return the root of the tree.
  Instruction *getRoot() const { return Root; }
  unsigned getLeavesCount() const { return LUVec.size(); }
  // Append Leaf along with its user U (part of the main tree trunk).
  // For example: ... Leaf
  //                |/
  //                U
  //                |
  void appendLeaf(Instruction *User, Value *Leaf, const OpcodeData &Opcode);
  void removeLeaf(unsigned);
  // Returns the leaf & user pair at 'idx'.
  const LeafUserPair &getLeafUserPair(int Idx) const { return LUVec[Idx]; }
  // Returns the vector of leaves (bottom-up).
  const LUPairVecTy &getLeavesAndUsers() const { return LUVec; }
  // Return true if we can match 'Leaf' with 'Opcode'. If found, also mark it
  // in VisitedLUs.
  bool matchLeaf(Value *Leaf, const OpcodeData &Opcode,
                 LUSetTy &VisitedLUs) const;
  // Returns the opcode of one of the trunk instructions that the leaf that
  // matches 'Leaf' will be attached to in the canonicalized (linearized)
  // form. If 'OpcodeToMatch' is provided, try to match it.
  OpcodeData
  getLeafCanonOpcode(Value *Leaf, LUSetTy &VisitedLUs,
                     const OpcodeData &OpcodeToMatch = OpcodeData()) const;
  // Return the user (trunk) instruction of 'Leaf'.
  // Since a tree can contain more than one identical leaves, we use
  // 'VisitedLUs' to mark the ones already visited.
  LeafUserPair &getNextLeafUserPair(Value *Leaf, LUSetTy &VisitedLUs) const;
  // 'U' is set as the user of 'Leaf'.
  bool replaceLeafUser(Value *Leaf, Instruction *OldU, Instruction *NewU);
  // Returns true if 'Leaf' is a leaf of this tree.
  bool hasLeaf(Value *Leaf) const;
  // Returns true if specified instruction logically is part of the tree
  // trunk. Please note that leafs are not part of the trunk.
  bool hasTrunkInstruction(const Instruction *I) const;
  // Returns number of shared leaves that are part of the trunk.
  int getSharedLeavesCount() const { return SharedLeavesCount; }
  // Increases/decreases number of shared leaves.
  void adjustSharedLeavesCount(int Count) { SharedLeavesCount += Count; }
  // Return true if this tree contains shared leaf candidate nodes.
  bool hasSharedLeafCandidate() const { return HasSharedLeafCandidate; }
  // Set the shared leaf candidate flag.
  void setSharedLeafCandidate(bool Flag) { HasSharedLeafCandidate = Flag; }
  // Returns true if this tree is larger than 'T2'.
  bool operator<(const Tree &T2) const {
    return getLeavesCount() > T2.getLeavesCount();
  }
  // Restores original tree state as of a construction time. After the call
  // the tree is in valid state and empty.
  void clear();

  // TODO:
  void addAssocInstruction(LeafUserPair &LU, Instruction *AI) {
    LU.appendAssocInstruction(AI);
    hasAssocInstr = true;
  }
  // bool hasAssocInstruction() const { return hasAssocInstr; }
  void emitAssocInstrutions();

  // Debug print.
  void dump() const;
};

// Represents a group of values that should be enclosed in parentheses.
class Group {
public:
  using ValOpTy = std::pair<Value *, OpcodeData>;
  using ValVecTy = SmallVector<ValOpTy, 2>;
  // Pairs of Leaves and user opcodes in bottom-up order.
  ValVecTy Values;

public:
  Group() {}
  Group(Value *V, const OpcodeData &UserOpc) { Values.push_back({V, UserOpc}); }
  bool empty() const { return Values.empty(); }
  // Return the vector of Leaves and user opcodes in bottom-up order.
  const ValVecTy &getValues() const { return Values; }
  void setValues(const ValVecTy &&ValuesNew) {
    assert(Values.size() == ValuesNew.size() && "Expected same size.");
    Values = std::move(ValuesNew);
  }
  void appendLeaf(Value *Leaf, const OpcodeData &Opcode) {
    Values.push_back({Leaf, Opcode});
  }
  void pop_back() { Values.pop_back(); }
  size_t size() const { return Values.size(); }
  // Returns a pair of unique and total number of associative instructions in
  // the group.
  std::pair<unsigned, unsigned> geAssocInstrCnt() const {
    std::unordered_set<AssocOpcodeData, HashIt<AssocOpcodeData>> Set;

    unsigned Total = 0;
    for (auto &Pair : Values) {
      for (const auto &AOD : Pair.second) {
        ++Total;
        Set.insert(AOD);
      }
    }
    return {Set.size(), Total};
  }
  ValOpTy operator[](int idx) { return Values[idx]; }
  OpcodeData getOpcodeFor(Value *V) const {
    for (auto &Pair : Values) {
      if (Pair.first == V) {
        return Pair.second;
      }
    }
    llvm_unreachable("V not found in group");
  }
  bool containsValue(Value *V) const;
  // Returns true if the opcodes/reverse-opcodes and instruction types match.
  bool isSimilar(const Group &G2);
  // Canonicalize the values in the group by sorting them.
  void sort();
  // Return the opcode of the Idx'th trunk instruction.
  unsigned getTrunkOpcode(int Idx) const {
    return Values[Idx].second.getOpcode();
  }
  // Change the trunk opcodes from Add to Sub and vice versa.
  void flipOpcodes();
  // Debug dump.
  void dumpDepth(int Depth = 1) const;
  void dump() const;
};
} // end namespace intel_addsubreassoc

using namespace intel_addsubreassoc;

template <> struct DenseMapInfo<LeafUserPair> {
  static inline LeafUserPair getEmptyKey() {
    return LeafUserPair(nullptr, nullptr, OpcodeData());
  }
  static inline LeafUserPair getTombstoneKey() {
    return LeafUserPair(reinterpret_cast<Value *>(-1),
                        reinterpret_cast<Instruction *>(-1), OpcodeData());
  }
  static unsigned getHashValue(const LeafUserPair &Val) {
    return Val.getHash();
  }
  static bool isEqual(const LeafUserPair &LHS, const LeafUserPair &RHS) {
    return LHS == RHS;
  }
};

//} // end namespace intel_addsubreassoc

namespace intel_addsubreassoc {

//
// This pass reassociates add-sub chains to improve expression reuse
//
// For example: X = A - B - C  -->  X = A - (B + C)
//              Y = A + B + C  -->  Y = A + (B + C)
//
class AddSubReassociate {
  // Typedefs.
  using ValSetTy = SmallPtrSet<Value *, 16>;
  using IVMapTy = DenseMap<Instruction *, Value *>;

  using TreePtr = std::unique_ptr<Tree>;
  using TreeVecTy = SmallVector<TreePtr, 16>;
  using TreeArrayTy = MutableArrayRef<TreePtr>;
  using WorkListTy = SmallVectorImpl<LeafUserPair>;
  using GroupTreeSignTy = std::pair<Tree *, bool>;
  using GroupTreesTy = SmallVector<GroupTreeSignTy, 16>;
  using GroupsTy = SmallVector<std::pair<Group, GroupTreesTy>, 4>;

public:
  AddSubReassociate(const DataLayout &DL, ScalarEvolution *SE, Function *F)
      : DL(DL), SE(SE), F(F){};

  // Main entry point to the optimization.
  bool run();

private:
  // Scans through \p AllTrees and returns the first one which containing \p I.
  static Tree *findEnclosingTree(TreeVecTy &AllTrees, const Instruction *I);
  // Scans through \p AllTrees and returns the first one which has root \p I.
  static Tree *findTreeWithRoot(TreeVecTy &AllTrees, const Instruction *I,
                                const Tree *skipTree);

  // Checks that instructions between root and leaves are in
  // canonical form, otherwise asserts with an error.
  void checkCanonicalized(Tree &T) const;

  // Returns true if we were able to compute distance of V1 and V2 or one of
  // their operands, false otherwise.
  bool getValDistance(Value *V1, Value *V2, int MaxDepth, int64_t &Distance);
  // Returns the sum of the absolute distances of SortedLeaves and G2.
  int64_t getSumAbsDistances(Group::ValVecTy &SortedLeaves, const Group &G2);
  // Recursively calls itself to explore the different orderings of G1's leaves
  // in order to match them best against G2.
  int64_t getBestSortedScoreRec(const Group &G1, const Group &G2,
                                Group::ValVecTy G1Leaves,
                                Group::ValVecTy G2Leaves,
                                Group::ValVecTy &SortedG1Leaves,
                                Group::ValVecTy &BestSortedG1Leaves,
                                int64_t &BestScore);
  // Returns false if we did not manage to get a good ordering that matches G2.
  bool getBestSortedLeaves(const Group &G1, const Group &G2,
                           Group::ValVecTy &BestSortedG1Leaves);
  // Canonicalize: (i) the order of the values in G1, (ii) the trunk opcodes, to
  // match the ones in G2.
  bool memCanonicalizeGroupBasedOn(Group &G1, const Group &G2,
                                   ScalarEvolution *SE);
  // Canonicalize 'G' based on 'BestGroups' memory accesses and opcodes.
  bool memCanonicalizeGroup(Group &G, GroupTreesTy &GroupTreeVec,
                            GroupsTy &BestGroups);
  // Form groups of nodes that reduce divergence across trees in TreeCluster.
  void buildMaxReuseGroups(const TreeArrayTy &TreeCluster,
                           GroupsTy &AllBestGroups);
  // Remove the old dead trunk instructions.
  void removeDeadTrunkInstrs(Tree *T, Instruction *OldRootI) const;
  // Massage the code in T to be a flat single-branch +/- expression tree.
  bool canonicalizeIRForTree(Tree &T) const;
  // Linearize the code that corresponds to the trees in \p AffectedTrees.
  bool canonicalizeIRForTrees(const ArrayRef<Tree *> AffectedTrees) const;
  // Applies 'G' to all trees in \p TreeAndSignand emits the code.
  void generateCode(Group &G, GroupTreeSignTy &TreeAndSign,
                    Instruction *Chain) const;
  // Simplifies the top instructions of the tree by removing the '0'.
  Instruction *simplifyTree(Instruction *Bridge, bool OptTrunk) const;
  // Emit Assoc Instructions for T.
  void emitAssocInstrs(Tree *T) const;
  // Calls generateCod(G, T) for all groups and all trees.
  void generateCode(GroupsTy &Groups,
                    const ArrayRef<Tree *> AffectedTrees) const;
  // Returns true if T1 and T2 contain similar values.
  bool treesMatch(const Tree *T1, const Tree *T2) const;
  // Create clusters of the trees in AllTrees.
  void clusterTrees(TreeVecTy &AllTrees,
                    SmallVectorImpl<TreeArrayTy> &TreeClusters);
  // Grow the tree upwards, towards the definitions.
  bool growTree(TreeVecTy &AllTrees, Tree *T, WorkListTy &&WorkList);
  // Returns true if all uses of a \p Leaf are from one of a tree in
  // \pTreeCluster, false otherwise. Additionally for each such use a Tree * and
  // Leaf index pair is put to \p WorkList.
  bool areAllUsesInsideTreeCluster(
      TreeArrayTy &TreeCluster, const Value *Leaf,
      SmallVectorImpl<std::pair<Tree *, unsigned>> &WorkList) const;
  // Returns true if we were able to find a leaf with multiple uses from trees
  // in \p TreeCluster only, false otherwise. Each found use is pushed to a \p
  // WorkList as a Tree* and Leaf index pair.
  bool getSharedLeave(TreeArrayTy &TreeCluster,
                      SmallVectorImpl<std::pair<Tree *, unsigned>> &WorkList);
  // Enlarge trees in \p TreeCluster by growing them towards shared leaves.
  void extendTrees(TreeVecTy &AllTrees, TreeArrayTy &TreeCluster);
  // Build Add/Sub trees from code in BB.
  void buildInitialTrees(BasicBlock *BB, TreeVecTy &AllTrees);
  // Build all trees within BB.
  void buildTrees(BasicBlock *BB, TreeVecTy &AllTrees,
                  SmallVector<TreeArrayTy, 8> &Clusters, bool UnshareLeaves);

  // Debug print functions.
  void dumpTreeVec(const TreeVecTy &TreeVec) const;
  void dumpTreeArray(const TreeArrayTy &TreeVec) const;
  void dumpTreeArrayVec(SmallVectorImpl<TreeArrayTy> &Clusters) const;
  void dumpGroups(const GroupsTy &Groups) const;

private:
  const DataLayout &DL;
  ScalarEvolution *SE;
  Function *F;
};
} // end namespace intel_addsubreassoc

class AddSubReassociatePass : public PassInfoMixin<AddSubReassociatePass> {
public:
  // Entry point for AddSub reassociation.
  bool runImpl(Function *F, ScalarEvolution *SE);
  PreservedAnalyses run(Function &F, FunctionAnalysisManager &AM);
};

class AddSubReassociateLegacyPass : public FunctionPass {
  AddSubReassociatePass Impl;

public:
  static char ID; // Pass identification, replacement for typeid

  AddSubReassociateLegacyPass() : FunctionPass(ID) {
    initializeAddSubReassociateLegacyPassPass(*PassRegistry::getPassRegistry());
  }

  bool runOnFunction(Function &F) override;
  void getAnalysisUsage(AnalysisUsage &AU) const override;
};

} // end namespace llvm

#endif // LLVM_TRANSFORMS_SCALAR_ADDSUBREASSOCIATE_H
