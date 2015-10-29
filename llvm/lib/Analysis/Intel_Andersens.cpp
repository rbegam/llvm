//===- Intel_Andersens.cpp - Andersen's Interprocedural Alias Analysis ---===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===---------------------------------------------------------------------===//
//
// This file defines an implementation of Andersen's interprocedural alias
// analysis
//
// In pointer analysis terms, this is a subset-based, flow-insensitive,
// field-sensitive, and context-insensitive algorithm pointer algorithm.
//
// This algorithm is implemented as three stages:
//   1. Object identification.
//   2. Inclusion constraint identification.
//   3. Offline constraint graph optimization
//   4. Inclusion constraint solving.
//
// The object identification stage identifies all of the memory objects in the
// program, which includes globals, heap allocated objects, and stack allocated
// objects.
//
// The inclusion constraint identification stage finds all inclusion constraints
// in the program by scanning the program, looking for pointer assignments and
// other statements that effect the points-to graph.  For a statement like "A =
// B", this statement is processed to indicate that A can point to anything that
// B can point to.  Constraints can handle copies, loads, and stores, and
// address taking.
//
// The offline constraint graph optimization portion includes offline variable
// substitution algorithms intended to compute pointer and location
// equivalences.  Pointer equivalences are those pointers that will have the
// same points-to sets, and location equivalences are those variables that
// always appear together in points-to sets.  It also includes an offline
// cycle detection algorithm that allows cycles to be collapsed sooner 
// during solving.
//
// The inclusion constraint solving phase iteratively propagates the inclusion
// constraints until a fixed point is reached.  This is an O(N^3) algorithm.
//
// Function constraints are handled as if they were structs with X fields.
// Thus, an access to argument X of function Y is an access to node index
// getNode(Y) + X.  This representation allows handling of indirect calls
// without any issues.  To wit, an indirect call Y(a,b) is equivalent to
// *(Y + 1) = a, *(Y + 2) = b.
// The return node for a function is always located at getNode(F) +
// CallReturnPos. The arguments start at getNode(F) + CallArgPos.
//
// Future Improvements:
//   Use of BDD's.
//===----------------------------------------------------------------------===//

#include "llvm/ADT/SCCIterator.h"
#include "llvm/ADT/Statistic.h"
#include "llvm/Analysis/Intel_Andersens.h"
#include "llvm/Analysis/Passes.h"
#include "llvm/Analysis/TargetLibraryInfo.h"
#include "llvm/Analysis/ValueTracking.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/Support/Atomic.h"
#include "llvm/Support/CommandLine.h"
#include "llvm/Support/Compiler.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/ErrorHandling.h"

#include <algorithm>



#define DEBUG_TYPE "anders-aa"

// Determining the actual set of nodes the universal set can consist of is very
// expensive because it means propagating around very large sets.  We rely on
// other analysis being able to determine which nodes can never be pointed to in
// order to disambiguate further than "points-to anything".
#define FULL_UNIVERSAL 0

using namespace llvm;
STATISTIC(NumConstraints, "Number of constraints");
STATISTIC(NumNodes      , "Number of nodes");
STATISTIC(NumUnified    , "Number of variables unified");
STATISTIC(NumErased     , "Number of redundant constraints erased");
STATISTIC(NumAliasQuery     , "Number of Alias query");
STATISTIC(NumPtrQuery     , "Number of Ptr query");

static cl::opt<bool> PrintAndersConstraints("print-anders-constraints", cl::ReallyHidden);
static cl::opt<bool> PrintAndersPointsTo("print-anders-points-to", cl::ReallyHidden);
static cl::opt<bool> PrintAndersAliasQueries("print-anders-alias-queries", cl::ReallyHidden);
static cl::opt<bool> PrintAndersModRefQueries("print-anders-modref-queries", cl::ReallyHidden);
static cl::opt<bool> PrintAndersConstMemQueries("print-anders-const-mem-queries", cl::ReallyHidden);
static cl::opt<bool> UseIntelModRef("use-intel-mod-ref", cl::init(true), cl::ReallyHidden);

static const unsigned SelfRep = (unsigned)-1;
static const unsigned Unvisited = (unsigned)-1;
// Position of the function return node relative to the function node.
static const unsigned CallReturnPos = 1;
// Position of the function call node relative to the function node.
static const unsigned CallFirstArgPos = 2;

struct AndersensAAResult::BitmapKeyInfo {
  static inline SparseBitVector<> *getEmptyKey() {
    return reinterpret_cast<SparseBitVector<> *>(-1);
  }
  static inline SparseBitVector<> *getTombstoneKey() {
    return reinterpret_cast<SparseBitVector<> *>(-2);
  }
  static unsigned getHashValue(const SparseBitVector<> *bitmap) {
    uint64_t HashVal = 0;

    for (SparseBitVector<>::iterator bi = bitmap->begin(), E = bitmap->end();
         bi != E; ++bi) {
      HashVal ^= *bi;
    }
    return HashVal;
  }
  static bool isEqual(const SparseBitVector<> *LHS,
                      const SparseBitVector<> *RHS) {
    if (LHS == RHS)
      return true;
    else if (LHS == getEmptyKey() || RHS == getEmptyKey()
             || LHS == getTombstoneKey() || RHS == getTombstoneKey())
      return false;

    return *LHS == *RHS;
  }

  static bool isPod() { return true; }
};

// struct Node;

// Constraint - Objects of this structure are used to represent the various
// constraints identified by the algorithm.  The constraints are 'copy',
// for statements like "A = B", 'load' for statements like "A = *B",
// 'store' for statements like "*A = B", and AddressOf for statements like
// A = alloca;  The Offset is applied as *(A + K) = B for stores,
// A = *(B + K) for loads, and A = B + K for copies.  It is
// illegal on addressof constraints (because it is statically
// resolvable to A = &C where C = B + K)

struct AndersensAAResult::Constraint {
  enum ConstraintType { Copy, Load, Store, AddressOf } Type;
  unsigned Dest;
  unsigned Src;
  unsigned Offset;

  Constraint(ConstraintType Ty, unsigned D, unsigned S, unsigned O = 0)
    : Type(Ty), Dest(D), Src(S), Offset(O) {
    assert((Offset == 0 || Ty != AddressOf) &&
           "Offset is illegal on addressof constraints");
  }

  bool operator==(const Constraint &RHS) const {
    return RHS.Type == Type
      && RHS.Dest == Dest
      && RHS.Src == Src
      && RHS.Offset == Offset;
  }

  bool operator!=(const Constraint &RHS) const {
    return !(*this == RHS);
  }

  bool operator<(const Constraint &RHS) const {
    if (RHS.Type != Type)
      return RHS.Type < Type;
    else if (RHS.Dest != Dest)
      return RHS.Dest < Dest;
    else if (RHS.Src != Src)
      return RHS.Src < Src;
    return RHS.Offset < Offset;
  }
};

// Information DenseSet requires implemented in order to be able to do
// it's thing
struct AndersensAAResult::PairKeyInfo {
  static inline std::pair<unsigned, unsigned> getEmptyKey() {
    return std::make_pair(~0U, ~0U);
  }
  static inline std::pair<unsigned, unsigned> getTombstoneKey() {
    return std::make_pair(~0U - 1, ~0U - 1);
  }
  static unsigned getHashValue(const std::pair<unsigned, unsigned> &P) {
    return P.first ^ P.second;
  }
  static unsigned isEqual(const std::pair<unsigned, unsigned> &LHS,
                          const std::pair<unsigned, unsigned> &RHS) {
    return LHS == RHS;
  }
};
    
struct AndersensAAResult::ConstraintKeyInfo {
  static inline Constraint getEmptyKey() {
    return Constraint(Constraint::Copy, ~0U, ~0U, ~0U);
  }
  static inline Constraint getTombstoneKey() {
    return Constraint(Constraint::Copy, ~0U - 1, ~0U - 1, ~0U - 1);
  }
  static unsigned getHashValue(const Constraint &C) {
    return C.Src ^ C.Dest ^ C.Type ^ C.Offset;
  }
  static bool isEqual(const Constraint &LHS,
                      const Constraint &RHS) {
    return LHS.Type == RHS.Type && LHS.Dest == RHS.Dest
      && LHS.Src == RHS.Src && LHS.Offset == RHS.Offset;
  }
};

// Node class - This class is used to represent a node in the constraint
// graph.  Due to various optimizations, it is not always the case that
// there is a mapping from a Node to a Value.  In particular, we add
// artificial Node's that represent the set of pointed-to variables shared
// for each location equivalent Node.
struct AndersensAAResult::Node {
private:
  static volatile sys::cas_flag Counter;

public:
  Value *Val;
  SparseBitVector<> *Edges;
  SparseBitVector<> *PointsTo;
  SparseBitVector<> *OldPointsTo;
  std::list<Constraint> Constraints;

  // Pointer and location equivalence labels
  unsigned PointerEquivLabel;
  unsigned LocationEquivLabel;
  // Predecessor edges, both real and implicit
  SparseBitVector<> *PredEdges;
  SparseBitVector<> *ImplicitPredEdges;
  // Set of nodes that point to us, only use for location equivalence.
  SparseBitVector<> *PointedToBy;
  // Number of incoming edges, used during variable substitution to early
  // free the points-to sets
  unsigned NumInEdges;
  // True if our points-to set is in the Set2PEClass map
  bool StoredInHash;
  // True if our node has no indirect constraints (complex or otherwise)
  bool Direct;
  // True if the node is address taken, *or* it is part of a group of nodes
  // that must be kept together.  This is set to true for functions and
  // their arg nodes, which must be kept at the same position relative to
  // their base function node.
  bool AddressTaken;

  // Nodes in cycles (or in equivalence classes) are united together using a
  // standard union-find representation with path compression.  NodeRep
  // gives the index into GraphNodes for the representative Node.
  unsigned NodeRep;

  // Modification timestamp.  Assigned from Counter.
  // Used for work list prioritization.
  unsigned Timestamp;

  explicit Node(bool direct = true) :
    Val(0), Edges(0), PointsTo(0), OldPointsTo(0), 
    PointerEquivLabel(0), LocationEquivLabel(0), PredEdges(0),
    ImplicitPredEdges(0), PointedToBy(0), NumInEdges(0),
    StoredInHash(false), Direct(direct), AddressTaken(false),
    NodeRep(SelfRep), Timestamp(0) { }

  Node *setValue(Value *V) {
    assert(Val == 0 && "Value already set for this node!");
    Val = V;
    return this;
  }

  /// getValue - Return the LLVM value corresponding to this node.
  ///
  Value *getValue() const { return Val; }

  /// addPointerTo - Add a pointer to the list of pointees of this node,
  /// returning true if this caused a new pointer to be added, or false if
  /// we already knew about the points-to relation.
  bool addPointerTo(unsigned Node) {
    return PointsTo->test_and_set(Node);
  }

  /// intersects - Return true if the points-to set of this node intersects
  /// with the points-to set of the specified node.
  bool intersects(Node *N) const;

  /// intersectsIgnoring - Return true if the points-to set of this node
  /// intersects with the points-to set of the specified node on any nodes
  /// except for the specified node to ignore.
  bool intersectsIgnoring(Node *N, unsigned) const;

  // Timestamp a node (used for work list prioritization)
  void Stamp() {
    Timestamp = sys::AtomicIncrement(&Counter);
    --Timestamp;
  }

  bool isRep() const {
    return( (int) NodeRep < 0 );
  }
};

AndersensAAResult::WorkListElement::WorkListElement(Node* n, unsigned t) : 
                                            node(n), Timestamp(t) {}

  // Note that we reverse the sense of the comparison because we
  // actually want to give low timestamps the priority over high,
  // whereas priority is typically interpreted as a greater value is
  // given high priority.
bool AndersensAAResult::WorkListElement::operator<(const WorkListElement& that) const {
  return( this->Timestamp > that.Timestamp );
}

// Priority-queue based work list specialized for Nodes.
void AndersensAAResult::WorkList::insert(Node* n) {
  Q.push( WorkListElement(n, n->Timestamp) );
}

// We automatically discard non-representative nodes and nodes
// that were in the work list twice (we keep a copy of the
// timestamp in the work list so we can detect this situation by
// comparing against the node's current timestamp).
AndersensAAResult::Node* AndersensAAResult::WorkList::pop() {
  while( !Q.empty() ) {
    WorkListElement x = Q.top(); Q.pop();
    Node* INode = x.node;

    if( INode->isRep() &&
        INode->Timestamp == x.Timestamp ) {
      return(x.node);
    }
  }
  return(0);
}

bool AndersensAAResult::WorkList::empty() {
  return Q.empty();
}

void AndersensAAResult::RunAndersensAnalysis(Module &M)  {
  IndirectCallList.clear();
  IdentifyObjects(M);
  CollectConstraints(M);
  if (PrintAndersConstraints) {
      errs() << " Constraints Dump " << "\n";
      PrintConstraints();
  }

#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa-constraints"
  DEBUG(PrintConstraints());
#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa"
  SolveConstraints();
  DEBUG(PrintPointsToGraph());
  if (PrintAndersPointsTo) {
      errs() << " Points-to Graph Dump" << "\n";
      PrintPointsToGraph();
  }
  
  // Register Callback Handles here
  for (DenseMap<Value*, unsigned>::iterator Iter = ValueNodes.begin(),
       EV = ValueNodes.end(); Iter != EV; ++Iter) {
    AndersensHandles.insert(
          AndersensDeletionCallbackHandle(*this, (Iter->first)));
  }


  // Free the constraints list, as we don't need it to respond to alias
  // requests.
  std::vector<Constraint>().swap(Constraints);
  //These are needed for Print() (-analyze in opt)
  //ObjectNodes.clear();
  //ReturnNodes.clear();
  //VarargNodes.clear();

  if (UseIntelModRef) {
      IMR.reset(new IntelModRef(this));
      IMR->runAnalysis(M);
  }

  //return false;
}


AndersensAAResult::AndersensAAResult(const DataLayout &DL,
                                 const TargetLibraryInfo &TLI)
    : AAResultBase(TLI), DL(DL) {}

// Partial data of AndersensAAResult is copied here. Once Andersens
// points-to analysis is done, only GraphNodes, ValueNodes, ObjectNodes,
// ReturnNodes and VarargNodes are used by interface routines like 
// alias, pointsToConstantMemory etc  and PrintNode that is called from
// interface routines. IndirectCallList is copied for future 
// use to implement indirect-call conversion. 
//
AndersensAAResult::AndersensAAResult(AndersensAAResult &&Arg)
    : AAResultBase(std::move(Arg)), DL(Arg.DL),
      IndirectCallList(std::move(Arg.IndirectCallList)),
      GraphNodes(std::move(Arg.GraphNodes)),
      ValueNodes(std::move(Arg.ValueNodes)),
      ObjectNodes(std::move(Arg.ObjectNodes)),
      ReturnNodes(std::move(Arg.ReturnNodes)),
      VarargNodes(std::move(Arg.VarargNodes)),
      IMR(std::move(Arg.IMR)) {}


/*static*/ AndersensAAResult
AndersensAAResult::analyzeModule(Module &M, const TargetLibraryInfo &TLI,
                               CallGraph &CG) {
  AndersensAAResult Result(M.getDataLayout(), TLI);

  // Run Andersens'ss points-to analysis.
  Result.RunAndersensAnalysis(M);

  return Result;
}


AndersensAAResult AndersensAA::run(Module &M, AnalysisManager<Module> *AM) {
  return AndersensAAResult::analyzeModule(M,
                                      AM->getResult<TargetLibraryAnalysis>(M),
                                      AM->getResult<CallGraphAnalysis>(M));
}


char AndersensAA::PassID;

char AndersensAAWrapperPass::ID = 0;

static cl::opt<unsigned>
MaxAliasQuery("max-alias-query", cl::ReallyHidden, cl::init(40000000));
static cl::opt<unsigned>
MaxPtrQuery("max-ptr-query", cl::ReallyHidden, cl::init(4000));

INITIALIZE_PASS_BEGIN(AndersensAAWrapperPass, "anders-aa",
                   "Andersen Interprocedural AA", false, true)
INITIALIZE_PASS_DEPENDENCY(CallGraphWrapperPass)
INITIALIZE_PASS_DEPENDENCY(TargetLibraryInfoWrapperPass)
INITIALIZE_PASS_END(AndersensAAWrapperPass, "anders-aa",
                    "Andersen Interprocedural AA", false, true)


ModulePass *llvm::createAndersensAAWrapperPass() {
  return new AndersensAAWrapperPass();
}

AndersensAAWrapperPass::AndersensAAWrapperPass() : ModulePass(ID) {
  initializeAndersensAAWrapperPassPass(*PassRegistry::getPassRegistry());
}

bool AndersensAAWrapperPass::runOnModule(Module &M) {
  Result.reset(new AndersensAAResult(AndersensAAResult::analyzeModule(
      M, getAnalysis<TargetLibraryInfoWrapperPass>().getTLI(),
      getAnalysis<CallGraphWrapperPass>().getCallGraph())));
  return false;
}

bool AndersensAAWrapperPass::doFinalization(Module &M) {
  Result.reset();
  return false;
}

void AndersensAAWrapperPass::getAnalysisUsage(AnalysisUsage &AU) const {
  AU.setPreservesAll();
  AU.addRequired<CallGraphWrapperPass>();
  AU.addRequired<TargetLibraryInfoWrapperPass>();
}


/// getNode - Return the node corresponding to the specified pointer scalar.
///
unsigned AndersensAAResult::getNode(Value *V) {
  if (Constant *C = dyn_cast<Constant>(V))
    if (!isa<GlobalValue>(C))
      return getNodeForConstantPointer(C);

  DenseMap<Value*, unsigned>::const_iterator I = ValueNodes.find(V);
  if (I == ValueNodes.end()) {
    return UniversalSet;
  }
  return I->second;
}

/// getObject - Return the node corresponding to the memory object for the
/// specified global or allocation instruction.
unsigned AndersensAAResult::getObject(Value *V) const {
  DenseMap<Value*, unsigned>::const_iterator I = ObjectNodes.find(V);
  assert(I != ObjectNodes.end() &&
         "Value does not have an object in the points-to graph!");
  return I->second;
}

/// getReturnNode - Return the node representing the return value for the
/// specified function.
unsigned AndersensAAResult::getReturnNode(Function *F) const {
  DenseMap<Function*, unsigned>::const_iterator I = ReturnNodes.find(F);
  assert(I != ReturnNodes.end() && "Function does not return a value!");
  return I->second;
}

/// getVarargNode - Return the node representing the variable arguments
/// formal for the specified function.
unsigned AndersensAAResult::getVarargNode(Function *F) const {
  DenseMap<Function*, unsigned>::const_iterator I = VarargNodes.find(F);
  assert(I != VarargNodes.end() && "Function does not take var args!");
  return I->second;
}

/// getNodeValue - Get the node for the specified LLVM value and set the
/// value for it to be the specified value.
unsigned AndersensAAResult::getNodeValue(Value &V) {
  unsigned Index = getNode(&V);
  GraphNodes[Index].setValue(&V);
  return Index;
}


// Initialize Timestamp Counter (static).
volatile llvm::sys::cas_flag AndersensAAResult::Node::Counter = 0;

//ModulePass *llvm::createAndersensPass() { return new Andersens(); }

//===---------------------------------------------------------------------===//
//                  AliasAnalysis Interface Implementation
//===---------------------------------------------------------------------===//

