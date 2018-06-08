//===----------------- Intel_FunctionSplitting.cpp ----------------------===//
//
// Copyright (C) 2017-2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive property
// of Intel Corporation and may not be disclosed, examined or reproduced in
// whole or in part without explicit written authorization from the company.
//
//===--------------------------------------------------------------------===//
// Function splitting transformation:
//  - Extract cold basic blocks to new function to improve code locality.
//
// This file implements splitting cold regions of code (based on the PGO
// BlockFrequencyInfo) into one or more separate functions that will be placed
// in the text.unlikely section of the object file. This will allow more of
// the hot code to remain in pages that are in the ITLB.
//
// The basic steps are of the transformation are:
// 1. Collect the set of functions that the transformation can be applied on
//    into a worklist.
// 2. For each function in worklist, collect a list of blocks that will be
//    candidates to start splitting the code at.
// 3. Walk the DOM tree from top to bottom, checking blocks that were
//    identified in step 2 as being the start of a valid and worthy region
//    to split out. Collect these into a set.
// 4. For each region collected in step 3, split that code into a new
//    function.
//
//===--------------------------------------------------------------------===//

#include "llvm/Transforms/Instrumentation/Intel_FunctionSplitting.h"

#include "llvm/ADT/GraphTraits.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/Analysis/BlockFrequencyInfo.h"
#include "llvm/Analysis/PostDominators.h"
#include "llvm/Analysis/ProfileSummaryInfo.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/Dominators.h"
#include "llvm/IR/Instructions.h"
#include "llvm/IR/IntrinsicInst.h"
#include "llvm/ProfileData/ProfileCommon.h"
#include "llvm/Support/FileSystem.h"
#include "llvm/Support/GraphWriter.h"
#include "llvm/Transforms/Utils/BasicBlockUtils.h"
#include "llvm/Transforms/Utils/CodeExtractor.h"

using namespace llvm;

#define DEBUG_TYPE "functionsplitting"

// Command line option to control a minimum size for the number
// instructions needed in the cold region to be worth splitting
// into a new function.
static cl::opt<unsigned> FunctionSplittingMinSize(
  "function-splitting-min-size", cl::init(25), cl::Hidden,
  cl::desc("Minimum number of instructions in a splinter region to be "
    "considered worthy of function splitting"));

// Command line option to control how cold a region needs to be for
// consideration of splitting. This value represents a percentage of
// the block execution count relative to the execution count of the
// function's entry basic block. i.e. a value of 1 means that a block
// which executes less than 1% of the times the function is invoked
// will be considered a candidate for splitting.
static cl::opt<unsigned> FunctionSplittingColdThresholdPercentage(
  "function-splitting-cold-threshold-percentage", cl::init(1), cl::Hidden,
  cl::desc("Blocks with execution frequency below this percentage are "
    "considered as candidates for function splitting."));

// Command line option to control which functions are considered for splitting.
// When 'true', only functions that are in 'text.hot' are considered for
// splitting. When 'false', any function with an execution count will be
// considered.
static cl::opt<bool> FunctionSplittingOnlyHot(
  "function-splitting-only-hot", cl::init(true), cl::Hidden,
  cl::desc("Only apply function splitting for functions in text.hot"));

// Command line option that enables .dot graph files of the CFG to be emitted
// for each function that gets split, prior to the splitting. The graph
// will color code the blocks that were chosen to be split, and which
// were rejected.
static cl::opt<bool> FunctionSplittingEmitDebugGraphs(
  "function-splitting-emit-graphs", cl::init(false), cl::ReallyHidden,
  cl::desc("Emit graphs of function splitting for debugging"));

namespace {

// Data types

// Used for the set of basic blocks that will be considered to as candidates
// to start a region of code to split to a new function.
using CandidateBlocksT = SmallPtrSet<BasicBlock *, 16>;

// A region of blocks that are to be extracted from the function, and replaced
// with a call to a new function. The new function will take ownership of the
// blocks when the function is split.
using SplinterRegionT = SmallSetVector<BasicBlock *, 16>;


// Forward declarations
class FunctionSplitter;

static void collectColdBlocks(Function &F, BlockFrequencyInfo &BFI,
  CandidateBlocksT *ColdBlocks);

static void writeGraph(const Function *F, FunctionSplitter *FS);

// Print the names of the blocks contained in the splinter region or
// candidate region
template <typename ContainerT>
void printNames(raw_ostream &OS, const ContainerT &Container)
{
  unsigned Col = 0;
  OS << "Region Blocks:\n";
  for (auto BB : Container) {
    if (Col > 80) {
      OS << "\n";
    }
    OS << BB->getName() << "    ";
    Col += BB->getName().size() + 4;
  }
  OS << "\n";
}

raw_ostream &operator<<(raw_ostream &OS, const SplinterRegionT &Region)
{
  printNames(OS, Region);
  return OS;
}

// Helper class that implements the functionality for splitting a
// SplinterRegion out of the Function.
class RegionSplitter {
public:
  RegionSplitter(DominatorTree &DT, BlockFrequencyInfo &BFI)
    : DT(DT), BFI(BFI)
  {
  }