AliasResult AndersensAAResult::alias(const MemoryLocation &LocA,
                        const MemoryLocation &LocB)  {
  NumAliasQuery++; 
  if (NumAliasQuery > MaxAliasQuery) {
      return AAResultBase::alias(LocA, LocB);
  }

  auto *V1 = const_cast<Value *>(LocA.Ptr);
  auto *V2 = const_cast<Value *>(LocB.Ptr);

  Node *N1 = &GraphNodes[FindNode(getNode(const_cast<Value*>(V1)))];
  Node *N2 = &GraphNodes[FindNode(getNode(const_cast<Value*>(V2)))];

  if (PrintAndersAliasQueries) {
      errs() << " Alias_Begin \n";
      errs() << "Loc 1: " << *V1 << "\n";
      errs() << "Loc 2: " << *V2 << "\n";
      errs() << " Node 1: ";
      PrintNode(N1);
      errs() << " \n";
      errs() << " Node 2: ";
      PrintNode(N2);
      errs() << " \n";
  }

  if (N1->PointsTo->test(UniversalSet) || N2->PointsTo->test(UniversalSet)) {
      if (PrintAndersAliasQueries) {
          errs() << " One of them is Universal \n";
          errs() << " Alias_End \n";
      }
      return AAResultBase::alias(LocA, LocB);
  }

  // Check to see if the two pointers are known to not alias. They don't alias
  // if their points-to sets do not intersect.
  if (!N1->intersectsIgnoring(N2, NullObject)) {
    if (PrintAndersAliasQueries) {
        errs() << " Result: NoAlias \n";
        errs() << " Alias_End \n";
    }
    return NoAlias;
  }

  if (PrintAndersAliasQueries) {
      errs() << " Can't determine using points-to \n";
      errs() << " Alias_End \n";
  }
  return AAResultBase::alias(LocA, LocB);

}

// Get a printable name for the ModRef result.
static const char* getModRefResultStr(ModRefInfo R)
{
  const char *Names[] = { "NoModRef", "Ref", "Mod", "ModRef" };

  assert(R >= MRI_NoModRef && R <= MRI_ModRef);
  return Names[R];
}

ModRefInfo
AndersensAAResult::getModRefInfo(ImmutableCallSite CS,
                         const MemoryLocation &LocA) {
  if (PrintAndersModRefQueries) {
      errs() << " getModRefInfo_begin\n";
      errs() << "CS:  " << *(CS.getInstruction()) << "\n";
      errs() << "Loc: " << *(LocA.Ptr) << "\n";
  }

  // Try to use the collected Mod/Ref sets, if available.
  ModRefInfo R = MRI_ModRef;
   if (UseIntelModRef && IMR) {
        R = IMR->getModRefInfo(CS, LocA);
   }

   if (R != MRI_NoModRef) {
       ModRefInfo Others = AAResultBase::getModRefInfo(CS, LocA);
       R = ModRefInfo(R & Others);
   }

   if (PrintAndersModRefQueries) {
      errs() << "Result: " << getModRefResultStr(R) << "\n";
      errs() << " getModRefInfo_end\n";
  }

  return R;
}

ModRefInfo AndersensAAResult::getModRefInfo(ImmutableCallSite CS1, 
                                            ImmutableCallSite CS2) {
  if (PrintAndersModRefQueries) {
      errs() << " getModRefInfo_begin\n";
      errs() << "CS1: " << *(CS1.getInstruction()) << "\n";
      errs() << "CS2: " << *(CS2.getInstruction()) << "\n";
  }

  // Just forward the request along the chain. Note, a downstream analysis
  // may return to the Andersens to check for aliases via the AAChain
  // parameter.
  ModRefInfo R = AAResultBase::getModRefInfo(CS1, CS2);
  if (PrintAndersModRefQueries) {
      errs() << "Result: " << getModRefResultStr(R) << "\n";
      errs() << " getModRefInfo_end\n";
  }

  return R;
}

/// pointsToConstantMemory - If we can determine that this pointer only points
/// to constant memory, return true.  In practice, this means that if the
/// pointer can only point to constant globals, functions, or the null pointer,
/// return true.
///
bool AndersensAAResult::pointsToConstantMemory(const MemoryLocation &Loc,
                                               bool OrLocal) {
  NumPtrQuery++;
  if (NumPtrQuery > MaxPtrQuery) {
      return AAResultBase::pointsToConstantMemory(Loc, OrLocal);
  }
  auto *P = const_cast<Value *>(Loc.Ptr);
  Node *N = &GraphNodes[FindNode(getNode(const_cast<Value*>(P)))];
  unsigned i;

  if (PrintAndersConstMemQueries) {
      errs() << " ConstMem_Begin \n";
      errs() << "Loc : " << *P << "\n";
      errs() << " Node : ";
      PrintNode(N);
      errs() << "\n";
  }

  for (SparseBitVector<>::iterator bi = N->PointsTo->begin(), 
       E = N->PointsTo->end(); bi != E; ++bi) {
    i = *bi;
    Node *Pointee = &GraphNodes[i];

    if (PrintAndersConstMemQueries) {
      errs() << " Pointee : ";
      PrintNode(Pointee);
      errs() << "\n";
    }

    if (Value *V = Pointee->getValue()) {
      if (!isa<GlobalValue>(V) || (isa<GlobalVariable>(V) &&
                                   !cast<GlobalVariable>(V)->isConstant())) {
          if (PrintAndersConstMemQueries) {
              errs() << " Points-to can't decide \n";
              errs() << " ConstMem_End \n";
          }
          return AAResultBase::pointsToConstantMemory(Loc, OrLocal);
      }
    } else {
      if (i != NullObject) {
          if (PrintAndersConstMemQueries) {
              errs() << " Points-to can't decide \n";
              errs() << " ConstMem_End \n";
          }
          return AAResultBase::pointsToConstantMemory(Loc, OrLocal);
      }
    }
  }

  if (PrintAndersConstMemQueries) {
      errs() << " Result: true \n";
      errs() << " ConstMem_End \n";
  }
  return true;
}

//===----------------------------------------------------------------------===//
//                       Object Identification Phase
//===----------------------------------------------------------------------===//

/// IdentifyObjects - This stage scans the program, adding an entry to the
/// GraphNodes list for each memory object in the program (global stack or
/// heap), and populates the ValueNodes and ObjectNodes maps for these objects.
///
void AndersensAAResult::IdentifyObjects(Module &M) {
  unsigned NumObjects = 0;

  // Object #0 is always the universal set: the object that we don't know
  // anything about.
  assert(NumObjects == UniversalSet && "Something changed!");
  ++NumObjects;

  // Object #1 always represents the null pointer.
  assert(NumObjects == NullPtr && "Something changed!");
  ++NumObjects;

  // Object #2 always represents the null object (the object pointed to by null)
  assert(NumObjects == NullObject && "Something changed!");
  ++NumObjects;

  // Add all the globals first.
  for (Module::global_iterator I = M.global_begin(), E = M.global_end();
       I != E; ++I) {
    ObjectNodes[I] = NumObjects++;
    ValueNodes[I] = NumObjects++;
  }

  // Add nodes for all of the functions and the instructions inside of them.
  for (Module::iterator F = M.begin(), E = M.end(); F != E; ++F) {
    // The function itself is a memory object.
    unsigned First = NumObjects;
    ValueNodes[F] = NumObjects++;
    if (isa<PointerType>(F->getFunctionType()->getReturnType()))
      ReturnNodes[F] = NumObjects++;
    if (F->getFunctionType()->isVarArg())
      VarargNodes[F] = NumObjects++;


    // Add nodes for all of the incoming pointer arguments.
    for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
         I != E; ++I)
      {
        if (isa<PointerType>(I->getType()))
          ValueNodes[I] = NumObjects++;
      }
    MaxK[First] = NumObjects - First;

    // Scan the function body, creating a memory object for each heap/stack
    // allocation in the body of the function and a node to represent all
    // pointer values defined by instructions and used as operands.
    for (inst_iterator II = inst_begin(F), E = inst_end(F); II != E; ++II) {
      // If this is an heap or stack allocation, create a node for the memory
      // object.
      if (isa<PointerType>(II->getType())) {
        ValueNodes[&*II] = NumObjects++;
        if (AllocaInst *AI = dyn_cast<AllocaInst>(&*II))
          ObjectNodes[AI] = NumObjects++;
      }

      // Calls to inline asm need to be added as well because the callee isn't
      // referenced anywhere else.
      if (CallInst *CI = dyn_cast<CallInst>(&*II)) {
        Value *Callee = CI->getCalledValue();
        if (isa<InlineAsm>(Callee))
          ValueNodes[Callee] = NumObjects++;

        ImmutableCallSite cs1(&*II);
        if (const Function *F1 = cs1.getCalledFunction()) {
            // TODO: Make this condition as utility function later
            // after adding more malloc-like calls
            if (F1->getName() == "malloc" || F1->getName() == "calloc") {
                  ObjectNodes[CI] = NumObjects++;
           }
        }
      }
    }
  }

  // Now that we know how many objects to create, make them all now!
  GraphNodes.resize(NumObjects);
  NumNodes += NumObjects;
}

//===----------------------------------------------------------------------===//
//                     Constraint Identification Phase
//===----------------------------------------------------------------------===//

/// getNodeForConstantPointer - Return the node corresponding to the constant
/// pointer itself.
unsigned AndersensAAResult::getNodeForConstantPointer(Constant *C) {
  assert(isa<PointerType>(C->getType()) && "Not a constant pointer!");

  if (isa<ConstantPointerNull>(C) || isa<UndefValue>(C))
    return NullPtr;
  else if (GlobalValue *GV = dyn_cast<GlobalValue>(C))
    return getNode(GV);
  else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
    switch (CE->getOpcode()) {
    case Instruction::GetElementPtr:
      return getNodeForConstantPointer(CE->getOperand(0));
    case Instruction::IntToPtr:
      return UniversalSet;
    case Instruction::BitCast:
      return getNodeForConstantPointer(CE->getOperand(0));
    default:
      errs() << "Constant Expr not yet handled: " << *CE << "\n";
      llvm_unreachable(0);
    }
  } else if (const BlockAddress *BA = dyn_cast<BlockAddress>(C)) {
      return UniversalSet;
  } else {
    errs() << "Constant not yet handled: " << *C << "\n";
    llvm_unreachable("Unknown constant pointer!");
  }
  return 0;
}

/// getNodeForConstantPointerTarget - Return the node POINTED TO by the
/// specified constant pointer.
unsigned AndersensAAResult::getNodeForConstantPointerTarget(Constant *C) {
  assert(isa<PointerType>(C->getType()) && "Not a constant pointer!");

  if (isa<ConstantPointerNull>(C))
    return NullObject;
  else if (GlobalValue *GV = dyn_cast<GlobalValue>(C))
    return getObject(GV);
  else if (ConstantExpr *CE = dyn_cast<ConstantExpr>(C)) {
    switch (CE->getOpcode()) {
    case Instruction::GetElementPtr:
      return getNodeForConstantPointerTarget(CE->getOperand(0));
    case Instruction::IntToPtr:
      return UniversalSet;
    case Instruction::BitCast:
      return getNodeForConstantPointerTarget(CE->getOperand(0));
    default:
      errs() << "Constant Expr not yet handled: " << *CE << "\n";
      llvm_unreachable(0);
    }
  } else if (const BlockAddress *BA = dyn_cast<BlockAddress>(C)) {
      return UniversalSet;
  } else {
    llvm_unreachable("Unknown constant pointer!");
  }
  return 0;
}

/// AddGlobalInitializerConstraints - Add inclusion constraints for the memory
/// object N, which contains values indicated by C.
void AndersensAAResult::AddGlobalInitializerConstraints(unsigned NodeIndex,
                                                Constant *C) {
  if (C->getType()->isSingleValueType()) {
    if (isa<PointerType>(C->getType()))
      Constraints.push_back(Constraint(Constraint::Copy, NodeIndex,
                                       getNodeForConstantPointer(C)));
  } else if (C->isNullValue()) {
    Constraints.push_back(Constraint(Constraint::Copy, NodeIndex,
                                     NullObject));
    return;
  } else if (!isa<UndefValue>(C)) {
    // If this is an array or struct, include constraints for each element.
    assert(isa<ConstantArray>(C) || isa<ConstantDataSequential>(C) || 
           isa<ConstantStruct>(C));
    for (unsigned i = 0, e = C->getNumOperands(); i != e; ++i)
      AddGlobalInitializerConstraints(NodeIndex,
                                      cast<Constant>(C->getOperand(i)));
  }
}

/// AddConstraintsForNonInternalLinkage - If this function does not have
/// internal linkage, realize that we can't trust anything passed into or
/// returned by this function.
void AndersensAAResult::AddConstraintsForNonInternalLinkage(Function *F) {
  for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end(); I != E; ++I) {
    if (isa<PointerType>(I->getType()))
      // If this is an argument of an externally accessible function, the
      // incoming pointer might point to anything.
      Constraints.push_back(Constraint(Constraint::Copy, getNode(I),
                                       UniversalSet));
   }
}

/// AddConstraintsForCall - If this is a call to a "known" function, add the
/// constraints and return true.  If this is a call to an unknown function,
/// return false.
bool AndersensAAResult::AddConstraintsForExternalCall(CallSite CS, Function *F) {
  assert(F->isDeclaration() && "Not an external function!");


  // These functions don't induce any points-to constraints.
  if (F->getName() == "atoi" || F->getName() == "atof" ||
      F->getName() == "atol" || F->getName() == "atoll" ||
      F->getName() == "remove" || F->getName() == "unlink" ||
      F->getName() == "rename" || F->getName() == "memcmp" ||
      F->getName() == "llvm.memset" ||
      F->getName() == "strcmp" || F->getName() == "strncmp" ||
      F->getName() == "execl" || F->getName() == "execlp" ||
      F->getName() == "execle" || F->getName() == "execv" ||
      F->getName() == "execvp" || F->getName() == "chmod" ||
      F->getName() == "puts" || F->getName() == "write" ||
      F->getName() == "open" || F->getName() == "create" ||
      F->getName() == "truncate" || F->getName() == "chdir" ||
      F->getName() == "mkdir" || F->getName() == "rmdir" ||
      F->getName() == "read" || F->getName() == "pipe" ||
      F->getName() == "wait" || F->getName() == "time" ||
      F->getName() == "stat" || F->getName() == "fstat" ||
      F->getName() == "lstat" || F->getName() == "strtod" ||
      F->getName() == "strtof" || F->getName() == "strtold" ||
      F->getName() == "fopen" || F->getName() == "fdopen" ||
      F->getName() == "freopen" ||
      F->getName() == "fflush" || F->getName() == "feof" ||
      F->getName() == "fileno" || F->getName() == "clearerr" ||
      F->getName() == "rewind" || F->getName() == "ftell" ||
      F->getName() == "ferror" || F->getName() == "fgetc" ||
      F->getName() == "fgetc" || F->getName() == "_IO_getc" ||
      F->getName() == "fwrite" || F->getName() == "fread" ||
      F->getName() == "fgets" || F->getName() == "ungetc" ||
      F->getName() == "fputc" ||
      F->getName() == "fputs" || F->getName() == "putc" ||
      F->getName() == "ftell" || F->getName() == "rewind" ||
      F->getName() == "_IO_putc" || F->getName() == "fseek" ||
      F->getName() == "fgetpos" || F->getName() == "fsetpos" ||
      F->getName() == "printf" || F->getName() == "fprintf" ||
      F->getName() == "sprintf" || F->getName() == "vprintf" ||
      F->getName() == "vfprintf" || F->getName() == "vsprintf" ||
      F->getName() == "scanf" || F->getName() == "fscanf" ||
      F->getName() == "sscanf" || F->getName() == "__assert_fail" ||
      F->getName() == "modf")
    return true;


  // These functions do induce points-to edges.
  if (F->getName() == "llvm.memcpy" ||
      F->getName() == "llvm.memmove" ||
      F->getName() == "memmove") {

    const FunctionType *FTy = F->getFunctionType();
    if (FTy->getNumParams() > 1 && 
        isa<PointerType>(FTy->getParamType(0)) &&
        isa<PointerType>(FTy->getParamType(1))) {

      // *Dest = *Src, which requires an artificial graph node to represent the
      // constraint.  It is broken up into *Dest = temp, temp = *Src
      unsigned FirstArg = getNode(CS.getArgument(0));
      unsigned SecondArg = getNode(CS.getArgument(1));
      unsigned TempArg = GraphNodes.size();
      GraphNodes.push_back(Node());
      Constraints.push_back(Constraint(Constraint::Store,
                                       FirstArg, TempArg));
      Constraints.push_back(Constraint(Constraint::Load,
                                       TempArg, SecondArg));
      // In addition, Dest = Src
      Constraints.push_back(Constraint(Constraint::Copy,
                                       FirstArg, SecondArg));
      return true;
    }
  }

  // Result = Arg0
  if (F->getName() == "realloc" || F->getName() == "strchr" ||
      F->getName() == "strrchr" || F->getName() == "strstr" ||
      F->getName() == "strtok") {
    const FunctionType *FTy = F->getFunctionType();
    if (FTy->getNumParams() > 0 && 
        isa<PointerType>(FTy->getParamType(0))) {
      Constraints.push_back(Constraint(Constraint::Copy,
                                       getNode(CS.getInstruction()),
                                       getNode(CS.getArgument(0))));
      return true;
    }
  }

  return false;
}

/// CollectConstraints - This stage scans the program, adding a constraint to
/// the Constraints list for each instruction in the program that induces a
/// constraint, and setting up the initial points-to graph.
///
void AndersensAAResult::CollectConstraints(Module &M) {
  // First, the universal set points to itself.
  Constraints.push_back(Constraint(Constraint::AddressOf, UniversalSet,
                                   UniversalSet));
  Constraints.push_back(Constraint(Constraint::Store, UniversalSet,
                                   UniversalSet));

  // Next, the null pointer points to the null object.
  Constraints.push_back(Constraint(Constraint::AddressOf, NullPtr, 
                                   NullObject));

  // Next, add any constraints on global variables and their initializers.
  for (Module::global_iterator I = M.global_begin(), E = M.global_end();
       I != E; ++I) {
    // Associate the address of the global object as pointing to the memory for
    // the global: &G = <G memory>
    unsigned ObjectIndex = getObject(I);
    Node *Object = &GraphNodes[ObjectIndex];
    Object->setValue(I);
    Constraints.push_back(Constraint(Constraint::AddressOf, getNodeValue(*I),
                                     ObjectIndex));

    if (I->hasDefinitiveInitializer()) {
      AddGlobalInitializerConstraints(ObjectIndex, I->getInitializer());
      if (!I->hasLocalLinkage()) {
        Constraints.push_back(Constraint(Constraint::Copy, ObjectIndex,
                                           UniversalSet));
      }
    } else {
      // If it doesn't have an initializer (i.e. it's defined in another
      // translation unit), it points to the universal set.
      Constraints.push_back(Constraint(Constraint::Copy, ObjectIndex,
                                       UniversalSet));
    }
  }

  for (Module::iterator F = M.begin(), E = M.end(); F != E; ++F) {
    // Set function address
    GraphNodes[ValueNodes[F]].setValue(F);
    Constraints.push_back(Constraint(Constraint::AddressOf, ValueNodes[F],
                                     ValueNodes[F]));
    Constraints.push_back(Constraint(Constraint::Store, ValueNodes[F],
                                     ValueNodes[F]));
    // Set up the return value node.
    if (isa<PointerType>(F->getFunctionType()->getReturnType()))
      GraphNodes[getReturnNode(F)].setValue(F);
    if (F->getFunctionType()->isVarArg())
      GraphNodes[getVarargNode(F)].setValue(F);

    // Set up incoming argument nodes.
    for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
         I != E; ++I)
      if (isa<PointerType>(I->getType()))
        getNodeValue(*I);

    // At some point we should just add constraints for the escaping functions
    // at solve time, but this slows down solving. For now, we simply mark
    // address taken functions as escaping and treat them as external until
    // Escape analysis is implemented.
    if (!F->hasLocalLinkage() || F->hasAddressTaken())
      AddConstraintsForNonInternalLinkage(F);

    if (!F->isDeclaration()) {
      // Scan the function body, creating a memory object for each heap/stack
      // allocation in the body of the function and a node to represent all
      // pointer values defined by instructions and used as operands.
      visit(F);
    } else {
      // External functions that return pointers return the universal set.
      if (isa<PointerType>(F->getFunctionType()->getReturnType()))
        Constraints.push_back(Constraint(Constraint::Copy,
                                         getReturnNode(F),
                                         UniversalSet));

      // Any pointers that are passed into the function have the universal set
      // stored into them.
      for (Function::arg_iterator I = F->arg_begin(), E = F->arg_end();
           I != E; ++I)
        if (isa<PointerType>(I->getType())) {
          // Pointers passed into external functions could have anything stored
          // through them.
          Constraints.push_back(Constraint(Constraint::Store, getNode(I),
                                           UniversalSet));
          // Memory objects passed into external function calls can have the
          // universal set point to them.
#if FULL_UNIVERSAL
          Constraints.push_back(Constraint(Constraint::Copy,
                                           UniversalSet,
                                           getNode(I)));
#else
          Constraints.push_back(Constraint(Constraint::Copy,
                                           getNode(I),
                                           UniversalSet));
#endif
        }

      // If this is an external varargs function, it can also store pointers
      // into any pointers passed through the varargs section.
      if (F->getFunctionType()->isVarArg())
        Constraints.push_back(Constraint(Constraint::Store, getVarargNode(F),
                                         UniversalSet));
    }
  }
  NumConstraints += Constraints.size();
}


void AndersensAAResult::visitInstruction(Instruction &I) {
#ifdef NDEBUG
  return;          // This function is just a big assert.
#endif
  // Most instructions don't have any effect on pointer values.
  switch (I.getOpcode()) {
  case Instruction::Br:
  case Instruction::Switch:
  case Instruction::Unreachable:
  case Instruction::ICmp:
  case Instruction::FCmp:
  case Instruction::Resume:
  case Instruction::IndirectBr:
  case Instruction::Fence:
    return;

  default:
    // Is this something we aren't handling yet?
    errs() << "Unknown instruction: " << I;
    llvm_unreachable(0);
  }
}

void AndersensAAResult::visitInsertValueInst(InsertValueInst &AI) {
    if (!AI.getType()->isPointerTy()) {
        return;
    }
    Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(AI),
                                     getNode(AI.getOperand(0)))); 
    Constraints.push_back(Constraint(Constraint::Store, getNodeValue(AI),
                                     getNode(AI.getOperand(1)))); 
}

void AndersensAAResult::visitExtractValueInst(ExtractValueInst &AI) {
    if (!AI.getType()->isPointerTy()) {
        return;
    }
    Constraints.push_back(Constraint(Constraint::Load, getNodeValue(AI),
                                     getNode(AI.getAggregateOperand()))); 
}

void AndersensAAResult::visitAtomicRMWInst(AtomicRMWInst &AI) {
    if (!isa<PointerType>(AI.getValOperand()->getType())) {
        return;
    }
    Constraints.push_back(Constraint(Constraint::Store,
                                     getNode(AI.getPointerOperand()),
                                     getNode(AI.getValOperand())));
}

void AndersensAAResult::visitBinaryOperator(BinaryOperator &AI) {
    if (!AI.getType()->isPointerTy()) {
        return;
    }
    Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(AI),
                                     getNode(AI.getOperand(0))));
    Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(AI),
                                     getNode(AI.getOperand(1))));
}

void AndersensAAResult::visitPtrToIntInst(PtrToIntInst &AI) {
    Constraints.push_back(Constraint(Constraint::Copy, 
                   getNode(AI.getOperand(0)), UniversalSet));
}

void AndersensAAResult::visitIntToPtrInst(IntToPtrInst &AI) {
    Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(AI), 
                   UniversalSet));
}

void AndersensAAResult::visitExtractElementInst(ExtractElementInst &AI) {
  if (!AI.getType()->isPointerTy()) {
      return;
  }
  Constraints.push_back(Constraint(Constraint::Load, getNodeValue(AI),
                                   getNode(AI.getVectorOperand()))); 
}

void AndersensAAResult::visitInsertElementInst(InsertElementInst &AI) {
    if (!AI.getType()->isPointerTy()) {
        return;
    }
    Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(AI),
                                     getNode(AI.getOperand(0)))); 
    Constraints.push_back(Constraint(Constraint::Store, getNodeValue(AI),
                                     getNode(AI.getOperand(1)))); 
}

void AndersensAAResult::visitShuffleVectorInst(ShuffleVectorInst &AI) {
    if (!AI.getType()->isPointerTy()) {
        return;
    }
    Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(AI),
                                     getNode(AI.getOperand(0)))); 
    Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(AI),
                                     getNode(AI.getOperand(1)))); 
}

void AndersensAAResult::visitLandingPadInst(LandingPadInst &AI) {
  if (!AI.getType()->isPointerTy()) {
      return;
  }
  Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(AI),
                                UniversalSet));

}

void AndersensAAResult::visitAtomicCmpXchgInst(AtomicCmpXchgInst &AI) {
    if (!isa<PointerType>(AI.getNewValOperand()->getType())) {
        return;
    }
    Constraints.push_back(Constraint(Constraint::Store,
                                     getNode(AI.getPointerOperand()),
                                     getNode(AI.getNewValOperand())));
}

void AndersensAAResult::visitAllocaInst(AllocaInst &AI) {
  unsigned ObjectIndex = getObject(&AI);
  GraphNodes[ObjectIndex].setValue(&AI);
  Constraints.push_back(Constraint(Constraint::AddressOf, getNodeValue(AI),
                                   ObjectIndex));
}

void AndersensAAResult::visitReturnInst(ReturnInst &RI) {
  if (RI.getNumOperands() && isa<PointerType>(RI.getOperand(0)->getType()))
    // return V   -->   <Copy/retval{F}/v>
    Constraints.push_back(Constraint(Constraint::Copy,
                                     getReturnNode(RI.getParent()->getParent()),
                                     getNode(RI.getOperand(0))));
}

void AndersensAAResult::visitLoadInst(LoadInst &LI) {
  if (isa<PointerType>(LI.getType()))
    // P1 = load P2  -->  <Load/P1/P2>
    Constraints.push_back(Constraint(Constraint::Load, getNodeValue(LI),
                                     getNode(LI.getOperand(0))));
}

void AndersensAAResult::visitStoreInst(StoreInst &SI) {
  ConstantExpr *CE;

  if (isa<PointerType>(SI.getOperand(0)->getType())) {
    // CQ377860: So far, “value to store” operand of “Instruction::Store”
    // is treated as either “value” or constant expression. But, it is 
    // possible that “value to store” operand of “Instruction::Store” 
    // can be “Instruction::Select” instruction when “Instruction::Select”
    // is constant expression.
    //
    // Ex: store void (i8*)* select (i1 icmp eq (void (i8*)* inttoptr 
    // (i64 3 to void (i8*)*), void (i8*)* @DeleteScriptLimitCallback), 
    // void (i8*)* @Tcl_Free, void (i8*)* @DeleteScriptLimitCallback), 
    // void (i8*)** %18
    if ((CE = dyn_cast<ConstantExpr>(SI.getOperand(0))) &&
        (CE->getOpcode() == Instruction::Select)) {
      // Store (Select C1, C2), P2  -- <Store/P2/C1> and <Store/P2/C2> 
      unsigned SIN = getNode(SI.getOperand(1));
      Constraints.push_back(Constraint(Constraint::Store, SIN,
                                       getNode(CE->getOperand(1))));
      Constraints.push_back(Constraint(Constraint::Store, SIN,
                                       getNode(CE->getOperand(2))));
    }
    else {
      // store P1, P2  -->  <Store/P2/P1>
      Constraints.push_back(Constraint(Constraint::Store,
                                       getNode(SI.getOperand(1)),
                                       getNode(SI.getOperand(0))));
    }
  }
}

void AndersensAAResult::visitGetElementPtrInst(GetElementPtrInst &GEP) {
  // P1 = getelementptr P2, ... --> <Copy/P1/P2>
  //errs() << "GetElementPtr: " << GEP << "\n";
  Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(GEP),
                                   getNode(GEP.getOperand(0))));
}

void AndersensAAResult::visitPHINode(PHINode &PN) {
  if (isa<PointerType>(PN.getType())) {
    unsigned PNN = getNodeValue(PN);
    for (unsigned i = 0, e = PN.getNumIncomingValues(); i != e; ++i)
      // P1 = phi P2, P3  -->  <Copy/P1/P2>, <Copy/P1/P3>, ...
      Constraints.push_back(Constraint(Constraint::Copy, PNN,
                                       getNode(PN.getIncomingValue(i))));
  }
}

void AndersensAAResult::visitCastInst(CastInst &CI) {
  Value *Op = CI.getOperand(0);
  if (isa<PointerType>(CI.getType())) {
    if (isa<PointerType>(Op->getType())) {
      // P1 = cast P2  --> <Copy/P1/P2>
      Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(CI),
                                       getNode(CI.getOperand(0))));
    } else {
    // IntToPtr and PtrToInt instructions are handled separately in 
    // visitPtrToIntInst and visitIntToPtrInst. This code is not 
    // required anymore. 
    // TODO: Cleanup this code after confirming that this is code is 
    // not required anymore
      // P1 = cast int --> <Copy/P1/Univ>
#if 0
      Constraints.push_back(Constraint(Constraint::Copy, getNodeValue(CI),
                                       UniversalSet));
#else
      getNodeValue(CI);
#endif
    }
  } else if (isa<PointerType>(Op->getType())) {
    // int = cast P1 --> <Copy/Univ/P1>
#if 0
    Constraints.push_back(Constraint(Constraint::Copy,
                                     UniversalSet,
                                     getNode(CI.getOperand(0))));
#else
    getNode(CI.getOperand(0));
#endif
  }
}

void AndersensAAResult::visitSelectInst(SelectInst &SI) {
  if (isa<PointerType>(SI.getType())) {
    unsigned SIN = getNodeValue(SI);
    // P1 = select C, P2, P3   ---> <Copy/P1/P2>, <Copy/P1/P3>
    Constraints.push_back(Constraint(Constraint::Copy, SIN,
                                     getNode(SI.getOperand(1))));
    Constraints.push_back(Constraint(Constraint::Copy, SIN,
                                     getNode(SI.getOperand(2))));
  }
}

void AndersensAAResult::visitVAArg(VAArgInst &I) {
  llvm_unreachable("vaarg not handled yet!");
  //Constraints.push_back(Constraint(Constraint::Copy, getNode(I),
  //                             getVarargNode(I->getParent()->getParent())));
}

// Create Constraints for direct calls
//
void AndersensAAResult::AddConstraintsForDirectCall(CallSite CS, Function *F)
{
  CallSite::arg_iterator arg_itr = CS.arg_begin();
  CallSite::arg_iterator arg_end = CS.arg_end();
  Function::arg_iterator formal_itr = F->arg_begin();
  Function::arg_iterator formal_end = F->arg_end();
  Function::arg_iterator last_formal = F->arg_end();

  if (CS.getType()->isPointerTy()) {
    Constraints.push_back(Constraint(Constraint::Copy, 
                          getNode(CS.getInstruction()), 
                          getReturnNode(F)));
  }

  for (; formal_itr != formal_end;) {
    Argument* formal = formal_itr;
    Value* actual = *arg_itr;
    if (formal->getType()->isPointerTy()) {
      if (actual->getType()->isPointerTy()) {
        Constraints.push_back(Constraint(Constraint::Copy, 
                              getNode(formal_itr), getNode(actual)));
      }
      else {
        Constraints.push_back(Constraint(Constraint::Copy, 
                              getNode(formal_itr), UniversalSet));
      }     
    }
    last_formal = formal_itr;
    ++formal_itr;
    ++arg_itr;
  }

  if (F->getFunctionType()->isVarArg()) {
    if (last_formal == F->arg_end()) {
      // Handle calls like "call @_Z3h1pz(...)  
      // TODO:  
      AddConstraintsForInitActualsToUniversalSet(CS);
      return;
    }
    for (; arg_itr != arg_end && last_formal != formal_end; ++arg_itr) {
      Value* actual = *arg_itr;
      if (actual->getType()->isPointerTy()) {
        Constraints.push_back(Constraint(Constraint::Copy, 
                              getNode(last_formal), getNode(actual)));
      }
    } 
  }
}

// Set actuals of 'CS' to UniversalSet.
//
void AndersensAAResult::AddConstraintsForInitActualsToUniversalSet(CallSite CS) {

  CallSite::arg_iterator arg_itr = CS.arg_begin();
  CallSite::arg_iterator arg_end = CS.arg_end();

  if (CS.getType()->isPointerTy()) {
    Constraints.push_back(Constraint(Constraint::Copy, 
                          getNode(CS.getInstruction()), UniversalSet));
  }

  for (; arg_itr != arg_end; ++arg_itr) {
    Value* actual = *arg_itr;
    if (actual->getType()->isPointerTy()) {
      Constraints.push_back(Constraint(Constraint::Store, getNode(actual),
                                       UniversalSet));
    }
  }
}

/// AddConstraintsForCall - Add constraints for a call with actual arguments
/// specified by CS to the function specified by F.  Note that the types of
/// arguments might not match up in the case where this is an indirect call and
/// the function pointer has been casted.  If this is the case, do something
/// reasonable.
void AndersensAAResult::AddConstraintsForCall(CallSite CS, Function *F) {

  if (F == NULL) {
    // Handle Indirect calls differently
    IndirectCallList.push_back(CS);
    return; 
  }

  // If this is a call to an external function, try to handle it directly to get
  // some taste of context sensitivity.
  if (F->isDeclaration() || F->isIntrinsic()) {
    if (AddConstraintsForExternalCall(CS, F)) {
      return;
    }
    AddConstraintsForInitActualsToUniversalSet(CS);

    return;
  }

  // Handle Direct calls here
  AddConstraintsForDirectCall(CS, F);
}

void AndersensAAResult::visitCallSite(CallSite CS) {
  // TODO: Make this condition as utility function to handle all
  // malloc-like calls.   
  if (CS.getCalledFunction() && (CS.getCalledFunction()->getName() == "malloc"
                      || CS.getCalledFunction()->getName() == "calloc")) {
      // Instruction* inst = CS.getInstruction();
      unsigned ObjectIndex = getObject(CS.getInstruction());
      GraphNodes[ObjectIndex].setValue(CS.getInstruction());
      Constraints.push_back(Constraint(Constraint::AddressOf, 
                            getNodeValue(*CS.getInstruction()), ObjectIndex));
      return;
  }
  if (isa<PointerType>(CS.getType()))
    getNodeValue(*CS.getInstruction());

  if (Function *F = CS.getCalledFunction()) {
    AddConstraintsForCall(CS, F);
  } else {
    AddConstraintsForCall(CS, nullptr);
  }
}

//===----------------------------------------------------------------------===//
//                         Constraint Solving Phase
//===----------------------------------------------------------------------===//

/// intersects - Return true if the points-to set of this node intersects
/// with the points-to set of the specified node.
bool AndersensAAResult::Node::intersects(Node *N) const {
  return PointsTo->intersects(N->PointsTo);
}

/// intersectsIgnoring - Return true if the points-to set of this node
/// intersects with the points-to set of the specified node on any nodes
/// except for the specified node to ignore.
bool AndersensAAResult::Node::intersectsIgnoring(Node *N, unsigned Ignoring) const {
  // TODO: If we are only going to call this with the same value for Ignoring,
  // we should move the special values out of the points-to bitmap.
  bool WeHadIt = PointsTo->test(Ignoring);
  bool NHadIt = N->PointsTo->test(Ignoring);
  bool Result = false;
  if (WeHadIt)
    PointsTo->reset(Ignoring);
  if (NHadIt)
    N->PointsTo->reset(Ignoring);
  Result = PointsTo->intersects(N->PointsTo);
  if (WeHadIt)
    PointsTo->set(Ignoring);
  if (NHadIt)
    N->PointsTo->set(Ignoring);
  return Result;
}


/// Clump together address taken variables so that the points-to sets use up
/// less space and can be operated on faster.

void AndersensAAResult::ClumpAddressTaken() {
#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa-renumber"
  std::vector<unsigned> Translate;
  std::vector<Node> NewGraphNodes;

  Translate.resize(GraphNodes.size());
  unsigned NewPos = 0;

  for (unsigned i = 0; i < Constraints.size(); ++i) {
    Constraint &C = Constraints[i];
    if (C.Type == Constraint::AddressOf) {
      GraphNodes[C.Src].AddressTaken = true;
    }
  }
  for (unsigned i = 0; i < NumberSpecialNodes; ++i) {
    unsigned Pos = NewPos++;
    Translate[i] = Pos;
    NewGraphNodes.push_back(GraphNodes[i]);
    //errs() << "Renumbering node " << i << " to node " << Pos << "\n";
  }

  // I believe this ends up being faster than making two vectors and splicing
  // them.
  for (unsigned i = NumberSpecialNodes; i < GraphNodes.size(); ++i) {
    if (GraphNodes[i].AddressTaken) {
      unsigned Pos = NewPos++;
      Translate[i] = Pos;
      NewGraphNodes.push_back(GraphNodes[i]);
      //errs() << "Renumbering node " << i << " to node " << Pos << "\n";
    }
  }

  for (unsigned i = NumberSpecialNodes; i < GraphNodes.size(); ++i) {
    if (!GraphNodes[i].AddressTaken) {
      unsigned Pos = NewPos++;
      Translate[i] = Pos;
      NewGraphNodes.push_back(GraphNodes[i]);
      //errs() << "Renumbering node " << i << " to node " << Pos << "\n";
    }
  }

  for (DenseMap<Value*, unsigned>::iterator Iter = ValueNodes.begin(), 
       EV = ValueNodes.end();
       Iter != EV;
       ++Iter)
    Iter->second = Translate[Iter->second];

  for (DenseMap<Value*, unsigned>::iterator Iter = ObjectNodes.begin(),
       EO = ObjectNodes.end();
       Iter != EO;
       ++Iter)
    Iter->second = Translate[Iter->second];

  for (DenseMap<Function*, unsigned>::iterator Iter = ReturnNodes.begin(),
       ER = ReturnNodes.end();
       Iter != ER;
       ++Iter)
    Iter->second = Translate[Iter->second];

  for (DenseMap<Function*, unsigned>::iterator Iter = VarargNodes.begin(),
       EA = VarargNodes.end();
       Iter != EA;
       ++Iter)
    Iter->second = Translate[Iter->second];

  for (unsigned i = 0; i < Constraints.size(); ++i) {
    Constraint &C = Constraints[i];
    C.Src = Translate[C.Src];
    C.Dest = Translate[C.Dest];
  }

  GraphNodes.swap(NewGraphNodes);
#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa"
}

//
//
void AndersensAAResult::CollectPossibleIndirectNodes(void) {
  std::vector<CallSite>::const_iterator calllist_itr;

  PossibleSourceOfPointsToInfo.clear();
  for (unsigned i = 0, e = IndirectCallList.size(); i != e; ++i) {
    if (IndirectCallList[i].getType()->isPointerTy()) {
      PossibleSourceOfPointsToInfo.insert(
                    getNode(IndirectCallList[i].getInstruction()));
    }
  }
}

/// The technique used here is described in "Exploiting Pointer and Location
/// Equivalence to Optimize Pointer Analysis. In the 14th International Static
/// Analysis Symposium (SAS), August 2007."  It is known as the "HVN" algorithm,
/// and is equivalent to value numbering the collapsed constraint graph without
/// evaluating unions.  This is used as a pre-pass to HU in order to resolve
/// first order pointer dereferences and speed up/reduce memory usage of HU.
/// Running both is equivalent to HRU without the iteration
/// HVN in more detail:
/// Imagine the set of constraints was simply straight line code with no loops
/// (we eliminate cycles, so there are no loops), such as:
/// E = &D
/// E = &C
/// E = F
/// F = G
/// G = F
/// Applying value numbering to this code tells us:
/// G == F == E
///
/// For HVN, this is as far as it goes.  We assign new value numbers to every
/// "address node", and every "reference node".
/// To get the optimal result for this, we use a DFS + SCC (since all nodes in a
/// cycle must have the same value number since the = operation is really
/// inclusion, not overwrite), and value number nodes we receive points-to sets
/// before we value our own node.
/// The advantage of HU over HVN is that HU considers the inclusion property, so
/// that if you have
/// E = &D
/// E = &C
/// E = F
/// F = G
/// F = &D
/// G = F
/// HU will determine that G == F == E.  HVN will not, because it cannot prove
/// that the points to information ends up being the same because they all
/// receive &D from E anyway.