  // Split the function, and return a pointer to the newly created Function.
  Function *splitRegion(const SplinterRegionT &Region);

private:
  // Update the IR to prepare the PHINodes of function to receive values
  // from the split out code.
  bool prepareRegionForSplit(const SplinterRegionT &Region);

  // Do the actual split.
  Function *doSplit(const SplinterRegionT &Region);

  // Handles to the DominatorTree and BlockFrequencyInfo analysis
  // for the function are needed for calls to the CodeExtractor
  // class.
  DominatorTree &DT;
  BlockFrequencyInfo &BFI;
};

// Public interface routine that performs all the steps required
// to split the 'Region' into a new function. Returns a pointer
// to the newly created function.
//
Function *RegionSplitter::splitRegion(const SplinterRegionT &Region)
{
  prepareRegionForSplit(Region);
  Function *NewF = doSplit(Region);

  return NewF;
}

// Modifies the IR to overcome some limitations in the type of IR that
// is handled by the CodeExtractor class. Eventually, the CodeExtractor
// class may be updated to handle these cases, but for now handle them
// here.
//
bool RegionSplitter::prepareRegionForSplit(const SplinterRegionT &Region)
{
  // Some edges that exit the region need to be split so that each path that
  // exits the splinter region and returns to the original function will be
  // uniquely identifiable. This is necessary to handle the case where 2 or
  // more Value objects get defined within the region being split out, and
  // get referenced by the same PHI node.
  //
  // For example, if the original function contains the following IR:
  //
  // if.end11:                      ; preds = %while.cond, %if.else9, %if.then
  // %rs.0 = phi i32[365, %if.then], [%0, %if.else9], [%x.addr.0, %while.cond]
  //
  // If the 'if.else9' and 'while.cond' nodes are both within the splinter
  // region, following the extraction, they would both be defined by the block
  // containing call to the new function following the code extraction, such
  // as:
  //   %rs.0 = phi i32[365, %if.then], [%0, %splitR], [%x.addr.0, %splitR]
  // This would be invalid, because it would not clear which value should be
  // used.
  //
  // By splitting the necessary edges, the source values for the PHI nodes
  // stay will stay in blocks that are kept within the original function.
  // A return value of the call to the extracted function will be used to
  // determine edge should be executed following the return of the function
  // call.

  // Collect the set of edges which exit the splinter region, and execute
  // a PHINode instruction.
  SetVector<std::pair<BasicBlock*, BasicBlock*>> SplitEdges;

  for (BasicBlock *BB : Region.getArrayRef()) {
    for (auto SI = succ_begin(BB), SE = succ_end(BB); SI != SE; ++SI) {
      if (!Region.count(*SI)) {
        // Only need to check the first instruction, since any PHI nodes must
        // be at the start of the basic block.
        Instruction *I = &(*SI)->getInstList().front();
        if (isa<PHINode>(*I))
          SplitEdges.insert(std::make_pair(BB, *SI));
      }
    }
  }

  for (auto E : SplitEdges) {
    SplitEdge(E.first, E.second);
  }

  return true;
}

// Do the steps to extract the 'Region' to a new function.
// Returns the new function, if successful, otherwise nullptr.
Function *RegionSplitter::doSplit(const SplinterRegionT &Region)
{
  CodeExtractor Extractor(Region.getArrayRef(), &DT, &BFI);
  Function *NewF = Extractor.extractCodeRegion();
  if (NewF == nullptr) {
    return nullptr;
  }

  // Mark the function to be kept in a cold segment.
  NewF->setSectionPrefix(getUnlikelySectionPrefix());

  // Override any inlining directives, if present, and prevent the split out
  // routine from being inlined back to the original function.
  NewF->removeFnAttr(Attribute::AlwaysInline);
  NewF->removeFnAttr(Attribute::AlwaysInlineRecursive);
  NewF->removeFnAttr(Attribute::InlineHint);
  NewF->removeFnAttr(Attribute::InlineHintRecursive);
  NewF->addFnAttr(Attribute::NoInline);

  return NewF;
}

// The main class that implements the function splitting process. This
// class is responsible for analyzing the function to select regions that
// are to be split, and then uses the RegionSplitter helper class to
// perform the actual split.
class FunctionSplitter {
public:
  FunctionSplitter(
    Function &F,
    BlockFrequencyInfo &BFI,
    DominatorTree &DT,
    PostDominatorTree  &PDT,
    CandidateBlocksT &Candidates) :
    F(F), BFI(BFI), DT(DT), PDT(PDT), CandidateBlocks(Candidates)
  { }