void AndersensAAResult::HVN() {
  //errs() << "Beginning HVN\n";
  // Build a predecessor graph.  This is like our constraint graph with the
  // edges going in the opposite direction, and there are edges for all the
  // constraints, instead of just copy constraints.  We also build implicit
  // edges for constraints are implied but not explicit.  I.E for the constraint
  // a = &b, we add implicit edges *a = b.  This helps us capture more cycles
  for (unsigned i = 0, e = Constraints.size(); i != e; ++i) {
    Constraint &C = Constraints[i];
    if (C.Type == Constraint::AddressOf) {
      GraphNodes[C.Src].AddressTaken = true;
      GraphNodes[C.Src].Direct = false;

      // Dest = &src edge
      unsigned AdrNode = C.Src + FirstAdrNode;
      if (!GraphNodes[C.Dest].PredEdges)
        GraphNodes[C.Dest].PredEdges = new SparseBitVector<>;
      GraphNodes[C.Dest].PredEdges->set(AdrNode);

      // *Dest = src edge
      unsigned RefNode = C.Dest + FirstRefNode;
      if (!GraphNodes[RefNode].ImplicitPredEdges)
        GraphNodes[RefNode].ImplicitPredEdges = new SparseBitVector<>;
      GraphNodes[RefNode].ImplicitPredEdges->set(C.Src);
    } else if (C.Type == Constraint::Load) {
      if (C.Offset == 0) {
        // dest = *src edge
        if (!GraphNodes[C.Dest].PredEdges)
          GraphNodes[C.Dest].PredEdges = new SparseBitVector<>;
        GraphNodes[C.Dest].PredEdges->set(C.Src + FirstRefNode);
      } else {
        GraphNodes[C.Dest].Direct = false;
      }
    } else if (C.Type == Constraint::Store) {
      if (C.Offset == 0) {
        // *dest = src edge
        unsigned RefNode = C.Dest + FirstRefNode;
        if (!GraphNodes[RefNode].PredEdges)
          GraphNodes[RefNode].PredEdges = new SparseBitVector<>;
        GraphNodes[RefNode].PredEdges->set(C.Src);
      }
    } else {
      // Dest = Src edge and *Dest = *Src edge
      auto I = PossibleSourceOfPointsToInfo.find(C.Src);
      if (I != PossibleSourceOfPointsToInfo.end()) {
        // Mark C.Src as Indirect so that a new PointerEquivLabel
        // is created for the node to avoid treating it as non-pointer.
        GraphNodes[C.Src].Direct = false;
      }
      if (!GraphNodes[C.Dest].PredEdges)
        GraphNodes[C.Dest].PredEdges = new SparseBitVector<>;
      GraphNodes[C.Dest].PredEdges->set(C.Src);
      unsigned RefNode = C.Dest + FirstRefNode;
      if (!GraphNodes[RefNode].ImplicitPredEdges)
        GraphNodes[RefNode].ImplicitPredEdges = new SparseBitVector<>;
      GraphNodes[RefNode].ImplicitPredEdges->set(C.Src + FirstRefNode);
    }
  }
  PEClass = 1;
  // Do SCC finding first to condense our predecessor graph
  DFSNumber = 0;
  Node2DFS.insert(Node2DFS.begin(), GraphNodes.size(), 0);
  Node2Deleted.insert(Node2Deleted.begin(), GraphNodes.size(), false);
  Node2Visited.insert(Node2Visited.begin(), GraphNodes.size(), false);

  for (unsigned i = 0; i < FirstRefNode; ++i) {
    unsigned Node = VSSCCRep[i];
    if (!Node2Visited[Node])
      HVNValNum(Node);
  }
  for (BitVectorMap::iterator Iter = Set2PEClass.begin();
       Iter != Set2PEClass.end();
       ++Iter)
    delete Iter->first;
  Set2PEClass.clear();
  Node2DFS.clear();
  Node2Deleted.clear();
  Node2Visited.clear();
  //errs() << "Finished HVN\n";

}

/// This is the workhorse of HVN value numbering. We combine SCC finding at the
/// same time because it's easy.
void AndersensAAResult::HVNValNum(unsigned NodeIndex) {
  unsigned MyDFS = DFSNumber++;
  Node *N = &GraphNodes[NodeIndex];
  Node2Visited[NodeIndex] = true;
  Node2DFS[NodeIndex] = MyDFS;

  // First process all our explicit edges
  if (N->PredEdges)
    for (SparseBitVector<>::iterator Iter = N->PredEdges->begin(),
         EN = N->PredEdges->end();
         Iter != EN;
         ++Iter) {
      unsigned j = VSSCCRep[*Iter];
      if (!Node2Deleted[j]) {
        if (!Node2Visited[j])
          HVNValNum(j);
        if (Node2DFS[NodeIndex] > Node2DFS[j])
          Node2DFS[NodeIndex] = Node2DFS[j];
      }
    }

  // Now process all the implicit edges
  if (N->ImplicitPredEdges)
    for (SparseBitVector<>::iterator Iter = N->ImplicitPredEdges->begin(),
         EI = N->ImplicitPredEdges->end();
         Iter != EI;
         ++Iter) {
      unsigned j = VSSCCRep[*Iter];
      if (!Node2Deleted[j]) {
        if (!Node2Visited[j])
          HVNValNum(j);
        if (Node2DFS[NodeIndex] > Node2DFS[j])
          Node2DFS[NodeIndex] = Node2DFS[j];
      }
    }

  // See if we found any cycles
  if (MyDFS == Node2DFS[NodeIndex]) {
    while (!SCCStack.empty() && Node2DFS[SCCStack.top()] >= MyDFS) {
      unsigned CycleNodeIndex = SCCStack.top();
      Node *CycleNode = &GraphNodes[CycleNodeIndex];
      VSSCCRep[CycleNodeIndex] = NodeIndex;
      // Unify the nodes
      N->Direct &= CycleNode->Direct;

      if (CycleNode->PredEdges) {
        if (!N->PredEdges)
          N->PredEdges = new SparseBitVector<>;
        *(N->PredEdges) |= CycleNode->PredEdges;
        delete CycleNode->PredEdges;
        CycleNode->PredEdges = NULL;
      }
      if (CycleNode->ImplicitPredEdges) {
        if (!N->ImplicitPredEdges)
          N->ImplicitPredEdges = new SparseBitVector<>;
        *(N->ImplicitPredEdges) |= CycleNode->ImplicitPredEdges;
        delete CycleNode->ImplicitPredEdges;
        CycleNode->ImplicitPredEdges = NULL;
      }

      SCCStack.pop();
    }

    Node2Deleted[NodeIndex] = true;

    if (!N->Direct) {
      GraphNodes[NodeIndex].PointerEquivLabel = PEClass++;
      return;
    }

    // Collect labels of successor nodes
    bool AllSame = true;
    unsigned First = ~0;
    SparseBitVector<> *Labels = new SparseBitVector<>;
    bool Used = false;

    if (N->PredEdges)
      for (SparseBitVector<>::iterator Iter = N->PredEdges->begin();
           Iter != N->PredEdges->end();
         ++Iter) {
        unsigned j = VSSCCRep[*Iter];
        unsigned Label = GraphNodes[j].PointerEquivLabel;
        // Ignore labels that are equal to us or non-pointers
        if (j == NodeIndex || Label == 0)
          continue;
        if (First == (unsigned)~0)
          First = Label;
        else if (First != Label)
          AllSame = false;
        Labels->set(Label);
    }

    // We either have a non-pointer, a copy of an existing node, or a new node.
    // Assign the appropriate pointer equivalence label.
    if (Labels->empty()) {
      GraphNodes[NodeIndex].PointerEquivLabel = 0;
    } else if (AllSame) {
      GraphNodes[NodeIndex].PointerEquivLabel = First;
    } else {
      GraphNodes[NodeIndex].PointerEquivLabel = Set2PEClass[Labels];
      if (GraphNodes[NodeIndex].PointerEquivLabel == 0) {
        unsigned EquivClass = PEClass++;
        Set2PEClass[Labels] = EquivClass;
        GraphNodes[NodeIndex].PointerEquivLabel = EquivClass;
        Used = true;
      }
    }
    if (!Used)
      delete Labels;
  } else {
    SCCStack.push(NodeIndex);
  }
}

/// The technique used here is described in "Exploiting Pointer and Location
/// Equivalence to Optimize Pointer Analysis. In the 14th International Static
/// Analysis Symposium (SAS), August 2007."  It is known as the "HU" algorithm,
/// and is equivalent to value numbering the collapsed constraint graph
/// including evaluating unions.
void AndersensAAResult::HU() {
  //errs() << "Beginning HU\n";
  // Build a predecessor graph.  This is like our constraint graph with the
  // edges going in the opposite direction, and there are edges for all the
  // constraints, instead of just copy constraints.  We also build implicit
  // edges for constraints are implied but not explicit.  I.E for the constraint
  // a = &b, we add implicit edges *a = b.  This helps us capture more cycles
  for (unsigned i = 0, e = Constraints.size(); i != e; ++i) {
    Constraint &C = Constraints[i];
    if (C.Type == Constraint::AddressOf) {
      GraphNodes[C.Src].AddressTaken = true;
      GraphNodes[C.Src].Direct = false;

      GraphNodes[C.Dest].PointsTo->set(C.Src);
      // *Dest = src edge
      unsigned RefNode = C.Dest + FirstRefNode;
      if (!GraphNodes[RefNode].ImplicitPredEdges)
        GraphNodes[RefNode].ImplicitPredEdges = new SparseBitVector<>;
      GraphNodes[RefNode].ImplicitPredEdges->set(C.Src);
      GraphNodes[C.Src].PointedToBy->set(C.Dest);
    } else if (C.Type == Constraint::Load) {
      if (C.Offset == 0) {
        // dest = *src edge
        if (!GraphNodes[C.Dest].PredEdges)
          GraphNodes[C.Dest].PredEdges = new SparseBitVector<>;
        GraphNodes[C.Dest].PredEdges->set(C.Src + FirstRefNode);
      } else {
        GraphNodes[C.Dest].Direct = false;
      }
    } else if (C.Type == Constraint::Store) {
      if (C.Offset == 0) {
        // *dest = src edge
        unsigned RefNode = C.Dest + FirstRefNode;
        if (!GraphNodes[RefNode].PredEdges)
          GraphNodes[RefNode].PredEdges = new SparseBitVector<>;
        GraphNodes[RefNode].PredEdges->set(C.Src);
      }
    } else {
      // Dest = Src edge and *Dest = *Src edg
      if (!GraphNodes[C.Dest].PredEdges)
        GraphNodes[C.Dest].PredEdges = new SparseBitVector<>;
      GraphNodes[C.Dest].PredEdges->set(C.Src);
      unsigned RefNode = C.Dest + FirstRefNode;
      if (!GraphNodes[RefNode].ImplicitPredEdges)
        GraphNodes[RefNode].ImplicitPredEdges = new SparseBitVector<>;
      GraphNodes[RefNode].ImplicitPredEdges->set(C.Src + FirstRefNode);
    }
  }
  PEClass = 1;
  // Do SCC finding first to condense our predecessor graph
  DFSNumber = 0;
  Node2DFS.insert(Node2DFS.begin(), GraphNodes.size(), 0);
  Node2Deleted.insert(Node2Deleted.begin(), GraphNodes.size(), false);
  Node2Visited.insert(Node2Visited.begin(), GraphNodes.size(), false);

  for (unsigned i = 0; i < FirstRefNode; ++i) {
    if (FindNode(i) == i) {
      unsigned Node = VSSCCRep[i];
      if (!Node2Visited[Node])
        Condense(Node);
    }
  }

  // Reset tables for actual labeling
  Node2DFS.clear();
  Node2Visited.clear();
  Node2Deleted.clear();
  // Pre-grow our densemap so that we don't get really bad behavior
  Set2PEClass.resize(GraphNodes.size());

  // Visit the condensed graph and generate pointer equivalence labels.
  Node2Visited.insert(Node2Visited.begin(), GraphNodes.size(), false);
  for (unsigned i = 0; i < FirstRefNode; ++i) {
    if (FindNode(i) == i) {
      unsigned Node = VSSCCRep[i];
      if (!Node2Visited[Node])
        HUValNum(Node);
    }
  }
  // PEClass nodes will be deleted by the deleting of N->PointsTo in our caller.
  Set2PEClass.clear();
  //errs() << "Finished HU\n";
}


/// Implementation of standard Tarjan SCC algorithm as modified by Nuutilla.
void AndersensAAResult::Condense(unsigned NodeIndex) {
  unsigned MyDFS = DFSNumber++;
  Node *N = &GraphNodes[NodeIndex];
  Node2Visited[NodeIndex] = true;
  Node2DFS[NodeIndex] = MyDFS;

  // First process all our explicit edges
  if (N->PredEdges)
    for (SparseBitVector<>::iterator Iter = N->PredEdges->begin();
         Iter != N->PredEdges->end();
         ++Iter) {
      unsigned j = VSSCCRep[*Iter];
      if (!Node2Deleted[j]) {
        if (!Node2Visited[j])
          Condense(j);
        if (Node2DFS[NodeIndex] > Node2DFS[j])
          Node2DFS[NodeIndex] = Node2DFS[j];
      }
    }

  // Now process all the implicit edges
  if (N->ImplicitPredEdges)
    for (SparseBitVector<>::iterator Iter = N->ImplicitPredEdges->begin();
         Iter != N->ImplicitPredEdges->end();
         ++Iter) {
      unsigned j = VSSCCRep[*Iter];
      if (!Node2Deleted[j]) {
        if (!Node2Visited[j])
          Condense(j);
        if (Node2DFS[NodeIndex] > Node2DFS[j])
          Node2DFS[NodeIndex] = Node2DFS[j];
      }
    }

  // See if we found any cycles
  if (MyDFS == Node2DFS[NodeIndex]) {
    while (!SCCStack.empty() && Node2DFS[SCCStack.top()] >= MyDFS) {
      unsigned CycleNodeIndex = SCCStack.top();
      Node *CycleNode = &GraphNodes[CycleNodeIndex];
      VSSCCRep[CycleNodeIndex] = NodeIndex;
      // Unify the nodes
      N->Direct &= CycleNode->Direct;

      *(N->PointsTo) |= CycleNode->PointsTo;
      delete CycleNode->PointsTo;
      CycleNode->PointsTo = NULL;
      if (CycleNode->PredEdges) {
        if (!N->PredEdges)
          N->PredEdges = new SparseBitVector<>;
        *(N->PredEdges) |= CycleNode->PredEdges;
        delete CycleNode->PredEdges;
        CycleNode->PredEdges = NULL;
      }
      if (CycleNode->ImplicitPredEdges) {
        if (!N->ImplicitPredEdges)
          N->ImplicitPredEdges = new SparseBitVector<>;
        *(N->ImplicitPredEdges) |= CycleNode->ImplicitPredEdges;
        delete CycleNode->ImplicitPredEdges;
        CycleNode->ImplicitPredEdges = NULL;
      }
      SCCStack.pop();
    }

    Node2Deleted[NodeIndex] = true;

    // Set up number of incoming edges for other nodes
    if (N->PredEdges)
      for (SparseBitVector<>::iterator Iter = N->PredEdges->begin();
           Iter != N->PredEdges->end();
           ++Iter)
        ++GraphNodes[VSSCCRep[*Iter]].NumInEdges;
  } else {
    SCCStack.push(NodeIndex);
  }
}

void AndersensAAResult::HUValNum(unsigned NodeIndex) {
  Node *N = &GraphNodes[NodeIndex];
  Node2Visited[NodeIndex] = true;

  // Eliminate dereferences of non-pointers for those non-pointers we have
  // already identified.  These are ref nodes whose non-ref node:
  // 1. Has already been visited determined to point to nothing (and thus, a
  // dereference of it must point to nothing)
  // 2. Any direct node with no predecessor edges in our graph and with no
  // points-to set (since it can't point to anything either, being that it
  // receives no points-to sets and has none).
  if (NodeIndex >= FirstRefNode) {
    unsigned j = VSSCCRep[FindNode(NodeIndex - FirstRefNode)];
    if ((Node2Visited[j] && !GraphNodes[j].PointerEquivLabel)
        || (GraphNodes[j].Direct && !GraphNodes[j].PredEdges
            && GraphNodes[j].PointsTo->empty())){
      return;
    }
  }
    // Process all our explicit edges
  if (N->PredEdges)
    for (SparseBitVector<>::iterator Iter = N->PredEdges->begin();
         Iter != N->PredEdges->end();
         ++Iter) {
      unsigned j = VSSCCRep[*Iter];
      if (!Node2Visited[j])
        HUValNum(j);

      // If this edge turned out to be the same as us, or got no pointer
      // equivalence label (and thus points to nothing) , just decrement our
      // incoming edges and continue.
      if (j == NodeIndex || GraphNodes[j].PointerEquivLabel == 0) {
        --GraphNodes[j].NumInEdges;
        continue;
      }

      *(N->PointsTo) |= GraphNodes[j].PointsTo;

      // If we didn't end up storing this in the hash, and we're done with all
      // the edges, we don't need the points-to set anymore.
      --GraphNodes[j].NumInEdges;
      if (!GraphNodes[j].NumInEdges && !GraphNodes[j].StoredInHash) {
        delete GraphNodes[j].PointsTo;
        GraphNodes[j].PointsTo = NULL;
      }
    }
  // If this isn't a direct node, generate a fresh variable.
  if (!N->Direct) {
    N->PointsTo->set(FirstRefNode + NodeIndex);
  }

  // See If we have something equivalent to us, if not, generate a new
  // equivalence class.
  if (N->PointsTo->empty()) {
    delete N->PointsTo;
    N->PointsTo = NULL;
  } else {
    if (N->Direct) {
      N->PointerEquivLabel = Set2PEClass[N->PointsTo];
      if (N->PointerEquivLabel == 0) {
        unsigned EquivClass = PEClass++;
        N->StoredInHash = true;
        Set2PEClass[N->PointsTo] = EquivClass;
        N->PointerEquivLabel = EquivClass;
      }
    } else {
      N->PointerEquivLabel = PEClass++;
    }
  }
}

/// Rewrite our list of constraints so that pointer equivalent nodes are
/// replaced by their the pointer equivalence class representative.
void AndersensAAResult::RewriteConstraints() {
  std::vector<Constraint> NewConstraints;
  DenseSet<Constraint, ConstraintKeyInfo> Seen;

  PEClass2Node.clear();
  PENLEClass2Node.clear();

  // We may have from 1 to Graphnodes + 1 equivalence classes.
  PEClass2Node.insert(PEClass2Node.begin(), GraphNodes.size() + 1, -1);
  PENLEClass2Node.insert(PENLEClass2Node.begin(), GraphNodes.size() + 1, -1);

  // Rewrite constraints, ignoring non-pointer constraints, uniting equivalent
  // nodes, and rewriting constraints to use the representative nodes.
  for (unsigned i = 0, e = Constraints.size(); i != e; ++i) {
    Constraint &C = Constraints[i];
    unsigned RHSNode = FindNode(C.Src);
    unsigned LHSNode = FindNode(C.Dest);
    unsigned RHSLabel = GraphNodes[VSSCCRep[RHSNode]].PointerEquivLabel;
    unsigned LHSLabel = GraphNodes[VSSCCRep[LHSNode]].PointerEquivLabel;

    // First we try to eliminate constraints for things we can prove don't point
    // to anything.
    if (LHSLabel == 0) {
      DEBUG(PrintNode(&GraphNodes[LHSNode]));
      //errs() << " is a non-pointer, ignoring constraint.\n";
      continue;
    }
    if (RHSLabel == 0) {
      DEBUG(PrintNode(&GraphNodes[RHSNode]));
      //errs() << " is a non-pointer, ignoring constraint.\n";
      continue;
    }
    // This constraint may be useless, and it may become useless as we translate
    // it.
    if (C.Src == C.Dest && C.Type == Constraint::Copy)
      continue;

    C.Src = FindEquivalentNode(RHSNode, RHSLabel);
    C.Dest = FindEquivalentNode(FindNode(LHSNode), LHSLabel);
    if ((C.Src == C.Dest && C.Type == Constraint::Copy)
        || Seen.count(C))
      continue;

    Seen.insert(C);
    NewConstraints.push_back(C);
  }
  Constraints.swap(NewConstraints);
  PEClass2Node.clear();
}

/// See if we have a node that is pointer equivalent to the one being asked
/// about, and if so, unite them and return the equivalent node.  Otherwise,
/// return the original node.
unsigned AndersensAAResult::FindEquivalentNode(unsigned NodeIndex,
                                       unsigned NodeLabel) {
  if (!GraphNodes[NodeIndex].AddressTaken) {
    if (PEClass2Node[NodeLabel] != -1) {
      // We found an existing node with the same pointer label, so unify them.
      // We specifically request that Union-By-Rank not be used so that
      // PEClass2Node[NodeLabel] U= NodeIndex and not the other way around.
      return UniteNodes(PEClass2Node[NodeLabel], NodeIndex, false);
    } else {
      PEClass2Node[NodeLabel] = NodeIndex;
      PENLEClass2Node[NodeLabel] = NodeIndex;
    }
  } else if (PENLEClass2Node[NodeLabel] == -1) {
    PENLEClass2Node[NodeLabel] = NodeIndex;
  }

  return NodeIndex;
}

void AndersensAAResult::PrintLabels() const {
  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    if (i < FirstRefNode) {
      PrintNode(&GraphNodes[i]);
    } else if (i < FirstAdrNode) {
      errs() << "REF(";
      PrintNode(&GraphNodes[i-FirstRefNode]);
      errs() <<")";
    } else {
      errs() << "ADR(";
      PrintNode(&GraphNodes[i-FirstAdrNode]);
      errs() <<")";
    }

    errs() << " has pointer label " << GraphNodes[i].PointerEquivLabel
         << " and SCC rep " << VSSCCRep[i]
         << " and is " << (GraphNodes[i].Direct ? "Direct" : "Not direct")
         << "\n";
  }
}

/// The technique used here is described in "The Ant and the
/// Grasshopper: Fast and Accurate Pointer Analysis for Millions of
/// Lines of Code. In Programming Language Design and Implementation
/// (PLDI), June 2007." It is known as the "HCD" (Hybrid Cycle
/// Detection) algorithm. It is called a hybrid because it performs an
/// offline analysis and uses its results during the solving (online)
/// phase. This is just the offline portion; the results of this
/// operation are stored in SDT and are later used in SolveContraints()
/// and UniteNodes().
void AndersensAAResult::HCD() {
  //errs() << "Starting HCD.\n";
  HCDSCCRep.resize(GraphNodes.size());

  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    GraphNodes[i].Edges = new SparseBitVector<>;
    HCDSCCRep[i] = i;
  }

  for (unsigned i = 0, e = Constraints.size(); i != e; ++i) {
    Constraint &C = Constraints[i];
    assert (C.Src < GraphNodes.size() && C.Dest < GraphNodes.size());
    if (C.Type == Constraint::AddressOf) {
      continue;
    } else if (C.Type == Constraint::Load) {
      if( C.Offset == 0 )
        GraphNodes[C.Dest].Edges->set(C.Src + FirstRefNode);
    } else if (C.Type == Constraint::Store) {
      if( C.Offset == 0 )
        GraphNodes[C.Dest + FirstRefNode].Edges->set(C.Src);
    } else {
      GraphNodes[C.Dest].Edges->set(C.Src);
    }
  }

  Node2DFS.insert(Node2DFS.begin(), GraphNodes.size(), 0);
  Node2Deleted.insert(Node2Deleted.begin(), GraphNodes.size(), false);
  Node2Visited.insert(Node2Visited.begin(), GraphNodes.size(), false);
  SDT.insert(SDT.begin(), GraphNodes.size() / 2, -1);

  DFSNumber = 0;
  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    unsigned Node = HCDSCCRep[i];
    if (!Node2Deleted[Node])
      Search(Node);
  }

  for (unsigned i = 0; i < GraphNodes.size(); ++i)
    if (GraphNodes[i].Edges != NULL) {
      delete GraphNodes[i].Edges;
      GraphNodes[i].Edges = NULL;
    }

  while( !SCCStack.empty() )
    SCCStack.pop();

  Node2DFS.clear();
  Node2Visited.clear();
  Node2Deleted.clear();
  HCDSCCRep.clear();
  //errs() << "HCD complete.\n";
}

// Component of HCD: 
// Use Nuutila's variant of Tarjan's algorithm to detect
// Strongly-Connected Components (SCCs). For non-trivial SCCs
// containing ref nodes, insert the appropriate information in SDT.
void AndersensAAResult::Search(unsigned Node) {
  unsigned MyDFS = DFSNumber++;

  Node2Visited[Node] = true;
  Node2DFS[Node] = MyDFS;

  for (SparseBitVector<>::iterator Iter = GraphNodes[Node].Edges->begin(),
                                   End  = GraphNodes[Node].Edges->end();
       Iter != End;
       ++Iter) {
    unsigned J = HCDSCCRep[*Iter];
    assert(GraphNodes[J].isRep() && "Debug check; must be representative");
    if (!Node2Deleted[J]) {
      if (!Node2Visited[J])
        Search(J);
      if (Node2DFS[Node] > Node2DFS[J])
        Node2DFS[Node] = Node2DFS[J];
    }
  }

  if( MyDFS != Node2DFS[Node] ) {
    SCCStack.push(Node);
    return;
  }

  // This node is the root of a SCC, so process it.
  //
  // If the SCC is "non-trivial" (not a singleton) and contains a reference 
  // node, we place this SCC into SDT.  We unite the nodes in any case.
  if (!SCCStack.empty() && Node2DFS[SCCStack.top()] >= MyDFS) {
    SparseBitVector<> SCC;

    SCC.set(Node);

    bool Ref = (Node >= FirstRefNode);

    Node2Deleted[Node] = true;

    do {
      unsigned P = SCCStack.top(); SCCStack.pop();
      Ref |= (P >= FirstRefNode);
      SCC.set(P);
      HCDSCCRep[P] = Node;
    } while (!SCCStack.empty() && Node2DFS[SCCStack.top()] >= MyDFS);

    if (Ref) {
      unsigned Rep = SCC.find_first();
      assert(Rep < FirstRefNode && "The SCC didn't have a non-Ref node!");

      SparseBitVector<>::iterator i = SCC.begin();

      // Skip over the non-ref nodes
      while( *i < FirstRefNode )
        ++i;

      while( i != SCC.end() )
        SDT[ (*i++) - FirstRefNode ] = Rep;
    }
  }
}


/// Optimize the constraints by performing offline variable substitution and
/// other optimizations.
void AndersensAAResult::OptimizeConstraints() {
  //errs() << "Beginning constraint optimization\n";

  SDTActive = false;

  // Function related nodes need to stay in the same relative position and can't
  // be location equivalent.
  for (std::map<unsigned, unsigned>::iterator Iter = MaxK.begin();
       Iter != MaxK.end();
       ++Iter) {
    for (unsigned i = Iter->first;
         i != Iter->first + Iter->second;
         ++i) {
      GraphNodes[i].AddressTaken = true;
      GraphNodes[i].Direct = false;
    }
  }

  ClumpAddressTaken();
  FirstRefNode = GraphNodes.size();
  FirstAdrNode = FirstRefNode + GraphNodes.size();
  GraphNodes.insert(GraphNodes.end(), 2 * GraphNodes.size(),
                    Node(false));
  VSSCCRep.resize(GraphNodes.size());
  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    VSSCCRep[i] = i;
  }
  CollectPossibleIndirectNodes();
  HVN();
  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    Node *N = &GraphNodes[i];
    delete N->PredEdges;
    N->PredEdges = NULL;
    delete N->ImplicitPredEdges;
    N->ImplicitPredEdges = NULL;
  }
#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa-labels"
  DEBUG(PrintLabels());
#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa"
  RewriteConstraints();
  // Delete the adr nodes.
  GraphNodes.resize(FirstRefNode * 2);

  // Now perform HU
  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    Node *N = &GraphNodes[i];
    if (FindNode(i) == i) {
      N->PointsTo = new SparseBitVector<>;
      N->PointedToBy = new SparseBitVector<>;
      // Reset our labels
    }
    VSSCCRep[i] = i;
    N->PointerEquivLabel = 0;
  }
  //HU();
#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa-labels"
  DEBUG(PrintLabels());
#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa"
  //RewriteConstraints();
  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    if (FindNode(i) == i) {
      Node *N = &GraphNodes[i];
      delete N->PointsTo;
      N->PointsTo = NULL;
      delete N->PredEdges;
      N->PredEdges = NULL;
      delete N->ImplicitPredEdges;
      N->ImplicitPredEdges = NULL;
      delete N->PointedToBy;
      N->PointedToBy = NULL;
    }
  }

  // perform Hybrid Cycle Detection (HCD)
  HCD();
  SDTActive = true;

  // No longer any need for the upper half of GraphNodes (for ref nodes).
  GraphNodes.erase(GraphNodes.begin() + FirstRefNode, GraphNodes.end());

  // HCD complete.

  //errs() << "Finished constraint optimization\n";
  FirstRefNode = 0;
  FirstAdrNode = 0;
}

/// Unite pointer but not location equivalent variables, now that the constraint
/// graph is built.
void AndersensAAResult::UnitePointerEquivalences() {
  //errs() << "Uniting remaining pointer equivalences\n";
  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    if (GraphNodes[i].AddressTaken && GraphNodes[i].isRep()) {
      unsigned Label = GraphNodes[i].PointerEquivLabel;

      if (Label && PENLEClass2Node[Label] != -1)
        UniteNodes(i, PENLEClass2Node[Label]);
    }
  }
  //errs() << "Finished remaining pointer equivalences\n";
  PENLEClass2Node.clear();
}

/// Create the constraint graph used for solving points-to analysis.
///
void AndersensAAResult::CreateConstraintGraph() {
  for (unsigned i = 0, e = Constraints.size(); i != e; ++i) {
    Constraint &C = Constraints[i];
    assert (C.Src < GraphNodes.size() && C.Dest < GraphNodes.size());
    if (C.Type == Constraint::AddressOf)
      GraphNodes[C.Dest].PointsTo->set(C.Src);
    else if (C.Type == Constraint::Load)
      GraphNodes[C.Src].Constraints.push_back(C);
    else if (C.Type == Constraint::Store)
      GraphNodes[C.Dest].Constraints.push_back(C);
    else if (C.Offset != 0)
      GraphNodes[C.Src].Constraints.push_back(C);
    else
      GraphNodes[C.Src].Edges->set(C.Dest);
  }
}

// Perform DFS and cycle detection.
bool AndersensAAResult::QueryNode(unsigned Node) {
  assert(GraphNodes[Node].isRep() && "Querying a non-rep node");
  unsigned OurDFS = ++DFSNumber;
  SparseBitVector<> ToErase;
  SparseBitVector<> NewEdges;
  Tarjan2DFS[Node] = OurDFS;

  // Changed denotes a change from a recursive call that we will bubble up.
  // Merged is set if we actually merge a node ourselves.
  bool Changed = false, Merged = false;

  for (SparseBitVector<>::iterator bi = GraphNodes[Node].Edges->begin();
       bi != GraphNodes[Node].Edges->end();
       ++bi) {
    unsigned RepNode = FindNode(*bi);
    // If this edge points to a non-representative node but we are
    // already planning to add an edge to its representative, we have no
    // need for this edge anymore.
    if (RepNode != *bi && NewEdges.test(RepNode)){
      ToErase.set(*bi);
      continue;
    }

    // Continue about our DFS.
    if (!Tarjan2Deleted[RepNode]){
      if (Tarjan2DFS[RepNode] == 0) {
        Changed |= QueryNode(RepNode);
        // May have been changed by QueryNode
        RepNode = FindNode(RepNode);
      }
      if (Tarjan2DFS[RepNode] < Tarjan2DFS[Node])
        Tarjan2DFS[Node] = Tarjan2DFS[RepNode];
    }

    // We may have just discovered that this node is part of a cycle, in
    // which case we can also erase it.
    if (RepNode != *bi) {
      ToErase.set(*bi);
      NewEdges.set(RepNode);
    }
  }

  GraphNodes[Node].Edges->intersectWithComplement(ToErase);
  GraphNodes[Node].Edges |= NewEdges;

  // If this node is a root of a non-trivial SCC, place it on our 
  // worklist to be processed.
  if (OurDFS == Tarjan2DFS[Node]) {
    while (!SCCStack.empty() && Tarjan2DFS[SCCStack.top()] >= OurDFS) {
      Node = UniteNodes(Node, SCCStack.top());

      SCCStack.pop();
      Merged = true;
    }
    Tarjan2Deleted[Node] = true;

    if (Merged)
      NextWL->insert(&GraphNodes[Node]);
  } else {
    SCCStack.push(Node);
  }

  return(Changed | Merged);
}

// Add Edge in points-to Graph.
//
void AndersensAAResult::AddEdgeInGraph(unsigned N1, unsigned N2) {
  N1 = FindNode(N1);
  N2 = FindNode(N2);

  if (GraphNodes[N2].Edges->test_and_set(N1)) {
    if (GraphNodes[N1].PointsTo |= *(GraphNodes[N2].PointsTo)) {
      NextWL->insert(&GraphNodes[N1]);
    }
  }
}

// Create edges from all actuals of 'CS' to UniversalSet.
//
void AndersensAAResult::InitIndirectCallActualsToUniversalSet(CallSite CS) {

  CallSite::arg_iterator arg_itr = CS.arg_begin();
  CallSite::arg_iterator arg_end = CS.arg_end();

  if (CS.getType()->isPointerTy()) {
    AddEdgeInGraph(getNode(CS.getInstruction()), UniversalSet);
  }

  for (; arg_itr != arg_end; ++arg_itr) {
    Value* actual = *arg_itr;
    if (actual->getType()->isPointerTy()) {
      // TODO: Need to think more about it. ICC is not doing it.
      // Need to check with small test cases.
    }
  }
}

// Map actuals of 'CS' to formals of 'F'
//
void AndersensAAResult::IndirectCallActualsToFormals(CallSite CS, Function *F) {

  if (F->isDeclaration() || F->isIntrinsic()) {
    // TODO: Model Library calls like malloc here and change Graph
    InitIndirectCallActualsToUniversalSet(CS);
    return;
  } 

  CallSite::arg_iterator arg_itr = CS.arg_begin();
  CallSite::arg_iterator arg_end = CS.arg_end();
  Function::arg_iterator formal_itr = F->arg_begin();
  Function::arg_iterator formal_end = F->arg_end();
  Function::arg_iterator last_formal = NULL;

  // TODO: Ignore non-vararg functions if number of formals 
  // doesn’t match with number of arguments of the call-site 
  // to improve accuracy of points-to sets.   


  if (CS.getType()->isPointerTy()) {
    if (isa<PointerType>(F->getFunctionType()->getReturnType())) {
      AddEdgeInGraph(getNode(CS.getInstruction()), getReturnNode(F));
    }
    else {
      AddEdgeInGraph(getNode(CS.getInstruction()), UniversalSet);
    }
  }

  // CQ377744: Stop trying to map arguments and formals if 
  // arg_itr or formal_itr reached an end.
  for (; formal_itr != formal_end && arg_itr != arg_end;) {
    Argument* formal = formal_itr;
    Value* actual = *arg_itr;
    if (formal->getType()->isPointerTy()) {
      if (actual->getType()->isPointerTy()) {
        AddEdgeInGraph(getNode(formal_itr), getNode(actual));
      }
      else {
        AddEdgeInGraph(getNode(formal_itr), UniversalSet);
      }
    }
    last_formal = formal_itr;
    ++formal_itr;
    ++arg_itr;
  }

  if (F->getFunctionType()->isVarArg()) {
    for (; arg_itr != arg_end && last_formal != formal_end; ++arg_itr) {
      Value* actual = *arg_itr;
      if (actual->getType()->isPointerTy()) {
        AddEdgeInGraph(getNode(last_formal), getNode(actual));
      }
    }
  }
}

// Process Indirect call during propagation of points-to sets.
//
void AndersensAAResult::ProcessIndirectCall(CallSite CS) {
  SparseBitVector<> PointsToDiff;
  Value* call_fptr = CS.getCalledValue();
  assert(call_fptr && "Expecting function fptr");
  const Node *N = &GraphNodes[FindNode(getNode(call_fptr))];

  PointsToDiff.intersectWithComplement(N->PointsTo, N->OldPointsTo);
  if (PointsToDiff.empty()) {
    return;
  }

  for (SparseBitVector<>::iterator bi = PointsToDiff.begin(),
       EP = PointsToDiff.end();
       bi != EP; ++bi) {
    Node* N1 = &GraphNodes[*bi];

    // Not sure why we have NullPtr?
    // TODO: Check it and fix basic issue
    if (N1 == &GraphNodes[NullObject] || N1 == &GraphNodes[NullPtr]) {
      continue;
    }
    if (N1 == &GraphNodes[UniversalSet]) {
      InitIndirectCallActualsToUniversalSet(CS);
      continue;
    }
    
    Value *V = N1->getValue();
    if (Function *F = dyn_cast<Function>(V)) {
      if (F->getFunctionType()->isVarArg() || F->arg_size() == CS.arg_size()) {
        IndirectCallActualsToFormals(CS, F);
      }
    }
    // Don't do anything for now if it pointsto non-function object
    //Rep = UniteNodes(Rep,Node);
    //NextWL->insert(&GraphNodes[Rep]);
  }
}

// Process all indirect calls during propagation of points-to sets.
//
void AndersensAAResult::ProcessIndirectCalls() {

  std::vector<CallSite>::const_iterator calllist_itr;

  for (unsigned i = 0, e = IndirectCallList.size(); i != e; ++i) {
    ProcessIndirectCall(IndirectCallList[i]);
  }
}

/// SolveConstraints - This stage iteratively processes the constraints list
/// propagating constraints (adding edges to the Nodes in the points-to graph)
/// until a fixed point is reached.
///
/// We use a variant of the technique called "Lazy Cycle Detection", which is
/// described in "The Ant and the Grasshopper: Fast and Accurate Pointer
/// Analysis for Millions of Lines of Code. In Programming Language Design and
/// Implementation (PLDI), June 2007."
/// The paper describes performing cycle detection one node at a time, which can
/// be expensive if there are no cycles, but there are long chains of nodes that
/// it heuristically believes are cycles (because it will DFS from each node
/// without state from previous nodes).
/// Instead, we use the heuristic to build a worklist of nodes to check, then
/// cycle detect them all at the same time to do this more cheaply.  This
/// catches cycles slightly later than the original technique did, but does it
/// make significantly cheaper.

void AndersensAAResult::SolveConstraints() {
  CurrWL = &w1;
  NextWL = &w2;

  OptimizeConstraints();
#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa-constraints"
      DEBUG(PrintConstraints());
#undef DEBUG_TYPE
#define DEBUG_TYPE "anders-aa"


  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    Node *N = &GraphNodes[i];
    N->PointsTo = new SparseBitVector<>;
    N->OldPointsTo = new SparseBitVector<>;
    N->Edges = new SparseBitVector<>;
  }
  CreateConstraintGraph();
  UnitePointerEquivalences();
  assert(SCCStack.empty() && "SCC Stack should be empty by now!");
  Node2DFS.clear();
  Node2Deleted.clear();
  Node2DFS.insert(Node2DFS.begin(), GraphNodes.size(), 0);
  Node2Deleted.insert(Node2Deleted.begin(), GraphNodes.size(), false);
  DFSNumber = 0;
  DenseSet<Constraint, ConstraintKeyInfo> Seen;
  DenseSet<std::pair<unsigned,unsigned>, PairKeyInfo> EdgesChecked;

  // Order graph and add initial nodes to work list.
  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    Node *INode = &GraphNodes[i];

    // Add to work list if it's a representative and can contribute to the
    // calculation right now.
    if (INode->isRep() && !INode->PointsTo->empty()
        && (!INode->Edges->empty() || !INode->Constraints.empty())) {
      INode->Stamp();
      CurrWL->insert(INode);
    }
  }
  std::queue<unsigned int> TarjanWL;