  bool runOnFunction();

private:
  // For debug traces, once a region is identified, it is evaluated for
  // suitability of splitting, and given one of the following reasons.
  typedef enum {
    NotEvaluated,
    RegionIneligible,   // Code extractor does not support splitting
    RegionNotSESE,      // Region is not Single-Entry/Single-Exit
    RegionSmall,        // Region does not meet size limit
    RegionOk            // Region selected for splitting.
  } RegionDecisionT;

  // A decision, and a region size for debugging.
  using EvaluationT = std::pair<RegionDecisionT, unsigned>;

  bool isCandidateBlock(const BasicBlock *BB) const {
    return CandidateBlocks.count(BB) != 0;
  }

  void identifySplinterRegions();
  void populateCandidateRegion(const DomTreeNode *Node,
    SplinterRegionT &Region);

  EvaluationT evaluateCandidateRegion(SplinterRegionT &Region);
  bool isSingleEntrySingleExit(const SplinterRegionT &Region) const;
  unsigned estimateRegionSize(const SplinterRegionT &Region) const;

  void addRegionToSplitList(SplinterRegionT &Region);
  void tryPruneRejectedRegion(SplinterRegionT &Region, EvaluationT &Eval);

  bool splitRegions();
  void stripDebugInfoIntrinsics(Function &F);

  //===------------------------------------------------------------------===//
  // The following methods are just used to support the .dot graph drawing
  // routines to expose the internal state of the class to the traits classes
  // used there.
  //===------------------------------------------------------------------===//
  friend GraphTraits<FunctionSplitter*>;
  friend DOTGraphTraits<FunctionSplitter*>;
private:
  // Get a handle to the function. This is just used for the graphing helper
  // class to draw the CFG.
  Function *getFunction() const {
    return &F;
  }

  // If the basic block is part of a region to be split from the function,
  // return an index (1..N) to identify the region. Otherwise, return 0.
  unsigned getSplinterRegionNumber(const BasicBlock *BB) const {
    auto It = BlockToRegionMapping.find(BB);
    if (It == BlockToRegionMapping.end())
      return 0;

    return It->second;
  }

  RegionDecisionT getRegionDecision(const BasicBlock *BB) const {
    auto It = BlockToEvaluationMapping.find(BB);
    if (It == BlockToEvaluationMapping.end())
      return NotEvaluated;

    return It->second.first;
  }

  unsigned getRegionSize(const BasicBlock *BB) const {
    auto It = BlockToEvaluationMapping.find(BB);
    if (It == BlockToEvaluationMapping.end())
      return 0;

    return It->second.second;
  }

  //===------------------------------------------------------------------===//
  // Data members
  //===------------------------------------------------------------------===//

  // Handle to the original function that is being processed for splitting.
  Function &F;

  // Handles to the analysis structures needed to process the function.
  BlockFrequencyInfo &BFI;
  DominatorTree &DT;
  PostDominatorTree  &PDT;

  // List of blocks that may be used to start a region to be split out of the
  // function. Typically, this will be a set of blocks that have been
  // determined to be cold by some criteria. This class doesn't care about
  // the criteria, it just uses these blocks to look for sections of code
  // that are dominated by them.
  CandidateBlocksT CandidateBlocks;

  // The list of code regions to be split out of the function.
  SmallVector<SplinterRegionT, 4> RegionsToSplit;

  // The blocks that have been chosen for a split region. This is
  // used during the walking of the dominator tree to detect blocks
  // that have already been assigned to a region. We could just store
  // the set of blocks, but we keep a mapping to the region number
  // to support annotating the DOT graphs that can be emitted.
  DenseMap<const BasicBlock *, unsigned> BlockToRegionMapping;

  // Mapping of the basic block that were evaluated as starting a
  // a region to the evaluation result to support annotating the DOT
  // graphs.
  DenseMap<const BasicBlock *, EvaluationT> BlockToEvaluationMapping;
};

// Process the function for splitting.
bool FunctionSplitter::runOnFunction()
{
  identifySplinterRegions();

  if (FunctionSplittingEmitDebugGraphs)
    writeGraph(&F, this);

  if (RegionsToSplit.empty())
    return false;

  bool Changed = splitRegions();
  return Changed;
}

// Collect code regions that start from a block in the block
// candidate list, and for the ones that are valid and worth splitting
// put them into RegionsToSplit collection.
void FunctionSplitter::identifySplinterRegions()
{
  // Each region begins with a dominating node. Walk the DomTree from
  // top to bottom to identify the regions to be split.
  DomTreeNode *Root = DT.getRootNode();

  std::stack<DomTreeNode *> Worklist;
  Worklist.push(Root);

  while (!Worklist.empty()) {
    DomTreeNode *CurNode = Worklist.top();
    Worklist.pop();

    // Find the immediate post-dominator block of the current node.
    DomTreeNode *PDTCurNode = PDT.getNode(CurNode->getBlock());
    DomTreeNode *PDTCurIDom = PDTCurNode->getIDom();
    const BasicBlock *CurIdomBlock = PDTCurIDom->getBlock();

    // Check the immediately dominated nodes of the current block as
    // candidates to split out of the function.
    for (auto &Child : CurNode->getChildren()) {
      const BasicBlock *BB = Child->getBlock();
      if (BlockToRegionMapping.count(BB))
        continue;

      // Check the post-dominator info to be sure the child is not forming a
      // CFG triangle, such as the following:
      //      if
      //       |\
      //       | \
      //       |  if.then
      //       |  /
      //       | /
      //       if.end
      //
      // In this case, the block that starts with 'if.then' should be
      // tested as the start of a region to be extracted, but the block
      // that begins with 'if.end' should not.
      //
      if (isCandidateBlock(BB) && BB != CurIdomBlock) {
        // Populate a candidate region to be the dominance tree that begins
        // with the candidate node.
        //
        // Note, if there is a loop within the set of blocks, the execution
        // counts of the looping basic blocks could exceed the threshold
        // execution percentage that triggers the region selection. However,
        // since that loop is going to be rarely reached, we will still allow
        // the region to be split out.
        SplinterRegionT Candidate;
        populateCandidateRegion(Child, Candidate);
        EvaluationT Eval = evaluateCandidateRegion(Candidate);
        BlockToEvaluationMapping.insert(std::make_pair(BB, Eval));
        if (Eval.first == RegionOk) {
          addRegionToSplitList(Candidate);
        }
        else {
          tryPruneRejectedRegion(Candidate, Eval);
        }
      }

      Worklist.push(Child);
    }
  }
}

// Add the block, and all blocks dominated by it to the 'Region'
void FunctionSplitter::populateCandidateRegion(const DomTreeNode *Node,
  SplinterRegionT &Region)
{
  BasicBlock *BB = Node->getBlock();

  Region.insert(BB);
  for (auto &Child : Node->getChildren()) {
    populateCandidateRegion(Child, Region);
  }
}

// Check the 'Region' for validity and worthiness of splitting.
//
// The validity tests make sure that there is only a single point of entry
// to the region, and all exits lead to the same block. Also, makes sure
// the CodeExtractor module will accept the region.
//
// The worthiness tests check that the size of the region is large enough
// to be worth splitting.
//
FunctionSplitter::EvaluationT FunctionSplitter::evaluateCandidateRegion(
  SplinterRegionT &Region)
{
  if (!isSingleEntrySingleExit(Region)) {
    LLVM_DEBUG(dbgs() << "Region has paths into it besides entry block: "
                      << Region << "\n");
    return std::make_pair(RegionNotSESE, 0);
  }

  CodeExtractor Extractor(Region.getArrayRef(), &DT);
  if (!Extractor.isEligible()) {
    LLVM_DEBUG(dbgs() << "Ineligible region: " << Region << "\n");
    return std::make_pair(RegionIneligible, 0);
  }

  unsigned int RegionSize = estimateRegionSize(Region);
  if (RegionSize <= FunctionSplittingMinSize) {
    LLVM_DEBUG(dbgs() << "Region is too small: " << Region << "\n");
    return std::make_pair(RegionSmall, RegionSize);
  }

  // Check if the size of code extracted is large enough, and overcomes
  // the cost of making a function call.
  SetVector<Value *> Inputs, Outputs, Allocas;
  Extractor.findInputsOutputs(Inputs, Outputs, Allocas);
  unsigned int NumInputs = Inputs.size();
  unsigned int NumOutputs = Outputs.size();

  // Estimate one instruction per argument and one instruction for the call
  // statement. Check if the cost is "large" enough to justify the split.
  unsigned CallSize = 1 + NumInputs + NumOutputs;
  if (RegionSize < 2 * CallSize) {
    LLVM_DEBUG(dbgs() << "Region is too small: " << Region << "\n");
    return std::make_pair(RegionSmall, RegionSize);
  }

  LLVM_DEBUG(dbgs() << "Region ok for split: " << Region << "\n");
  return std::make_pair(RegionOk, RegionSize);
}

// Return 'true' if there is only a single entry basic block that enters the
// region, and all exits from the region go to the same basic block outside of
// the region (or all paths out of the region return from the function).
bool FunctionSplitter::isSingleEntrySingleExit(
  const SplinterRegionT &Region) const
{
  assert(!Region.empty());

  const BasicBlock *EntryBlock = Region.front();
  BasicBlock *RegionSuccessorBlock = nullptr;
  bool RegionExitsFunction = false;

  for (auto &BB : Region.getArrayRef()) {
    // Check for entry points into the region, other than the initial
    // block.
    if (BB != EntryBlock) {
      for (auto Pred : predecessors(BB)) {
        if (!Region.count(Pred)) {
          return false;
        }
      }
    }

    if (BB->getTerminator() && BB->getTerminator()->getNumSuccessors() == 0) {
      if (RegionSuccessorBlock != nullptr) {
        return false;
      }

      RegionExitsFunction = true;
    }

    // Check if after executing the region, control flow could go to more
    // than 1 block of the original function.
    for (auto Succ : successors(BB)) {
      if (!Region.count(Succ)) {
        if ((RegionSuccessorBlock && RegionSuccessorBlock != Succ) ||
          RegionExitsFunction) {
          return false;
        }

        RegionSuccessorBlock = Succ;
      }
    }
  }

  return true;
}

// Get an estimate for the size of the region. Currently, this is a summation
// of the number of instructions that will be generated for the blocks. In the
// future, this may be extended to model different IR instructions
// differently. Another option would be to use the TargetLibraryInfo cost to
// get a cost, but that is modeling execution cycles, and we care more about
// size for this.
unsigned FunctionSplitter::estimateRegionSize(
  const SplinterRegionT &Region) const
{
  unsigned Size = 0;
  for (auto &BB : Region.getArrayRef()) {
    Size += std::distance(BB->begin(), BB->end());
  }

  return Size;
}
void FunctionSplitter::addRegionToSplitList(SplinterRegionT &Region)
{
  unsigned int Num = RegionsToSplit.size() + 1;

  for (auto BB : Region.getArrayRef()) {
    BlockToRegionMapping.insert(std::make_pair(BB, Num));
  }

  RegionsToSplit.push_back(Region);
}

void FunctionSplitter::tryPruneRejectedRegion(SplinterRegionT &Region,
  EvaluationT &Eval)
{
  // If the region was rejected as being too small, then mark all basic
  // blocks of the region as having been visited. A region begins with a
  // dominant basic block and contains all blocks dominated by it. If
  // this region was too small, then any regions that can be started
  // by a block within it are also going to be too small.
  if (Eval.first == RegionSmall) {
    for (auto &BB : Region.getArrayRef()) {
      BlockToRegionMapping.insert(std::make_pair(BB, 0));
      BlockToEvaluationMapping.insert(
        std::make_pair(BB, std::make_pair(RegionSmall, 0)));
    }
  }

  // Note: There may be some pruning that can be done for regions that are
  // not single-entry/single-exit to avoid rebuilding entire subregions for
  // re-evaluating, but that is not currently implemented.
}


bool FunctionSplitter::splitRegions()
{
  bool Changed = false;
  // TODO: Currently, if there are "llvm.dbg.declare" statements in the
  // function, then splitting the function can result in these statements
  // being in the hot function for a variable that belongs to the cold
  // cold function or vice-versa following the split. For now, remove all
  // of these to avoid verification errors.
  stripDebugInfoIntrinsics(F);

  RegionSplitter Splitter(DT, BFI);

  for (auto &R : RegionsToSplit) {
    LLVM_DEBUG(dbgs() << F.getName() << ": Extracting " << R.size()
                      << " blocks\n");
    Function *ColdF = Splitter.splitRegion(R);
    if (ColdF) {
      Changed = true;
    }
    else {
      LLVM_DEBUG(dbgs() << "Function split of " << F.getName() << " @ "
                        << R.front()->getName() << " was unsuccessful\n");
    }
  }

  return Changed;
}

void FunctionSplitter::stripDebugInfoIntrinsics(Function &F)
{
  for (auto &BB : F) {
    for (BasicBlock::iterator BI = BB.begin(), BE = BB.end(); BI != BE;) {
      Instruction *Insn = &*BI++;
      if (DbgValueInst *DVI = dyn_cast<DbgValueInst>(Insn)) {
        DVI->eraseFromParent();
      }
      else if (DbgDeclareInst *DDI = dyn_cast<DbgDeclareInst>(Insn)) {
        DDI->eraseFromParent();
      }
    }
  }
}

// Populate the 'ColdBlocks' set with the blocks that should be considered
// as candidates for being split from the function.
static void collectColdBlocks(Function &F, BlockFrequencyInfo &BFI,
  CandidateBlocksT *ColdBlocks)
{
  // Consider the block cold, if the execution count is less than
  // some percentage of the entry block's frequency.
  BlockFrequency EntryFreq = BFI.getBlockFreq(&F.front());
  if (EntryFreq == 0) {
    return;
  }

  BlockFrequency ColdFreq = EntryFreq * BranchProbability(
    FunctionSplittingColdThresholdPercentage, 100);

  for (auto &BB : F)
    if (BFI.getBlockFreq(&BB) <= ColdFreq)
      ColdBlocks->insert(&BB);
}

} // namespace

  //===------------------------------------------------------------------===//
  // Helpers to generate DOT graphs that show the CFG annotated with the
  // FunctionSplitting state. Specifically, blocks that candidates for
  // function splitting have their text in a different color from
  //non-candidate blocks. Blocks that are chosen for splitting are filled
  // with a background color.