#if !FULL_UNIVERSAL
  // "Rep and special variables" - in order for HCD to maintain conservative
  // results when !FULL_UNIVERSAL, we need to treat the special variables in
  // the same way that the !FULL_UNIVERSAL tweak does throughout the rest of
  // the analysis - it's ok to add edges from the special nodes, but never
  // *to* the special nodes.
  std::vector<unsigned int> RSV;
#endif
  while( !CurrWL->empty() ) {

    Node* CurrNode;
    unsigned CurrNodeIndex;

    // Actual cycle checking code.  We cycle check all of the lazy cycle
    // candidates from the last iteration in one go.
    if (!TarjanWL.empty()) {
      DFSNumber = 0;
      
      Tarjan2DFS.clear();
      Tarjan2Deleted.clear();
      while (!TarjanWL.empty()) {
        unsigned int ToTarjan = TarjanWL.front();
        TarjanWL.pop();
        if (!Tarjan2Deleted[ToTarjan]
            && GraphNodes[ToTarjan].isRep()
            && Tarjan2DFS[ToTarjan] == 0)
          QueryNode(ToTarjan);
      }
    }
    
    // Add to work list if it's a representative and can contribute to the
    // calculation right now.
    while( (CurrNode = CurrWL->pop()) != NULL ) {
      CurrNodeIndex = CurrNode - &GraphNodes[0];
      CurrNode->Stamp();
      
          
      // Figure out the changed points to bits
      SparseBitVector<> CurrPointsTo;
      CurrPointsTo.intersectWithComplement(CurrNode->PointsTo,
                                           CurrNode->OldPointsTo);
      if (CurrPointsTo.empty())
        continue;

      *(CurrNode->OldPointsTo) |= CurrPointsTo;

      // Check the offline-computed equivalencies from HCD.
      bool SCC = false;
      unsigned Rep;

      if (SDT[CurrNodeIndex] >= 0) {
        SCC = true;
        Rep = FindNode(SDT[CurrNodeIndex]);

#if !FULL_UNIVERSAL
        RSV.clear();
#endif
        for (SparseBitVector<>::iterator bi = CurrPointsTo.begin();
             bi != CurrPointsTo.end(); ++bi) {
          unsigned Node = FindNode(*bi);
#if !FULL_UNIVERSAL
          if (Node < NumberSpecialNodes) {
            RSV.push_back(Node);
            continue;
          }
#endif
          Rep = UniteNodes(Rep,Node);
        }
#if !FULL_UNIVERSAL
        RSV.push_back(Rep);
#endif

        NextWL->insert(&GraphNodes[Rep]);

        if ( ! CurrNode->isRep() )
          continue;
      }

      Seen.clear();

      /* Now process the constraints for this node.  */
      for (std::list<Constraint>::iterator li = CurrNode->Constraints.begin();
           li != CurrNode->Constraints.end(); ) {
        li->Src = FindNode(li->Src);
        li->Dest = FindNode(li->Dest);

        // Delete redundant constraints
        if( Seen.count(*li) ) {
          std::list<Constraint>::iterator lk = li; li++;

          CurrNode->Constraints.erase(lk);
          ++NumErased;
          continue;
        }
        Seen.insert(*li);

        // Src and Dest will be the vars we are going to process.
        // This may look a bit ugly, but what it does is allow us to process
        // both store and load constraints with the same code.
        // Load constraints say that every member of our RHS solution has K
        // added to it, and that variable gets an edge to LHS. We also union
        // RHS+K's solution into the LHS solution.
        // Store constraints say that every member of our LHS solution has K
        // added to it, and that variable gets an edge from RHS. We also union
        // RHS's solution into the LHS+K solution.
        unsigned *Src;
        unsigned *Dest;
        unsigned K = li->Offset;
        unsigned CurrMember;
        if (li->Type == Constraint::Load) {
          Src = &CurrMember;
          Dest = &li->Dest;
        } else if (li->Type == Constraint::Store) {
          Src = &li->Src;
          Dest = &CurrMember;
        } else {
          // TODO Handle offseted copy constraint
          li++;
          continue;
        }

        // See if we can use Hybrid Cycle Detection (that is, check
        // if it was a statically detected offline equivalence that
        // involves pointers; if so, remove the redundant constraints).
        if( SCC && K == 0 ) {
#if FULL_UNIVERSAL
          CurrMember = Rep;

          if (GraphNodes[*Src].Edges->test_and_set(*Dest))
            if (GraphNodes[*Dest].PointsTo |= *(GraphNodes[*Src].PointsTo))
              NextWL->insert(&GraphNodes[*Dest]);
#else
          for (unsigned i=0; i < RSV.size(); ++i) {
            CurrMember = RSV[i];

            if (*Dest < NumberSpecialNodes)
              continue;
            if (GraphNodes[*Src].Edges->test_and_set(*Dest))
              if (GraphNodes[*Dest].PointsTo |= *(GraphNodes[*Src].PointsTo))
                NextWL->insert(&GraphNodes[*Dest]);
          }
#endif
          // since all future elements of the points-to set will be
          // equivalent to the current ones, the complex constraints
          // become redundant.
          //
          std::list<Constraint>::iterator lk = li; li++;
#if !FULL_UNIVERSAL
          // In this case, we can still erase the constraints when the
          // elements of the points-to sets are referenced by *Dest,
          // but not when they are referenced by *Src (i.e. for a Load
          // constraint). This is because if another special variable is
          // put into the points-to set later, we still need to add the
          // new edge from that special variable.
          if( lk->Type != Constraint::Load)
#endif
          GraphNodes[CurrNodeIndex].Constraints.erase(lk);
        } else {
          const SparseBitVector<> &Solution = CurrPointsTo;

          for (SparseBitVector<>::iterator bi = Solution.begin();
               bi != Solution.end();
               ++bi) {
            CurrMember = *bi;

            // Need to increment the member by K since that is where we are
            // supposed to copy to/from.  Note that in positive weight cycles,
            // which occur in address taking of fields, K can go past
            // MaxK[CurrMember] elements, even though that is all it could point
            // to.
            if (K > 0 && K > MaxK[CurrMember])
              continue;
            else
              CurrMember = FindNode(CurrMember + K);

            // Add an edge to the graph, so we can just do regular
            // bitmap ior next time.  It may also let us notice a cycle.
#if !FULL_UNIVERSAL
            if (*Dest < NumberSpecialNodes)
              continue;
#endif
            if (GraphNodes[*Src].Edges->test_and_set(*Dest))
              if (GraphNodes[*Dest].PointsTo |= *(GraphNodes[*Src].PointsTo))
                NextWL->insert(&GraphNodes[*Dest]);

          }
          li++;
        }
      }
      SparseBitVector<> NewEdges;
      SparseBitVector<> ToErase;

      // Now all we have left to do is propagate points-to info along the
      // edges, erasing the redundant edges.
      for (SparseBitVector<>::iterator bi = CurrNode->Edges->begin();
           bi != CurrNode->Edges->end();
           ++bi) {

        unsigned DestVar = *bi;
        unsigned Rep = FindNode(DestVar);

        // If we ended up with this node as our destination, or we've already
        // got an edge for the representative, delete the current edge.
        if (Rep == CurrNodeIndex ||
            (Rep != DestVar && NewEdges.test(Rep))) {
            ToErase.set(DestVar);
            continue;
        }
        
        std::pair<unsigned,unsigned> edge(CurrNodeIndex,Rep);
        
        // This is where we do lazy cycle detection.
        // If this is a cycle candidate (equal points-to sets and this
        // particular edge has not been cycle-checked previously), add to the
        // list to check for cycles on the next iteration.
        if (!EdgesChecked.count(edge) &&
            *(GraphNodes[Rep].PointsTo) == *(CurrNode->PointsTo)) {
          EdgesChecked.insert(edge);
          TarjanWL.push(Rep);
        }
        // Union the points-to sets into the dest
#if !FULL_UNIVERSAL
        if (Rep >= NumberSpecialNodes)
#endif
        if (GraphNodes[Rep].PointsTo |= CurrPointsTo) {
          NextWL->insert(&GraphNodes[Rep]);
        }
        // If this edge's destination was collapsed, rewrite the edge.
        if (Rep != DestVar) {
          ToErase.set(DestVar);
          NewEdges.set(Rep);
        }
      }
      CurrNode->Edges->intersectWithComplement(ToErase);
      CurrNode->Edges |= NewEdges;
    }

    // Process Indirect calls here for now. 
    // TODO: Need to find correct placement for this call later.
    ProcessIndirectCalls();

    // Switch to other work list.
    WorkList* t = CurrWL; CurrWL = NextWL; NextWL = t;
  }


  Node2DFS.clear();
  Node2Deleted.clear();
  for (unsigned i = 0; i < GraphNodes.size(); ++i) {
    Node *N = &GraphNodes[i];
    delete N->OldPointsTo;
    delete N->Edges;
  }
  SDTActive = false;
  SDT.clear();
}

//===----------------------------------------------------------------------===//
//                               Union-Find
//===----------------------------------------------------------------------===//

// Unite nodes First and Second, returning the one which is now the
// representative node.  First and Second are indexes into GraphNodes
unsigned AndersensAAResult::UniteNodes(unsigned First, unsigned Second,
                               bool UnionByRank) {
  assert (First < GraphNodes.size() && Second < GraphNodes.size() &&
          "Attempting to merge nodes that don't exist");

  Node *FirstNode = &GraphNodes[First];
  Node *SecondNode = &GraphNodes[Second];

  assert (SecondNode->isRep() && FirstNode->isRep() &&
          "Trying to unite two non-representative nodes!");
  if (First == Second)
    return First;

  if (UnionByRank) {
    int RankFirst  = (int) FirstNode ->NodeRep;
    int RankSecond = (int) SecondNode->NodeRep;

    // Rank starts at -1 and gets decremented as it increases.
    // Translation: higher rank, lower NodeRep value, which is always negative.
    if (RankFirst > RankSecond) {
      unsigned t = First; First = Second; Second = t;
      Node* tp = FirstNode; FirstNode = SecondNode; SecondNode = tp;
    } else if (RankFirst == RankSecond) {
      FirstNode->NodeRep = (unsigned) (RankFirst - 1);
    }
  }

  SecondNode->NodeRep = First;
#if !FULL_UNIVERSAL
  if (First >= NumberSpecialNodes)
#endif
  if (FirstNode->PointsTo && SecondNode->PointsTo)
    FirstNode->PointsTo |= *(SecondNode->PointsTo);
  if (FirstNode->Edges && SecondNode->Edges)
    FirstNode->Edges |= *(SecondNode->Edges);
  if (!SecondNode->Constraints.empty())
    FirstNode->Constraints.splice(FirstNode->Constraints.begin(),
                                  SecondNode->Constraints);
  if (FirstNode->OldPointsTo) {
    delete FirstNode->OldPointsTo;
    FirstNode->OldPointsTo = new SparseBitVector<>;
  }

  // Destroy interesting parts of the merged-from node.
  delete SecondNode->OldPointsTo;
  delete SecondNode->Edges;
  delete SecondNode->PointsTo;
  SecondNode->Edges = NULL;
  SecondNode->PointsTo = NULL;
  SecondNode->OldPointsTo = NULL;

  NumUnified++;
  //errs() << "Unified Node ";
  //DEBUG(PrintNode(FirstNode));
  //errs() << " and Node ";
  //DEBUG(PrintNode(SecondNode));
  //errs() << "\n";

  if (SDTActive)
    if (SDT[Second] >= 0) {
      if (SDT[First] < 0)
        SDT[First] = SDT[Second];
      else {
        UniteNodes( FindNode(SDT[First]), FindNode(SDT[Second]) );
        First = FindNode(First);
      }
    }

  return First;
}

// Find the index into GraphNodes of the node representing Node, performing
// path compression along the way
unsigned AndersensAAResult::FindNode(unsigned NodeIndex) {
 
  Node *N;

  if (NodeIndex < GraphNodes.size()) {
      N = &GraphNodes[NodeIndex];
  }
  else {
      N = &GraphNodes[UniversalSet];
  }
  //assert (NodeIndex < GraphNodes.size()
  //        && "Attempting to find a node that can't exist");
  if (N->isRep())
    return NodeIndex;
  else
    return (N->NodeRep = FindNode(N->NodeRep));
}

// Find the index into GraphNodes of the node representing Node, 
// don't perform path compression along the way (for Print)
unsigned AndersensAAResult::FindNode(unsigned NodeIndex) const {
  const Node *N;
  if (NodeIndex < GraphNodes.size()) {
      N = &GraphNodes[NodeIndex];
  }
  else {
      N = &GraphNodes[UniversalSet];
  }
  //assert (NodeIndex < GraphNodes.size()
  //        && "Attempting to find a node that can't exist");
  if (N->isRep())
    return NodeIndex;
  else
    return FindNode(N->NodeRep);
}

// Get the points-to set for mod-ref computation.
unsigned AndersensAAResult::getPointsToSet(const Value *V, std::vector<llvm::Value*>& PtVec)
{
    Node *N1;

    unsigned Result = 0;
    unsigned int NodeNum = FindNode(getNode(const_cast<Value*>(V)));

    N1 = &GraphNodes[NodeNum];
    if (N1 == nullptr) {
        return PointsToBottom;
    }
    for (SparseBitVector<>::iterator bi = N1->PointsTo->begin();
        bi != N1->PointsTo->end();
        ++bi) {
        Node *N = &GraphNodes[*bi];
        if (N == &GraphNodes[UniversalSet]) {
            Result |= PointsToNonLocalLoc;
            continue;
        }
        else if (N == &GraphNodes[NullPtr]) {
            PtVec.clear();
            return PointsToBottom;
        }
        else if (N == &GraphNodes[NullObject]) {
            // NULL object just means pointer was assigned to NULL.
            continue;
        }
        if (!N->getValue()) {
            PtVec.clear();
            return PointsToBottom;
        }

        Value *V = N->getValue();
        PtVec.push_back(V);
        Result |= PointsToValue;
    }

    return Result;
}

void AndersensAAResult::printValueNode(const Value *V)
{
    Node *N = &GraphNodes[FindNode(getNode(const_cast<Value*>(V)))];
    PrintNode(N);

}


//===----------------------------------------------------------------------===//
//                               Debugging Output
//===----------------------------------------------------------------------===//

void AndersensAAResult::PrintNode(const Node *N) const {
  if (N == &GraphNodes[UniversalSet]) {
    errs() << "<universal>";
    return;
  } else if (N == &GraphNodes[NullPtr]) {
    errs() << "<nullptr>";
    return;
  } else if (N == &GraphNodes[NullObject]) {
    errs() << "<null>";
    return;
  }
  if (!N->getValue()) {
    errs() << "artificial" << (intptr_t) N;
    return;
  }

  assert(N->getValue() != 0 && "Never set node label!");
  Value *V = N->getValue();
  if (Function *F = dyn_cast<Function>(V)) {
    if (isa<PointerType>(F->getFunctionType()->getReturnType()) &&
        N == &GraphNodes[getReturnNode(F)]) {
      errs() << F->getName() << ":retval";
      return;
    } else if (F->getFunctionType()->isVarArg() &&
               N == &GraphNodes[getVarargNode(F)]) {
      errs() << F->getName() << ":vararg";
      return;
    } else {
      errs() << "Function:" << F->getName();
      return;
    }
  }

  if (Instruction *I = dyn_cast<Instruction>(V))
    errs() << I->getParent()->getParent()->getName() << ":";
  else if (Argument *Arg = dyn_cast<Argument>(V))
    errs() << Arg->getParent()->getName() << ":";

  if (V->hasName())
    errs() << V->getName();
  else
    errs() << "(unnamed:" << V <<")";

  if (isa<GlobalValue>(V) || isa<AllocaInst>(V))
    if (N == &GraphNodes[getObject(V)])
      errs() << "<mem>";
}
void AndersensAAResult::PrintConstraint(const Constraint &C) const {
  if (C.Type == Constraint::Store) {
    errs() << "*";
    if (C.Offset != 0)
      errs() << "(";
  }
  PrintNode(&GraphNodes[C.Dest]);
  if (C.Type == Constraint::Store && C.Offset != 0)
    errs() << " + " << C.Offset << ")";
  errs() << " = ";
  if (C.Type == Constraint::Load) {
    errs() << "*";
    if (C.Offset != 0)
      errs() << "(";
  }
  else if (C.Type == Constraint::AddressOf)
    errs() << "&";
  PrintNode(&GraphNodes[C.Src]);
  if (C.Offset != 0 && C.Type != Constraint::Store)
    errs() << " + " << C.Offset;
  if (C.Type == Constraint::Load && C.Offset != 0)
    errs() << ")";
  errs() << "\n";
}

void AndersensAAResult::PrintConstraints() const {
  errs() << "Constraints:\n";

  for (unsigned i = 0, e = Constraints.size(); i != e; ++i)
    PrintConstraint(Constraints[i]);
}

void AndersensAAResult::PrintPointsToGraph() const {
  errs() << "Points-to graph:" << GraphNodes.size() << "\n";
  for (unsigned i = 0, e = GraphNodes.size(); i != e; ++i) {
    const Node *N = &GraphNodes[i];
    if (FindNode(i) != i) {
      PrintNode(N);
      errs() << "\t--> same as ";
      PrintNode(&GraphNodes[FindNode(i)]);
      errs() << "\n";
    } else if (N->PointsTo) {
      errs() << "[" << (N->PointsTo->count()) << "] ";
      PrintNode(N);
      errs() << "\t--> ";

      bool first = true;
      for (SparseBitVector<>::iterator bi = N->PointsTo->begin();
           bi != N->PointsTo->end();
           ++bi) {
        if (!first)
          errs() << ", ";
        PrintNode(&GraphNodes[*bi]);
        first = false;
      }
      errs() << "\n";
    }
    else {
      errs() << "error: \n";
    }
  }
}

//===----------------------------------------------------------------------===//
//                               IntelModRef module
//===----------------------------------------------------------------------===//
#undef DEBUG_TYPE

// Debug_types for Intel ModRef
//
// imr 
//   - general trace, and summary of sets collected
//
// imr-ir
//   - dump the IR for the function during collection
//
// imr-collect 
//   - print the results of collection
//
// imr-collect-trace
//   - trace the collection set building
//
// imr-collect-exp
//   - trace of the expansion of the direct modref sets based on points-to
//     information
//
// imr-propagate
//   - trace the scc processing for propagating information between functions
//
// imr-propagate-all
//   - trace the before/after sets at each step of the propagation
//
// imr-query
//   - trace the queries and results for mod/ref information

/// IntelModRefImpl - This class implements mod/ref set tracking based on
/// the points-to sets collected for each pointer used in a routine.
/// 
/// For each function, mod/ref sets are collected based on each pointer
/// access. Then using points-to, the sets are expanded to include all
/// potential aliases. After all routines have been collected, a
/// propagation will merge information for all routines called.
namespace llvm {
  class IntelModRefImpl {
  private:

    // Internal structure used for mapping Values to a ModRefResult
    // enumeration id, to record one of 'Mod', 'Ref', or 'ModRef'.
    struct ModRefMap {

      // The Value-enum mapping. Uses a 'unsigned' for the
      // actaul value to support bit-logical operations.
      MapVector<Value*, unsigned> Map;

      // Update the map to include V as a Modified value.
      // Return true, if this causes a change to the map.
      bool addMod(const Value *V) {
        return addModRef(V, MRI_Mod);
      }