namespace llvm {

  // Iteration of the FunctionSplitter for graph drawing will iterate over
  // the BasicBlocks pointers that make up the function.
  //
  template<>
  struct GraphTraits<FunctionSplitter*> {
    using NodeRef = const BasicBlock *;
    using ChildIteratorType = succ_const_iterator;
    using nodes_iterator = pointer_iterator<Function::const_iterator>;

    static NodeRef getEntryNode(const FunctionSplitter *G) {
      return &G->getFunction()->front();
    }

    static ChildIteratorType child_begin(const NodeRef N) {
      return succ_begin(N);
    }

    static ChildIteratorType child_end(const NodeRef N) {
      return succ_end(N);
    }

    static nodes_iterator nodes_begin(const FunctionSplitter *G) {
      return nodes_iterator(G->getFunction()->begin());
    }

    static nodes_iterator nodes_end(const FunctionSplitter *G) {
      return nodes_iterator(G->getFunction()->end());
    }
  };

  // List of colors to use for shading the regions selected for extraction.
  // If there are more regions than colors, we will just cycle through the
  // colors again.
  static const char *Colors[] = {
    "LightSkyBlue", "DeepSkyBlue", "CornflowerBlue",
    "Aquamarine", "SteelBlue", "Cyan", "LightBlue",
    "LightSteelBlue"
  };

  template<>
  struct DOTGraphTraits<FunctionSplitter*> : public DefaultDOTGraphTraits {
    using GTraits = GraphTraits<FunctionSplitter*>;
    using NodeRef = typename GTraits::NodeRef;
    using EdgeIter = typename GTraits::ChildIteratorType;
    using NodeIter = typename GTraits::nodes_iterator;

    explicit DOTGraphTraits(bool isSimple = false)
      : DefaultDOTGraphTraits(isSimple)
    { }

    // For the BasicBlocks, highlight the graph as follows:
    //  - Blocks were candidates according the hotness criteria. -> Blue text
    //  - Blocks that were dominant nodes that were tested for validity and
    //    size, but were rejected. -> gray background
    //  - Blocks were chosen for extraction. -> Color background to show
    //    all blocks of the region in the same color.
    //
    std::string getNodeAttributes(const BasicBlock *N,
      FunctionSplitter* const &G) {
      std::string Result;
      raw_string_ostream OS(Result);
      bool NeedComma = false;

      if (G->isCandidateBlock(N)) {
        OS << "fontcolor=blue";
        NeedComma = true;
      }

      unsigned RegionNum = G->getSplinterRegionNumber(N);
      if (RegionNum != 0) {
        if (NeedComma)
          OS << ",";
        OS << "style=filled, fillcolor=" << Colors[(RegionNum - 1) %
          (sizeof(Colors) / sizeof(char*))];
      }
      else {
        FunctionSplitter::RegionDecisionT Res = G->getRegionDecision(N);
        if (Res != FunctionSplitter::NotEvaluated) {
          if (NeedComma)
            OS << ",";
          OS << "style=filled, fillcolor=gray";
        }
      }

      OS.flush();
      return Result;
    }