      // Update the map to include V as a Referenced value.
      // Return true, if this causes a change to the map.
      bool addRef(const Value *V) {
        return addModRef(V, MRI_Ref);
      }

      // Update the map to include V as a based on the mask value.
      // Return true, if this causes a change to the map.
      bool addModRef(const Value *V, unsigned mask = MRI_ModRef) {
        auto &Info = Map[const_cast<Value*>(V)];
        auto Prev = Info;
        Info |= mask;
        return Prev != Info;
      }

      // Prune the list of elements that have NoModRef as their value.
      void removeNoMod() {
        MapVector<Value*, unsigned> Tmp;
        for (auto I = Map.begin(), E = Map.end(); I != E; ++I) {
          if (I->second != MRI_NoModRef) {
            Tmp.insert(*I);
          }
        }
        std::swap(Tmp, Map);
      }

      // Print the elements of the map that have the 'mask' bits set.
      void printMR(raw_ostream &O, unsigned mask) const {
        O << "  {\n";
        for (auto I = Map.begin(), E = Map.end();
          I != E; ++I) {
          if (I->second & mask) {
            O << *I->first << "\n";
          }
        }
        O << "  }\n";
      }
    };

    // Information about a single function.
    struct FunctionRecord {
      // The function this record represents.
      // Used only for debug dumps.
      Function *F;

      // Information for why a set was set to bottom.
      // Used only for debug dumps.
      typedef enum {
        NotBottom, NotCollected, ExternalCall, IndirectCall,
        UnknownPointsTo, Propagated, Other, LastBottomReason
      } BottomReasonsEnum;

      // Get a string that represents the bottom reason.
      const char *getBottomReasonStr(BottomReasonsEnum Reason) const {
        static const char *Str[LastBottomReason] = {
          "", "NotCollected", "ExternalCall", "IndirectCall",
          "UnknownPointsTo", "Propagated", "Other"
        };

        assert(Reason >= NotBottom && Reason < LastBottomReason);
        return Str[Reason];
      }

      BottomReasonsEnum ModBottomReason;
      BottomReasonsEnum RefBottomReason;

      /// FunctionEffect - Capture whether or not this function reads 
      /// or writes to known/unknown memory.
      enum FunctionEffectMask {
        DoesNotAccessMemory = 0x00,  // Equiv to LOC_SET_EMPTY
        ReadsMemory = 0x01,
        WritesMemory = 0x02,
        ReadsNonLocalLoc = 0x04,     // Equiv to Ref(NonLocalLoc)
        WritesNonLocalLoc = 0x08,    // Equiv to Mod(NonLocalLoc)
        ReadsUnknownMemory = 0x10,   // Equiv to Ref(BOTTOM)
        WritesUnknownMemory = 0x20   // Equiv to Mod(BOTTOM)
      };

      // Return whether the function effect is known to read any memory
      bool EffectReadsMemory(FunctionEffectMask E) const {
        return E &
          (ReadsMemory | ReadsNonLocalLoc | ReadsUnknownMemory);
      }

      // Return whether the function effect is known to write any memory
      bool EffectWritesMemory(FunctionEffectMask E) const {
        return E &
          (WritesMemory | WritesNonLocalLoc | WritesUnknownMemory);
      }

      FunctionEffectMask getFunctionEffect() const {
        return FunctionEffect;
      }

      // Update the function effect. If the effect after modification is
      // bottom, then clear the other bits, and just leave it as bottom.
      void addFunctionEffect(FunctionEffectMask E) {
        FunctionEffect = FunctionEffectMask(FunctionEffect | E);

        if (isRefBottom()) {
          FunctionEffect =
            FunctionEffectMask(FunctionEffect & ~ReadsNonLocalLoc);
        }

        if (isModBottom()) {
          FunctionEffect =
            FunctionEffectMask(FunctionEffect & ~WritesNonLocalLoc);
        }
      }
    public:
      FunctionRecord() : F(nullptr),
        ModBottomReason(NotBottom), RefBottomReason(NotBottom),
        FunctionEffect(DoesNotAccessMemory) {}

      ~FunctionRecord() {}

      // Checks if the function is marked as reading memory
      bool FunctionReadsMemory() const {
        return EffectReadsMemory(getFunctionEffect());
      }

      // Checks if the function is marked as writing memory
      bool FunctionWritesMemory() const {
        return EffectWritesMemory(getFunctionEffect());
      }

      // Add the Value to the list of modified locations for this function.
      // Return true if a change in the sets occurs.
      bool addMod(const Value *V) {
        if (isModBottom()) {
          // No reason to add it if the function is bottom.
          return false;
        }
        bool changed = AndersenModRefInfo.addMod(V);
        addFunctionEffect(WritesMemory);
        return changed;
      }

      // Add the Value to the list of ref'd locations for this function.
      // Return true if a change in the sets occurs.
      bool addRef(const Value *V) {
        if (isRefBottom()) {
          // No reason to add it if the function is bottom.
          return false;
        }

        bool changed = AndersenModRefInfo.addRef(V);
        addFunctionEffect(ReadsMemory);
        return changed;
      }

      bool addModRef(const Value *V, unsigned mask) {
        if (isModBottom()) {
          mask &= ~MRI_Mod;
        }
        if (isRefBottom()) {
          mask &= ~MRI_Ref;
        }

        if (mask == 0) {
          return false;
        }

        bool changed = AndersenModRefInfo.addModRef(V, mask);

        FunctionEffectMask Effect = FunctionEffectMask(
          (mask & MRI_Ref ?
        ReadsMemory : DoesNotAccessMemory) |
                      (mask & MRI_Mod ?
                    WritesMemory : DoesNotAccessMemory));
        addFunctionEffect(Effect);
        return changed;
      }

      void removeValue(const Value *V) {
        auto I = AndersenModRefInfo.Map.find(const_cast<Value*>(V));
        if (I != AndersenModRefInfo.Map.end()) {
          AndersenModRefInfo.Map.erase(I);
        }
      }

      void addModNonLocalLoc() {
        if (!isModBottom()) {
          addFunctionEffect(WritesNonLocalLoc);
        }
      }

      bool isModNonLocalLoc() const {
        return getFunctionEffect() & WritesNonLocalLoc;
      }

      void addRefNonLocalLoc() {
        if (!isRefBottom()) {
          addFunctionEffect(ReadsNonLocalLoc);
        }
      }

      bool isRefNonLocalLoc() const {
        return getFunctionEffect() & ReadsNonLocalLoc;
      }

      void setToBottom(BottomReasonsEnum Reason) {
        addFunctionEffect(
          FunctionEffectMask(ReadsUnknownMemory | WritesUnknownMemory));
        ModBottomReason = Reason;
        RefBottomReason = Reason;
        AndersenModRefInfo.Map.clear();
      }

      void setModBottom(BottomReasonsEnum Reason) {
        addFunctionEffect(WritesUnknownMemory);
        ModBottomReason = Reason;

        if (isRefBottom()) {
          AndersenModRefInfo.Map.clear();
        }
        else {
          for (auto I = AndersenModRefInfo.Map.begin(),
            E = AndersenModRefInfo.Map.end(); I != E; ++I) {
            I->second &= ~MRI_Mod;
          }
        }
      }

      void setRefBottom(BottomReasonsEnum Reason) {
        addFunctionEffect(ReadsUnknownMemory);
        RefBottomReason = Reason;
        if (isModBottom()) {
          AndersenModRefInfo.Map.clear();
        }
        else {
          for (auto I = AndersenModRefInfo.Map.begin(),
            E = AndersenModRefInfo.Map.end(); I != E; ++I) {
            I->second &= ~MRI_Ref;
          }
        }
      }

      // Check if the mod set is BOTTOM
      bool isModBottom() const {
        return getFunctionEffect() & WritesUnknownMemory;
      }

      // Check if the ref set is BOTTOM
      bool isRefBottom() const {
        return getFunctionEffect() & ReadsUnknownMemory;
      }

      // Check if the Value is already known to be Modified
      bool mustModify(const Value *V) const {
        auto I = AndersenModRefInfo.Map.find(const_cast<Value*>(V));
        if (I == AndersenModRefInfo.Map.end()) {
          return false;
        }
        unsigned MR_Mask = I->second;
        return MR_Mask & MRI_Mod;
      }

      // Check if the Value is already known to be Referenced
      bool mustReference(const Value *V) const {
        auto I = AndersenModRefInfo.Map.find(const_cast<Value*>(V));
        if (I == AndersenModRefInfo.Map.end()) {
          return false;
        }
        unsigned MR_Mask = I->second;
        return MR_Mask & MRI_Ref;
      }

      // Check if the function either Modifies or References the Value.
      bool haveInfo(const Value *V) const {
        return !(AndersenModRefInfo.Map.find(const_cast<Value*>(V)) ==
          AndersenModRefInfo.Map.end());
      }

      // Get the ModRef information for the specified variable. If no
      // information is available, return 'ModRef'
      ModRefInfo getInfo(const Value *V) const {
        auto I = AndersenModRefInfo.Map.find(const_cast<Value*>(V));
        if (I == AndersenModRefInfo.Map.end()) {
          return MRI_ModRef;
        }

        return ModRefInfo(I->second);
      }

      // Print the Mod/Ref sets for the function.
      void printFuncMR(raw_ostream &O, const StringRef &Name,
        bool Summary = false) const {
        O << "PMOD(" << Name << ")";
        if (isModBottom()) {
          O << " --> BOTTOM: " <<
            getBottomReasonStr(ModBottomReason);
        }
        if (isModNonLocalLoc()) {
          O << "  + Non_local_loc";
        }
        O << "\n";

        if (!Summary) {
          AndersenModRefInfo.printMR(O, MRI_Mod);
        }


        O << "PREF(" << Name << ")";
        if (isRefBottom()) {
          O << " --> BOTTOM: " <<
            getBottomReasonStr(RefBottomReason);
        }
        if (isRefNonLocalLoc()) {
          O << "  + Non_local_loc";
        }

        O << "\n";
        if (!Summary) {
          AndersenModRefInfo.printMR(O, MRI_Ref);
        }
      }

      void dump() const {
        printFuncMR(llvm::errs(), F->getName(), false);
      }

    public:
      // Global effect of the function with regard to reading/writing memory
      FunctionEffectMask FunctionEffect;

      // Map of Value that are mod/ref'd by this function
      ModRefMap AndersenModRefInfo;
    };

    struct DeletionCallbackHandle final : CallbackVH {
      IntelModRefImpl &IMR;

      DeletionCallbackHandle(IntelModRefImpl &IMR, Value *V) :
        CallbackVH(V), IMR(IMR) {}

      void deleted() override {
        Value *V = getValPtr();
        // Clear the value handle, so that the object can be destroyed.
        setValPtr(nullptr);
        IMR.valueDeleted(V);
      }
    };
  public:
    // Constructor. Save a pointer to the Andersens object for invoking
    // the specific methods for points-to only available from Andersens.
    // Also save the pointer as the base class for invoking the base class
    // methods.
    IntelModRefImpl(AndersensAAResult *Ander) : Ander(Ander) {}

    // Destructor. No memory is allocated with 'new', so nothing to delete.
    ~IntelModRefImpl() {};

    // Examine all the functions in the module to build and propagate the
    // modref sets.
    bool runOnModule(Module &M);

    // Return the ModRef state for the Location.
    ModRefInfo getModRefInfo(ImmutableCallSite CS,
      const MemoryLocation &Loc);

    // Print all the ModRef sets.
    void dump() const;

    // Print all the ModRef sets to the ostream. If Summary is set, just
    // report Bottom or NonLocalLoc, instead of the complete sets.
    void print(raw_ostream &O, bool Summary = false) const;

  private:

    // Handle to Andersesns Alias Analysis object that will provide
    // points-to info.
    AndersensAAResult *Ander;

    // Pointer for DataLayout for GetUnderlyingObject calls 
    const DataLayout *DL;

    // Mapping between Functions and the ModRef sets for them.
    typedef MapVector<Function*, FunctionRecord> FunctionRecordMap;
    FunctionRecordMap FunctionInfo;

    std::set<DeletionCallbackHandle> Handles;

    // Get a pointer to the FunctionRecord for the specified function, or
    // nullptr if no information is available.
    FunctionRecord *getFunctionInfo(const Function *F) {
      auto I = FunctionInfo.find(const_cast<Function*>(F));
      if (I != FunctionInfo.end())
        return &I->second;
      return nullptr;
    }

    // Build the mod-ref set for the function
    void collectFunction(Function *F);

    // Check if the function contains something that will cause the ModRef
    // sets to be BOTTOM
    FunctionRecord::BottomReasonsEnum isResolvable(Function *F) const;

    // Check if a call to specific function can be resolved with ModRef info.
    bool isResolvableCallee(const Function *F) const;

    // Expand the ModRef sets to include aliases.
    void expandModRefSets(FunctionRecord *FR, ModRefMap *DirectModRef);

    // Prune modref sets to just be the set we want to track
    void pruneModRefSets(FunctionRecord *FR);

    // Propagate the sets around the call graph.
    void propagate(Module &M);

    // Build the SCC for propagation

    std::unique_ptr<llvm::CallGraph> buildPropagationSCC(Module &M);

    // Combine the ModRef sets of function so they are equivalent to one
    // another because they are in the same component of the SCC.
    bool fuseModRefSets(FunctionRecord *FR1, FunctionRecord *FR2);

    // Add the elements from 'Src' to Dest's ModRef Set.
    bool mergeModRefSets(FunctionRecord *Dest, const FunctionRecord *Src);

    // Add Non-Local-Loc to the sets that may access symbols the escape
    // the compilation unit.
    void applyNonLocalLocClosure();
    void applyNonLocalLocClosure(FunctionRecord *FR);

    void registerHandlers();
    void valueDeleted(Value *V);

    // Check whether Value V is a Global that could escape the compilation
    // unit.
    bool isGlobalEscape(const Value *V) const;

  };
} // namespace llvm


bool IntelModRefImpl::runOnModule(Module &M)
{
    DL = &M.getDataLayout();

    DEBUG_WITH_TYPE("imr", errs() << "Beginning IntelModRefImpl\n");
    DEBUG_WITH_TYPE("imr", errs() << "---------------------\n");

    for (Function &F : M) {
        collectFunction(&F);
    }

    DEBUG_WITH_TYPE("imr", errs() << "Before propagate\n");
    DEBUG_WITH_TYPE("imr", errs() << "----------------\n");
    DEBUG_WITH_TYPE("imr", dump());
    DEBUG_WITH_TYPE("imr", errs() << "----------------\n");


    propagate(M);
    registerHandlers();

    DEBUG_WITH_TYPE("imr", errs() << "After propagate\n");
    DEBUG_WITH_TYPE("imr", errs() << "----------------\n");
    DEBUG_WITH_TYPE("imr", dump());
    DEBUG_WITH_TYPE("imr", errs() << "----------------\n");

    return false;
}

// Helper routine to determine if the Value is a pointer that needs to be
// considered during modref collection.
static inline bool isInterestingPointer(Value *V) {
    return V->getType()->isPointerTy()
        && !isa<ConstantPointerNull>(V);
}

// Collect pointers (and points-to aliases) for each pointer directly
// modified or referenced in the routine.
void IntelModRefImpl::collectFunction(Function *F)
{
    // Only run collection on the body of a function.
    if (F->isDeclaration()) {
        return;
    }

    DEBUG_WITH_TYPE("imr-ir", F->dump());

    DEBUG_WITH_TYPE("imr-collect",
        errs() << "Collecting for: " << F->getName() << "\n");

    FunctionRecord &FR = FunctionInfo[F];
    FR.F = F;

    // Check if the function has characteristics that will prevent
    // knowing mod/ref sets, so that we can give up now if the result is
    // going to be bottom anyway.
    FunctionRecord::BottomReasonsEnum Reason = isResolvable(F);
    if (Reason != FunctionRecord::NotBottom) {
        DEBUG_WITH_TYPE("imr-collect", errs() <<
            "Unable to determine ModRef sets for function: " <<
            F->getName() << "\n");

        FR.setToBottom(Reason);
        return;
    }

    ModRefMap DirectModRef;
    for (inst_iterator I = inst_begin(F), E = inst_end(F); I != E; ++I) {
        if (LoadInst *LI = dyn_cast<LoadInst>(&*(I))) {
            Value *ValOperand = LI->getPointerOperand();
            bool Changed = DirectModRef.addRef(ValOperand);
            if (Changed) {
                DEBUG_WITH_TYPE("imr-collect-trace", errs() << (*I) << "\n");
                DEBUG_WITH_TYPE("imr-collect-trace", errs() <<
                                "REF: " << *ValOperand << "\n\n");
            }
        }
        else if (StoreInst *SI = dyn_cast<StoreInst>(&(*I))) {
            Value *PtrOperand = SI->getPointerOperand();
            bool Changed = DirectModRef.addMod(PtrOperand);
            if (Changed) {
                DEBUG_WITH_TYPE("imr-collect-trace", errs() << (*I) << "\n");
                DEBUG_WITH_TYPE("imr-collect-trace", errs() <<
                                "MOD: " << *PtrOperand << "\n\n");
            }

            // Consider the rest of the operands as Loads.
            Value *ValOperand = SI->getValueOperand();
            if (isInterestingPointer(ValOperand)) {
                bool Changed = DirectModRef.addRef(ValOperand);
                if (Changed) {
                    DEBUG_WITH_TYPE("imr-collect-trace", errs() << (*I) << "\n");
                    DEBUG_WITH_TYPE("imr-collect-trace", errs() <<
                                    "REF: " << *ValOperand << "\n\n");
                }
            }
            continue;
        }
        else if (BitCastInst *BC = dyn_cast<BitCastInst>(&(*I))) {
            Value *ValOperand = BC->getOperand(0);
            bool Changed = DirectModRef.addRef(ValOperand);
            if (Changed) {
                DEBUG_WITH_TYPE("imr-collect-trace", errs() << (*I) << "\n");
                DEBUG_WITH_TYPE("imr-collect-trace", errs() <<
                                "MODREF: " << *ValOperand << "\n\n");
            }
        }
        else if (AtomicCmpXchgInst *ACX = dyn_cast<AtomicCmpXchgInst>(&(*I))) {
            Value *ValOperand = ACX->getPointerOperand();
            bool Changed = DirectModRef.addModRef(ValOperand);
            if (Changed) {
                DEBUG_WITH_TYPE("imr-collect-trace", errs() << (*I) << "\n");
                DEBUG_WITH_TYPE("imr-collect-trace", errs() <<
                                "MODREF: " << *ValOperand << "\n\n");
            }
        }
        else if (AtomicRMWInst *AWMW = dyn_cast<AtomicRMWInst>(&(*I))) {
            Value *ValOperand = AWMW->getPointerOperand();
            bool Changed = DirectModRef.addMod(ValOperand);
            if (Changed) {
                DEBUG_WITH_TYPE("imr-collect-trace", errs() << (*I) << "\n");
                DEBUG_WITH_TYPE("imr-collect-trace", errs() <<
                                "MOD: " << *ValOperand << "\n\n");
            }
        }
        else if (isInterestingPointer(&(*I))) {
            Value *ValPtr = &(*I);
            bool Changed = DirectModRef.addMod(ValPtr);
            if (Changed) {
                DEBUG_WITH_TYPE("imr-collect-trace", errs() << (*I) << "\n");
                DEBUG_WITH_TYPE("imr-collect-trace", errs() <<
                                "MOD: " << *ValPtr << "\n\n");
            }
        }

        if (CallSite CS = CallSite(&(*I))) {
            // Collect all the values passed
            int ArgNo = 0;
            for (CallSite::arg_iterator AI = CS.arg_begin(), AE = CS.arg_end();
                AI != AE; ++AI, ++ArgNo) {
                if (isInterestingPointer(*AI)) {
                    bool Changed = DirectModRef.addRef(*AI);
                    if (Changed) {
                        DEBUG_WITH_TYPE("imr-collect-trace", errs() << (*I) << "\n");
                        DEBUG_WITH_TYPE("imr-collect-trace", errs() <<
                                        "REF: " << *(*AI) << "\n\n");
                    }
                }
            }
        }
    }

    DEBUG_WITH_TYPE("imr-collect", errs() << "DirectMod:\n");
    DEBUG_WITH_TYPE("imr-collect", DirectModRef.printMR(errs(), MRI_Mod));
    DEBUG_WITH_TYPE("imr-collect", errs() << "DirectRef:\n");
    DEBUG_WITH_TYPE("imr-collect", DirectModRef.printMR(errs(), MRI_Ref));

    // Collect all the aliases of the directly modified Values.
    expandModRefSets(&FR, &DirectModRef);

    // Prune modref sets to just be the set we want to track
    pruneModRefSets(&FR);
}