    // For the BasicBlocks, label the graph with the names of the blocks.
    // Additionally, annotate the blocks which were evaluated for extraction.
    //
    std::string getNodeLabel(const BasicBlock *N,
      FunctionSplitter* const &G) {
      std::string Result;
      raw_string_ostream OS(Result);

      FunctionSplitter::RegionDecisionT Res = G->getRegionDecision(N);
      switch (Res) {
      case FunctionSplitter::NotEvaluated:
        break;
      case FunctionSplitter::RegionOk:
        OS << "Size = " << G->getRegionSize(N) << "\\l\\l";
        break;
      case FunctionSplitter::RegionNotSESE:
        OS << "[Not SESE]\\l\\l";
        break;
      case FunctionSplitter::RegionIneligible:
        OS << "[Ineligible]\\l\\l";
        break;
      case FunctionSplitter::RegionSmall:
        OS << "[Too small. Size = " << G->getRegionSize(N) << "]\\l\\l";
        break;
      }

      OS << N->getName();

      OS.flush();
      return Result;

    }

    // Label the exits from the block.
    std::string getEdgeSourceLabel(const BasicBlock *Node,
      succ_const_iterator I) {
      // Label source of conditional branches with "T" or "F"
      if (const BranchInst *BI = dyn_cast<BranchInst>(Node->getTerminator()))
        if (BI->isConditional())
          return (I == succ_begin(Node)) ? "T" : "F";

      // Label source of switch edges with the associated value.
      if (const SwitchInst *SI = dyn_cast<SwitchInst>(Node->getTerminator())) {
        unsigned SuccNo = I.getSuccessorIndex();

        if (SuccNo == 0) return "def";

        std::string Str;
        raw_string_ostream OS(Str);
        auto Case = *SwitchInst::ConstCaseIt::fromSuccessorIndex(SI, SuccNo);
        OS << Case.getCaseValue()->getValue();
        return OS.str();
      }
      return "";
    }
  };

} // namespace llvm

namespace {
  // Generate a .dot file for the function with the CFG annotated with
  // information from the FunctionSplitter analysis.
  //
  static void writeGraph(const Function *F, FunctionSplitter *FS)
  {
    std::string Filename = ("func_split." + F->getName() + ".dot").str();
    errs() << "Writing '" << Filename << "'...";

    std::error_code EC;
    raw_fd_ostream File(Filename, EC, sys::fs::F_Text);

    if (!EC)
      WriteGraph(File, FS, false);
    else
      errs() << "  error opening file for writing!";
    errs() << "\n";
  }
} // namespace

//===--------------------------------------------------------------------===//

// Implementation of function splitting compiler pass that is run for either
// the old or new pass manager.
class FunctionSplittingImpl {
public:
  bool runOnModule(
    Module &M,
    ProfileSummaryInfo *PSI,
    std::function<BlockFrequencyInfo &(Function &)> *GetBFI,
    std::function<DominatorTree &(Function &)> *GetDT,
    std::function<PostDominatorTree &(Function &)> *GetPDT);

private:

  bool processFunction(Function &F,
    std::function<BlockFrequencyInfo &(Function &)> *GetBFI,
    std::function<DominatorTree &(Function &)> *GetDT,
    std::function<PostDominatorTree &(Function &)> *GetPDT);
};

bool FunctionSplittingImpl::runOnModule(Module &M,
  ProfileSummaryInfo *PSI,
  std::function<BlockFrequencyInfo &(Function &)> *GetBFI,
  std::function<DominatorTree &(Function &)> *GetDT,
  std::function<PostDominatorTree &(Function &)> *GetPDT)
{
  bool Changed = false;

  std::vector<Function *> Worklist;
  Worklist.reserve(M.size());
  for (Function &F : M)
    if (!F.isDeclaration() &&
       (!FunctionSplittingOnlyHot ||
        PSI->isFunctionHotInCallGraph(&F, (*GetBFI)(F)))) {
      Worklist.push_back(&F);
    }

  for (Function *F : Worklist)
    Changed |= processFunction(*F, GetBFI, GetDT, GetPDT);

  return Changed;
}

bool FunctionSplittingImpl::processFunction(
  Function &F,
  std::function<BlockFrequencyInfo &(Function &)> *GetBFI,
  std::function<DominatorTree &(Function &)> *GetDT,
  std::function<PostDominatorTree &(Function &)> *GetPDT)
{
  // Collect a list of blocks based on the PGO data that a candidates
  // to split out of the function.
  BlockFrequencyInfo &BFI = (*GetBFI)(F);

  CandidateBlocksT ColdBlocks;
  collectColdBlocks(F, BFI, &ColdBlocks);
  if (ColdBlocks.empty())
    return false;

  DominatorTree &DT = (*GetDT)(F);
  PostDominatorTree &PDT = (*GetPDT)(F);
  FunctionSplitter Splitter(F, BFI, DT, PDT, ColdBlocks);

  return Splitter.runOnFunction();
}

// New pass manager version
PreservedAnalyses FunctionSplittingPass::run(Module &M,
  ModuleAnalysisManager &AM)
{
  ProfileSummaryInfo *PSI = &AM.getResult<ProfileSummaryAnalysis>(M);
  auto &FAM = AM.getResult<FunctionAnalysisManagerModuleProxy>(M).getManager();

  std::function<BlockFrequencyInfo &(Function &)> GetBFI =
    [&FAM](Function &F) -> BlockFrequencyInfo & {
    return FAM.getResult<BlockFrequencyAnalysis>(F);
  };

  std::function<DominatorTree &(Function &)> GetDT =
    [&FAM](Function &F) -> DominatorTree & {
    return  FAM.getResult<DominatorTreeAnalysis>(F);
  };

  std::function<PostDominatorTree &(Function &)> GetPDT =
    [&FAM](Function &F) -> PostDominatorTree & {
    return  FAM.getResult<PostDominatorTreeAnalysis>(F);
  };

  FunctionSplittingImpl Impl;
  bool Changed = Impl.runOnModule(M, PSI, &GetBFI, &GetDT, &GetPDT);

  if (!Changed)
    return PreservedAnalyses::all();

  return PreservedAnalyses::none();
}

// Wrapper pass for old pass manager
class FunctionSplittingWrapper : public ModulePass {
private:
public:
  static char ID;
  FunctionSplittingWrapper() : ModulePass(ID) {
    initializeFunctionSplittingWrapperPass(*PassRegistry::getPassRegistry());
  };

  bool runOnModule(Module &M) override;

  void getAnalysisUsage(AnalysisUsage &AU) const override {
    AU.addRequired<BlockFrequencyInfoWrapperPass>();
    AU.addRequired<DominatorTreeWrapperPass>();
    AU.addRequired<PostDominatorTreeWrapperPass>();
    AU.addRequired<ProfileSummaryInfoWrapperPass>();
  }
};

bool FunctionSplittingWrapper::runOnModule(Module &M)
{
  ProfileSummaryInfo *PSI =
    getAnalysis<ProfileSummaryInfoWrapperPass>().getPSI();

  std::function<BlockFrequencyInfo &(Function &)> GetBFI =
    [this](Function &F) -> BlockFrequencyInfo & {
    return this->getAnalysis<BlockFrequencyInfoWrapperPass>(F).getBFI();
  };

  std::function<DominatorTree &(Function &)> GetDT =
    [this](Function &F) -> DominatorTree & {
    return this->getAnalysis<DominatorTreeWrapperPass>(F).getDomTree();
  };

  std::function<PostDominatorTree &(Function &)> GetPDT =
    [this](Function &F) -> PostDominatorTree & {
    return this->getAnalysis<PostDominatorTreeWrapperPass>(F).getPostDomTree();
  };

  FunctionSplittingImpl Impl;
  bool Changed = Impl.runOnModule(M, PSI, &GetBFI, &GetDT, &GetPDT);

  return Changed;
}

char FunctionSplittingWrapper::ID = 0;
INITIALIZE_PASS_BEGIN(FunctionSplittingWrapper, "function-splitting",
  "Split cold code regions out of functions",
  false, false)
INITIALIZE_PASS_DEPENDENCY(BlockFrequencyInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(ProfileSummaryInfoWrapperPass)
INITIALIZE_PASS_DEPENDENCY(DominatorTreeWrapperPass)
INITIALIZE_PASS_DEPENDENCY(PostDominatorTreeWrapperPass)
INITIALIZE_PASS_END(FunctionSplittingWrapper, "function-splitting",
    "Split cold code regions out of functions",
    false, false)

ModulePass *llvm::createFunctionSplittingWrapperPass() {
  return new FunctionSplittingWrapper();
}