// Check if there is something about the routine that will cause ModRef
// sets to always be bottom.
IntelModRefImpl::FunctionRecord::BottomReasonsEnum IntelModRefImpl::isResolvable(
    Function *F
) const
{
    // Check if all call-sites can be resolved.
    for (inst_iterator I = inst_begin(*F), E = inst_end(*F); I != E; ++I) {
        if (CallSite CS = CallSite(&(*I))) {
            const Value *V = CS.getCalledValue();
            if (isa<InlineAsm>(*V)) {
                DEBUG_WITH_TYPE("imr-collect", 
                    errs() << F->getName() << ": has inline-asm\n");
                return FunctionRecord::Other;
            }

            if (const Function *Callee = CS.getCalledFunction()) {
                if (!isResolvableCallee(Callee)) {
                    DEBUG_WITH_TYPE("imr-collect", 
                        errs() << F->getName() << ": has unknown call " <<
                        (Callee ? Callee->getName() : "<null>") << "\n");
                    return FunctionRecord::ExternalCall;
                }
            }
            else {
                // Indirect call. go conservative for now.
                // TODO: check if all callsites known.
                DEBUG_WITH_TYPE("imr-collect", errs() << F->getName() <<
                    ": has Indirect call: " << *V << "\n");
                return FunctionRecord::IndirectCall;
            }
        }
    }

    return FunctionRecord::NotBottom;
}

// Check if a call to specific function can be resolved with ModRef info.
bool IntelModRefImpl::isResolvableCallee(const Function *F) const
{
    if (!F) {
        return false;
    }

    // if we have the body of the function, we will resolve it during
    // propagation, so treat the call as resolvable.
    if (F->isDeclaration() == false) {
        return true;
    }

    // If the function does not touch memory, then any calls to it do not
    // matter.
    if (Ander->getModRefBehavior(F) == FMRB_DoesNotAccessMemory) {
        return true;
    }

    // treat some llvm intrinsics as not-modifying memory.
    switch (F->getIntrinsicID()) {
    default: break;
    case Intrinsic::lifetime_start:
    case Intrinsic::lifetime_end:
        return true;
    }

    // We will not be able to resolve any information about the function call.
    return false;
}

// Extend the Mod/Ref sets based on the points-to information for the items
// in the DirectModRef set.
void IntelModRefImpl::expandModRefSets(
    FunctionRecord *FR,
    ModRefMap *DirectModRef
)
{
    std::vector<llvm::Value*> PtVec;

    for (auto I = DirectModRef->Map.begin(), E = DirectModRef->Map.end();
        I != E; ++I) {
        PtVec.clear();
        DEBUG_WITH_TYPE("imr-collect-exp",
            errs() << "Processing aliases for: ");
        DEBUG_WITH_TYPE("imr-collect-exp", Ander->printValueNode(I->first));
        DEBUG_WITH_TYPE("imr-collect-exp", errs() << "\n");

        const Value *V = I->first;
        unsigned PtsToResult = Ander->getPointsToSet(V, PtVec);
        if (PtsToResult == AndersensAAResult::PointsToBottom) {
            DEBUG_WITH_TYPE("imr-collect-exp", errs() << FR->F->getName() <<
                ": No Pts to set for: " << *(I->first) << "\n");
            FR->setToBottom(FunctionRecord::UnknownPointsTo);
            return;
        }

        if (PtsToResult & AndersensAAResult::PointsToNonLocalLoc) {
            DEBUG_WITH_TYPE("imr-collect-exp", errs() << FR->F->getName() <<
                ": Getting Non-local-loc due to " << *(I->first) << "\n");
            if (I->second & MRI_Mod) {
                FR->addModNonLocalLoc();
            }
            if (I->second & MRI_Ref) {
                FR->addRefNonLocalLoc();
            }
        }

        for (auto I2 = PtVec.begin(), E2 = PtVec.end(); I2 != E2; ++I2) {
            if (I->second & MRI_Mod) {
                if (!FR->mustModify(*I2)) {
                    DEBUG_WITH_TYPE("imr-collect-exp",
                        errs() << "  : add mod ");
                    DEBUG_WITH_TYPE("imr-collect-exp",
                        Ander->printValueNode(*I2));
                    DEBUG_WITH_TYPE("imr-collect-exp", errs() << "\n");
                }
                FR->addMod(*I2);
            }
            if (I->second & MRI_Ref) {
                if (!FR->mustReference(*I2)) {
                    DEBUG_WITH_TYPE("imr-collect-exp",
                        errs() << "  : add ref ");
                    DEBUG_WITH_TYPE("imr-collect-exp",
                        Ander->printValueNode(*I2));
                    DEBUG_WITH_TYPE("imr-collect-exp", errs() << "\n");
                }

                FR->addRef(*I2);
            }
        }
    }
}

// Prune the modref sets.
// In this version, we are going to limit the sets to GlobalVars, and let
// the on-demand testing of the other AAs handle everything else.
void IntelModRefImpl::pruneModRefSets(FunctionRecord *FR)
{
  for (auto I = FR->AndersenModRefInfo.Map.begin(), E = FR->AndersenModRefInfo.Map.end();
    I != E; ++I) {
    if (!isa<GlobalValue>(I->first)) {
      // Set the other items are NoModRef so that the removeNoMod call can
      // eliminate them all at once.
      I->second = MRI_NoModRef;
    }
  }

  FR->AndersenModRefInfo.removeNoMod();
}

// Propagate the Mod/Ref sets around the call graph.
void IntelModRefImpl::propagate(Module &M)
{
    auto G = buildPropagationSCC(M);
    unsigned sccNum = 0;

    for (auto I = scc_begin(G.get()); !I.isAtEnd(); ++I) {
        const std::vector<CallGraphNode *> &SCC = *I;
        assert(!SCC.empty() && "SCC with no functions?");

        DEBUG_WITH_TYPE("imr-propagate",
            errs() << "\nSCC #" << ++sccNum << " : ");

        Function *F = SCC[0]->getFunction();
        DEBUG_WITH_TYPE("imr-propagate", errs() <<
            (F ? F->getName() : "external node") << ", ");

        if (!F) {
            continue;
        }

        DEBUG_WITH_TYPE("imr-propagate", errs() << "Propagate for " <<
            F->getName() << "\n");

        FunctionRecord *FR = getFunctionInfo(F);
        if (FR) {
            // Merge in the information about all the callees to this
            // routine's function record.
            for (unsigned i = 0, e = SCC.size(); i != e; ++i) {
                for (CallGraphNode::iterator CI = SCC[i]->begin(),
                    E = SCC[i]->end(); CI != E; ++CI) {

                    if (Function *Callee = CI->second->getFunction()) {
                        FunctionRecord *CalleeFR = getFunctionInfo(Callee);
                        if (CalleeFR) {
                            mergeModRefSets(FR, CalleeFR);
                        }
                    }
                    else {
                        // We should have already gone BOTTOM for unresolved
                        // indirect calls.
                        assert(FR->isModBottom());
                    }
                }
            }

            // Combine all the elements of the SCC element together so they
            // are all the same.
            bool changed = true;
            while (changed) {
                changed = false;
                FunctionRecord *PrevFR = FR;
                auto I = SCC.begin();

                ++I;
                for (auto E = SCC.end(); I != E; ++I) {
                    Function* CurF = (*I)->getFunction();
                    FunctionRecord *CurFR = getFunctionInfo(CurF);
                    if (FR) {
                        changed = fuseModRefSets(PrevFR, CurFR);
                        PrevFR = CurFR;
                    }
                    else {
                        if (!(PrevFR->isModBottom() || PrevFR->isRefBottom()))
                        {
                            PrevFR->setToBottom(FunctionRecord::Propagated);
                            changed = true;
                        }
                    }
                }
            }
        }
    }

    applyNonLocalLocClosure();
}

// Create callbacks for all tracked values so that the sets can be updated
// if a function or variable is deleted from the program.
void IntelModRefImpl::registerHandlers()
{
  std::set<Value*> Tracked;
  for (auto I = FunctionInfo.begin(), E = FunctionInfo.end(); I != E; ++I) {
    Tracked.insert(I->first);

    for (auto SI = I->second.AndersenModRefInfo.Map.begin(),
      SE = I->second.AndersenModRefInfo.Map.end(); SI != SE; ++SI) {
      Tracked.insert(SI->first);
    }
  }

  for (auto I = Tracked.begin(), E = Tracked.end(); I != E; ++I) {
    Handles.insert(DeletionCallbackHandle(*this, (*I)));
  }
}

void IntelModRefImpl::valueDeleted(Value *V)
{
  if (auto *F = dyn_cast<Function>(V)) {
    FunctionInfo.erase(F);
  }

  if (GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
    // Remove the GlobalValue from all the ModRef sets.
    for (auto FI = FunctionInfo.begin(), FE = FunctionInfo.end();
      FI != FE; ++FI) {
      FunctionRecord *FR = &(FI->second);
      FR->removeValue(GV);
    }
  }
}

// Get a SCC graph for use in propagating Mod/Ref sets from callees to callers
std::unique_ptr<llvm::CallGraph> IntelModRefImpl::buildPropagationSCC(Module &M)
{
    std::unique_ptr<llvm::CallGraph> G;
    G.reset(new llvm::CallGraph(M));

    // TODO: Add arcs for indirect function calls.
    return G;
}



// Combine ModRef sets of FR1 and FR2, such that they are equivalent following
// this call.
bool IntelModRefImpl::fuseModRefSets(FunctionRecord *FR1, FunctionRecord *FR2)
{
    bool changed = false;
    changed |= mergeModRefSets(FR1, FR2);
    changed |= mergeModRefSets(FR2, FR1);
    return changed;
}

// Merge the contents of the Src ModRef set to the Dest ModRef set.
bool IntelModRefImpl::mergeModRefSets(FunctionRecord *Dest,
    const FunctionRecord *Src)
{
    bool changed = false;
    unsigned MergeMask = MRI_Mod | MRI_Ref;

    DEBUG_WITH_TYPE("imr-propagate-all", errs() << "Merge-2: " <<
        Src->F->getName() << " into " << Dest->F->getName() << "\n");
    DEBUG_WITH_TYPE("imr-propagate-all", errs() << "Before  merge:\n");
    DEBUG_WITH_TYPE("imr-propagate-all",
        Src->printFuncMR(errs(), Src->F->getName()));
    DEBUG_WITH_TYPE("imr-propagate-all", 
        Dest->printFuncMR(errs(), Dest->F->getName()));
    DEBUG_WITH_TYPE("imr-propagate-all", errs() << "=====================\n");

    if (Src->isModBottom()) {
        if (!Dest->isModBottom()) {
            Dest->setModBottom(FunctionRecord::Propagated);
            changed = true;
        }

        MergeMask &= ~MRI_Mod;
    }

    if (Src->isRefBottom()) {
        if (!Dest->isRefBottom()) {
            Dest->setRefBottom(FunctionRecord::Propagated);
            changed = true;
        }

        MergeMask &= ~MRI_Ref;
    }

    if (MergeMask == 0) {
        // Both Mod and Ref have gone bottom, no need to merge individual
        // elements. 

        DEBUG_WITH_TYPE("imr-propagate-all", errs() << "After merge:\n");
        DEBUG_WITH_TYPE("imr-propagate-all",
            Dest->printFuncMR(errs(), Dest->F->getName()));
        DEBUG_WITH_TYPE("imr-propagate-all",
            errs() << "--------------------\n");

        return changed;
    }

    if (Src->isModNonLocalLoc()) {
        if (!Dest->isModNonLocalLoc()) {
            Dest->addModNonLocalLoc();
            changed = true;
        }
    }

    if (Src->isRefNonLocalLoc()) {
        if (!Dest->isRefNonLocalLoc()) {
            Dest->addRefNonLocalLoc();
            changed = true;
        }
    }

    for (auto I = Src->AndersenModRefInfo.Map.begin(), E = Src->AndersenModRefInfo.Map.end();
        I != E; ++I) {
        if (I->second & MergeMask) {
            changed |= Dest->addModRef(I->first, I->second & MergeMask);
        }
    }

    DEBUG_WITH_TYPE("imr-propagate-all", errs() << "After merge:\n");
    DEBUG_WITH_TYPE("imr-propagate-all",
        Dest->printFuncMR(errs(), Dest->F->getName()));
    DEBUG_WITH_TYPE("imr-propagate-all", errs() << "--------------------\n");

    return changed;
}

// Walk over all the mod/ref sets for all the functions, and add the
// non_local_loc set to anything that contains a global variable that could
// be accessed outside of the compilation scope.
void IntelModRefImpl::applyNonLocalLocClosure()
{
    for (auto I = FunctionInfo.begin(), E = FunctionInfo.end(); I != E; ++I) {
        applyNonLocalLocClosure(&(I->second));
    }
}

// Add non-local loc if the function accesses a global variable that may
// escape the compilation scope.
void IntelModRefImpl::applyNonLocalLocClosure(FunctionRecord *FR)
{
    bool ModContainsNLL = FR->isModNonLocalLoc();
    bool RefContainsNLL = FR->isRefNonLocalLoc();

    for (auto I = FR->AndersenModRefInfo.Map.begin(), 
      E = FR->AndersenModRefInfo.Map.end(); I != E; ++I) {
        if (ModContainsNLL && RefContainsNLL) {
            break;
        }

        if (isGlobalEscape(I->first)) {
            if (!ModContainsNLL && (I->second & MRI_Mod)) {
                FR->addModNonLocalLoc();
                ModContainsNLL = true;

                DEBUG_WITH_TYPE("imr-propagate", errs() << 
                  "Closure: Adding NonLocalLoc to MOD set of: " <<
                  FR->F->getName() << "\n");
            }
            if (!RefContainsNLL && (I->second & MRI_Ref)) {
                FR->addRefNonLocalLoc();
                RefContainsNLL = true;

                DEBUG_WITH_TYPE("imr-propagate", errs() << 
                  "Closure: Adding NonLocalLoc to REF set of: " <<
                  FR->F->getName() << "\n");
            }
        }
    }
}

// Check if the global variable may escape.
bool IntelModRefImpl::isGlobalEscape(const Value *V) const
{
    if (const GlobalValue *GV = dyn_cast<GlobalValue>(V)) {
        // If the symbol is visible outside the compilation unit, treat the
        // function as accesing a non-local-loc
        if (GV->hasExternalLinkage()) {
            return true;
        }
    }

    return false;
}

void IntelModRefImpl::dump() const
{
    print(errs());
}

void IntelModRefImpl::print(raw_ostream &O, bool Summary) const
{
    for (FunctionRecordMap::const_iterator I = FunctionInfo.begin(),
        E = FunctionInfo.end(); I != E; ++I) {
        (I->second).printFuncMR(O, I->first->getName(), Summary);
    }
}


// Check the ModRef sets to see if a specific call will Modify or Reference
// (or Both) the Location.
ModRefInfo IntelModRefImpl::getModRefInfo(ImmutableCallSite CS,
    const MemoryLocation &Loc)
{
    ModRefInfo Result = MRI_ModRef;
    const Value *Object = GetUnderlyingObject(Loc.Ptr, *DL);

    DEBUG_WITH_TYPE("imr-query", errs() << "IntelModRefImpl::getModRefInfo(" <<
        (CS.getCalledFunction() ? CS.getCalledFunction()->getName() :
        "<indirect>")
        << ", ");

    if (Object == nullptr) {
        DEBUG_WITH_TYPE("imr-query",
            errs() << "  Could not get underlying object\n");
        return MRI_ModRef;
    }

    DEBUG_WITH_TYPE("imr-query", errs() << *Object);
    DEBUG_WITH_TYPE("imr-query", errs() <<
      (isa<GlobalValue>(Object) ? "[global]" : ""));
    DEBUG_WITH_TYPE("imr-query", errs() << ")\n");

    const Function *F = CS.getCalledFunction();
    if (!F) {
        DEBUG_WITH_TYPE("imr-query", errs() << "  Indirect destination\n");
        return MRI_ModRef;
    }

    const FunctionRecord *FR = getFunctionInfo(F);
    if (!FR) {
        DEBUG_WITH_TYPE("imr-query", errs() << "  Unknown function\n");
        return MRI_ModRef;
    }

    if (FR->isModBottom() || FR->isRefBottom()) {
        DEBUG_WITH_TYPE("imr-query", errs() << "  Function is BOTTOM\n");
        return MRI_ModRef;
    }

    // Clear the bits to form a minimum status, if possible.
    if (!FR->FunctionReadsMemory()) {
        Result = ModRefInfo(Result & ~MRI_Ref);
    }
    if (!FR->FunctionWritesMemory()) {
        Result = ModRefInfo(Result & ~MRI_Mod);
    }
    
    if (!isa<GlobalValue>(Object)) {
      DEBUG_WITH_TYPE("imr-query", errs() <<
        "  Only handling GlobalValue objects in this version\n");
      return MRI_ModRef;
    }

    bool Known = FR->haveInfo(Object);
    if (Known) {
        // Return the computed value based on the points-to
        // propagation.
        ModRefInfo Result = FR->getInfo(Object);
        DEBUG_WITH_TYPE("imr-query",
            errs() << "  Result=" << getModRefResultStr(Result) << "\n");
        return Result;
    }

    // If the value is not in the list, and we know all the locations
    // accessible by the function. The Object must not be
    // accessed by the routine.
    if (!(FR->isModNonLocalLoc() || FR->isRefNonLocalLoc())) {
            DEBUG_WITH_TYPE("imr-query", errs() << "  Result=" << 
                getModRefResultStr(MRI_NoModRef) << "\n");
        return MRI_NoModRef;
    }

    if (const GlobalValue *GV = dyn_cast<GlobalValue>(Object)) {
        // The Global variable is not in the list of modified
        // or referenced locations, but the function can
        // read/write some unknown memory locations.
        // If we know the globals accessed from this function
        // or one if its call, and the Object  P is not one
        // of them, and the Object does not escape the
        // compilation module, then it will not be accessed
        // as a non_local_loc, so we can so NoModRef.
        if (GV->isDiscardableIfUnused()) {
            DEBUG_WITH_TYPE("imr-query", errs() << "  Result=" <<
                getModRefResultStr(MRI_NoModRef) << "\n");
            return MRI_NoModRef;
        }
    }        

    DEBUG_WITH_TYPE("imr-query",
        errs() << "  Result=" <<
        getModRefResultStr(Result) << "\n");
    return Result;
}

// Constructor of actual implementation object
AndersensAAResult::IntelModRef::IntelModRef(AndersensAAResult *AnderAA)
{
  Impl = new IntelModRefImpl(AnderAA);
}

// Destructor of actual implementation object
AndersensAAResult::IntelModRef::~IntelModRef()
{
  delete Impl;
}

// Interface method to run the mod/ref set collection
void AndersensAAResult::IntelModRef::runAnalysis(Module &M)
{
  assert(Impl);
  Impl->runOnModule(M);

}

// Interface to query for mod/ref information about a memory location.
ModRefInfo AndersensAAResult::IntelModRef::getModRefInfo(
  ImmutableCallSite CS, const MemoryLocation &Loc)
{
  if (!Impl) {
    return MRI_ModRef;
  }

  return Impl->getModRefInfo(CS, Loc);
}
