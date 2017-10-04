//===- FPGA-Advisor-Analysis.cpp
//---------------------------------------------------===//
//
// Copyright (c) 2016, Intel Corporation
// All rights reserved.
//
// Redistribution and use in source and binary forms, with or without
// modification, are permitted provided that the following conditions are met:
//
// Redistributions of source code must retain the above copyright notice, this
// list of conditions and the following disclaimer.
//
// Redistributions in binary form must reproduce the above copyright notice,
// this list of conditions and the following disclaimer in the documentation
// and/or other materials provided with the distribution.
//
// Neither the name of the Intel Corporation nor the names of its contributors
// may be used to endorse or promote products derived from this software
// without specific prior written permission.
//
// THIS SOFTWARE IS PROVIDED BY THE COPYRIGHT HOLDERS AND CONTRIBUTORS "AS IS"
// AND ANY EXPRESS OR IMPLIED WARRANTIES, INCLUDING, BUT NOT LIMITED TO, THE
// IMPLIED WARRANTIES OF MERCHANTABILITY AND FITNESS FOR A PARTICULAR PURPOSE
// ARE DISCLAIMED. IN NO EVENT SHALL THE COPYRIGHT OWNER OR CONTRIBUTORS BE
// LIABLE FOR ANY DIRECT, INDIRECT, INCIDENTAL, SPECIAL, EXEMPLARY, OR
// CONSEQUENTIAL DAMAGES (INCLUDING, BUT NOT LIMITED TO, PROCUREMENT OF
// SUBSTITUTE GOODS OR SERVICES; LOSS OF USE, DATA, OR PROFITS; OR BUSINESS
// INTERRUPTION) HOWEVER CAUSED AND ON ANY THEORY OF LIABILITY, WHETHER IN
// CONTRACT, STRICT LIABILITY, OR TORT (INCLUDING NEGLIGENCE OR OTHERWISE)
// ARISING IN ANY WAY OUT OF THE USE OF THIS SOFTWARE, EVEN IF ADVISED OF THE
// POSSIBILITY OF SUCH DAMAGE.
//
//===----------------------------------------------------------------------===//
//
// This file implements the FPGA-Advisor Analysis pass
// The analysis pass is intended to be run after the instrumentation pass. It
// also assumes that a program trace has been produced by the instrumented code.
// The analysis will reconstruct the trace from the file and perform analysis on
// the loops within the module.
//
// This file implements the FPGA-Advisor Analysis and pass
// This pass is used in the second stage of FPGA-Advisor tool and will provide
// both static compile time statistics as well as instrument the program
// which allows dynamic run time statistics. The list of statistics this pass
// will gather is listed below:
//
// Static statistics:
//	- # functions
//	- # basic blocks in each function
//	- # loops in each function
//	- # parallelizable loops in each function
//	- loop size (determined by)
//		- # instructions within loop
//		- # operations within loop
//
// Dynamic statistics:
// 	- # of times each basic block is run
//
// Beyond these statistics, the pass will also notify the user when a program
// is not expected to perform well on an FPGA as well as when it contains
// constructs which cannot be implemented on the FPGA.
//
// Describe the analysis details here ----- I am still not sure how it will be
// implemented !!!!!!!!
//
//===----------------------------------------------------------------------===//
//
// Author: chenyuti
//
//===----------------------------------------------------------------------===//

// FIXME Need to change the direction of the trace graph.... sighh

#include "AdvisorAnalysis.h"
#include "AdvisorCommon.h"
#include "DependenceGraph.h"
#include "ModuleAreaEstimator.h"
#include "ModuleScheduler.h"
#include "stack_trace.h"

#include <exception>
#include <fstream>
#include <fstream>
#include <map>
#include <queue>
#include <regex>
#include <thread>
#include <time.h>

// include tbb components
#include "tbb/task.h"
#include "tbb/task_group.h"
#include "tbb/task_scheduler_init.h"
#include "tbb/tick_count.h"

//#define DEBUG_TYPE "fpga-advisor-analysis"
#define DEBUG_TYPE "fpga-advisor"
#define BUFSIZE 1024

using namespace llvm;
using namespace fpga;
using std::ifstream;

//===----------------------------------------------------------------------===//
// Having some fun with colors
//===----------------------------------------------------------------------===//
#define RESET "\033[0m"
#define BLACK "\033[30m"              /* Black */
#define RED "\033[31m"                /* Red */
#define GREEN "\033[32m"              /* Green */
#define YELLOW "\033[33m"             /* Yellow */
#define BLUE "\033[34m"               /* Blue */
#define MAGENTA "\033[35m"            /* Magenta */
#define CYAN "\033[36m"               /* Cyan */
#define WHITE "\033[37m"              /* White */
#define BOLDBLACK "\033[1m\033[30m"   /* Bold Black */
#define BOLDRED "\033[1m\033[31m"     /* Bold Red */
#define BOLDGREEN "\033[1m\033[32m"   /* Bold Green */
#define BOLDYELLOW "\033[1m\033[33m"  /* Bold Yellow */
#define BOLDBLUE "\033[1m\033[34m"    /* Bold Blue */
#define BOLDMAGENTA "\033[1m\033[35m" /* Bold Magenta */
#define BOLDCYAN "\033[1m\033[36m"    /* Bold Cyan */
#define BOLDWHITE "\033[1m\033[37m"   /* Bold White */

//===----------------------------------------------------------------------===//
// Some globals ... is that bad? :/
// I can move this into the class and clean up every time a different function
// is being analyzed. The problem is that I have written a module pass.
//===----------------------------------------------------------------------===//

std::error_code AEC;
MemoryDependenceResults *MDA;
DominatorTree *DT;
DepGraph *functionDepGraph;
DepGraph *globalDepGraph;
std::unordered_map<BasicBlock *, DepGraph::vertex_descriptor> BlockMap;
// latency tables
std::map<BasicBlock *, LatencyStruct>
    *LT; // filled in by ModuleScheduler - simple visitation of instructions
// std::map<BasicBlock *, int> *LTCPU; // filled in after getting dynamic trace
// area table
std::map<BasicBlock *, int> *AT;
int64_t cpuCycle; // This will be a problem for threading.
std::vector<unsigned long long> startTime;

//===----------------------------------------------------------------------===//
// Advisor Analysis Pass options
//===----------------------------------------------------------------------===//

static cl::opt<std::string> TraceFileName("trace-file",
                                          cl::desc("Name of the trace file"),
                                          cl::Hidden, cl::init("trace.log"));
static cl::opt<bool>
    IgnoreSanity("ignore-sanity",
                 cl::desc("Enable to ignore trace sanity check"), cl::Hidden,
                 cl::init(false));
static cl::opt<bool>
    HideGraph("hide-graph",
              cl::desc("If enabled, disables printing of dot graphs"),
              cl::Hidden, cl::init(false));
static cl::opt<bool>
    NoMessage("no-message",
              cl::desc("If enabled, disables printing of messages for debug"),
              cl::Hidden, cl::init(false));
static cl::opt<bool>
    PerFunction("per-function",
                cl::desc("If enabled, does per-function analysis (old way)"),
                cl::Hidden, cl::init(false));
static cl::opt<bool> StaticDepsOnly(
    "static-deps-only",
    cl::desc("If enabled, program is analyzed only with dependence information "
             "that is statically avaiable"),
    cl::Hidden, cl::init(false));
static cl::opt<unsigned int>
    TraceThreshold("trace-threshold",
                   cl::desc("Maximum lines of input trace to read"), cl::Hidden,
                   cl::init(UINT_MAX));
static cl::opt<unsigned int> AreaConstraint("area-constraint",
                                            cl::desc("Set the area constraint"),
                                            cl::Hidden, cl::init(0));

static cl::opt<unsigned int>
    RapidConvergence("rapid-convergence",
                     cl::desc("specify number of steps to use in 'geometric "
                              "descent' fast convergence method"),
                     cl::Hidden, cl::init(0));

static cl::opt<double>
    MaxDerivativeError("max-derivative-error",
                       cl::desc(" derivative error guardrail to use in 'max "
                                "derivative error' fast convergence method"),
                       cl::Hidden, cl::init(0.04));

static cl::opt<unsigned int>
    UseThreads("use-threads",
               cl::desc("specify number of threads to use in gradient descent"),
               cl::Hidden, cl::init(8));

static cl::opt<unsigned int> SerialGradientCutoff(
    "serial-cutoff",
    cl::desc("specifies lower bound for computation of serial gradient"),
    cl::Hidden, cl::init(0));

static cl::opt<unsigned int> ParallelizeOneZero(
    "parallelize-one-zero",
    cl::desc("parallelizes one-zero transistions without changing latencies"),
    cl::Hidden, cl::init(0));

static cl::opt<unsigned int> ParllelGradientCutoff(
    "parallel-cutoff",
    cl::desc("specifies lower bound for computation of parallel gradient"),
    cl::Hidden, cl::init(0));

static cl::opt<unsigned> UserTransitionDelay(
    "transition-delay",
    cl::desc("Set the fpga to cpu transition delay baseline"), cl::Hidden,
    cl::init(0));

static cl::opt<unsigned> UseDynamicBlockRuntime(
    "dynamic-block-runtime",
    cl::desc("Set the fpga to cpu transition delay baseline"), cl::Hidden,
    cl::init(0));

static cl::opt<unsigned>
    AssumePipelining("assume-pipelining",
                     cl::desc("Assumes basic blocks are pipelined and "
                              "available after the specified number of cycles"),
                     cl::Hidden, cl::init(0));

static cl::opt<unsigned>
    HaltForGDB("halt-for-gdb",
               cl::desc("Halts program, allowing gdb to attach"), cl::Hidden,
               cl::init(0));

//===----------------------------------------------------------------------===//
// List of statistics -- not necessarily the statistics listed above,
// this is at a module level
//===----------------------------------------------------------------------===//

STATISTIC(FunctionCounter, "Number of functions in module");
STATISTIC(BasicBlockCounter,
          "Number of basic blocks in all functions in module");
STATISTIC(InstructionCounter,
          "Number of instructions in all functions in module");
// STATISTIC(LoopCounter, "Number of loops in all functions in module");
// STATISTIC(ParallelizableLoopCounter, "Number of parallelizable loops in all
// functions in module");
// STATISTIC(LoopInstructionCounter, "Number of instructions in all loops in all
// functions in module");
// STATISTIC(ParallelizableLoopInstructionCounter, "Number of instructions in
// all parallelizable loops in all functions in module");
STATISTIC(ConvergenceCounter,
          "Number of steps taken to converge in gradient descent optimization");

//===----------------------------------------------------------------------===//
// Helper functions
//===----------------------------------------------------------------------===//
// template <typename T> void output_dot_graph(std::ostream stream, T const &g)
// {
//	boost::write_graphviz(stream, g);
//}

//===----------------------------------------------------------------------===//
// AdvisorAnalysis Class functions
//===----------------------------------------------------------------------===//

AdvisorAnalysis::AdvisorAnalysis()
    : ModulePass(ID), tidPool(UseThreads), bbInstanceCounts(), init(UseThreads),
      areaConstraint(1000) {}

// Function: runOnModule
// This is the main analysis pass
bool AdvisorAnalysis::runOnModule(Module &M) {
  std::cerr << "Starting FPGA Advisor Analysis Phase...\n";
  // take some run time stats
  tbb::tick_count start, end;
  start = tbb::tick_count::now();
  //=------------------------------------------------------=//
  // [1] Initialization
  //=------------------------------------------------------=//
  // log file
  raw_fd_ostream OL("fpga-advisor-analysis.log", AEC, sys::fs::F_RW);
  outputLog = &OL;
  if (NoMessage) {
    outputLog = &nulls();
  } else {
    DEBUG(outputLog = &dbgs());
  }
  *outputLog << "FPGA-Advisor Analysis Pass Starting.\n";

  // output results
  raw_fd_ostream OF("fpga-advisor-analysis-result.log", AEC, sys::fs::F_RW);
  outputFile = &OF;

  mod = &M;

  ExecutionOrderList emptyOrderList;
  ExecutionOrder newOrder;
  newOrder.clear();
  emptyOrderList.push_back(newOrder);
  globalExecutionOrder = emptyOrderList.end();
  globalExecutionOrder--;

  TraceGraphList newTraceGraphList;
  TraceGraph newGraph;
  newTraceGraphList.push_back(newGraph);
  globalTraceGraph = newTraceGraphList.end();
  globalTraceGraph--;

  globalDepGraph = new DepGraph;
  //=------------------------------------------------------=//
  // [2] Static analyses and setup
  //=------------------------------------------------------=//
  callGraph = &getAnalysis<CallGraphWrapperPass>().getCallGraph();
  find_recursive_functions(M);

  // basic statistics gathering
  // also populates the functionMap
  // disable this statistic for now
  // visit(M);

  *outputLog << "Finished visit.\n";

  //=------------------------------------------------------=//
  // [3] Read trace from file into memory
  //=------------------------------------------------------=//
  if (!getProgramTrace(TraceFileName)) {
    errs() << "Could not process trace file: " << TraceFileName << "!\n";
    return false;
  }

  *outputLog << "Finished importing program trace.\n";
  //*outputLog << "Global Execution Order.\n";
  // print_execution_order(globalExecutionOrder);

  // should also contain a sanity check to follow the trace and make sure
  // the paths are valid
  // if (! IgnoreSanity && ! check_trace_sanity()) {
  //	errs() << "Trace from file is broken, path does not follow control flow
  // graph!\n";
  //	return false;
  //}

  // Initialize thread allocation pool
  for (unsigned i = 0; i < UseThreads; i++) {
    tidPool.bounded_push(i);
  }

  //=------------------------------------------------------=//
  // [4] Analysis after dynamic feedback for each function
  //=------------------------------------------------------=//
  if (PerFunction) {
    for (auto &F : M) {
      run_on_function(&F);
    }
  } else {
    run_on_module(M);
  }

  //=------------------------------------------------------=//
  // [5] Printout statistics [turned off, this isn't even useful]
  //=------------------------------------------------------=//
  //*outputLog << "Print static information\n"; // there is no need to announce
  // this..
  // pre-instrumentation statistics => work with uninstrumented code
  // print_statistics();

  end = tbb::tick_count::now();
  std::cerr << "TOTAL ANALYSIS RUNTIME: " << (end - start).seconds()
            << " seconds\n";

  return true;
}

void AdvisorAnalysis::visitFunction(Function &F) {
  *outputLog << "visit Function: " << F.getName() << "\n";
  FunctionCounter++;

  // create and initialize a node for this function
  FunctionInfo *newFuncInfo = new FunctionInfo();
  newFuncInfo->function = &F;
  newFuncInfo->bbList.clear();
  newFuncInfo->instList.clear();
  newFuncInfo->loopList.clear();

  if (!F.isDeclaration()) {
    // only get the loop info for functions with a body, else will get assertion
    // error
    newFuncInfo->loopInfo = &getAnalysis<LoopInfoWrapperPass>(F).getLoopInfo();
    *outputLog << "PRINTOUT THE LOOPINFO\n";
    newFuncInfo->loopInfo->print(*outputLog);
    *outputLog << "\n";
    // find all the loops in this function
    for (LoopInfo::reverse_iterator li = newFuncInfo->loopInfo->rbegin(),
                                    le = newFuncInfo->loopInfo->rend();
         li != le; li++) {
      *outputLog << "Encountered a loop!\n";
      (*li)->print(*outputLog);
      *outputLog << "\n" << (*li)->isAnnotatedParallel() << "\n";
      // append to the loopList
      // newFuncInfo->loopList.push_back(*li);
      LoopIterInfo newLoop;
      // newLoop.loopInfo = *li;
      // how many subloops are contained within the loop
      *outputLog << "This natural loop contains " << (*li)->getSubLoops().size()
                 << " subloops\n";
      newLoop.subloops = (*li)->getSubLoopsVector();
      *outputLog << "Copied subloops " << newLoop.subloops.size() << "\n";
      newLoop.maxIter = 0;
      newLoop.parIter = 0;
      newFuncInfo->loopList.push_back(newLoop);
    }
  }

  // insert into the map
  functionMap.insert({&F, newFuncInfo});
}

void AdvisorAnalysis::visitBasicBlock(BasicBlock &BB) {
  //*outputLog << "visit BasicBlock: " << BB.getName() << "\n";
  BasicBlockCounter++;

  // make sure function is in functionMap
  assert(functionMap.find(BB.getParent()) != functionMap.end());
  FunctionInfo *FI = functionMap.find(BB.getParent())->second;
  FI->bbList.push_back(&BB);
}

void AdvisorAnalysis::visitInstruction(Instruction &I) {
  //*outputLog << "visit Instruction: " << I << "\n";
  InstructionCounter++;

  // make sure function is in functionMap
  assert(functionMap.find(I.getParent()->getParent()) != functionMap.end());
  FunctionInfo *FI = functionMap.find(I.getParent()->getParent())->second;
  FI->instList.push_back(&I);

  // eliminate instructions which are not synthesizable
}

// visit callsites -- count function calls
// visit memory related instructions

// Function: print_statistics
// Return: nothing
void AdvisorAnalysis::print_statistics() {
  errs() << "Number of Functions : " << functionMap.size() << "\n";
  // iterate through each function info block
  for (auto it = functionMap.begin(), et = functionMap.end(); it != et; it++) {
    errs() << it->first->getName() << ":\n";
    errs() << "\t"
           << "Number of BasicBlocks : " << it->second->bbList.size() << "\n";
    errs() << "\t"
           << "Number of Instructions : " << it->second->instList.size()
           << "\n";
    errs() << "\t"
           << "Number of Loops : " << it->second->loopList.size() << "\n";
  }
}

// Function: find_recursive_functions
// Return: nothing
void AdvisorAnalysis::find_recursive_functions(Module &M) {
  DEBUG(*outputLog << __func__ << "\n");
  // look at call graph for loops
  // CallGraph &CG = getAnalysis<CallGraphWrapperPass>().getCallGraph();
  // DEBUG(CG.print(dbgs()); dbgs() << "\n");
  DEBUG(callGraph->print(dbgs()); dbgs() << "\n");
  // CallGraph &CG = CallGraphAnalysis::run(&M);

  // do a depth first search to find the recursive functions
  // a function is recursive if any of its called functions is
  // either itself or contains a call to itself
  // (ironically), use recursion for this...
  // store onto the recursiveFunctionList
  for (auto &F : M) {
    if (!F.isDeclaration()) {
      DEBUG(*outputLog << "Calling does_function_recurse on function: "
                       << F.getName() << "\n");
      std::vector<Function *> fStack;
      // function will modify recursiveFunctionList directly
      does_function_recurse(&F, callGraph->getOrInsertFunction(&F), fStack);
      assert(fStack.empty());
    } else {
      errs() << __func__ << " ignored.\n";
    }
  }
  DEBUG(print_recursive_functions());
}

// Function: does_function_recurse
// Return: nothing
// Modifies recursiveFunctionList vector
void AdvisorAnalysis::does_function_recurse(Function *func, CallGraphNode *CGN,
                                            std::vector<Function *> &stack) {
  if (CGN->getFunction())
    DEBUG(*outputLog << "does_function_recurse: "
                     << CGN->getFunction()->getName() << "\n");
  else
    DEBUG(*outputLog << "does_function_recurse: "
                     << "indirect call"
                     << "\n");
  DEBUG(*outputLog << "stack size: " << stack.size() << "\n");
  // if this function exists within the stack, function recurses and add to list
  if ((stack.size() > 0) && (std::find(stack.begin(), stack.end(),
                                       CGN->getFunction()) != stack.end())) {
    if (CGN->getFunction())
      DEBUG(*outputLog << "Function recurses: " << CGN->getFunction()->getName()
                       << "\n");
    else
      DEBUG(*outputLog << "does_function_recurse: "
                       << "indirect call"
                       << "\n");

    // delete functions off "stack"
    // while (stack[stack.size()-1] != CGN->getFunction()) {
    // pop off stack
    //	stack.pop_back();
    //	*outputLog << "stack size: " << stack.size() << "\n";
    //}
    // stack.pop_back();
    //*outputLog << "stack size: " << stack.size() << "\n";
    // add to recursiveFunctionList only if this is the function we are checking
    // to be recursive or not
    // this avoids the situation where a recursive function is added to the list
    // multiple times
    if (CGN->getFunction() == func) {
      recursiveFunctionList.push_back(CGN->getFunction());
    }
    return;
  }

  // else, add the function to the stack and call does_function_recurse on each
  // of the functions
  // contained by this CGN
  stack.push_back(CGN->getFunction());
  for (auto it = CGN->begin(), et = CGN->end(); it != et; it++) {
    CallGraphNode *calledGraphNode = it->second;
    if (!calledGraphNode->getFunction()) {
      errs() << __func__ << " is being ignored, it is an indirect call.\n";
      continue;
    }

    DEBUG(*outputLog << "Found a call to function: "
                     << calledGraphNode->getFunction()->getName() << "\n");
    if (calledGraphNode->getFunction()->getName().find("pthread_create") !=
        std::string::npos) {
      std::cerr << "WARNING: call to pthread_create() found in "
                << CGN->getFunction()->getName().str() << "\n";
    }
    // stack.push_back(calledGraphNode->getFunction());
    // ignore this function if its primary definition is outside current module

    if (!calledGraphNode->getFunction()->isDeclaration()) {
      does_function_recurse(func, calledGraphNode, stack);
    } else { // print a warning
      DEBUG(errs() << calledGraphNode->getFunction()->getName()
                   << " is being ignored, it is declared outside of this "
                      "translational unit.\n");
    }
    DEBUG(*outputLog << "Returned from call to function: "
                     << calledGraphNode->getFunction()->getName() << "\n");
  }
  // pop off the stack
  stack.pop_back();
  DEBUG(*outputLog << "stack size: " << stack.size() << "\n");
  return;
}

void AdvisorAnalysis::print_recursive_functions() {
  dbgs() << "Found recursive functions: \n";
  for (auto r = recursiveFunctionList.begin(), re = recursiveFunctionList.end();
       r != re; r++) {
    Function *F = *r;
    dbgs() << "  " << F->getName() << "\n";
  }
}

// Function: run_on_module
bool AdvisorAnalysis::run_on_module(Module &M) {
  unsigned cpuOnlyLatency = UINT_MAX;
  unsigned fpgaOnlyLatency = UINT_MAX;
  unsigned fpgaOnlyArea = 0;

  if (AreaConstraint > 0) {
    areaConstraint = AreaConstraint;
  }

  LT = &getAnalysis<ModuleScheduler>().getFPGALatencyTable();
  AT = &getAnalysis<ModuleAreaEstimator>().getAreaTable();
  // fill in latency table for cpu by traversing execution graph
  getGlobalCPULatencyTable(M, LT, *globalExecutionOrder, *globalTraceGraph);

  std::string dgFileName = "dg.global.log";
  if (!getDependenceGraphFromFile(dgFileName, &globalDepGraph,
                                      true /*is_global*/)) {
    std::cerr << "Could not get the dependence graph! Error opening file "
              << dgFileName << "\n";
    assert(0);
  }
  // we want to find the optimal tiling for the basicblocks
  // the starting point of the algorithm is the MOST parallel
  // configuration, which can be found by scheduling independent
  // blocks in the earliest cycle that it is allowed to be executed
  find_maximal_configuration_for_module(M, fpgaOnlyLatency, fpgaOnlyArea);
  *outputLog << "Maximal basic block configuration for module: \n";
  for (auto &F : M) {
    print_basic_block_configuration(&F, outputLog);
  }

  // print this to output file
  *outputFile << "Maximal basic block configuration for module: \n";
  for (auto &F : M) {
    *outputFile << "Maximal basic block configuration for function:"
                << F.getName().str() << "\n";
    print_basic_block_configuration(&F, outputFile);
  }

  std::cerr << "Finished computing maximal configuration\n";

  // Now that we have a replication factor, we will prune it to honor the area
  // constraints of the device.
  std::cerr << "Maximal basic blocks: "
            << get_total_basic_block_instances_global(M) << "\n";
  std::cerr << "Accelerator-only latency: " << fpgaOnlyLatency << "\n";
  *outputFile << "Maximal basic blocks: "
              << get_total_basic_block_instances_global(M) << "\n";
  prune_basic_block_configuration_to_device_area_global(M);
  std::cerr << "Pruned basic blocks: "
            << get_total_basic_block_instances_global(M) << "\n";
  *outputFile << "Pruned basic blocks: "
              << get_total_basic_block_instances_global(M) << "\n";

  unsigned pruned_area = get_area_requirement_global(M);
  unsigned area_delta = pruned_area - areaConstraint;

  int pruning_steps = RapidConvergence;

  // Do not apply rapid convergence if pruning arrived at an optimal solution.
  if (pruned_area < areaConstraint) {
    pruning_steps = 0;
  }

  // adjust this factor for faster or slower termination (and lesser/greater
  // quality of results)

  double area_root = pow((double)area_delta, 1.0 / (double)pruning_steps);

  std::cerr << "Pruned area: " << pruned_area << std::endl;
  std::cerr << "convergence steps: " << pruning_steps << std::endl;
  std::cerr << "areaConstraint: " << areaConstraint << std::endl;
  std::cerr << "Area delta is: " << area_delta << std::endl;
  std::cerr << "Area root is: " << area_root << std::endl;

  // Construct a series of steps to permit gradual elimination of area.
  double baseArea = 1.00;
  for (int i = 0; i < pruning_steps; i++) {
    thresholds.push_back(std::max(areaConstraint + baseArea,
                                  (double)areaConstraint)); // Encode the
                                                            // difference as the
                                                            // amount of area we
                                                            // must reduce.
    DEBUG(std::cerr << "Pushing threshold: " << (areaConstraint + baseArea)
                    << std::endl);
    baseArea = baseArea * area_root;
  }

  // by this point, the basic blocks have been annotated by the maximal
  // legal replication factor
  // build a framework that is able to methodically perturb the basic block
  // to follow the gradient descent method of restricting basic block
  // replication to achieve most optimal area-latency result
  // Description of gradient descent method:
  //	- determine differential in performance/area for each basic block
  //		i.e. reduce the basic block resource by 1 to determine the
  //		impact on performance
  //	- move in the direction of maximum performance/area
  //		i.e. reduce the basic block which provides the least
  // performance/area
  //	- for now, we will finish iterating when we find a local maximum of
  // performance/area
  find_optimal_configuration_for_module(M, cpuOnlyLatency, fpgaOnlyLatency,
                                        fpgaOnlyArea);
  *outputLog << "===-------------------------------------===\n";
  *outputLog << "Final optimal basic block configuration for module: \n";
  for (auto &F : M) {
    *outputLog << "Final optimal basic block configuration for function: "
               << F.getName() << "\n";
    print_basic_block_configuration(&F, outputLog);
  }
  *outputLog << "===-------------------------------------===\n";

  // print this to output file
  *outputFile << "===-------------------------------------===\n";
  *outputFile << "Final optimal basic block configuration for module: \n";
  for (auto &F : M) {
    *outputFile << "Final optimal basic block configuration for function: "
                << F.getName() << "\n";
    print_basic_block_configuration(&F, outputFile);
  }
  *outputFile << "===-------------------------------------===\n";

#if 0
    if (!HideGraph) {
        for (auto &F : M) {
            print_optimal_configuration_for_all_calls(&F);
        }
    }
#endif
  return true;
}

// Function: run_on_function
// Return: false if function cannot be synthesized
// Function looks at the loops within the function
bool AdvisorAnalysis::run_on_function(Function *F) {
  unsigned cpuOnlyLatency = UINT_MAX;
  unsigned fpgaOnlyLatency = UINT_MAX;
  unsigned fpgaOnlyArea = 0;

  // We may have an indirect call.
  if (!F) {
    //*outputLog << "Found an indirect call, moving on.\n";
    return false;
  }

  if (AreaConstraint > 0) {
    areaConstraint = AreaConstraint;
  }

  std::cerr << "Examine function: " << F->getName().str() << "\n";
  // Find constructs that are not supported by HLS
  if (has_unsynthesizable_construct(F)) {
    //*outputLog << "Function contains unsynthesizable constructs, moving
    // on.\n";
    return false;
  }

  // was this function even executed in run
  if (executionGraph.find(F) == executionGraph.end()) {
    // *outputLog << "Did not find execution of function in program trace.
    // Skipping.\n";
    return false;
  }

  // make sure execution was recorded in execution order
  if (executionOrderListMap.find(F) == executionOrderListMap.end()) {
    *outputLog
        << "Did not find execution of function in execution order. Error.\n";
    assert(0);
  }

  LT = &getAnalysis<ModuleScheduler>().getFPGALatencyTable();
  AT = &getAnalysis<ModuleAreaEstimator>().getAreaTable();
  // fill in latency table for cpu by traversing execution graph
  getCPULatencyTable(F, LT, executionOrderListMap[F], executionGraph[F]);

  // get the dependence graph for the function
  // depGraph = &getAnalysis<DependenceGraph>().getDepGraph();
  std::string dgFileName = "dg." + F->getName().str() + ".log";
  if (!getDependenceGraphFromFile(dgFileName, &functionDepGraph,
                                      false /*is_global*/)) {
    std::cerr << "Could not get the dependence graph! Error opening file "
              << dgFileName << "\n";
    assert(0);
  }

  // for each execution of the function found in the trace
  // we want to find the optimal tiling for the basicblocks
  // the starting point of the algorithm is the MOST parallel
  // configuration, which can be found by scheduling independent
  // blocks in the earliest cycle that it is allowed to be executed
  find_maximal_configuration_for_all_calls(F, fpgaOnlyLatency, fpgaOnlyArea);

  *outputLog << "Maximal basic block configuration for function: "
             << F->getName() << "\n";
  print_basic_block_configuration(F, outputLog);

  // print this to output file
  *outputFile << "Maximal basic block configuration for function: "
              << F->getName() << "\n";
  print_basic_block_configuration(F, outputFile);

  std::cerr << "Finished computing maximal configuration\n";
#if 0
	*outputLog << "Trace Graph for function " << F->getName().str() << "\n";
    TraceGraphList_iterator fIt;
    for (fIt = executionGraph[F].begin();
       fIt != executionGraph[F].end(); fIt++) 
    {
         print_trace_graph(fIt);
    }
#endif

  // Now that we have a replication factor, we will prune it to honor the area
  // constraints of the device.
  std::cerr << "Maximal basic blocks: " << get_total_basic_block_instances(F)
            << "\n";
  std::cerr << "Accelerator-only latency: " << fpgaOnlyLatency << "\n";
  *outputFile << "Maximal basic blocks: " << get_total_basic_block_instances(F)
              << "\n";
  prune_basic_block_configuration_to_device_area(F);
  std::cerr << "Pruned basic blocks: " << get_total_basic_block_instances(F)
            << "\n";
  *outputFile << "Pruned basic blocks: " << get_total_basic_block_instances(F)
              << "\n";

  unsigned pruned_area = get_area_requirement(F);
  unsigned area_delta = pruned_area - areaConstraint;

  int pruning_steps = RapidConvergence;

  // Do not apply rapid convergence if pruning arrived at an optimal solution.
  if (pruned_area < areaConstraint) {
    pruning_steps = 0;
  }

  // adjust this factor for faster or slower termination (and lesser/greater
  // quality of results)

  double area_root = pow((double)area_delta, 1.0 / (double)pruning_steps);

  std::cerr << "Pruned area: " << pruned_area << std::endl;
  std::cerr << "convergence steps: " << pruning_steps << std::endl;
  std::cerr << "areaConstraint: " << areaConstraint << std::endl;
  std::cerr << "Area delta is: " << area_delta << std::endl;
  std::cerr << "Area root is: " << area_root << std::endl;

  // Construct a series of steps to permit gradual elimination of area.
  double baseArea = 1.00;
  for (int i = 0; i < pruning_steps; i++) {
    thresholds.push_back(std::max(areaConstraint + baseArea,
                                  (double)areaConstraint)); // Encode the
                                                            // difference as the
                                                            // amount of area we
                                                            // must reduce.
    DEBUG(std::cerr << "Pushing threshold: " << (areaConstraint + baseArea)
                    << std::endl);
    baseArea = baseArea * area_root;
  }

  // by this point, the basic blocks have been annotated by the maximal
  // legal replication factor
  // build a framework that is able to methodically perturb the basic block
  // to follow the gradient descent method of restricting basic block
  // replication to achieve most optimal area-latency result
  // Description of gradient descent method:
  //	- determine differential in performance/area for each basic block
  //		i.e. reduce the basic block resource by 1 to determine the
  //		impact on performance
  //	- move in the direction of maximum performance/area
  //		i.e. reduce the basic block which provides the least
  // performance/area
  //	- for now, we will finish iterating when we find a local maximum of
  // performance/area
  find_optimal_configuration_for_all_calls(F, cpuOnlyLatency, fpgaOnlyLatency,
                                           fpgaOnlyArea);

  *outputLog << "===-------------------------------------===\n";
  *outputLog << "Final optimal basic block configuration for function: "
             << F->getName() << "\n";
  print_basic_block_configuration(F, outputLog);
  *outputLog << "===-------------------------------------===\n";

  // print this to output file
  *outputFile << "===-------------------------------------===\n";
  *outputFile << "Final optimal basic block configuration for function: "
              << F->getName() << "\n";
  print_basic_block_configuration(F, outputFile);
  *outputFile << "===-------------------------------------===\n";

  if (!HideGraph) {
    print_optimal_configuration_for_all_calls(F);
  }

  return true;
}

// Function: has_unsynthesizable_construct
// Return: true if module contains code which is not able to be run through HLS
// tools
// These contain:
// 	- Recursion
// 	- Dynamic memory allocation
//	- Arbitrary pointer accesses
// 	- Some tools do not support pthread/openmp but LegUp does (so we will
// ignore it)
bool AdvisorAnalysis::has_unsynthesizable_construct(Function *F) {
  // is defined externally, which we test by looking to see if there are any
  // basic blocks
  if (F->getBasicBlockList().begin() == F->getBasicBlockList().end()) {
    //*outputLog << "Function is external.\n";
    return true;
  }

  // no recursion
  if (has_recursive_call(F)) {
    //*outputLog << "Function has recursive call.\n";
    return true;
  }

  // no external function calls
  if (has_external_call(F)) {
    //*outputLog << "Function has external function call.\n";
    // return true;
    return false; // we ignore these for now!!?!? FIXME
  }

  // examine memory accesses

  return false;
}

// Function: is_recursive_function
// Return: true if function is contained in recursiveFunctionList
// A function recurses if it or any of the functions it calls calls itself
// TODO?? I do not handle function pointers by the way
bool AdvisorAnalysis::is_recursive_function(Function *F) {
  return (std::find(recursiveFunctionList.begin(), recursiveFunctionList.end(),
                    F) != recursiveFunctionList.end());
}

// Function: has_recursive_call
// Return: true if function is recursive or contains a call to a recursive
// function on the recursiveFunctionList
bool AdvisorAnalysis::has_recursive_call(Function *F) {
  if (is_recursive_function(F)) {
    return true;
  }

  bool result = false;

  // look through the CallGraph for this function to see if this function makes
  // calls to
  // recursive functions either directly or indirectly
  // F->dump();
  // callGraph->dump();
  if (!F->isDeclaration()) {
    result = does_function_call_recursive_function(
        callGraph->getOrInsertFunction(F));
  } else {
    // errs() << __func__ << " is being ignored, it is declared outside of this
    // translational unit.\n";
  }

  return result;
}

// Function: does_function_call_recursive_function
// Return: true if function contain a call to a function which is recursive
// This function should not recurse infinitely since it will stop at a recursive
// function
// and therefore not get stuck in a loop in the call graph
bool AdvisorAnalysis::does_function_call_recursive_function(
    CallGraphNode *CGN) {
  if (CGN->getFunction() && (is_recursive_function(CGN->getFunction()))) {
    return true;
  }

  bool result = false;

  for (auto it = CGN->begin(), et = CGN->end(); it != et; it++) {
    CallGraphNode *calledGraphNode = it->second;
    if (calledGraphNode->getFunction()) {
      //*outputLog << "Found a call to function: " <<
      // calledGraphNode->getFunction()->getName() << "\n";
      if (!calledGraphNode->getFunction()->isDeclaration()) {
        result |= does_function_call_recursive_function(calledGraphNode);
      } else {
        // errs() << __func__ << " is being ignored, it is declared outside of
        // this translational unit.\n";
      }
    } else {
      *outputLog << "Found an indirect call, assuming there is no recursion "
                    "involved.\n";
      result = false;
    }
  }
  return result;
}

// Function: has_external_call
// Return: true if function is or contains a call to an external function
// external functions are not declared within the current module => library
// function
bool AdvisorAnalysis::has_external_call(Function *F) {
  if (F->isDeclaration()) {
    return true;
  }

  bool result =
      does_function_call_external_function(callGraph->getOrInsertFunction(F));

  return result;
}

// Function: does_function_call_external_function
// Return: true if function contain a call to a function which is extenal to the
// module
// Always beware of recursive functions when dealing with the call graph
bool AdvisorAnalysis::does_function_call_external_function(CallGraphNode *CGN) {
  if (CGN->getFunction() && (CGN->getFunction()->isDeclaration())) {
    return true;
  }

  bool result = false;

  for (auto it = CGN->begin(), et = CGN->end(); it != et; it++) {
    CallGraphNode *calledGraphNode = it->second;
    if (calledGraphNode->getFunction()) {
      //*outputLog << "Found a call to function: " <<
      // calledGraphNode->getFunction()->getName() << "\n";
      if (std::find(recursiveFunctionList.begin(), recursiveFunctionList.end(),
                    calledGraphNode->getFunction()) ==
          recursiveFunctionList.end()) {
        result |= does_function_call_external_function(calledGraphNode);
      } else {
        // errs() << __func__ << " is being ignored, it is recursive.\n";
      }
    } else {
      *outputLog << "Found an indirect call, assuming there is no external "
                    "call involved.\n";
      result = false;
    }
  }
  return result;
}

// Reads input trace file, parses and stores trace into executionTrace map
bool AdvisorAnalysis::getProgramTrace(std::string fileIn) {
  // the instrumentation phase will instrument all functions as long as
  // they are not external to the module (this will include recursive functions)
  // when recording the trace, create the trace for each function encountered,
  // however, simply ignore them later

  // read file
  ifstream fin;
  fin.open(fileIn.c_str());
  if (!fin.good()) {
    return false; // file not found
  }

  if (fileIn.length() + 25 >= BUFSIZE) {
    std::cerr << "BUFSIZE " << BUFSIZE
              << " too small to hold trace-file-name: " << fileIn << std::endl;
    return false;
  }

  std::string line;

  // unique ID for each basic block executed
  int ID = 0;

  // for keeping track of which function and execution graph to insert into
  TraceGraph::vertex_descriptor lastVertex = UINT_MAX;
  TraceGraphList::iterator latestTraceGraph;
  Function *latestFunction = NULL;
  ExecutionOrderList::iterator latestExecutionOrder;

  // use a stack to keep track of where we should return to
  std::stack<FunctionExecutionRecord> funcStack;

  bool showProgressBar = true;
  // get total line number from file using wc command
  FILE *in;
  char buf[BUFSIZE + 1]; // need to change hard-coded size

  unsigned int fileLineNum;
  unsigned int traceThreshold = TraceThreshold;

  std::string cmd = "wc " + fileIn;

  std::cerr << "command " << cmd << std::endl;
  if (!(in = popen(cmd.c_str(), "r"))) {
    // if cannot execute command, don't show progress bar
    showProgressBar = false;
    fileLineNum = UINT_MAX;
  } else {
    // assert(fgets(buf, sizeof(buf), in) != NULL);
    while (fgets(buf, BUFSIZE, in)) {
    }

    DEBUG(*outputLog << "WC " << buf << "\n");
    char *pch = std::strtok(buf, " ");
    fileLineNum = atoi(pch);
    *outputLog << "Total lines from " << fileIn << ": " << fileLineNum << "\n";
    std::cerr << "Total lines " << fileLineNum << "\n";
  }

  std::cerr << "Processing program trace.\n" << std::flush;

  unsigned int lineNum = 0;
  unsigned int totalLineNum = std::min(traceThreshold, fileLineNum);

  unsigned int times = 0;
  const char *delim = " ";
  while (std::getline(fin, line)) {
    if (lineNum > traceThreshold) {
      break;
    }

    if (showProgressBar) {
      // print a processing progress bar
      // 20 points, print progress every 5% processed
      unsigned int fivePercent = totalLineNum / 20;
      if ((lineNum % fivePercent) == 0) {
        std::cerr << " [ " << 5 * times << "% ] " << lineNum << "/"
                  << totalLineNum << "\n";
        times++;
      }
    }

    DEBUG(*outputLog << "PROCESSING LINE: " << line << " (" << lineNum
                     << ")\n");
    lineNum++;
    //*outputLog << "latestTraceGraph iterator: " << latestTraceGraph << "\n";
    DEBUG(*outputLog << "lastVertex: " << lastVertex << "\n");
    // There are 5 types of messages:
    // 1. Enter Function: <func name>
    // 2. Basic Block: <basic block name> Function: <func name>
    // 3. Return from: <func name>
    // 4. Store at address: <addr start> size in bytes: <size>
    // 5. Load from address: <addr start> size in bytes: <size>

    std::vector<char> rawLine(line.size()+1);
    strncpy(&rawLine[0], line.c_str(), line.length());
    rawLine[line.length()] = '\0';

    char *token = std::strtok(&rawLine[0], delim);
    if (token != NULL) {
      if (strcmp(token, "B:") == 0) {
        lastVertex = UINT_MAX;
        if (!processBasicBlockEntry(line, latestFunction, ID, latestTraceGraph, lastVertex, latestExecutionOrder)) {
          *outputLog << "process basic block entry: FAILED.\n";
          return false;
        }
      }
      else if (strcmp(token, "BSTR:") == 0) {
        if (!processTime(line, latestTraceGraph, lastVertex, true)) {
          *outputLog << "process time start: FAILED.\n";
          return false;          
        }
      }
      else if (strcmp(token, "BSTP:") == 0) {
        if (!processTime(line, latestTraceGraph, lastVertex, false)) {
          *outputLog << "process time stop: FAILED.\n";
          return false;          
        }
      }
      else if (strcmp(token, "ST:") == 0) {
        if (!processStore(line, latestFunction, latestTraceGraph, lastVertex)) {
          *outputLog << "process store: FAILED.\n";
          return false;
        }        
      }
      else if (strcmp(token, "LD:") == 0) {
        if (!processLoad(line, latestFunction, latestTraceGraph, lastVertex)) {
          *outputLog << "process load: FAILED.\n";
          return false;
        }        
      }
      else if (strcmp(token, "Entering") == 0) {
        if (!processFunctionEntry(line, &latestFunction, latestTraceGraph,
                                    lastVertex, latestExecutionOrder,
                                    funcStack)) {
          *outputLog << "process function entry: FAILED.\n";
          return false;
        } else {
          DEBUG(*outputLog << "returned from process_function_entry()\n");
        }        
      }
      else if (strcmp(token, "Return") == 0) {
        if (!processFunctionReturn(line, &latestFunction, funcStack,
                                     latestTraceGraph, lastVertex,
                                     latestExecutionOrder)) {
          // With ROI, we could see a function return without a
          // matching entry to the function.
          *outputLog << "IGNORING process function return: FAILED.\n";
        } else {
          DEBUG(*outputLog << "returned from process_function_return()\n");
        }
      }
    }
  }
  DEBUG(*outputLog << "End of " << __func__ << " " << "\n");
  return true;
}


// process one line of trace containing a time start or stop
bool AdvisorAnalysis::processTime(const std::string &line,
                                  TraceGraphList::iterator latestTraceGraph,
                                  TraceGraph::vertex_descriptor lastVertex,
                                  bool start) {
  DEBUG(*outputLog << __func__ << " " << line << "\n");
  const char *delimiter = " ";

  // get the time value
  // make a non-const copy of the line
  std::vector<char> lineCopy(line.begin(), line.end());
  lineCopy.push_back(0);

  // separate line by space to get keywords
  //=---------------------------------=//
  char *pch;
  pch = std::strtok(&lineCopy[6], delimiter);

  std::string cycleString(pch);
  //=---------------------------------=//

  // convert the string to long int
  unsigned long long cycle = std::strtol(cycleString.c_str(), NULL, 0);

  if (start) {
    DEBUG(*outputLog << "Start time : " << cycle << " cycles\n");
    // store the starts in stack, pop stack when stop is encountered
    // assert(startTime.empty());
    startTime.push_back(cycle);
  } else {
    DEBUG(*outputLog << "Stop time : " << cycle << " cycles\n");
    // update the timer
    // with region processing, we can see mismatched BSTPs
    if (startTime.size() != 0) {
      unsigned long long start = startTime.back();
      startTime.pop_back();

      // update the graph
      if (lastVertex != UINT_MAX) {
        if (PerFunction) {
          (*latestTraceGraph)[lastVertex].cpuCycles = (cycle - start);
        } else {
          (*globalTraceGraph)[lastVertex].cpuCycles = (cycle - start);
        }
      }
    }
  }
  return true;
}

// process one line of trace containing return
bool AdvisorAnalysis::processFunctionReturn(
    const std::string &line, Function **function,
    std::stack<FunctionExecutionRecord> &stack,
    TraceGraphList::iterator &lastTraceGraph,
    TraceGraph::vertex_descriptor &lastVertex,
    ExecutionOrderList::iterator &lastExecutionOrder) {
  DEBUG(*outputLog << __func__ << " " << line << "\n");

  if (!PerFunction) {
    // nothing to do here for global scheduling
    return true;
  }
  const char *delimiter = " ";

  // make non-const copy of line
  std::vector<char> lineCopy(line.begin(), line.end());
  lineCopy.push_back(0);

  //=---------------------------------=//
  // Return<space>from:<space>function
  char *pch = std::strtok(&lineCopy[13], delimiter);
  std::string funcString(pch);
  //=---------------------------------=//

  // make sure that this is the last function on stack
  Function *F = find_function_by_name(funcString);
  assert(F);

  // update current function after returning
  if (*function == NULL) {
    DEBUG(*outputLog << "NULL function returning false\n");
    lastVertex = UINT_MAX;
    return false;
  } else if (*function != NULL && stack.size() == 0) {
    *function = NULL;
    lastVertex = UINT_MAX;
    return true;
  } else {
    *function = stack.top().function;
    lastTraceGraph = stack.top().graph;
    lastVertex = stack.top().vertex;
    lastExecutionOrder = stack.top().executionOrder;
    DEBUG(*outputLog << "<<<< Return to function " << (*function)->getName()
                     << "\n");
  }

  if (stack.size() > 0) {
    stack.pop();
  }

  return true;
}

// process one line of trace containing load
bool AdvisorAnalysis::processLoad(const std::string &line, Function *function,
                                  TraceGraphList::iterator lastTraceGraph,
                                  TraceGraph::vertex_descriptor lastVertex) {
  DEBUG(*outputLog << __func__ << " " << line << "\n");
  const char *delimiter = " ";

  // make a non-const copy of line
  std::vector<char> lineCopy(line.begin(), line.end());
  lineCopy.push_back(0);

  // separate line by space to get keywords
  //=---------------------------------=//
  // Load<space>from<space>address:<space>addr<space>size<space>in<space>bytes:<space>size\n
  // separate out string by tokens
  char *pch = std::strtok(&lineCopy[4], delimiter);
  std::string addrString(pch);

  pch = strtok(NULL, delimiter);
  // bytes:
  pch = strtok(NULL, delimiter);
  std::string byteString(pch);
  //=---------------------------------=//

  // convert the string to uint64_t
  uint64_t addrStart = std::strtoul(addrString.c_str(), NULL, 0);
  uint64_t width = std::strtoul(byteString.c_str(), NULL, 0);
  DEBUG(*outputLog << "Discovered a load with starting address : " << addrStart
                   << "\n");
  DEBUG(*outputLog << "Load width in bytes : " << width << "\n");

  // TraceGraph &latestGraph = executionGraph[function].back();
  TraceGraph &latestGraph = *lastTraceGraph;
  // std::pair<uint64_t, uint64_t> addrWidthTuple = std::make_pair(addrStart,
  // width);
  DEBUG(*outputLog << "after pair\n");
  if (lastVertex != UINT_MAX) {
    try {
      // latestGraph[latestVertex].memoryReadTuples.push_back(std::make_pair(addrStart,
      // width));
      DEBUG(*outputLog << "before push_back read tuples "
                       << latestGraph[lastVertex].memoryReadTuples.size()
                       << "\n");
      // latestGraph[latestVertex].memoryReadTuples.push_back(addrWidthTuple);
      if (PerFunction) {
        latestGraph[lastVertex].memoryReadTuples.emplace_back(addrStart, width);
      } else {
        (*globalTraceGraph)[lastVertex].memoryReadTuples.emplace_back(addrStart,
                                                                      width);
      }
      DEBUG(*outputLog << "after push_back read tuples\n");
    } catch (std::exception &e) {
      std::cerr << "An error occured." << e.what() << "\n";
    }
  }
  DEBUG(*outputLog << "after load\n");

  return true;
}

// process one line of trace containing store
bool AdvisorAnalysis::processStore(const std::string &line, Function *function,
                                   TraceGraphList::iterator lastTraceGraph,
                                   TraceGraph::vertex_descriptor lastVertex) {
  DEBUG(*outputLog << __func__ << " " << line << "\n");
  const char *delimiter = " ";

  // make a non-const copy of line
  std::vector<char> lineCopy(line.begin(), line.end());
  lineCopy.push_back(0);

  // separate line by space to get keywords
  //=---------------------------------=//
  // ST:<space>addr<space>B:<space>size\n
  // separate out string by tokens
  char *pch = std::strtok(&lineCopy[4], delimiter);
  std::string addrString(pch);

  // bytes:
  pch = strtok(NULL, delimiter);
  pch = strtok(NULL, delimiter);
  std::string bytesString(pch);
  //=---------------------------------=//

  // std::cerr << "Discovered a store with starting address : " << addrString
  // << "\n";
  // std::cerr << "Store width in bytes : " << bytesString << "\n";

  // convert the string to uint64_t
  uint64_t addrStart = std::strtoul(addrString.c_str(), NULL, 0);
  uint64_t width = std::strtoul(bytesString.c_str(), NULL, 0);
  DEBUG(*outputLog << "Discovered a store with starting address : " << addrStart
                   << "\n");
  DEBUG(*outputLog << "Store width in bytes : " << width << "\n");

  // TraceGraph &latestGraph = executionGraph[function].back();
  TraceGraph &latestGraph = *lastTraceGraph;
  if (lastVertex != UINT_MAX) {
    try {
      if (PerFunction) {
        latestGraph[lastVertex].memoryWriteTuples.push_back(
            std::make_pair(addrStart, width));
      } else {
        (*globalTraceGraph)[lastVertex].memoryWriteTuples.push_back(
            std::make_pair(addrStart, width));
      }
    } catch (std::exception &e) {
      std::cerr << "An error occured." << e.what() << "\n";
    }
  }
  return true;
}

// process one line of trace containing basic block entry
bool AdvisorAnalysis::processBasicBlockEntry(
    const std::string &line, Function *latestFunction, int &ID,
    TraceGraphList::iterator lastTraceGraph,
    TraceGraph::vertex_descriptor &lastVertex,
    ExecutionOrderList::iterator lastExecutionOrder) {
  DEBUG(*outputLog << __func__ << " " << line << "\n");
  const char *delimiter = " ";

  // make a non-const copy of line
  std::vector<char> lineCopy(line.begin(), line.end());
  lineCopy.push_back(0);

  // separate line by space to get keywords
  //=----------------------------=//
  // B:<space>bbName<space>F:<space>funcName\n
  // separate out string by tokens
  char *pch = std::strtok(&lineCopy[3], delimiter);
  // BasicBlock:
  // pch = strtok(NULL, delimiter);
  // bbName
  std::string bbString(pch);

  pch = strtok(NULL, delimiter);
  // Function:
  pch = strtok(NULL, delimiter);
  // funcName
  std::string funcString(pch);
  //=----------------------------=//

  if (PerFunction) {
    if (latestFunction == NULL) {
      // With ROI, there could be 'dangling' basic blocks without
      // any function entry seen. Ignore such basic blocks.
      DEBUG(*outputLog
            << "No latestFunction to attach the basic block, ignoring \n");
      return true;
    }
  }

  BasicBlock *BB = find_basicblock_by_name(bbString);
  if (!BB) {
    // could not find the basic block by name
    errs() << "Could not find the basic block from trace in program! "
           << bbString << "\n";
    return false;
  }

  DEBUG(*outputLog << "SOMETHING\n");

  if (isa<TerminatorInst>(BB->getFirstNonPHI())) {
    // if the basic block only contains a branch/control flow and no
    // computation
    // then skip it, do not add to graph
    // TODO if this is what I end up doing, need to remove looking at
    // these
    // basic blocks when considering transitions ?? I think that already
    // happens.
    return true;
  }

  DEBUG(*outputLog << "~~~~~~~~~\n");

  //==----------------------------------------------------------------==//
  // TraceGraph::vertex_descriptor currVertex =
  // boost::add_vertex(executionGraph[BB->getParent()].back());
  // TraceGraph &currGraph = executionGraph[BB->getParent()].back();
  TraceGraph::vertex_descriptor currVertex;
  TraceGraphList::iterator currGraph;
  if (PerFunction) {
    currVertex = boost::add_vertex(*lastTraceGraph);
    currGraph = lastTraceGraph;
  } else {
    currVertex = boost::add_vertex(*globalTraceGraph);
    currGraph = globalTraceGraph;
  }
  (*currGraph)[currVertex].basicblock = BB;
  (*currGraph)[currVertex].ID = ID;
  (*currGraph)[currVertex].minCycStart = -1;
  (*currGraph)[currVertex].minCycEnd = -1;
  (*currGraph)[currVertex].cpuCycles = 0;
  (*currGraph)[currVertex].name = BB->getName().str();
  (*currGraph)[currVertex].memoryWriteTuples.clear();
  (*currGraph)[currVertex].memoryReadTuples.clear();
  //==----------------------------------------------------------------==//

  // add to execution order
  ExecutionOrderList::iterator currOrder;
  if (PerFunction) {
    currOrder = lastExecutionOrder;
  } else {
    currOrder = globalExecutionOrder;
  }
  ExecutionOrder::iterator search = currOrder->find(BB);
  std::string ordername = ((PerFunction) ? "local " : "global ");
  if (search == currOrder->end()) {
    // insert BB into order
    DEBUG(*outputLog << "Inserting BB " + BB->getName() + " into " << ordername
                     << "execution order\n");
    std::vector<TraceGraph::vertex_descriptor> newVector;
    newVector.clear();
    try {
      newVector.push_back(currVertex);
      currOrder->insert(std::make_pair(BB, std::make_pair(-1, newVector)));
    } catch (std::exception &e) {
      std::cerr << "An error occured." << e.what() << "\n";
    }
  } else {
    // append to order
    try {
      DEBUG(*outputLog << "Appending BB " + BB->getName() + " to " << ordername
                       << " execution order\n");
      search->second.second.push_back(currVertex);
    } catch (std::exception &e) {
      std::cerr << "An error occured." << e.what() << "\n";
    }
  }
  // increment the node ID
  ID++;

  // set the latest added vertex
  lastVertex = currVertex;

  if (PerFunction) {
    DEBUG(*outputLog << "lululululu\n");
    DEBUG(*outputLog << (*lastTraceGraph)[lastVertex].name << "\n");
    DEBUG(*outputLog << "huhuhuhuhu\n");
  }

  return true;
}

// processes one line of input from trace of entering a function
bool AdvisorAnalysis::processFunctionEntry(
    const std::string &line, Function **function,
    TraceGraphList::iterator &latestTraceGraph,
    TraceGraph::vertex_descriptor &latestVertex,
    ExecutionOrderList::iterator &latestExecutionOrder,
    std::stack<FunctionExecutionRecord> &stack) {
  DEBUG(*outputLog << __func__ << " " << line << "\n");
  const char *delimiter = " ";

  // append to stack when entering a function from another calling function
  if (!PerFunction) {
    if (*function != NULL) {
      // keep track of caller
      FunctionExecutionRecord newRecord;
      newRecord.function = *function;
      newRecord.graph = latestTraceGraph;
      newRecord.vertex = latestVertex;
      newRecord.executionOrder = latestExecutionOrder;
      stack.emplace(newRecord);
    }
  }

  // make a non-const copy of line
  std::vector<char> lineCopy(line.begin(), line.end());
  lineCopy.push_back(0);

  // separate by space to get keywords
  //=----------------------------=//
  char *pch = std::strtok(&lineCopy[0], delimiter);
  // Entering
  pch = strtok(NULL, delimiter);
  // Function:
  pch = strtok(NULL, delimiter);
  // funcName
  std::string funcString(pch);
  //=----------------------------=//

  Function *F = find_function_by_name(funcString);
  if (!F) {
    // could not find function by name
    errs() << "Could not find the function from trace in program! "
           << funcString << "\n";
    return false;
  }
  *function = F;
  functionsSeen.insert(F);
  if (!PerFunction) {
    // nothing much to do here for global scheduling
    return true;
  }

  // append to stack when entering function
  // stack.push(F);

  // add to execution graph
  //==----------------------------------------------------------------==//
  ExecGraph::iterator fGraph = executionGraph.find(F);
  ExecutionOrderListMap::iterator fOrder = executionOrderListMap.find(F);
  if (fGraph == executionGraph.end() && fOrder == executionOrderListMap.end()) {
    // function does not exist as entry in execGraph
    TraceGraphList emptyList;
    executionGraph.insert(std::make_pair(F, emptyList));
    TraceGraph newGraph;
    try {
      executionGraph[F].push_back(newGraph);
      DEBUG(*outputLog << __func__ << " size of list: "
                       << executionGraph[F].size() << "\n");
      // update the latest trace graph created
      latestTraceGraph = executionGraph[F].end();
      latestTraceGraph--; // go to the last element in list
      assert(latestTraceGraph == executionGraph[F].begin());
    } catch (std::exception &e) {
      std::cerr << "An error occured." << e.what() << "\n";
    }

    ExecutionOrderList emptyOrderList;
    executionOrderListMap.insert(std::make_pair(F, emptyOrderList));
    ExecutionOrder newOrder;
    newOrder.clear();
    try {
      executionOrderListMap[F].push_back(newOrder);
      latestExecutionOrder = executionOrderListMap[F].end();
      latestExecutionOrder--;
      DEBUG(*outputLog << "11111\n");
      DEBUG(*outputLog << latestExecutionOrder->size() << "\n");
    } catch (std::exception &e) {
      std::cerr << "An error occured." << e.what() << "\n";
    }
  } else if (fGraph != executionGraph.end() &&
             fOrder != executionOrderListMap.end()) {
    // function exists
    TraceGraph newGraph;
    try {
      fGraph->second.push_back(newGraph);
      // update the latest trace graph created
      latestTraceGraph = fGraph->second.end();
      latestTraceGraph--; // go to the last element in list
    } catch (std::exception &e) {
      std::cerr << "An error occured." << e.what() << "\n";
    }

    ExecutionOrder newOrder;
    newOrder.clear();
    try {
      fOrder->second.push_back(newOrder);
      latestExecutionOrder = fOrder->second.end();
      latestExecutionOrder--;
    } catch (std::exception &e) {
      std::cerr << "An error occured." << e.what() << "\n";
    }
  } else {
    assert(0);
  }
  //==----------------------------------------------------------------==//
  return true;
}

void AdvisorAnalysis::getGlobalCPULatencyTable(
    Module &M, std::map<BasicBlock *, LatencyStruct> *LT,
    ExecutionOrder executionOrder, TraceGraph executionGraph) {
  *outputLog << __func__ << "\n";
  // traverse through each execution order
  // compute for each basic block across each execution
  for (auto &F : M) {
    for (auto &BB : F) {
      int iterCount = 0;
      float avgLatency = 0;
      auto search = (executionOrder).find(&BB);
      if (search == (executionOrder).end()) {
        // this basic block did not execute in this function call
        continue;
      }
      // average of all cpu executions of this basic block
      for (unsigned int i = 0; i < search->second.second.size(); i++) {
        int newElem = (executionGraph)[search->second.second[i]].cpuCycles;
        avgLatency = ((avgLatency * (float)iterCount) + (float)newElem) /
                     (float)(iterCount + 1);
        iterCount++;
      }
      // insert the entry

      // if basic block didn't exist, latency is just 0 ??
      // truncate to int
      int latency = (int)avgLatency;
      if (latency == 0) {
        latency++; // must be due to truncation
      }

      *outputLog << "Average Latency for basic block: " << BB.getName() << " "
                 << latency << "\n";

      // std::cerr << "Average Latency for basic block: " << BB.getName() << " "
      // << latency << "\n";

      auto mysearch = LT->find(&BB);
      assert(mysearch != LT->end());

      // Should we use the runtime latency or not.

      if (UseDynamicBlockRuntime) {
        mysearch->second.cpuLatency = latency;
      }
    }
  }

  *outputLog << "done\n";
}

void AdvisorAnalysis::getCPULatencyTable(
    Function *F, std::map<BasicBlock *, LatencyStruct> *LT,
    ExecutionOrderList &executionOrderList,
    TraceGraphList &executionGraphList) {
  *outputLog << __func__ << " for function: " << F->getName() << "\n";
  // traverse through each execution order
  ExecutionOrderList::iterator eol;
  TraceGraphList::iterator tgl;
  /*
  for (eol = executionOrderList.begin(), tgl = executionGraphList.begin();
                  eol != executionOrderList.end() && tgl !=
  executionOrderList.end(); eol++, tgl++) {
          // traverse through each basic block entry in the current execution
  order
          ExecutionOrder_iterator eo;
          for (eo = eol->begin(); eo != eol->end(); eo++) {
                  BasicBlock *BB = eo->first;
                  // look at each instance of execution of this basic block
                  for (auto inst = eo->second.begin(); inst != eo->second.end();
  inst++) {

                  }
          }
  }
  */

  // compute for each basic block across each execution
  for (auto &BB : *F) {
    int iterCount = 0;
    float avgLatency = 0;
    for (eol = (executionOrderList).begin(), tgl = (executionGraphList).begin();
         eol != (executionOrderList).end() && tgl != (executionGraphList).end();
         eol++, tgl++) {
      auto search = (*eol).find(&BB);
      if (search == (*eol).end()) {
        // this basic block did not execute in this function call
        continue;
      }
      // average of all cpu executions of this basic block
      for (unsigned int i = 0; i < search->second.second.size(); i++) {
        int newElem = (*tgl)[search->second.second[i]].cpuCycles;
        avgLatency = ((avgLatency * (float)iterCount) + (float)newElem) /
                     (float)(iterCount + 1);
        iterCount++;
      }
    }
    // insert the entry

    // if basic block didn't exist, latency is just 0 ??
    // truncate to int
    int latency = (int)avgLatency;
    if (latency == 0) {
      latency++; // must be due to truncation
    }

    *outputLog << "Average Latency for basic block: " << BB.getName() << " "
               << latency << "\n";

    // std::cerr << "Average Latency for basic block: " << BB.getName() << " "
    // << latency << "\n";

    auto search = LT->find(&BB);
    assert(search != LT->end());

    // Should we use the runtime latency or not.

    if (UseDynamicBlockRuntime) {
      search->second.cpuLatency = latency;
    }
  }

  *outputLog << "done\n";
}

/*
// TODO TODO TODO TODO TODO remember to check for external functions, I know
// you're going to forget this!!!!!!!!
bool AdvisorAnalysis::check_trace_sanity() {
        // run through the trace to see if these are valid paths
        // Here are the rules:
        //	1. The first basic block executed in the trace does not
necessarily
        //		have to be the entry block in the main program, we are
allowing
        //		traces to start somewhere arbitrarily
        //	2. For basicblock B executing after A within the same
function, B
        //		must be one of the successors of A
        //	3. For basicblock B executing after A across function
boundaries,
        //		we will use a stack to trace the function execution
order and
        //		the function A belongs to musts have been the caller
of the
        //		function B belongs to
        //std::stack<Function *> funcStack;
        std::string funcStr = executionTrace.front().first;
        std::string bbStr = executionTrace.front().second;

        BasicBlock *currBB = find_basicblock_by_name(funcStr, bbStr);
        if (!currBB) {
                // did not find basic block
                return false;
        }

        return trace_cfg(currBB);
}

// Function: trace_cfg
// Return: false if the dcfg trace does not follow the cfg
bool AdvisorAnalysis::trace_cfg(BasicBlock *startBB) {
        // TODO TODO TODO FIXME FIXME FIXME
        return true; // for now, do this function later
        // TODO TODO TODO FIXME FIXME FIXME
        // keep track of a function stack
        std::stack<Function *> funcStack;
        BasicBlock *currBB = startBB;

        // push the starting function onto stack
        funcStack.push_back(currBB->getParent());

        // iterate through trace
        for (auto it = executionTrace.begin(), et = executionTrace.end(); it !=
et; it++) {
                // check with stack that it is consistent
                if (strcmp(it->first, funcStack.top()->getName().str().c_str())
!= 0) {
                        // did we call another function? or did we return from a
function?!!
                }
        }
}
*/

// TODO: there's probably a much better way than this...
// TODO: but I need to show results!!!
// Function: find_basicblock_by_name
// Return: Pointer to the basic block NULL if not found
// Assumes: basic blocks are uniquely numbered so no duplicate names
//   across functions
BasicBlock *AdvisorAnalysis::find_basicblock_by_name(std::string bbName) {
  for (auto &F : *mod) {
    for (auto &BB : F) {
      if (strcmp(bbName.c_str(), BB.getName().str().c_str()) == 0) {
        return &BB;
      }
    }
  }
  return NULL;
}

// Function: find_function_by_name
// Return: Pointer to the function belonging to function, NULL if not found
Function *AdvisorAnalysis::find_function_by_name(std::string funcName) {
  for (auto &F : *mod) {
    if (strcmp(funcName.c_str(), F.getName().str().c_str()) != 0) {
      continue;
    }
    return &F;
  }
  return NULL;
}

// Function: find_maximal_configuration_for_module
// Return: true if successful, else false
// This function will find the maximum needed tiling for the entire module
// The parallelization factor will be stored in metadata for each basicblock
bool AdvisorAnalysis::find_maximal_configuration_for_module(
    Module &M, unsigned &fpgaOnlyLatency, unsigned &fpgaOnlyArea) {
  *outputLog << __func__ << "\n";
  ;
  bool scheduled = false;

  int unconstrainedLastCycle = -1;

  initialize_basic_block_instance_count_global(M);

  TraceGraphList::iterator fIt;
  ExecutionOrderList::iterator eoIt;
  // Define a resource table here. this will be expanded as we schedule the
  // graphs
  std::unordered_map<BasicBlock *, std::vector<unsigned>> resourceTable;
  resourceTable.clear();

  initialize_resource_table_global(M, &resourceTable, false);
  int lastCycle = -1;

  std::vector<TraceGraph::vertex_descriptor> rootVertices;
  rootVertices.clear();

  scheduled |= find_maximal_configuration_global(
      globalTraceGraph, globalExecutionOrder, rootVertices);
#if 0
	*outputLog << "Global Trace Graph.\n";
    print_trace_graph(globalTraceGraph);
#endif
  // find root vertices
  TraceGraph graph = *globalTraceGraph;

  // Schedule graph.
  // reset resource availability table
  for (auto itRT = resourceTable.begin(); itRT != resourceTable.end(); itRT++) {
    for (auto itRV = itRT->second.begin(); itRV != itRT->second.end(); itRV++) {
      *itRV = 0;
    }
  }

  std::cerr << "Scheduling without constraint globally" << std::endl;

  lastCycle += schedule_without_resource_constraints_global(globalTraceGraph,
                                                            &resourceTable);

  DEBUG(*outputLog << "Last Cycle: " << lastCycle << "\n");

  // after creating trace graphs, find maximal resources needed
  // to satisfy longest antichain

  // scheduled |= find_maximal_resource_requirement(F, fIt, rootVertices,
  // lastCycle);

  // use gradient descent method
  // modify_resource_requirement(F, fIt);

  // we have now found the best solution for the graph. We will update
  // the best possible configuration for the function.
  for (auto itRT = resourceTable.begin(); itRT != resourceTable.end(); itRT++) {
    int blockCount = itRT->second.size();
    BasicBlock *BB = itRT->first;

    std::cerr << " For Block " << BB->getName().str() << " from function "
              << BB->getParent()->getName().str()
              << " (area: " << ModuleAreaEstimator::getBasicBlockArea(*AT, BB)
              << ")"
              << " count is " << blockCount << std::endl;

    set_all_thread_pool_basic_block_instance_counts(BB, blockCount);
    set_basic_block_instance_count(BB, blockCount);
  }

  unconstrainedLastCycle = lastCycle;

  // keep this value for determining when to stop pursuing fpga accelerator
  // implementation
  fpgaOnlyLatency = unconstrainedLastCycle;
  fpgaOnlyArea = get_area_requirement_global(M);

  *outputFile << "Unconstrained schedule: " << unconstrainedLastCycle << "\n";
  *outputFile << "Area requirement: " << fpgaOnlyArea << "\n";
  std::cerr << "Unconstrained schedule: " << unconstrainedLastCycle << "\n";
  std::cerr << "Area requirement: " << fpgaOnlyArea << "\n";

  return scheduled;
}

// Function: find_maximal_configuration_for_all_calls
// Return: true if successful, else false
// This function will find the maximum needed tiling for a given function
// across all individual calls within the trace
// Does not look across function boundaries
// The parallelization factor will be stored in metadata for each basicblock
bool AdvisorAnalysis::find_maximal_configuration_for_all_calls(
    Function *F, unsigned &fpgaOnlyLatency, unsigned &fpgaOnlyArea) {
  *outputLog << __func__ << " for function " << F->getName() << "\n";
  ;
  // assert(executionTrace.find(F) != executionTrace.end());
  assert(executionGraph.find(F) != executionGraph.end());
  assert(executionOrderListMap.find(F) != executionOrderListMap.end());
  bool scheduled = false;

  int unconstrainedLastCycle = -1;

  initialize_basic_block_instance_count(F);

  // The ending condition should be determined by the user input of acceptable
  // area and latency constraints
  // while (1) {
  // iterate over all calls
  *outputLog << "There are " << executionGraph[F].size() << " calls to "
             << F->getName() << "\n";
  TraceGraphList::iterator fIt;
  ExecutionOrderList::iterator eoIt;
  // Define a resource table here. this will be expanded as we schedule the
  // graphs
  std::unordered_map<BasicBlock *, std::vector<unsigned>> resourceTable;
  resourceTable.clear();
  initialize_resource_table(F, &resourceTable, false);

  int lastCycle = -1;

  int callcount = 0;
  // JCB eoIT end check unnecessary?  AND it?  Curious about length of both
  // iterators
  for (fIt = executionGraph[F].begin(), eoIt = executionOrderListMap[F].begin();
       fIt != executionGraph[F].end(), eoIt != executionOrderListMap[F].end();
       fIt++, eoIt++) {

    std::vector<TraceGraph::vertex_descriptor> rootVertices;
    rootVertices.clear();

    callcount++;
    DEBUG(*outputLog << " Processing call number " << callcount << "\n");

    // This function really seems to annotate the graph with dependencies
    scheduled |=
        find_maximal_configuration_for_call(F, fIt, eoIt, rootVertices);
    // scheduled |= find_maximal_configuration_for_call(F, fIt, rootVertices);
    // after creating trace graphs representing maximal parallelism
    // compute maximal tiling
    // find_maximal_tiling_for_call(F, fIt);

    // find root vertices
    TraceGraph graph = *fIt;

    // Schedule graph.

    // reset resource availability table
    for (auto itRT = resourceTable.begin(); itRT != resourceTable.end();
         itRT++) {
      for (auto itRV = itRT->second.begin(); itRV != itRT->second.end();
           itRV++) {
        *itRV = 0;
      }
    }

    std::cerr << "Scheduling without constraint " << F->getName().str()
              << std::endl;

    lastCycle += schedule_without_resource_constraints(fIt, F, &resourceTable);

    DEBUG(*outputLog << "Last Cycle: " << lastCycle << "\n");

    // after creating trace graphs, find maximal resources needed
    // to satisfy longest antichain

    // scheduled |= find_maximal_resource_requirement(F, fIt, rootVertices,
    // lastCycle);

    // use gradient descent method
    // modify_resource_requirement(F, fIt);
  }

  // we have now found the best solution for the graph. We will update
  // the best possible configuration for the function.
  for (auto itRT = resourceTable.begin(); itRT != resourceTable.end(); itRT++) {
    int blockCount = itRT->second.size();
    BasicBlock *BB = itRT->first;

    std::cerr << " For Block " << BB->getName().str()
              << " (area: " << ModuleAreaEstimator::getBasicBlockArea(*AT, BB)
              << ")"
              << " count is " << blockCount << std::endl;

    set_all_thread_pool_basic_block_instance_counts(BB, blockCount);
    set_basic_block_instance_count(BB, blockCount);
  }

  unconstrainedLastCycle = lastCycle;

  //}

  // keep this value for determining when to stop pursuing fpga accelerator
  // implementation
  fpgaOnlyLatency = unconstrainedLastCycle;
  fpgaOnlyArea = get_area_requirement(F);

  *outputFile << "Unconstrained schedule: " << unconstrainedLastCycle << "\n";
  *outputFile << "Area requirement: " << fpgaOnlyArea << "\n";

  return scheduled;
}

// This
bool AdvisorAnalysis::find_maximal_configuration_for_call(
    Function *F, TraceGraphList::iterator graph,
    ExecutionOrderList::iterator execOrder,
    std::vector<TraceGraph::vertex_descriptor> &rootVertices) {
  DEBUG(*outputLog << __func__ << " for function " << F->getName() << "\n");
  // std::cerr << __func__ << " for function " << F->getName().str() << "\n";

  DEBUG(print_execution_order(execOrder));

  unsigned int totalNumVertices = boost::num_vertices(*graph);
  TraceGraph::vertex_iterator vi, ve;
  for (boost::tie(vi, ve) = boost::vertices(*graph); vi != ve; vi++) {
    TraceGraph::vertex_descriptor self = *vi;
    BasicBlock *selfBB = (*graph)[self].basicblock;
    DEBUG(*outputLog << "Inspecting vertex (" << self << "/" << totalNumVertices
                     << ") " << selfBB->getName() << "\n");
    BasicBlock *BB = find_basicblock_by_name(selfBB->getName());
    if (BB == NULL) {
      // now that we are ignoring 'dangling' basic blocks in
      // process_basic_block(), this case should not occur
      DEBUG(*outputLog << "WARNING bb " << selfBB->getName()
                       << " does not belong to " << F->getName() << "\n");
      assert(0);
      // continue;
    }

    // staticDeps vector keeps track of basic blocks that this basic block is
    // dependent on
    std::vector<BasicBlock *> staticDeps;
    staticDeps.clear();
    DependenceGraph::getAllBasicBlockDependencies(*functionDepGraph, selfBB,
                                                  staticDeps);

    // print out the static deps
    DEBUG(*outputLog << "Found number of static dependences: "
                     << staticDeps.size() << "\n");
    // for (auto sdi = staticDeps.begin(); sdi != staticDeps.end(); sdi++) {
    // DEBUG(*outputLog << "\tStatic dependence with: " << (*sdi)->getName() <<
    // "\n");
    //}

    // dynamicDeps vector keeps track of vertices in dynamic execution trace
    std::vector<TraceGraph::vertex_descriptor> dynamicDeps;
    dynamicDeps.clear();

    // fill the dynamicDeps vector by finding the most recent past execution of
    // the
    // dependent basic blocks in the dynamic trace
    for (auto sIt = staticDeps.begin(); sIt != staticDeps.end(); sIt++) {
      BasicBlock *depBB = *sIt;
      // find corresponding execution order vector
      auto search = (*execOrder).find(depBB);
      if (search == (*execOrder).end()) {
        // static dependence on basic block which was not executed in run
        continue;
      }
      // assert(search != (*execOrder).end());

      int currExec = search->second.first;
      std::vector<TraceGraph::vertex_descriptor> &execOrderVec =
          search->second.second;
      assert((int)currExec <= (int)execOrderVec.size());

      if (currExec < 0) {
        DEBUG(*outputLog << "Dependent basic block hasn't been executed yet. "
                         << depBB->getName() << "\n");
        // don't append dynamic dependence
      } else {
        // the dependent basic block has been executed before this basic block,
        // so possibly
        // need to add a dependence edge
        TraceGraph::vertex_descriptor dynDep = execOrderVec[currExec];

        if (!StaticDepsOnly) {
          bool dynamicDepExists =
              dynamic_memory_dependence_exists(self, dynDep, graph);
          bool trueDepExists = isBBDependenceTrue(
              (*graph)[self].basicblock, (*graph)[dynDep].basicblock,
              *functionDepGraph);
          DEBUG(*outputLog << "dynamicDepExists: " << dynamicDepExists << "\n");
          DEBUG(*outputLog << "trueDepExists: " << trueDepExists << "\n");
          if (!dynamicDepExists && !trueDepExists) {
            // don't add edge to node for which there are no true dependences
            // nor any dynamic memory dependences
            DEBUG(*outputLog << "Dynamic execution determined no true or "
                                "memory dependences between ");
            DEBUG(*outputLog << (*graph)[self].name << " (" << self << ") and "
                             << (*graph)[dynDep].name << " (" << dynDep
                             << ")\n");
            continue;
          }
        }

        dynamicDeps.push_back(dynDep);
      }
    }

    // remove redundant dynamic dependence entries
    // these are the dynamic dependences which another dynamic dependence is
    // directly
    // or indirectly dependent on

    //===-----------------------------------------------------------
    //------------===//
    //
    // I thought removal of these redundant dependencies would help performance
    // it *significantly* slowed down my analysis.. I am removing it for now.
    //
    //===-----------------------------------------------------------
    //------------===//
    // remove_redundant_dynamic_dependencies(graph, dynamicDeps);

    DEBUG(*outputLog << "Found number of dynamic dependences (after): "
                     << dynamicDeps.size() << "\n");

    // add dependency edges to graph
    for (auto it = dynamicDeps.begin(); it != dynamicDeps.end(); it++) {
      /*
      bool dynamicDepExists = dynamic_memory_dependence_exists(self, *it,
      graph);
      bool trueDepExists =
      DependenceGraph::is_basic_block_dependence_true((*graph)[self].basicblock,
      (*graph)[*it].basicblock, *depGraph);
      *outputLog << "dynamicDepExists: " << dynamicDepExists << "\n";
      *outputLog << "trueDepExists: " << trueDepExists << "\n";
      if (!StaticDepsOnly && !dynamicDepExists && !trueDepExists) {
              // don't add edge to node for which there are no true dependences
              // nor any dynamic memory dependences
              *outputLog << "Dynamic execution determined no true or memory
      dependences between ";
              *outputLog << (*graph)[self].name << " (" << self << ") and " <<
      (*graph)[*it].name << " (" << *it << ")\n";
              continue;
      }
      */

      DEBUG(*outputLog << "Dynamic execution determined true or memory "
                          "dependences EXIST between ");
      DEBUG(*outputLog << (*graph)[self].name << " (" << self << ") and "
                       << (*graph)[*it].name << " (" << *it << ")\n");
      if (!boost::edge(*it, self, *graph).second) { // avoid duplicates
        boost::add_edge(*it, self, *graph);
      }
    }

    // update the execution order index for current basic block after it has
    // been processed
    auto search = (*execOrder).find(selfBB);
    assert(search != (*execOrder).end());
    search->second.first++;
  }
  return true;
}

// This
bool AdvisorAnalysis::find_maximal_configuration_global(
    TraceGraphList::iterator graph, ExecutionOrderList::iterator execOrder,
    std::vector<TraceGraph::vertex_descriptor> &rootVertices) {
  DEBUG(*outputLog << __func__ << "\n");

  DEBUG(print_execution_order(execOrder));

  unsigned int totalNumVertices = boost::num_vertices(*graph);
  TraceGraph::vertex_iterator vi, ve;
  for (boost::tie(vi, ve) = boost::vertices(*graph); vi != ve; vi++) {
    TraceGraph::vertex_descriptor self = *vi;
    BasicBlock *selfBB = (*graph)[self].basicblock;
    DEBUG(*outputLog << "Inspecting vertex (" << self << "/" << totalNumVertices
                     << ") " << selfBB->getName() << "\n");
    // staticDeps vector keeps track of basic blocks that this basic block is
    // dependent on
    std::vector<BasicBlock *> staticDeps;
    staticDeps.clear();
    DependenceGraph::getAllBasicBlockDependencies(*globalDepGraph, selfBB,
                                                  staticDeps);

    // print out the static deps
    DEBUG(*outputLog << "Found number of static dependences: "
                     << staticDeps.size() << "\n");
    // for (auto sdi = staticDeps.begin(); sdi != staticDeps.end(); sdi++) {
    // DEBUG(*outputLog << "\tStatic dependence with: " << (*sdi)->getName() <<
    // "\n");
    //}

    // dynamicDeps vector keeps track of vertices in dynamic execution trace
    std::vector<TraceGraph::vertex_descriptor> dynamicDeps;
    dynamicDeps.clear();

    // fill the dynamicDeps vector by finding the most recent past execution of
    // the
    // dependent basic blocks in the dynamic trace
    for (auto sIt = staticDeps.begin(); sIt != staticDeps.end(); sIt++) {
      BasicBlock *depBB = *sIt;
      // find corresponding execution order vector
      auto search = (*execOrder).find(depBB);
      if (search == (*execOrder).end()) {
        // static dependence on basic block which was not executed in run
        continue;
      }
      // assert(search != (*execOrder).end());

      int currExec = search->second.first;
      std::vector<TraceGraph::vertex_descriptor> &execOrderVec =
          search->second.second;
      assert((int)currExec <= (int)execOrderVec.size());

      if (currExec < 0) {
        DEBUG(*outputLog << "Dependent basic block hasn't been executed yet. "
                         << depBB->getName() << "\n");
        // don't append dynamic dependence
      } else {
        // the dependent basic block has been executed before this basic block,
        // so possibly
        // need to add a dependence edge
        TraceGraph::vertex_descriptor dynDep = execOrderVec[currExec];

        if (!StaticDepsOnly) {
          bool dynamicDepExists =
              dynamic_memory_dependence_exists(self, dynDep, graph);
          bool trueDepExists = isBBDependenceTrue(
              (*graph)[self].basicblock, (*graph)[dynDep].basicblock,
              *globalDepGraph);
          DEBUG(*outputLog << "dynamicDepExists: " << dynamicDepExists << "\n");
          DEBUG(*outputLog << "trueDepExists: " << trueDepExists << "\n");
          if (!dynamicDepExists && !trueDepExists) {
            // don't add edge to node for which there are no true dependences
            // nor any dynamic memory dependences
            DEBUG(*outputLog << "Dynamic execution determined no true or "
                                "memory dependences between ");
            DEBUG(*outputLog << (*graph)[self].name << " (" << self << ") and "
                             << (*graph)[dynDep].name << " (" << dynDep
                             << ")\n");
            continue;
          }
        }

        dynamicDeps.push_back(dynDep);
      }
    }

    // remove redundant dynamic dependence entries
    // these are the dynamic dependences which another dynamic dependence is
    // directly
    // or indirectly dependent on

    //===-----------------------------------------------------------------------===//
    //
    // I thought removal of these redundant dependencies would help performance
    // it *significantly* slowed down my analysis.. I am removing it for now.
    //
    //===-----------------------------------------------------------------------===//
    // remove_redundant_dynamic_dependencies(graph, dynamicDeps);

    DEBUG(*outputLog << "Found number of dynamic dependences (after): "
                     << dynamicDeps.size() << "\n");

    // add dependency edges to graph
    for (auto it = dynamicDeps.begin(); it != dynamicDeps.end(); it++) {
      DEBUG(*outputLog << "Dynamic execution determined true or memory "
                          "dependences EXIST between ");
      DEBUG(*outputLog << (*graph)[self].name << " (" << self << ") and "
                       << (*graph)[*it].name << " (" << *it << ")\n");
      if (!boost::edge(*it, self, *graph).second) { // avoid duplicates
        boost::add_edge(*it, self, *graph);
      }
    }

    // update the execution order index for current basic block after it has
    // been processed
    auto search = (*execOrder).find(selfBB);
    assert(search != (*execOrder).end());
    search->second.first++;
  }
  return true;
}

/*
bool AdvisorAnalysis::true_dependence_exists(BasicBlock *BB1, BasicBlock *BB2) {
        // find corresponding edge in depGraph
        return DependenceGraph::is_basic_block_dependence_true(BB1, BB2,
depGraph);
}
*/

bool AdvisorAnalysis::dynamic_memory_dependence_exists(
    TraceGraph::vertex_descriptor child, TraceGraph::vertex_descriptor parent,
    TraceGraphList::iterator graph) {
  // examine each memory tuple between the two vertices
  // [1] compare parent store with child load RAW
  // [2] compare parent load with child store WAR
  // [3] compare parent store with child store WAW

  DEBUG(*outputLog
        << "determine if dynamic memory dependences exist between parent ("
        << parent << ") and child (" << child << ")\n");

  std::vector<std::pair<uint64_t, uint64_t>> &pWrite =
      (*graph)[parent].memoryWriteTuples;
  std::vector<std::pair<uint64_t, uint64_t>> &cWrite =
      (*graph)[child].memoryWriteTuples;
  std::vector<std::pair<uint64_t, uint64_t>> &pRead =
      (*graph)[parent].memoryReadTuples;
  std::vector<std::pair<uint64_t, uint64_t>> &cRead =
      (*graph)[child].memoryReadTuples;

  DEBUG(*outputLog << "Parent writes: " << pWrite.size() << "\n");
  DEBUG(*outputLog << "Parent reads: " << pRead.size() << "\n");
  DEBUG(*outputLog << "Child writes: " << cWrite.size() << "\n");
  DEBUG(*outputLog << "Child writes: " << cRead.size() << "\n");

  for (auto pwit = pWrite.begin(); pwit != pWrite.end(); pwit++) {
    for (auto cwit = cWrite.begin(); cwit != cWrite.end(); cwit++) {
      // [3]
      if (memory_accesses_conflict(*cwit, *pwit)) {
        DEBUG(*outputLog << "WAW conflict between : (" << pwit->first << ", "
                         << pwit->second);
        DEBUG(*outputLog << ") and (" << cwit->first << ", " << cwit->second
                         << ")\n";);
        return true;
      }
    }
    for (auto crit = cRead.begin(); crit != cRead.end(); crit++) {
      // [1]
      if (memory_accesses_conflict(*crit, *pwit)) {
        DEBUG(*outputLog << "RAW conflict between : (" << pwit->first << ", "
                         << pwit->second);
        DEBUG(*outputLog << ") and (" << crit->first << ", " << crit->second
                         << ")\n";);
        return true;
      }
    }
  }

  for (auto prit = pRead.begin(); prit != pRead.end(); prit++) {
    for (auto cwit = cWrite.begin(); cwit != cWrite.end(); cwit++) {
      // [2]
      if (memory_accesses_conflict(*cwit, *prit)) {
        DEBUG(*outputLog << "WAR conflict between : (" << prit->first << ", "
                         << prit->second);
        DEBUG(*outputLog << ") and (" << cwit->first << ", " << cwit->second
                         << ")\n";);
        return true;
      }
    }
  }

  return false;
}

bool AdvisorAnalysis::memory_accesses_conflict(
    std::pair<uint64_t, uint64_t> &access1,
    std::pair<uint64_t, uint64_t> &access2) {
  assert(access1.second > 0 && access2.second > 0);
  if (access1.first > access2.first) {
    if (access1.first < (access2.first + access2.second)) {
      return true;
    }
  } else if (access1.first < access2.first) {
    if (access2.first < (access1.first + access1.second)) {
      return true;
    }
  } else {
    return true;
  }

  return false;
}

void AdvisorAnalysis::print_execution_order(
    ExecutionOrderList::iterator execOrder) {
  *outputLog << "Execution Order: \n";
  for (auto it = (*execOrder).begin(); it != (*execOrder).end(); it++) {
    *outputLog << it->first->getName() << " ";
    for (auto eit = it->second.second.begin(); eit != it->second.second.end();
         eit++) {
      *outputLog << *eit << " ";
    }
    *outputLog << "\n";
  }
}

void AdvisorAnalysis::print_trace_graph(TraceGraphList::iterator traceGraph) {
  *outputLog << "Trace Graph: \n";
  // TraceGraphList_iterator fIt = globalTraceGraph;
  TraceGraph::vertex_iterator vi, ve;
  for (boost::tie(vi, ve) = boost::vertices(*traceGraph); vi != ve; vi++) {
    BasicBlock *BB = (*traceGraph)[*vi].basicblock;
    *outputLog << "vertex " << *vi << ": " << BB->getName().str() << "\n";
    *outputLog << "\tin-degree:" << in_degree(*vi, *traceGraph) << "\n";
    *outputLog << "\tout-degree:" << out_degree(*vi, *traceGraph) << "\n";
  }
  TraceGraph::edge_iterator edgeIt, edgeEnd;
  for (boost::tie(edgeIt, edgeEnd) = boost::edges(*traceGraph);
       edgeIt != edgeEnd; edgeIt++) {
    *outputLog << "edge " << source(*edgeIt, *traceGraph) << "-->"
               << target(*edgeIt, *traceGraph) << "\n";
  }
}

// sort trace graph vertex descriptor vector in reverse order
bool reverse_vertex_sort(TraceGraph::vertex_descriptor a,
                         TraceGraph::vertex_descriptor b) {
  return b < a;
}

// Function: remove_redundant_dynamic_dependencies
// Given a dynamic trace graph and a vector of vertices for which a executed
// basic block is
// dependent, remove the dependent vertices which are redundant. A redundant
// vertices are
// those which are depended on by other dependent vertices.
void AdvisorAnalysis::remove_redundant_dynamic_dependencies(
    TraceGraphList::iterator graph,
    std::vector<TraceGraph::vertex_descriptor> &dynamicDeps) {
  // sort in reverse order, may have more chance to find and remove redundancies
  // if
  // we start with vertices that executed later
  std::sort(dynamicDeps.begin(), dynamicDeps.end(), reverse_vertex_sort);

  for (auto it = dynamicDeps.begin(); it != dynamicDeps.end(); it++) {
    TraceGraph::vertex_descriptor v = *it;
    recursively_remove_redundant_dynamic_dependencies(graph, dynamicDeps, it,
                                                      v);
  }
}

void AdvisorAnalysis::recursively_remove_redundant_dynamic_dependencies(
    TraceGraphList::iterator graph,
    std::vector<TraceGraph::vertex_descriptor> &dynamicDeps,
    std::vector<TraceGraph::vertex_descriptor>::iterator search,
    TraceGraph::vertex_descriptor v) {
  // if v already exists as a parent/ancestor, remove from list because it is
  // redundant
  std::vector<TraceGraph::vertex_descriptor>::iterator found =
      std::find(search + 1, dynamicDeps.end(), v);
  if (found != dynamicDeps.end()) {
    dynamicDeps.erase(found);
  }

  // for each of its predecessors, recurse
  TraceGraph::in_edge_iterator ii, ie;
  for (boost::tie(ii, ie) = boost::in_edges(v, *graph); ii != ie; ii++) {
    TraceGraph::vertex_descriptor parent = boost::source(*ii, *graph);
    recursively_remove_redundant_dynamic_dependencies(graph, dynamicDeps,
                                                      search, parent);
  }
}

// Function: initialize_basic_block_instance_count
// Initializes the replication factor metadata for each basic block in function
// to zero
void AdvisorAnalysis::initialize_basic_block_instance_count(Function *F) {
  // delete anything left over from a previous run.

  for (auto it : threadPoolInstanceCounts) {
    free(it.second);
  }
  threadPoolInstanceCounts.clear();

  // first we must set up the threadpool structures.
  for (auto &BB : *F) {
    threadPoolInstanceCounts[&BB] = new std::unordered_map<BasicBlock *, int>();
    // Initialize to zero, since we used the thread-safe find in the set method.
    // Find needs something to 'find'.
    for (auto &zeroBB : *F) {
      threadPoolInstanceCounts[&BB]->insert(std::make_pair(&zeroBB, 0));
    }
  }

  for (auto &BB : *F) {
    // Initialize both the main count structure and the thread pool structure.
    set_basic_block_instance_count(&BB, 0);
  }
}

// Function: initialize_basic_block_instance_count_global
// Initializes the replication factor metadata for each basic block to zero
void AdvisorAnalysis::initialize_basic_block_instance_count_global(Module &M) {
  // delete anything left over from a previous run.

  for (auto it : threadPoolInstanceCounts) {
    free(it.second);
  }
  threadPoolInstanceCounts.clear();

  for (auto &F : M) {
    if (F.getBasicBlockList().begin() == F.getBasicBlockList().end()) {
      // "Function is external"
      continue;
    }
    // std::cerr << " Function " << F.getName().str() << "\n";
    for (auto &BB : F) {
      // first we must set up the threadpool structures.
      threadPoolInstanceCounts[&BB] =
          new std::unordered_map<BasicBlock *, int>();
      // Initialize to zero, since we used the thread-safe find in the set
      // method.
      // Find needs something to 'find'.
      for (auto &innerF : M) {
        for (auto &zeroBB : innerF) {
          threadPoolInstanceCounts[&BB]->insert(std::make_pair(&zeroBB, 0));
        }
      }
    }
#if 0
      std::cerr << " threadPoolInstanceCounts " << "\n";
      for (auto it = threadPoolInstanceCounts.begin(); it != threadPoolInstanceCounts.end(); it++) {
            std::cerr << " BB " << std::hex << it->first << "->[\n";
            for (auto iit = it->second->begin(); iit != it->second->end(); iit++) {
               std::cerr << "     BB " << std::hex << iit->first << "-->" << std::dec << iit->second << "\n";
            }
            std::cerr << "]\n";
      }
#endif
    for (auto &BB : F) {
      // Initialize both the main count structure and the thread pool structure.
      set_basic_block_instance_count(&BB, 0);
    }
  }
}

// Function: basicblock_is_dependent
// Return: true if child is dependent on parent and must execute after parent
bool AdvisorAnalysis::basicblock_is_dependent(BasicBlock *child,
                                              BasicBlock *parent,
                                              TraceGraph &graph) {
  // use dependence analysis to determine if basic block is dependent on another
  // we care about true dependencies and control flow dependencies only
  // true dependencies include any of these situations:
  // 	- if any instruction in the child block depends on an output produced
  // from the
  //	parent block
  //	- how do we account for loop dependencies??
  // compare each instruction in the child to the parent
  bool dependent = false;
  for (auto &cI : *child) {
    for (auto &pI : *parent) {
      dependent |= instruction_is_dependent(&cI, &pI);
    }
  }

  return dependent;
}

// Function: instruction_is_dependent
// Return: true if instruction inst1 is dependent on inst2 and must be executed
// after inst2
bool AdvisorAnalysis::instruction_is_dependent(Instruction *inst1,
                                               Instruction *inst2) {
  bool dependent = false;
  // handle different instruction types differently
  // namely, for stores and loads we need to consider memory dependence analysis
  // stufffff
  // flow dependence exists at two levels:
  //	1) inst1 directly consumes the output of inst2
  //		E.g.
  //			a = x + y
  //			b = load(a)
  //	Memory data dependence analysis:
  //	2) inst2 modifies memory which inst1 requires
  //		E.g.
  //			store(addr1, x)
  //			a = load(addr1)
  //	3) inst2 modifies a memory location which inst1 also modifies!?!?
  //		E.g.
  //			store(addr1, x)
  //			...
  //			store(addr2, y)
  //		Although, you could argue we would just get rid of the first
  // store
  // in this case
  //	4) inst1 modifies memory which inst2 first reads
  //		E.g.
  //			a = load(addr1)
  //			...
  //			store(addr1, x)
  // 1)
  if (true_dependence_exists(inst1, inst2)) {
    return true;
  }

  // only look at memory instructions
  // but don't care if both are loads
  if (inst1->mayReadOrWriteMemory() && inst2->mayReadOrWriteMemory() &&
      !(inst1->mayReadFromMemory() && inst2->mayReadFromMemory())) {
    *outputLog << "Looking at memory instructions: ";
    inst1->print(*outputLog);
    *outputLog << " & ";
    inst2->print(*outputLog);
    *outputLog << "\n";
    MemDepResult MDR = MDA->getDependency(inst1);
    if (Instruction *srcInst = MDR.getInst()) {
      if (srcInst == inst2) {
        *outputLog << "There is a memory dependence: ";
        inst1->print(*outputLog);
        *outputLog << " is dependent on ";
        srcInst->print(*outputLog);
        *outputLog << "\n";
        dependent |= true;
      } else {
        // inst1 is not dependent on inst2
      }
    } else {
      // Other:
      // Could be non-local to basic block => it is
      // Could be non-local to function
      // Could just be unknown
      if (MDR.isNonLocal()) {
        // this is what we expect...
        *outputLog << "Non-local dependency\n";
        /*
        MemDepResult nonLocalMDR = MDA->getDependency(inst1).getNonLocal();
        *outputLog << "---" << nonLocalMDR.isNonLocal() <<  " " <<
        MDR.isNonLocal() << "\n";
        if (Instruction *srcInst = nonLocalMDR.getInst()) {
                *outputLog << "Source of dependency: ";
                srcInst->print(*outputLog);
                *outputLog << "\n";
                if (srcInst == inst2) {
                        *outputLog << "There is a memory dependence: ";
                        inst1->print(*outputLog);
                        *outputLog << " is dependent on ";
                        srcInst->print(*outputLog);
                        *outputLog << "\n";
                        dependent |= true;
                }
        }*/

        SmallVector<NonLocalDepResult, 0> queryResult;
        MDA->getNonLocalPointerDependency(inst1, queryResult);
        // scan the query results to see if inst2 is in this set
        //*outputLog << "query result size " << queryResult.size() < "\n";
        for (SmallVectorImpl<NonLocalDepResult>::const_iterator qi =
                 queryResult.begin();
             qi != queryResult.end(); qi++) {
          // which basic block is this dependency originating from
          NonLocalDepResult NLDR = *qi;
          const MemDepResult nonLocalMDR = NLDR.getResult();

          *outputLog << "entry ";
          if (Instruction *srcInst = nonLocalMDR.getInst()) {
            srcInst->print(*outputLog);
            if (srcInst == inst2) {
              dependent |= true;
            }
          }
          *outputLog << "\n";
        }
      } else if (MDR.isNonFuncLocal()) {
        *outputLog << "Non-func-local dependency\n";
        // nothing.. this is fine
        // this is beyond our scope
      } else {
        *outputLog << "UNKNOWN\n";
        // unknown, so we will mark as dependent
        dependent |= true;
      }
    }
  }

  return dependent;
}

// Function: true_dependence_exists
// Returns: true if there is a flow dependence flowing from inst2 to inst1
// i.e. inst1 must execute after inst2
bool AdvisorAnalysis::true_dependence_exists(Instruction *inst1,
                                             Instruction *inst2) {
  // look at each operand of inst1
  User *user = dyn_cast<User>(inst1);
  assert(user);

  Value *val2 = dyn_cast<Value>(inst2);
  for (auto op = user->op_begin(); op != user->op_end(); op++) {
    Value *val1 = op->get();
    if (val1 == val2) {
      *outputLog << "True dependency exists: ";
      inst1->print(*outputLog);
      *outputLog << ", ";
      inst2->print(*outputLog);
      *outputLog << "\n";
      return true;
    }
  }

  return false;
}

// Function: basicblock_control_flow_dependent
// Return: true if child must execute after parent
// A child basic block must execute after the parent basic block if either
// 	1) parent does not unconditionally branch to child
//	2) child is not a dominator of parent
bool AdvisorAnalysis::basicblock_control_flow_dependent(BasicBlock *child,
                                                        BasicBlock *parent,
                                                        TraceGraph &graph) {

  TerminatorInst *TI = parent->getTerminator();
  BranchInst *BI = dyn_cast<BranchInst>(TI);
  if (BI && BI->isUnconditional() && (BI->getSuccessor(0) == child)) {
    *outputLog << "no control flow dependence " << parent->getName()
               << " uncond branch to " << child->getName() << "\n";
    return false;
  }

  // dominates -- do not use properlyDominates because it may be the same basic
  // block
  // check if child dominates parent
  if (DT->dominates(DT->getNode(child), DT->getNode(parent))) {
    *outputLog << "no control flow dependence " << child->getName()
               << " dominates " << parent->getName() << "\n";
    return false;
  }

  *outputLog << "control flow dependency exists. " << child->getName() << " & "
             << parent->getName() << "\n";

  return true;
}

void AdvisorAnalysis::find_new_parents(
    std::vector<TraceGraph::vertex_descriptor> &newParents,
    TraceGraph::vertex_descriptor child, TraceGraph::vertex_descriptor parent,
    TraceGraph &graph) {
  // std::cerr << __func__ << " parent: " << graph[parent].name << "\n";
  if (parent == child) {
    assert(0);
  }

  // find the corresponding vertices on the DG
  BasicBlock *childBB = graph[child].basicblock;
  BasicBlock *parentBB = graph[parent].basicblock;

  *outputLog << "Tracing through the execution graph -- child: "
             << childBB->getName() << " parent: " << parentBB->getName()
             << "\n";

  // if the child basic block only has one instruction and it is a
  // branch/control flow
  // instruction, it needs to remain in place FIXME???
  // if (isa<TerminatorInst>(childBB->getFirstNonPHI())) {
  //	newParents.push_back(parent);
  //	return;
  //}

  // if childBB can execute in parallel with parentBB i.e. childBB does not
  // depend on parentBB
  // then childBB can be moved up in the graph to inherit the parents of the
  // parentBB
  // this is done recursively until we find the final parents of the childBB
  // whose execution
  // the childBB *must* follow
  if (DependenceGraph::isBasicBlockDependent(childBB, parentBB,
                                             *functionDepGraph)) {
    DEBUG(*outputLog << "Must come after parent: " << parentBB->getName()
                     << "\n");
    if (std::find(newParents.begin(), newParents.end(), parent) ==
        newParents.end()) {
      newParents.push_back(parent);
    }
    return;
  } else {
    TraceGraph::in_edge_iterator ii, ie;
    for (boost::tie(ii, ie) = boost::in_edges(parent, graph); ii != ie; ii++) {
      TraceGraph::vertex_descriptor grandparent = boost::source(*ii, graph);
      find_new_parents(newParents, child, grandparent, graph);
    }
  }
}

// Function: annotate_schedule_for_call
// Return: true if successful, false otherwise
bool AdvisorAnalysis::annotate_schedule_for_call(
    Function *F, TraceGraphList::iterator graph_it, int &lastCycle) {
  // get the graph
  TraceGraphList::iterator graph = graph_it;
  // TraceGraph graph = *graph_it;

  // get the vertices which have no parents
  // these are the vertices that can be scheduled right away
  /*
  for (std::vector<TraceGraph_vertex_descriptor>::iterator rV =
  rootVertices.begin();
                  rV != rootVertices.end(); rV++) {
          ScheduleVisitor vis(graph, *LT, lastCycle);
          *outputLog << "bfs on root " << (*graph)[*rV].name << "\n";
          //boost::breadth_first_search(*graph, vertex(0, *graph),
  boost::visitor(vis).root_vertex(*rV));
          boost::depth_first_search(*graph,
  boost::visitor(vis).root_vertex(*rV));
  }*/

  // use depth first visit to discover all the vertices in the graph
  // do not need to give the root node of each disconnected subgraph
  // use dfs instead of bfs because bfs only traverses nodes through a graph
  // that is *reachable* from a starting node
  // also, since there are no resource constraints, each basic block
  // will be scheduled as early as possible, so no need for bfs here
  ScheduleVisitor vis(graph, this, *LT, lastCycle, SINGLE_THREAD_TID);

  boost::depth_first_search(*graph, boost::visitor(vis));

  // lastCycle += schedule_without_resource_constraints(graph, F, false);

  // for printing labels in graph output
  if (!HideGraph) {
    /*
    boost::dynamic_properties dpTG;
    //dpTG.property("label",
    boost::make_label_writer(boost::get(&BBSchedElem::name, *graph),
    boost::get(&BBSchedElem::minCycStart, *graph)));
    //dpTG.property("label", get(&BBSchedElem::name, *graph));
    dpTG.property("id", get(&BBSchedElem::ID, *graph));
    dpTG.property("start", get(&BBSchedElem::minCycStart, *graph));
    dpTG.property("end", get(&BBSchedElem::minCycEnd, *graph));
    boost::write_graphviz_dp(std::cerr, *graph, dpTG, std::string("id"));
    */
    TraceGraphVertexWriter<TraceGraph> vpw(*graph, this);
    TraceGraphEdgeWriter<TraceGraph> epw(*graph);
    std::string outfileName = "maximal_schedule." + F->getName().str() + ".dot";
    std::ofstream outfile(outfileName);
    boost::write_graphviz(outfile, *graph, vpw, epw);
  }

  return true;
}

// Function: find_maximal_resource_requirement
// Return: true if successful, false otherwise
bool AdvisorAnalysis::find_maximal_resource_requirement(
    Function *F, TraceGraphList::iterator graph_it,
    std::vector<TraceGraph::vertex_descriptor> &rootVertices, int lastCycle) {
  *outputLog << __func__ << "\n";

  // get the graph
  TraceGraphList::iterator graph = graph_it;
  // TraceGraph graph = *graph_it;

  // keep a chain of active basic blocks
  // at first, the active blocks are the roots (which start execution at cycle
  // 0)
  std::vector<TraceGraph::vertex_descriptor> antichain = rootVertices;

  // keep track of timestamp
  for (int timestamp = 0; timestamp < lastCycle; timestamp++) {
    DEBUG(*outputLog << "Examine Cycle: " << timestamp << "\n");
    // std::cerr << "Examine Cycle: " << timestamp << "\n";
    // activeBBs keeps track of the number of a particular
    // basic block resource that is needed to execute all
    // the basic blocks within the anti-chain for each given
    // cycle
    // it stores a pair of basic block ptr and an int representing
    // the number of that basic block needed
    std::map<BasicBlock *, int> activeBBs;
    activeBBs.clear();

    DEBUG(*outputLog << "anti-chain in cycle " << timestamp << ":\n");
    // look at all active basic blocks and annotate the IR
    // annotate annotate annotate
    for (auto it = antichain.begin(); it != antichain.end(); it++) {
      BasicBlock *BB = (*graph)[*it].basicblock;
      auto search = activeBBs.find(BB);
      if (search != activeBBs.end()) {
        // BB exists in activeBBs, increment count
        search->second++;
      } else {
        // else add it to activeBBs list
        activeBBs.insert(std::make_pair(BB, 1));
      }
      DEBUG(*outputLog << BB->getName() << "\n");
    }

    DEBUG(*outputLog << "activeBBs:\n");
    // update the IR
    // will store the replication factor of each basic block as metadata
    // could not find a way to directly attach metadata to each basic block
    // will instead attach to the terminator instruction of each basic block
    // this will be an issue if the basic block is merged/split...
    for (auto it = activeBBs.begin(); it != activeBBs.end(); it++) {
      DEBUG(*outputLog << it->first->getName() << " repfactor " << it->second
                       << "\n");
      // Instruction *inst = dyn_cast<Instruction>(it->first->getTerminator());
      // look at pre-existing replication factor
      // LLVMContext &C = inst->getContext();
      // MDNode *N = MDNode::get(C, MDString::get(C,
      // "FPGA_ADVISOR_REPLICATION_FACTOR"));
      // inst->setMetadata(repFactorStr, N);
      BasicBlock *currBB = (it->first);

      int repFactor = get_basic_block_instance_count(
          currBB); // zero indicates CPU execution
      repFactor = std::max(repFactor, it->second);

      set_basic_block_instance_count(currBB, repFactor);
    }

    DEBUG(*outputLog << ".\n");

// retire blocks which end this cycle and add their children
#if 0
		for (auto it = antichain.begin(); /*done in loop body*/; /*done in loop body*/) {
                  DEBUG(*outputLog << "1!!!\n");
			if (it == antichain.end()) {
				break;
			}
			*outputLog << *it << " 2!!!\n";
			if ((*graph)[*it].cycEnd == timestamp) {
			*outputLog << "3!!!\n";
				TraceGraph_out_edge_iterator oi, oe;
				TraceGraph_vertex_descriptor removed = *it;
				auto remove = it;
				it = antichain.erase(remove);
			*outputLog << "4!!!\n";
				for (boost::tie(oi, oe) = boost::out_edges(removed, *graph); oi != oe; oi++) {
			*outputLog << "5!!!\n";
					antichain.push_back(boost::target(*oi, *graph));
				}
				continue;
			}
			*outputLog << "6!!!\n";
			it++;
		}
#endif

    DEBUG(*outputLog << "antichain size: " << antichain.size() << "\n");
    std::vector<TraceGraph::vertex_descriptor> newantichain;
    newantichain.clear();
    for (auto it = antichain.begin(); it != antichain.end();) {
      DEBUG(*outputLog << *it << " s: " << (*graph)[*it].getMinStart()
                       << " e: " << (*graph)[*it].getMinEnd() << "\n");
      if ((*graph)[*it].getMinEnd() == timestamp) {
        // keep track of the children to add
        TraceGraph::out_edge_iterator oi, oe;
        for (boost::tie(oi, oe) = boost::out_edges(*it, *graph); oi != oe;
             oi++) {
          // designate the latest finishing parent to add child to antichain
          if (latest_parent(oi, graph)) {
            DEBUG(*outputLog << "new elements to add "
                             << boost::target(*oi, *graph));
            newantichain.push_back(boost::target(*oi, *graph));
          }
        }
        DEBUG(*outputLog << "erasing from antichain " << *it << "\n");
        it = antichain.erase(it);
      } else {
        it++;
      }
    }

    for (auto it = newantichain.begin(); it != newantichain.end(); it++) {
      DEBUG(*outputLog << "adding to antichain " << *it << "\n");
      antichain.push_back(*it);
    }

    DEBUG(*outputLog << "-\n");
  }
  return true;
}

// return true if this edge connects the latest finishing parent to the child
bool AdvisorAnalysis::latest_parent(TraceGraph::out_edge_iterator edge,
                                    TraceGraphList::iterator graph) {
  TraceGraph::vertex_descriptor thisParent = boost::source(*edge, *graph);
  TraceGraph::vertex_descriptor child = boost::target(*edge, *graph);
  TraceGraph::in_edge_iterator ii, ie;
  for (boost::tie(ii, ie) = boost::in_edges(child, *graph); ii != ie; ii++) {
    TraceGraph::vertex_descriptor otherParent = boost::source(*ii, *graph);
    if (otherParent == thisParent) {
      continue;
    }
    // designate to latest parent and also to parent whose vertex id is larger
    if ((*graph)[thisParent].getMinEnd() < (*graph)[otherParent].getMinEnd()) {
      return false;
    } else if (((*graph)[thisParent].getMinEnd() ==
                (*graph)[otherParent].getMinEnd()) &&
               thisParent < otherParent) {
      return false;
    }
  }
  return true;
}

// Function: find_optimal_configuration_for_all_calls
// Performs the gradient descent method for function F to find the optimal
// configuration of blocks on hardware vs. cpu
// Description of gradient descent method:
// With the gradient descent method we are trying to find the best configuration
// of basic blocks to be implemented on fpga and cpu such that we can achieve
// the
// best performance while satisfying the area constraints on an FPGA
// There are two goals of the optimization:
//	1) Fit the design on the hardware given some constraints
//	2) Maximize the performance
// We start from the maximal parallel configuration which implements the entire
// program
// on the fpga (as long as they can be implemented on hardware).
// If the design does not fit on the given resources, we find the basic block
// which
// contributes the least performance/area and remove it (remove an instance/push
// it
// onto cpu). We iterate this process until the design now "fits"
// If the design fits on the fpga, we now again use the gradient descent method
// to find blocks which contribute zero performance/area and remove them.
void AdvisorAnalysis::find_optimal_configuration_for_all_calls(
    Function *F, unsigned &cpuOnlyLatency, unsigned fpgaOnlyLatency,
    unsigned fpgaOnlyArea) {
  DEBUG(*outputLog << __func__ << "\n");
  assert(executionGraph.find(F) != executionGraph.end());

  // default hard-coded area constraint that means nothing

  bool done = false;

  // figure out the final latency when full cpu execution
  cpuOnlyLatency = get_cpu_only_latency(F);
  std::cerr << "CPU-only latency: " << cpuOnlyLatency << "\n";

  dumpBlockCounts(F, cpuOnlyLatency);

  // we care about area and delay
  unsigned area = UINT_MAX;
  // unsigned delay = UINT_MAX;

  std::cerr << F->getName().str() << "\n";

  // Build up various basic-block level data structures
  std::unordered_map<BasicBlock *, double> gradient;

  // clear out the prior resource table.
  for (auto tableIT = threadPoolResourceTables.begin();
       tableIT != threadPoolResourceTables.end(); tableIT++) {
    delete tableIT->second;
  }
  threadPoolResourceTables.clear();

  for (auto &BB : *F) {
    gradients[&BB] = new Gradient();
    gradient[&BB] = 0;
    std::unordered_map<BasicBlock *, std::vector<unsigned>> *resourceTable =
        new std::unordered_map<BasicBlock *, std::vector<unsigned>>();
    resourceTable->clear();
    initialize_resource_table(F, resourceTable, false);
    threadPoolResourceTables[&BB] = resourceTable;
  }

  int64_t initialLatency;
  int64_t previousLatency = fpgaOnlyLatency;

  while (!done) {
    ConvergenceCounter++; // for stats

    area = get_area_requirement(F);
    if (area > areaConstraint) {
      std::cerr << "Area constraint violated. Reduce area.\n\n\n\n\n";
      std::unordered_map<BasicBlock *, int> removeBB;
      int64_t deltaDelay = INT_MAX;
      bool cpuOnly = !incremental_gradient_descent(
          F, gradient, removeBB, deltaDelay, cpuOnlyLatency, fpgaOnlyLatency,
          fpgaOnlyArea, initialLatency);

      if (cpuOnly) {
        // decrement all basic blocks until cpu-only
        *outputLog << "[step] Remove all basic blocks\n";
        decrement_all_basic_block_instance_count_and_update_transition(F);
      } else {
        // decreme1nt_basic_block_instance_count(removeBB);

        decrease_basic_block_instance_count_and_update_transition(removeBB);
        // printout
        *outputLog << "Current basic block configuration.\n";
        print_basic_block_configuration(F, outputLog);
      }
    } else {
      // terminate the process if:
      // 1. removal of block results in increase in delay
      // 2. there are no blocks to remove

      *outputLog
          << "Area constraint satisfied, remove non performing blocks.\n";
      std::unordered_map<BasicBlock *, int> removeBB;
      int64_t deltaDelay = INT_MIN;
      incremental_gradient_descent(F, gradient, removeBB, deltaDelay,
                                   cpuOnlyLatency, fpgaOnlyLatency,
                                   fpgaOnlyArea, initialLatency);

      // only remove block if it doesn't negatively impact delay
      if (deltaDelay >= 0 && (removeBB.begin() != removeBB.end())) {
        // decrement_basic_block_instance_count(removeBB);
        decrease_basic_block_instance_count_and_update_transition(removeBB);
      }

      // printout
      *outputLog << "Current basic block configuration.\n";
      print_basic_block_configuration(F, outputLog);

      // [1]
      if (deltaDelay < 0) {
        done = true;
      }

      if (removeBB.begin() == removeBB.end()) {
        done = true;
      }
    }

    std::cerr << "CPU-Only Latency: " << cpuOnlyLatency
              << " FPGA Latency: " << fpgaOnlyLatency << std::endl;
    std::cerr << "Previous Latency: " << previousLatency
              << " Current Latency: " << initialLatency << " delta "
              << (initialLatency - previousLatency) << std::endl;
    previousLatency = initialLatency;
  }

  // print out final scheduling results and area
  unsigned finalLatency = 0;
  for (auto fIt = executionGraph[F].begin(); fIt != executionGraph[F].end();
       fIt++) {
    std::unordered_map<BasicBlock *, std::vector<unsigned>> resourceTable;
    resourceTable.clear();
    initialize_resource_table(F, &resourceTable, false);

    finalLatency += schedule_with_resource_constraints(fIt, F, &resourceTable,
                                                       SINGLE_THREAD_TID);
  }

  unsigned finalArea = get_area_requirement(F);

  std::cerr << "Function: " << F->getName().str() << "\n";
  std::cerr << "Implementation: \n";
  dumpImplementationCounts(F);
  std::cerr << "CPU-only latency: " << cpuOnlyLatency << "\n";
  std::cerr << "Accelerator Only Latency: " << fpgaOnlyLatency << "\n";
  std::cerr << "Accelerator Only Area: " << fpgaOnlyArea << "\n";
  std::cerr << "Final Latency: " << finalLatency << "\n";
  std::cerr << "Final Area: " << finalArea << "\n";
}

// Function: find_optimal_configuration_for_module
// Performs the gradient descent method for module M to find the optimal
// configuration of blocks on hardware vs. cpu
// Description of gradient descent method:
// With the gradient descent method we are trying to find the best configuration
// of basic blocks to be implemented on fpga and cpu such that we can achieve
// the
// best performance while satisfying the area constraints on an FPGA
// There are two goals of the optimization:
//	1) Fit the design on the hardware given some constraints
//	2) Maximize the performance
// We start from the maximal parallel configuration which implements the entire
// program
// on the fpga (as long as they can be implemented on hardware).
// If the design does not fit on the given resources, we find the basic block
// which
// contributes the least performance/area and remove it (remove an instance/push
// it
// onto cpu). We iterate this process until the design now "fits"
// If the design fits on the fpga, we now again use the gradient descent method
// to find blocks which contribute zero performance/area and remove them.
void AdvisorAnalysis::find_optimal_configuration_for_module(
    Module &M, unsigned &cpuOnlyLatency, unsigned fpgaOnlyLatency,
    unsigned fpgaOnlyArea) {
  DEBUG(*outputLog << __func__ << "\n");

  // default hard-coded area constraint that means nothing

  bool done = false;

  // figure out the final latency when full cpu execution
  cpuOnlyLatency = get_cpu_only_latency_global(M);
  std::cerr << "CPU-only latency: " << cpuOnlyLatency << "\n";

  dumpBlockCountsGlobal(cpuOnlyLatency);

  // we care about area and delay
  unsigned area = UINT_MAX;
  // unsigned delay = UINT_MAX;

  // Build up various basic-block level data structures
  std::unordered_map<BasicBlock *, double> gradient;

  // clear out the prior resource table.
  for (auto tableIT = threadPoolResourceTables.begin();
       tableIT != threadPoolResourceTables.end(); tableIT++) {
    delete tableIT->second;
  }
  threadPoolResourceTables.clear();

  for (auto &F : M) {
    for (auto &BB : F) {
      gradients[&BB] = new Gradient();
      gradient[&BB] = 0;
      std::unordered_map<BasicBlock *, std::vector<unsigned>> *resourceTable =
          new std::unordered_map<BasicBlock *, std::vector<unsigned>>();
      resourceTable->clear();
      initialize_resource_table_global(M, resourceTable, false);
      threadPoolResourceTables[&BB] = resourceTable;
    }
  }

  int64_t initialLatency;
  int64_t previousLatency = fpgaOnlyLatency;

  while (!done) {
    ConvergenceCounter++; // for stats

    area = get_area_requirement_global(M);
    if (area > areaConstraint) {
      std::cerr << "Area constraint violated. Reduce area.\n\n\n\n\n";
      std::unordered_map<BasicBlock *, int> removeBB;
      int64_t deltaDelay = INT_MAX;
      bool cpuOnly = !incremental_gradient_descent_global(
          M, gradient, removeBB, deltaDelay, cpuOnlyLatency, fpgaOnlyLatency,
          fpgaOnlyArea, initialLatency);

      if (cpuOnly) {
        // decrement all basic blocks until cpu-only
        *outputLog << "[step] Remove all basic blocks\n";
        decrement_all_basic_block_instance_count_and_update_transition_global(
            M);
      } else {
        // decreme1nt_basic_block_instance_count(removeBB);

        decrease_basic_block_instance_count_and_update_transition(removeBB);
        // printout
        *outputLog << "Current basic block configuration.\n";
        for (auto &F : M) {
          print_basic_block_configuration(&F, outputLog);
        }
      }
    } else {
      // terminate the process if:
      // 1. removal of block results in increase in delay
      // 2. there are no blocks to remove

      *outputLog
          << "Area constraint satisfied, remove non performing blocks.\n";
      std::unordered_map<BasicBlock *, int> removeBB;
      int64_t deltaDelay = INT_MIN;
      incremental_gradient_descent_global(M, gradient, removeBB, deltaDelay,
                                          cpuOnlyLatency, fpgaOnlyLatency,
                                          fpgaOnlyArea, initialLatency);

      // only remove block if it doesn't negatively impact delay
      if (deltaDelay >= 0 && (removeBB.begin() != removeBB.end())) {
        // decrement_basic_block_instance_count(removeBB);
        decrease_basic_block_instance_count_and_update_transition(removeBB);
      }

      // printout
      *outputLog << "Current basic block configuration.\n";
      for (auto &F : M) {
        print_basic_block_configuration(&F, outputLog);
      }

      // [1]
      if (deltaDelay < 0) {
        done = true;
      }

      if (removeBB.begin() == removeBB.end()) {
        done = true;
      }
    }

    std::cerr << "CPU-Only Latency: " << cpuOnlyLatency
              << " FPGA Latency: " << fpgaOnlyLatency << std::endl;
    std::cerr << "Previous Latency: " << previousLatency
              << " Current Latency: " << initialLatency << " delta "
              << (initialLatency - previousLatency) << std::endl;
    previousLatency = initialLatency;
  }

  // print out final scheduling results and area
  unsigned finalLatency = 0;
  auto fIt = globalTraceGraph;
  {
    std::unordered_map<BasicBlock *, std::vector<unsigned>> resourceTable;
    resourceTable.clear();
    initialize_resource_table_global(M, &resourceTable, false);

    finalLatency += schedule_with_resource_constraints_global(
        fIt, &resourceTable, SINGLE_THREAD_TID);
  }

  unsigned finalArea = get_area_requirement_global(M);

  std::cerr << "Implementation: \n";
  for (auto &F : M) {
    dumpImplementationCounts(&F);
  }
  std::cerr << "CPU-only latency: " << cpuOnlyLatency << "\n";
  std::cerr << "Accelerator Only Latency: " << fpgaOnlyLatency << "\n";
  std::cerr << "Accelerator Only Area: " << fpgaOnlyArea << "\n";
  std::cerr << "Final Latency: " << finalLatency << "\n";
  std::cerr << "Final Area: " << finalArea << "\n";
}

uint64_t rdtsc() {
  unsigned int lo, hi;
  __asm__ __volatile__("rdtsc" : "=a"(lo), "=d"(hi));
  return ((uint64_t)hi << 32) | lo;
}

// Function: incremental_gradient_descent
// Function will iterate through each basic block which has a hardware instance
// of more than 0
// to determine the change in delay with the removal of that basic block and
// finds the basic block
// whose contribution of delay/area is the least (closest to zero or negative)
bool AdvisorAnalysis::incremental_gradient_descent(
    Function *F, std::unordered_map<BasicBlock *, double> &gradient,
    std::unordered_map<BasicBlock *, int> &removeBBs, int64_t &deltaDelay,
    unsigned cpuOnlyLatency, unsigned fpgaOnlyLatency, unsigned fpgaOnlyArea,
    int64_t &initialLatency) {
  unsigned initialArea = get_area_requirement(F);
  *outputLog << "Initial area: " << initialArea << "\n";
  initialLatency = 0;

  BasicBlock *removeBB = NULL;

  uint64_t start = rdtsc();

  uint64_t finalArea = initialArea;

  int64_t finalDeltaLatency = 0;
  int64_t finalDeltaArea = 0;

  // this code must go away.
  // need to loop through all calls to function to get total latency
  for (auto fIt = executionGraph[F].begin(); fIt != executionGraph[F].end();
       fIt++) {

    std::unordered_map<BasicBlock *, std::vector<unsigned>> resourceTable;
    resourceTable.clear();
    initialize_resource_table(F, &resourceTable, false);

    initialLatency += schedule_with_resource_constraints(fIt, F, &resourceTable,
                                                         SINGLE_THREAD_TID);
  }

  // check to see if we should abandon search and opt for cpu only
  // implementation
  // This is the attempt to solve the local minima problem
  // The intuition behind this is that, given a latency-area curve and given
  // that
  // we know the solution for the accelerator only and cpu only implementations,
  // we have an idea of the projected performance that we should beat with the
  // accelerator-cpu implementation. If the performance of that is worse than
  // the projection and the accelerator area usage is low, we should abandon
  // the search and opt for cpu-only implementation instead.
  //
  //	|
  //	| * *
  //	|*   *
  //	|     *
  //	|      *
  //	|        *
  //	|            *
  //	|                     * *
  //	|____________________________
  //  c       a               f
  //
  //   point a is the point at which the projected performance intersects with
  //   the
  //   actual performance, to the left of point a, the performance of a
  //   cpu-accelerator
  //   mix will always perform worse than the cpu only
  uint64_t B = fpgaOnlyLatency;
  uint64_t dA = fpgaOnlyArea - initialArea;

  float m = (cpuOnlyLatency - fpgaOnlyLatency) / fpgaOnlyArea;
  float projectedPerformance = (m * dA) + B;
  DEBUG(*outputLog << "Projected Performance at area is "
                   << projectedPerformance << "\n");

  if ((initialLatency > (unsigned)projectedPerformance) &&
      initialArea < 100 /*hard coded...*/) {
    return false; // go to cpu only solution
  }

  // we will reuse resource table to avoid all those ugly calls to malloc.
  // Really, the table could be hoisted even higher, and that will come at some
  // point.
  std::unordered_map<BasicBlock *, std::vector<unsigned>> resourceTable;
  resourceTable.clear();
  initialize_resource_table(F, &resourceTable, false);

  // We need to maintain a list of those blocks which change latency.
  std::deque<BasicBlock *> blocks;

  /*{
    std::cerr << "Main Table " << std::endl;
    for (auto itRT = resourceTable.begin(); itRT != resourceTable.end(); itRT++)
  {
      std::cerr << "Block: " << itRT->first->getName().str() << " count: " <<
  itRT->second.second.size() << std::endl;
    }
  }*/

  for (auto &BB : *F) {
    // if (decrement_basic_block_instance_count(BB)) {
    auto search = resourceTable.find(&BB);
    std::vector<unsigned> &resourceVector = search->second;
    int count = resourceVector.size();
    if (count == 1) {
      blocks.push_back(&BB);
    } else if (count > 1) {
      blocks.push_front(&BB);
    }
  }

  uint64_t par_start = rdtsc();
  int jobCount = 0;
  // try removing each basic block
  for (auto itBB = blocks.begin(); itBB != blocks.end(); itBB++) {
    // if (decrement_basic_block_instance_count(BB)) {
    BasicBlock *BB = *itBB;
    auto search = resourceTable.find(BB);
    std::vector<unsigned> &resourceVector = search->second;
    int count = resourceVector.size();
    // std::cerr << "For job " << BB->getName().str() << "count is " << count <<
    // std::endl;
    // Check for at least one thread, so the single-threaded version does not
    // break.
    if (((count > 1) || ParallelizeOneZero) && (UseThreads > 0)) {
      // farm out a parallel job.
      // std::cerr << "Issuing parallel job for " << BB->getName().str() <<
      // std::endl;
      // Obtain structure pointers outside of the lambda scope so that
      // pass-by-value
      // gets the right type.
      auto gradientPointer = &gradient;
      group.run([=] {
        handle_basic_block_gradient(BB, gradientPointer, initialLatency,
                                    initialArea);
      });
      jobCount++;
    }
  }

  // make sure that all jobs have quiesced.
  group.wait();
  uint64_t par_finish = rdtsc();
  std::cerr << "Parallel region cycle count : " << (par_finish - par_start)
            << " Use Threads " << UseThreads << std::endl;
  if (jobCount > 0) {
    std::cerr << " By threads " << (par_finish - par_start) / jobCount
              << std::endl;
  }
  // These gradients are the single block ones.
  // They aren't so useful so we try to avoid computing them based on the result
  // of the current gradient.
  double min_utility = FLT_MAX;

  for (auto it = gradient.begin(); it != gradient.end(); it++) {
    std::cerr << "gradient " << it->first->getName().str() << " count "
              << get_basic_block_instance_count(it->first) << " utility "
              << it->second << std::endl;
    if ((it->second < min_utility) &&
        (get_basic_block_instance_count(it->first) > 0) && (it->second != 0)) {
      removeBB = it->first;
      min_utility = it->second;
      std::cerr << "Setting min utility " << removeBB->getName().str()
                << " count " << get_basic_block_instance_count(it->first)
                << " utility " << min_utility << std::endl;
    }
  }

  int seqCount = 0;
  uint64_t serial_start = rdtsc();
  if (!ParallelizeOneZero) {
    for (auto itBB = blocks.begin(); itBB != blocks.end(); itBB++) {
      // if (decrement_basic_block_instance_count(BB)) {
      BasicBlock *BB = *itBB;
      auto search = resourceTable.find(BB);
      std::vector<unsigned> &resourceVector = search->second;
      int count = resourceVector.size();
      // Check gradients to see if we need to recalculate.
      // In this case, we use the last gradient we
      // calculated as a guess. If it is not projected to be
      // useful, we don't recalculate.

      if ((count == 1) || (UseThreads == 1)) {
        if ((gradient[BB] == 0) ||
            (gradient[BB] < SerialGradientCutoff * min_utility)) {
          std::cerr << "Serial job for " << BB->getName().str() << std::endl;
          seqCount++;
          handle_basic_block_gradient(BB, &gradient, initialLatency,
                                      initialArea);
        } else if (SerialGradientCutoff == 0) {
          std::cerr << "Serial job for " << BB->getName().str() << std::endl;
          seqCount++;
          handle_basic_block_gradient(BB, &gradient, initialLatency,
                                      initialArea);
        } else {
          std::cerr << "Did not recompute gradient " << BB->getName().str()
                    << std::endl;
        }
      }
    }
  }
  uint64_t serial_finish = rdtsc();
  std::cerr << "Serial region cycle count: " << (serial_finish - serial_start);
  if (seqCount > 0) {
    std::cerr << " By threads " << (serial_finish - serial_start) / seqCount
              << std::endl;
  }

  uint64_t finish = rdtsc();

  // Decide how far to step in the gradient direction depends on where we are.

  std::unordered_map<BasicBlock *, double> coefs;
  double alpha;

  // set the 'removeBB' target to be the least useful block.
  min_utility = FLT_MAX;
  for (auto it = gradient.begin(); it != gradient.end(); it++) {
    std::cerr << "gradient " << it->first->getName().str() << " count "
              << get_basic_block_instance_count(it->first) << " utility "
              << it->second << std::endl;
    if ((it->second < min_utility) &&
        (get_basic_block_instance_count(it->first) > 0)) {
      removeBB = it->first;
      min_utility = it->second;
      std::cerr << "Setting min utility " << removeBB->getName().str()
                << " count " << get_basic_block_instance_count(it->first)
                << " utility " << min_utility << std::endl;
    }
  }

  // Rapid gradient descent method #1.  Removes area until partial derviatives
  // start to become unreliable.
  // models partial derivatives as 1/k^2 (Amdahl's Law).
  if (MaxDerivativeError) {

    double area_threshold = (initialArea - areaConstraint) / 2.0 +
                            areaConstraint - 10.0; // Try to remove half of the
                                                   // remaining area, but add a
                                                   // little extra.

    // if we ran out of thresholds, just remove one unit of area.
    if (area_threshold < 0) {
      area_threshold = 1.0;
    }

    // Now we must solve the linear combination to reduce area
    // by the required amount. We view the gradient coeffiencients
    // as a determining the ratio of blocks to remove.
    double sum = 0.0;
    double max_coef = 0.0;
    int max_area = 0;

    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      if (get_basic_block_instance_count(it->first) > 0) {
        double coef = 1 / (it->second + FLT_MIN);
        coefs[it->first] = coef;
        if (max_coef < coef) {
          max_coef = coef;
          max_area = ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
        }
      } else {
        // can't remove blocks that aren't there.
        removeBBs[it->first] = 0;
      }
    }

    // scale alpha such that we remove least enough blocks of
    // the largest type to get the area we care about.  We must
    // take care to ensure the we will remove at least one
    // block.  find a power of two that encompasses maximum
    // number of blocks we will remove.
    int max_count =
        (max_area) ? ((std::max(max_area, (int)area_threshold) / max_area) + 1)
                   : 0;
    int max_power = 1;
    while (max_power < max_count) {
      max_power <<= 1;
    }

    alpha = std::max((double)max_area, area_threshold) / (max_coef * max_area);

    std::cerr << "Alpha: " << alpha << "\n";
    std::cerr << "initial area: " << initialArea << "\n";
    std::cerr << "max coef: " << max_coef << "\n";
    std::cerr << "max area: " << max_area << "\n";
    std::cerr << "max count: " << max_count << "\n";
    std::cerr << "max power: " << max_power << "\n";
    std::cerr << "Area_threshold: " << area_threshold << "\n";
    std::cerr << "Sum: " << sum << "\n";

    // If the convergence distance is very small, we may not
    // find a block to remove.  We track this and force the
    // removal of the marginal block if no other blocks are
    // removed.
    bool foundNonZero = false;

    // track whether we violated the derivative max error.
    bool violatedMaxDerivativeError = false;
    // Multiply coefs to obtain block counts. Need to adjust alpha up to deal
    // with need to floor.
    double area_removed_floor = 0;
    double area_removed = 0;

    // We get some estimate of alpha.  Since we are doing
    // rounding and also cannot reduce block counts beneath 0,
    // we need to massage alpha to cut the right number of
    // blocks.  Essentially, we will do a binary search to find
    // the value of alpha that does the right thing.

    // Now, we set up a search to find the 'right' value of alpha.
    double alpha_step = 1.0;
    double alpha_scaler = 2 * alpha_step;
    double alpha_step_cutoff = 1.0 / (max_power * 128); // go a few extra steps.
    double last_passing_step = -1; // If we don't every find a
                                   // passing step, we should
                                   // take the last value of the scaler.

    double alpha_prime = alpha * alpha_scaler;

    // iterate until we find a passing value. Here passing is defined by the
    // maximum area that does
    // not violate the MaxDerivativeError
    do {
      foundNonZero = false;
      violatedMaxDerivativeError = false;
      area_removed_floor = 0;
      area_removed = 0;
      for (auto it = gradient.begin(); it != gradient.end(); it++) {
        // handle the case of already eliminated blocks
        int block_count = get_basic_block_instance_count(it->first);

        removeBBs[it->first] =
            std::max(0, std::min(block_count,
                                 int(floor(coefs[it->first] * alpha_prime))));

        // Check to see if we violated the derivative error bound
        // 2nd derivative of Amdahl's is 2/x^3
        // Are we removing a block?
        if (removeBBs[it->first] > 1) {
          // If we are removing a block, are we removing too many?
          int finalCount = block_count - removeBBs[it->first];
          double derivativeDelta = 1.0;

          if (finalCount != 0) {
            derivativeDelta = 1.0 / (finalCount * finalCount) -
                              1.0 / (block_count * block_count);
          }
          std::cerr << it->first->getName().str()
                    << "derivative delta: " << derivativeDelta << std::endl;
          if (derivativeDelta > MaxDerivativeError) {
            violatedMaxDerivativeError = true;
          }
        }

        if (floor(coefs[it->first] * alpha_prime) > .5) {
          foundNonZero = true;
        }
        // need to check for removal of more blocks than actually exist.
        area_removed_floor +=
            std::max(0, std::min(block_count,
                                 (int)floor(coefs[it->first] * alpha_prime))) *
            ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
        area_removed += coefs[it->first] * alpha_prime *
                        ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
      }

      std::cerr << "Alpha scaler: " << alpha_scaler << "Eliminated "
                << area_removed_floor << " units of area rounded from "
                << area_removed << "needed: " << area_threshold << std::endl;

      // Back off if we either moved too far in the gradient, or we
      // took away too much area.
      if (violatedMaxDerivativeError ||
          (areaConstraint > (initialArea - area_removed_floor))) {
        last_passing_step = alpha_prime;
        alpha_scaler = alpha_scaler - alpha_step;
      } else {
        alpha_scaler = alpha_scaler + alpha_step;
      }

      alpha_step = alpha_step / 2;
      alpha_prime = alpha * alpha_scaler;

    } while (alpha_step > alpha_step_cutoff);

    // Just in case we didn't find any steps that pass, assign some default.
    if (last_passing_step < 0) {
      last_passing_step = alpha_prime;
    }

    // use last passing step to set the removal vector.

    area_removed_floor = 0;
    area_removed = 0;
    foundNonZero = false;
    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      int block_count = get_basic_block_instance_count(it->first);
      // handle the case of already eliminated blocks

      removeBBs[it->first] =
          std::max(0, std::min(block_count, (int)floor(coefs[it->first] *
                                                       last_passing_step)));

      if (floor(coefs[it->first] * last_passing_step) > 1.0) {
        foundNonZero = true;
      }

      std::cerr << it->first->getName().str() << ", " << it->second << ", "
                << ModuleAreaEstimator::getBasicBlockArea(*AT, it->first)
                << ", " << get_basic_block_instance_count(it->first)
                << " removing " << removeBBs[it->first] << " -> "
                << (get_basic_block_instance_count(it->first) -
                    removeBBs[it->first])
                << "remain\n";
    }

    // sometimes, we eliminate too much area.  We should
    // consider iterating over the removed blocks. There's an
    // argument that this is not energy efficient?

    // Ensure that we remove at least one block. Given that we are bumping
    // alpha, this may not be needed.
    if (!foundNonZero) {
      removeBBs[removeBB] = 1;
    }

  }
  // Rapid gradient descent method #1.  Uses an area schedule to limit the
  // number of steps
  // in the gradient descent process.
  else if (RapidConvergence && (thresholds.size() > 0) &&
           (initialArea > areaConstraint)) {
    // assume that we will remove half of the area in each step.
    double area_threshold;
    double target_threshold;
    do {
      target_threshold = thresholds.back();
      area_threshold = initialArea - target_threshold;
      thresholds.pop_back();
    } while ((area_threshold < 0) && (thresholds.size() > 0));

    // if we ran out of thresholds, just remove one block.
    if (area_threshold < 0) {
      area_threshold = 1.0;
    }

    // Now we must solve the linear combination to reduce area
    // by the required amount. We view the gradient coeffiencients
    // as a determining the ratio of blocks to remove.
    double sum = 0.0;
    double max_coef = 0.0;
    int max_area = 0;

    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      if (get_basic_block_instance_count(it->first) > 0) {
        double coef = 1 / (it->second + FLT_MIN);
        coefs[it->first] = coef;
        if (max_coef < coef) {
          max_coef = coef;
          max_area = ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
        }
      } else {
        // can't remove blocks that aren't there.
        removeBBs[it->first] = 0;
      }
    }

    // scale alpha such that we remove least enough blocks of
    // the largest type to get the area we care about.  We must
    // take care to ensure the we will remove at least one
    // block.  find a power of two that encompasses maximum
    // number of blocks we will remove.
    int max_count = (std::max(max_area, (int)area_threshold) / max_area) + 1;
    int max_power = 1;
    while (max_power < max_count) {
      max_power <<= 1;
    }

    alpha = std::max((double)max_area, area_threshold) / (max_coef * max_area);

    std::cerr << "Alpha: " << alpha << "\n";
    std::cerr << "initial area: " << initialArea << "\n";
    std::cerr << "max coef: " << max_coef << "\n";
    std::cerr << "max area: " << max_area << "\n";
    std::cerr << "max count: " << max_count << "\n";
    std::cerr << "max power: " << max_power << "\n";
    std::cerr << "target  area: " << target_threshold << "\n";
    std::cerr << "Area_threshold: " << area_threshold << "\n";
    std::cerr << "Sum: " << sum << "\n";

    // If the convergence distance is very small, we may not
    // find a block to remove.  We track this and force the
    // removal of the marginal block if no other blocks are
    // removed.
    bool foundNonZero = false;

    // Multiply coefs to obtain block counts. Need to adjust alpha up to deal
    // with need to floor.
    double area_removed_floor = 0;
    double area_removed = 0;

    // We get some estimate of alpha.  Since we are doing
    // rounding and also cannot reduce block counts beneath 0,
    // we need to massage alpha to cut the right number of
    // blocks.  Essentially, we will do a binary search to find
    // the value of alpha that does the right thing.

    // Now, we set up a search to find the 'right' value of alpha.
    double alpha_step = 1.0;
    double alpha_scaler = 2 * alpha_step;
    double alpha_step_cutoff = 1.0 / (max_power * 128); // go a few extra steps.
    double last_passing_step = -1; // If we don't every find a
                                   // passing step, we should
                                   // take the last value of the scaler.

    double alpha_prime = alpha * alpha_scaler;

    // this doesn't need to be an iterative loop, probably.
    do {
      foundNonZero = false;
      area_removed_floor = 0;
      area_removed = 0;
      for (auto it = gradient.begin(); it != gradient.end(); it++) {
        // handle the case of already eliminated blocks
        int block_count = get_basic_block_instance_count(it->first);

        removeBBs[it->first] =
            std::max(0, std::min(block_count,
                                 int(floor(coefs[it->first] * alpha_prime))));

        if (floor(coefs[it->first] * alpha_prime) > .5) {
          foundNonZero = true;
        }
        // need to check for removal of more blocks than actually exist.
        area_removed_floor +=
            std::max(0, std::min(block_count,
                                 (int)floor(coefs[it->first] * alpha_prime))) *
            ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
        area_removed += coefs[it->first] * alpha_prime *
                        ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
      }

      std::cerr << "Alpha scaler: " << alpha_scaler << "Eliminated "
                << area_removed_floor << " units of area rounded from "
                << area_removed << "needed: " << area_threshold << std::endl;

      if (area_removed_floor > area_threshold) {
        last_passing_step = alpha_prime;
        alpha_scaler = alpha_scaler - alpha_step;
      } else {
        alpha_scaler = alpha_scaler + alpha_step;
      }

      alpha_step = alpha_step / 2;
      alpha_prime = alpha * alpha_scaler;

    } while (alpha_step > alpha_step_cutoff);

    // Just in case we didn't find any steps that pass, assign some default.
    if (last_passing_step < 0) {
      last_passing_step = alpha_prime;
    }

    // use last passing step to set the removal vector.

    area_removed_floor = 0;
    area_removed = 0;
    foundNonZero = false;
    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      int block_count = get_basic_block_instance_count(it->first);
      // handle the case of already eliminated blocks

      removeBBs[it->first] =
          std::max(0, std::min(block_count, (int)floor(coefs[it->first] *
                                                       last_passing_step)));

      if (floor(coefs[it->first] * last_passing_step) > 1.0) {
        foundNonZero = true;
      }

      std::cerr << it->first->getName().str() << ", " << it->second << ", "
                << ModuleAreaEstimator::getBasicBlockArea(*AT, it->first)
                << ", " << get_basic_block_instance_count(it->first)
                << " removing " << removeBBs[it->first] << " -> "
                << (get_basic_block_instance_count(it->first) -
                    removeBBs[it->first])
                << "remain\n";
    }

    // sometimes, we eliminate too much area.  We should
    // consider iterating over the removed blocks. There's an
    // argument that this is not energy efficient?

    // Ensure that we remove at least one block. Given that we are bumping
    // alpha, this may not be needed.
    if (!foundNonZero) {
      removeBBs[removeBB] = 1;
    }
  } else {

    // Just do one step here.
    if (removeBB != NULL) {
      removeBBs[removeBB] = 1;
    }

    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      std::cerr << it->first->getName().str() << " gradient: " << it->second
                << " area: "
                << ModuleAreaEstimator::getBasicBlockArea(*AT, it->first)
                << " count: " << get_basic_block_instance_count(it->first)
                << '\n';
    }
  }

  std::cerr << "Descent Step: " << finalArea << " ( " << finalDeltaArea << " ) "
            << "initial latency: " << initialLatency << " ( "
            << finalDeltaLatency << " ) in " << finish - start << " cycles"
            << std::endl;
  // deltaDelay = finalDeltaLatency;
  return true; // not going to cpu only solution
}

// Function: incremental_gradient_descent_global
// Function will iterate through each basic block which has a hardware instance
// of more than 0
// to determine the change in delay with the removal of that basic block and
// finds the basic block
// whose contribution of delay/area is the least (closest to zero or negative)
bool AdvisorAnalysis::incremental_gradient_descent_global(
    Module &M, std::unordered_map<BasicBlock *, double> &gradient,
    std::unordered_map<BasicBlock *, int> &removeBBs, int64_t &deltaDelay,
    unsigned cpuOnlyLatency, unsigned fpgaOnlyLatency, unsigned fpgaOnlyArea,
    int64_t &initialLatency) {
  unsigned initialArea = get_area_requirement_global(M);
  *outputLog << "Initial area: " << initialArea << "\n";
  initialLatency = 0;

  BasicBlock *removeBB = NULL;

  uint64_t start = rdtsc();

  uint64_t finalArea = initialArea;

  int64_t finalDeltaLatency = 0;
  int64_t finalDeltaArea = 0;

  // this code must go away.
  TraceGraphList::iterator fIt = globalTraceGraph;
  {

    std::unordered_map<BasicBlock *, std::vector<unsigned>> resourceTable;
    resourceTable.clear();
    initialize_resource_table_global(M, &resourceTable, false);

    initialLatency += schedule_with_resource_constraints_global(
        fIt, &resourceTable, SINGLE_THREAD_TID);
  }

  // check to see if we should abandon search and opt for cpu only
  // implementation
  // This is the attempt to solve the local minima problem
  // The intuition behind this is that, given a latency-area curve and given
  // that
  // we know the solution for the accelerator only and cpu only implementations,
  // we have an idea of the projected performance that we should beat with the
  // accelerator-cpu implementation. If the performance of that is worse than
  // the projection and the accelerator area usage is low, we should abandon
  // the search and opt for cpu-only implementation instead.
  //
  //	|
  //	| * *
  //	|*   *
  //	|     *
  //	|      *
  //	|        *
  //	|            *
  //	|                     * *
  //	|____________________________
  //  c       a               f
  //
  //   point a is the point at which the projected performance intersects with
  //   the
  //   actual performance, to the left of point a, the performance of a
  //   cpu-accelerator
  //   mix will always perform worse than the cpu only
  uint64_t B = fpgaOnlyLatency;
  uint64_t dA = fpgaOnlyArea - initialArea;

  float m = (cpuOnlyLatency - fpgaOnlyLatency) / fpgaOnlyArea;
  float projectedPerformance = (m * dA) + B;
  DEBUG(*outputLog << "Projected Performance at area is "
                   << projectedPerformance << "\n");

  if ((initialLatency > (unsigned)projectedPerformance) &&
      initialArea < 100 /*hard coded...*/) {
    return false; // go to cpu only solution
  }

  // we will reuse resource table to avoid all those ugly calls to malloc.
  // Really, the table could be hoisted even higher, and that will come at some
  // point.
  std::unordered_map<BasicBlock *, std::vector<unsigned>> resourceTable;
  resourceTable.clear();
  initialize_resource_table_global(M, &resourceTable, false);

  // We need to maintain a list of those blocks which change latency.
  std::deque<BasicBlock *> blocks;

  /*{
    std::cerr << "Main Table " << std::endl;
    for (auto itRT = resourceTable.begin(); itRT != resourceTable.end(); itRT++)
  {
      std::cerr << "Block: " << itRT->first->getName().str() << " count: " <<
  itRT->second.second.size() << std::endl;
    }
  }*/

  for (auto &F : M) {
    for (auto &BB : F) {
      auto search = resourceTable.find(&BB);
      std::vector<unsigned> &resourceVector = search->second;
      int count = resourceVector.size();
      if (count == 1) {
        blocks.push_back(&BB);
      } else if (count > 1) {
        blocks.push_front(&BB);
      }
    }
  }

  uint64_t par_start = rdtsc();
  int jobCount = 0;
  // try removing each basic block
  for (auto itBB = blocks.begin(); itBB != blocks.end(); itBB++) {
    // if (decrement_basic_block_instance_count(BB)) {
    BasicBlock *BB = *itBB;
    auto search = resourceTable.find(BB);
    std::vector<unsigned> &resourceVector = search->second;
    int count = resourceVector.size();
    // std::cerr << "For job " << BB->getName().str() << "count is " << count <<
    // std::endl;
    // Check for at least one thread, so the single-threaded version does not
    // break.
    if (((count > 1) || ParallelizeOneZero) && (UseThreads > 0)) {
      // farm out a parallel job.
      // std::cerr << "Issuing parallel job for " << BB->getName().str() <<
      // std::endl;
      // Obtain structure pointers outside of the lambda scope so that
      // pass-by-value
      // gets the right type.
      auto gradientPointer = &gradient;
      group.run([=] {
        handle_basic_block_gradient(BB, gradientPointer, initialLatency,
                                    initialArea);
      });
      jobCount++;
    }
  }

  // make sure that all jobs have quiesced.
  group.wait();
  uint64_t par_finish = rdtsc();
  std::cerr << "Parallel region cycle count : " << (par_finish - par_start)
            << " Use Threads " << UseThreads << std::endl;
  if (jobCount > 0) {
    std::cerr << " By threads " << (par_finish - par_start) / jobCount
              << std::endl;
  }
  // These gradients are the single block ones.
  // They aren't so useful so we try to avoid computing them based on the result
  // of the current gradient.
  double min_utility = FLT_MAX;

  for (auto it = gradient.begin(); it != gradient.end(); it++) {
    std::cerr << "gradient " << it->first->getName().str() << " count "
              << get_basic_block_instance_count(it->first) << " utility "
              << it->second << std::endl;
    if ((it->second < min_utility) &&
        (get_basic_block_instance_count(it->first) > 0) && (it->second != 0)) {
      removeBB = it->first;
      min_utility = it->second;
      std::cerr << "Setting min utility " << removeBB->getName().str()
                << " count " << get_basic_block_instance_count(it->first)
                << " utility " << min_utility << std::endl;
    }
  }

  int seqCount = 0;
  uint64_t serial_start = rdtsc();
  if (!ParallelizeOneZero) {
    for (auto itBB = blocks.begin(); itBB != blocks.end(); itBB++) {
      BasicBlock *BB = *itBB;
      auto search = resourceTable.find(BB);
      std::vector<unsigned> &resourceVector = search->second;
      int count = resourceVector.size();
      // Check gradients to see if we need to recalculate.
      // In this case, we use the last gradient we
      // calculated as a guess. If it is not projected to be
      // useful, we don't recalculate.

      if ((count == 1) || (UseThreads == 1)) {
        if ((gradient[BB] == 0) ||
            (gradient[BB] < SerialGradientCutoff * min_utility)) {
          std::cerr << "Serial job for " << BB->getName().str() << std::endl;
          seqCount++;
          handle_basic_block_gradient(BB, &gradient, initialLatency,
                                      initialArea);
        } else if (SerialGradientCutoff == 0) {
          std::cerr << "Serial job for " << BB->getName().str() << std::endl;
          seqCount++;
          handle_basic_block_gradient(BB, &gradient, initialLatency,
                                      initialArea);
        } else {
          std::cerr << "Did not recompute gradient " << BB->getName().str()
                    << std::endl;
        }
      }
    }
  }
  uint64_t serial_finish = rdtsc();
  std::cerr << "Serial region cycle count: " << (serial_finish - serial_start);
  if (seqCount > 0) {
    std::cerr << " By threads " << (serial_finish - serial_start) / seqCount
              << std::endl;
  }

  uint64_t finish = rdtsc();

  // Decide how far to step in the gradient direction depends on where we are.

  std::unordered_map<BasicBlock *, double> coefs;
  double alpha;

  // set the 'removeBB' target to be the least useful block.
  min_utility = FLT_MAX;
  for (auto it = gradient.begin(); it != gradient.end(); it++) {
    std::cerr << "gradient " << it->first->getName().str() << " count "
              << get_basic_block_instance_count(it->first) << " utility "
              << it->second << std::endl;
    if ((it->second < min_utility) &&
        (get_basic_block_instance_count(it->first) > 0)) {
      removeBB = it->first;
      min_utility = it->second;
      std::cerr << "Setting min utility " << removeBB->getName().str()
                << " count " << get_basic_block_instance_count(it->first)
                << " utility " << min_utility << std::endl;
    }
  }

  // Rapid gradient descent method #1.  Removes area until partial derviatives
  // start to become unreliable.
  // models partial derivatives as 1/k^2 (Amdahl's Law).
  if (MaxDerivativeError) {

    double area_threshold = (initialArea - areaConstraint) / 2.0 +
                            areaConstraint - 10.0; // Try to remove half of the
                                                   // remaining area, but add a
                                                   // little extra.

    // if we ran out of thresholds, just remove one unit of area.
    if (area_threshold < 0) {
      area_threshold = 1.0;
    }

    // Now we must solve the linear combination to reduce area
    // by the required amount. We view the gradient coeffiencients
    // as a determining the ratio of blocks to remove.
    double sum = 0.0;
    double max_coef = 0.0;
    int max_area = 0;

    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      if (get_basic_block_instance_count(it->first) > 0) {
        max_area = ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
        double coef = 1 / (it->second + FLT_MIN);
        coefs[it->first] = coef;
        if (max_coef < coef) {
          max_coef = coef;
        }
      } else {
        // can't remove blocks that aren't there.
        removeBBs[it->first] = 0;
      }
    }

    // scale alpha such that we remove least enough blocks of
    // the largest type to get the area we care about.  We must
    // take care to ensure the we will remove at least one
    // block.  find a power of two that encompasses maximum
    // number of blocks we will remove.
    int max_count = (std::max(max_area, (int)area_threshold) / max_area) + 1;
    int max_power = 1;
    while (max_power < max_count) {
      max_power <<= 1;
    }

    alpha = std::max((double)max_area, area_threshold) / (max_coef * max_area);

    std::cerr << "Alpha: " << alpha << "\n";
    std::cerr << "initial area: " << initialArea << "\n";
    std::cerr << "max coef: " << max_coef << "\n";
    std::cerr << "max area: " << max_area << "\n";
    std::cerr << "max count: " << max_count << "\n";
    std::cerr << "max power: " << max_power << "\n";
    std::cerr << "Area_threshold: " << area_threshold << "\n";
    std::cerr << "Sum: " << sum << "\n";

    // If the convergence distance is very small, we may not
    // find a block to remove.  We track this and force the
    // removal of the marginal block if no other blocks are
    // removed.
    bool foundNonZero = false;

    // track whether we violated the derivative max error.
    bool violatedMaxDerivativeError = false;
    // Multiply coefs to obtain block counts. Need to adjust alpha up to deal
    // with need to floor.
    double area_removed_floor = 0;
    double area_removed = 0;

    // We get some estimate of alpha.  Since we are doing
    // rounding and also cannot reduce block counts beneath 0,
    // we need to massage alpha to cut the right number of
    // blocks.  Essentially, we will do a binary search to find
    // the value of alpha that does the right thing.

    // Now, we set up a search to find the 'right' value of alpha.
    double alpha_step = 1.0;
    double alpha_scaler = 2 * alpha_step;
    double alpha_step_cutoff = 1.0 / (max_power * 128); // go a few extra steps.
    double last_passing_step = -1; // If we don't every find a
                                   // passing step, we should
                                   // take the last value of the scaler.

    double alpha_prime = alpha * alpha_scaler;

    // iterate until we find a passing value. Here passing is defined by the
    // maximum area that does
    // not violate the MaxDerivativeError
    do {
      foundNonZero = false;
      violatedMaxDerivativeError = false;
      area_removed_floor = 0;
      area_removed = 0;
      for (auto it = gradient.begin(); it != gradient.end(); it++) {
        // handle the case of already eliminated blocks
        int block_count = get_basic_block_instance_count(it->first);

        removeBBs[it->first] =
            std::max(0, std::min(block_count,
                                 int(floor(coefs[it->first] * alpha_prime))));

        // Check to see if we violated the derivative error bound
        // 2nd derivative of Amdahl's is 2/x^3
        // Are we removing a block?
        if (removeBBs[it->first] > 1) {
          // If we are removing a block, are we removing too many?
          int finalCount = block_count - removeBBs[it->first];
          double derivativeDelta = 1.0;

          if (finalCount != 0) {
            derivativeDelta = 1.0 / (finalCount * finalCount) -
                              1.0 / (block_count * block_count);
          }
          std::cerr << it->first->getName().str()
                    << "derivative delta: " << derivativeDelta << std::endl;
          if (derivativeDelta > MaxDerivativeError) {
            violatedMaxDerivativeError = true;
          }
        }

        if (floor(coefs[it->first] * alpha_prime) > .5) {
          foundNonZero = true;
        }
        // need to check for removal of more blocks than actually exist.
        area_removed_floor +=
            std::max(0, std::min(block_count,
                                 (int)floor(coefs[it->first] * alpha_prime))) *
            ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
        area_removed += coefs[it->first] * alpha_prime *
                        ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
      }

      std::cerr << "Alpha scaler: " << alpha_scaler << "Eliminated "
                << area_removed_floor << " units of area rounded from "
                << area_removed << "needed: " << area_threshold << std::endl;

      // Back off if we either moved too far in the gradient, or we
      // took away too much area.
      if (violatedMaxDerivativeError ||
          (areaConstraint > (initialArea - area_removed_floor))) {
        last_passing_step = alpha_prime;
        alpha_scaler = alpha_scaler - alpha_step;
      } else {
        alpha_scaler = alpha_scaler + alpha_step;
      }

      alpha_step = alpha_step / 2;
      alpha_prime = alpha * alpha_scaler;

    } while (alpha_step > alpha_step_cutoff);

    // Just in case we didn't find any steps that pass, assign some default.
    if (last_passing_step < 0) {
      last_passing_step = alpha_prime;
    }

    // use last passing step to set the removal vector.

    area_removed_floor = 0;
    area_removed = 0;
    foundNonZero = false;
    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      int block_count = get_basic_block_instance_count(it->first);
      // handle the case of already eliminated blocks

      removeBBs[it->first] =
          std::max(0, std::min(block_count, (int)floor(coefs[it->first] *
                                                       last_passing_step)));

      if (floor(coefs[it->first] * last_passing_step) > 1.0) {
        foundNonZero = true;
      }

      std::cerr << it->first->getName().str() << ", " << it->second << ", "
                << ModuleAreaEstimator::getBasicBlockArea(*AT, it->first)
                << ", " << get_basic_block_instance_count(it->first)
                << " removing " << removeBBs[it->first] << " -> "
                << (get_basic_block_instance_count(it->first) -
                    removeBBs[it->first])
                << "remain\n";
    }

    // sometimes, we eliminate too much area.  We should
    // consider iterating over the removed blocks. There's an
    // argument that this is not energy efficient?

    // Ensure that we remove at least one block. Given that we are bumping
    // alpha, this may not be needed.
    if (!foundNonZero) {
      removeBBs[removeBB] = 1;
    }

  }
  // Rapid gradient descent method #1.  Uses an area schedule to limit the
  // number of steps
  // in the gradient descent process.
  else if (RapidConvergence && (thresholds.size() > 0) &&
           (initialArea > areaConstraint)) {
    // assume that we will remove half of the area in each step.
    double area_threshold;
    double target_threshold;
    do {
      target_threshold = thresholds.back();
      area_threshold = initialArea - target_threshold;
      thresholds.pop_back();
    } while ((area_threshold < 0) && (thresholds.size() > 0));

    // if we ran out of thresholds, just remove one block.
    if (area_threshold < 0) {
      area_threshold = 1.0;
    }

    // Now we must solve the linear combination to reduce area
    // by the required amount. We view the gradient coeffiencients
    // as a determining the ratio of blocks to remove.
    double sum = 0.0;
    double max_coef = 0.0;
    int max_area = 0;

    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      if (get_basic_block_instance_count(it->first) > 0) {
        double coef = 1 / (it->second + FLT_MIN);
        coefs[it->first] = coef;
        if (max_coef < coef) {
          max_coef = coef;
          max_area = ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
        }
      } else {
        // can't remove blocks that aren't there.
        removeBBs[it->first] = 0;
      }
    }

    // scale alpha such that we remove least enough blocks of
    // the largest type to get the area we care about.  We must
    // take care to ensure the we will remove at least one
    // block.  find a power of two that encompasses maximum
    // number of blocks we will remove.
    int max_count =
        (max_area) ? ((std::max(max_area, (int)area_threshold) / max_area) + 1)
                   : 0;
    int max_power = 1;
    while (max_power < max_count) {
      max_power <<= 1;
    }

    alpha = std::max((double)max_area, area_threshold) / (max_coef * max_area);

    std::cerr << "Alpha: " << alpha << "\n";
    std::cerr << "initial area: " << initialArea << "\n";
    std::cerr << "max coef: " << max_coef << "\n";
    std::cerr << "max area: " << max_area << "\n";
    std::cerr << "max count: " << max_count << "\n";
    std::cerr << "max power: " << max_power << "\n";
    std::cerr << "target  area: " << target_threshold << "\n";
    std::cerr << "Area_threshold: " << area_threshold << "\n";
    std::cerr << "Sum: " << sum << "\n";

    // If the convergence distance is very small, we may not
    // find a block to remove.  We track this and force the
    // removal of the marginal block if no other blocks are
    // removed.
    bool foundNonZero = false;

    // Multiply coefs to obtain block counts. Need to adjust alpha up to deal
    // with need to floor.
    double area_removed_floor = 0;
    double area_removed = 0;

    // We get some estimate of alpha.  Since we are doing
    // rounding and also cannot reduce block counts beneath 0,
    // we need to massage alpha to cut the right number of
    // blocks.  Essentially, we will do a binary search to find
    // the value of alpha that does the right thing.

    // Now, we set up a search to find the 'right' value of alpha.
    double alpha_step = 1.0;
    double alpha_scaler = 2 * alpha_step;
    double alpha_step_cutoff = 1.0 / (max_power * 128); // go a few extra steps.
    double last_passing_step = -1; // If we don't every find a
                                   // passing step, we should
                                   // take the last value of the scaler.

    double alpha_prime = alpha * alpha_scaler;

    // this doesn't need to be an iterative loop, probably.
    do {
      foundNonZero = false;
      area_removed_floor = 0;
      area_removed = 0;
      for (auto it = gradient.begin(); it != gradient.end(); it++) {
        // handle the case of already eliminated blocks
        int block_count = get_basic_block_instance_count(it->first);

        removeBBs[it->first] =
            std::max(0, std::min(block_count,
                                 int(floor(coefs[it->first] * alpha_prime))));

        if (floor(coefs[it->first] * alpha_prime) > .5) {
          foundNonZero = true;
        }
        // need to check for removal of more blocks than actually exist.
        area_removed_floor +=
            std::max(0, std::min(block_count,
                                 (int)floor(coefs[it->first] * alpha_prime))) *
            ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
        area_removed += coefs[it->first] * alpha_prime *
                        ModuleAreaEstimator::getBasicBlockArea(*AT, it->first);
      }

      std::cerr << "Alpha scaler: " << alpha_scaler << "Eliminated "
                << area_removed_floor << " units of area rounded from "
                << area_removed << "needed: " << area_threshold << std::endl;

      if (area_removed_floor > area_threshold) {
        last_passing_step = alpha_prime;
        alpha_scaler = alpha_scaler - alpha_step;
      } else {
        alpha_scaler = alpha_scaler + alpha_step;
      }

      alpha_step = alpha_step / 2;
      alpha_prime = alpha * alpha_scaler;

    } while (alpha_step > alpha_step_cutoff);

    // Just in case we didn't find any steps that pass, assign some default.
    if (last_passing_step < 0) {
      last_passing_step = alpha_prime;
    }

    // use last passing step to set the removal vector.

    area_removed_floor = 0;
    area_removed = 0;
    foundNonZero = false;
    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      int block_count = get_basic_block_instance_count(it->first);
      // handle the case of already eliminated blocks

      removeBBs[it->first] =
          std::max(0, std::min(block_count, (int)floor(coefs[it->first] *
                                                       last_passing_step)));

      if (floor(coefs[it->first] * last_passing_step) > 1.0) {
        foundNonZero = true;
      }
      std::cerr << it->first->getName().str() << ", " << it->second << ", "
                << ModuleAreaEstimator::getBasicBlockArea(*AT, it->first)
                << ", " << get_basic_block_instance_count(it->first)
                << " removing " << removeBBs[it->first] << " -> "
                << (get_basic_block_instance_count(it->first) -
                    removeBBs[it->first])
                << "remain\n";
    }

    // sometimes, we eliminate too much area.  We should
    // consider iterating over the removed blocks. There's an
    // argument that this is not energy efficient?

    // Ensure that we remove at least one block. Given that we are bumping
    // alpha, this may not be needed.
    if (!foundNonZero) {
      removeBBs[removeBB] = 1;
    }
  } else {

    // Just do one step here.
    if (removeBB != NULL) {
      removeBBs[removeBB] = 1;
    }

    for (auto it = gradient.begin(); it != gradient.end(); it++) {
      std::cerr << it->first->getName().str() << " gradient: " << it->second
                << " area: "
                << ModuleAreaEstimator::getBasicBlockArea(*AT, it->first)
                << " count: " << get_basic_block_instance_count(it->first)
                << '\n';
    }
  }
  std::cerr << "Descent Step: " << finalArea << " ( " << finalDeltaArea << " ) "
            << "initial latency: " << initialLatency << " ( "
            << finalDeltaLatency << " ) in " << finish - start << " cycles"
            << std::endl;
  // deltaDelay = finalDeltaLatency;
  return true; // not going to cpu only solution
}

// This does the main work of scheduling the gradient.  It is thread safe.
void AdvisorAnalysis::handle_basic_block_gradient(
    BasicBlock *BB, std::unordered_map<BasicBlock *, double> *gradient,
    int initialLatency, int initialArea) {
  // get a thread resource id
  int tid;

  bool succ = tidPool.pop(tid);
  assert(succ);

  // find the resourceTable associated with this block.
  std::unordered_map<BasicBlock *, std::vector<unsigned>> *resourceTable;
  resourceTable = threadPoolResourceTables.find(BB)->second;

  Function *F = BB->getParent();

  auto search = resourceTable->find(BB);
  std::vector<unsigned> &resourceVector = search->second;
  int count = resourceVector.size();

  // Provisionally remove block
  decrement_thread_pool_basic_block_instance_count(BB);

  // display resource sizings.
  //{
  //  std::unique_lock<std::mutex> lk(threadPoolMutex);
  //  std::cerr << "Done with block" << BB->getName().str() << std::endl;
  //  for (auto itRT = resourceTable->begin(); itRT != resourceTable->end();
  //  itRT++) {
  //    std::cerr << "RT Block: " << itRT->first->getName().str() << " count: "
  //    << itRT->second.second.size() << std::endl;
  //  }
  //  for (auto itRT = instanceCounts->begin(); itRT != instanceCounts->end();
  //  itRT++) {
  //    std::cerr << "IC Block: " << itRT->first->getName().str() << " count: "
  //    << itRT->second << std::endl;
  //  }
  //}

  // transition costs only happen if we go from accelerator impl.
  // to software impl.
  if ((count == 1) && !ParallelizeOneZero) {
    update_transition(BB);
  }

  resourceVector.pop_back();

  DEBUG(*outputLog << "Performing removal of basic block " << BB->getName()
                   << "\n");
  // need to iterate through all calls made to function

  int64_t latency = 0;

  // reset all values to zero.
  // Really we should do our own maintainence here so as to reduce overhead.
  // One could even have a pool of these things reinitialized by a worker
  // thread.
  for (auto itRT = resourceTable->begin(); itRT != resourceTable->end();
       itRT++) {
    for (auto itRV = itRT->second.begin(); itRV != itRT->second.end(); itRV++) {
      *itRV = 0;
    }
  }

  if (PerFunction) {
    for (auto fIt = executionGraph[F].begin(); fIt != executionGraph[F].end();
         fIt++) {
      latency += schedule_with_resource_constraints(fIt, F, resourceTable, tid);
    }
  } else {
    auto fIt = globalTraceGraph;
    latency +=
        schedule_with_resource_constraints_global(fIt, resourceTable, tid);
  }

  tidPool.push(tid);

  int area = initialArea - ModuleAreaEstimator::getBasicBlockArea(*AT, BB);

  float deltaLatency = (float)(initialLatency - latency);
  if (latency == initialLatency)
    deltaLatency = -1 * FLT_MIN;
  float deltaArea = (float)(initialArea - area);

  float marginalPerformance;
  if (area == initialArea) {
    // this block contributes no area
    // never remove a block that contributes no area?? No harm.
    marginalPerformance = FLT_MAX;
  } else {
    marginalPerformance = deltaLatency / deltaArea;
  }

  GradientPoint gPoint;
  gPoint.blockCount = count;
  gPoint.grad = marginalPerformance;

  gradients.find(BB)->second->gradientPoints.push_back(gPoint);

  // this is important.  It is where we communicate our result back.
  // we use the below find syntax to ensure thread safety.
  gradient->find(BB)->second = marginalPerformance;

  // restore the basic block count after removal
  increment_thread_pool_basic_block_instance_count(BB);

  // If we went ACC -> CPU, we need to fixup transition times.
  if ((count == 1) && !ParallelizeOneZero) {
    update_transition(BB);
  }

  resourceVector.push_back(0);

  {
    std::unique_lock<std::mutex> lk(threadPoolMutex);
    std::cerr << "Done with block" << BB->getName().str()
              << " grad: " << marginalPerformance << "delta latency"
              << deltaLatency << "delta area" << deltaArea << std::endl;
    std::cerr << "Initial latency: " << initialLatency << "\n";
    std::cerr << "New latency: " << latency << "\n";
    std::cerr << "delta latency: " << deltaLatency << "\n";
    std::cerr << "initial area: " << initialArea << "\n";
    std::cerr << "New area: " << area << "\n";
    std::cerr << "delta area: " << deltaArea << "\n";
  }
}

unsigned AdvisorAnalysis::get_cpu_only_latency(Function *F) {
  *outputLog << "Calculating schedule for CPU only execution.\n";

  unsigned cpuOnlyLatency = 0;

  // loop through all calls to function, get total latency
  for (auto fIt = executionGraph[F].begin(); fIt != executionGraph[F].end();
       fIt++) {
    cpuOnlyLatency += schedule_cpu(fIt, F);
  }

  return cpuOnlyLatency;
}

unsigned AdvisorAnalysis::get_cpu_only_latency_global(Module &M) {
  *outputLog << "Calculating schedule for CPU only execution.\n";

  unsigned cpuOnlyLatency = 0;

  cpuOnlyLatency += schedule_cpu_global(globalTraceGraph);

  return cpuOnlyLatency;
}

// Function: schedule_with_resource_constraints
// Return: latency of execution of trace
// This function will use the execution trace graph generated previously
// and the resource constraints embedded in the IR as metadata to determine
// the latency of the particular function call instance represented by this
// execution trace
int64_t AdvisorAnalysis::schedule_with_resource_constraints(
    TraceGraphList::iterator graph_it, Function *F,
    std::unordered_map<BasicBlock *, std::vector<unsigned>> *resourceTable,
    int tid) {
  DEBUG(*outputLog << __func__ << "\n");

  TraceGraph graph = *graph_it;
  // perform the scheduling with resource considerations

  // use hash table to keep track of resources available
  // the key is the basicblock resource
  // each key indexes into a vector of unsigned integers
  // the number of elements in the vector correspond to the
  // number of available resources of that basic block
  // the vector contains unsigned ints which represent the cycle
  // at which the resource next becomes available
  // the bool of the pair in the value is the CPU resource flag
  // if set to true, no additional hardware is required
  // however, a global value is used to keep track of the cpu
  // idleness

  // reset the cpu free cycle global!
  int64_t cpuCycle = 1;

  int64_t lastCycle = 1;

  // build a queue of schedulable BBs from the task graph. In reality, we should
  // build this once and encode it
  // in the graph datatype as a pointer list. But for now, we can waste time.
  std::queue<TraceGraph::vertex_descriptor> schedulableBB;
  TraceGraph::vertex_iterator vi, ve;

  // set the vertices up with zero values for this tid.
  for (boost::tie(vi, ve) = vertices(graph); vi != ve; vi++) {
    // Mark node as unscheduled, with a count of its parent
    // dependencies. Once a node hits zero, it can be scheduled.
    // this gives us the O(V+E) runtime we want.
    auto degree = boost::in_degree(*vi, graph);
    if (degree == 0) {
      graph[*vi].setStart(0, tid);
      BasicBlock *thisBB = graph[*vi].basicblock;
      BasicBlock *BB = find_basicblock_by_name(thisBB->getName());
      if (BB == NULL) {
        // now that we are ignoring 'dangling' basic blocks in
        // process_basic_block(), this case should not occur
        DEBUG(*outputLog << "WARNING bb " << thisBB->getName()
                         << " does not belong to " << F->getName() << "\n");
        assert(0);
        // continue;
      }
      schedulableBB.push(*vi);
    } else {
      // Is this in the graph?
      graph[*vi].setStart(-1 * degree, tid);
    }
  }

  while (!schedulableBB.empty()) {

    TraceGraph::vertex_descriptor v = schedulableBB.front();

    // std::cerr << "\nScheduleBB: v " << v << " " <<
    // graph[v].basicblock->getName().str() << "\n";

    schedulableBB.pop();

    assert((graph[v].getStart(tid) == 0) || (graph[v].getStart(tid) == -1));

    // if we changed how we handle the vector and made it an array, we could do
    // much better.

    // find the latest finishing parent
    // if no parent, start at 0
    int64_t start = -1;

    // Probably we can refactor this to remove the loop. This might help us.
    TraceGraph::in_edge_iterator ii, ie;
    for (boost::tie(ii, ie) = boost::in_edges(v, graph); ii != ie; ii++) {
      TraceGraph::vertex_descriptor s = boost::source(*ii, graph);
      // TraceGraph_vertex_descriptor t = boost::target(*ii, (graph));
      // std::cerr << (graph)[s].ID << " -> " << (graph)[t].ID << "\n";
      int64_t transitionDelay =
          (int)boost::get(boost::edge_weight_t(), graph, *ii);

      start = std::max(start, graph[s].getEnd(tid) + transitionDelay);
      // std::cerr << "NEW START: " << start << "\n";
    }
    start += 1;
    // std::cerr << "deps ready: " << start << "\n";

    BasicBlock *BB = graph[v].basicblock;

    // this differs from the maximal parallelism configuration scheduling
    // in that it also considers resource requirement
    // Above here
    // first sort the vector
    auto search = resourceTable->find(BB);
    // assert(search != resourceTable.end());
    // DEBUG(if (search == resourceTable.end()) {
    //	// if not found, could mean that either
    //	// a) basic block to be executed on cpu
    //	// b) resource table not initialized properly o.o
    //	std::cerr << "Basic block " << (*graph_ref)[v].name << " not found in
    // resource table.\n";
    //	assert(0);
    //  })

    int64_t resourceReady = UINT_MAX;
    unsigned minIdx;

    // should we do something with start. It may be that there will be a
    // unit we could use before hand, but we will instead use an earlier
    // available unit.
    std::vector<unsigned> &resourceVector = search->second;
    bool cpu = (resourceVector.size() == 0);

    if (cpu) { // cpu resource flag
      resourceReady = cpuCycle;
    } else {
      // find the minimum index
      for (unsigned idx = 0; idx < resourceVector.size(); idx++) {
        if (resourceVector[idx] < resourceReady) {
          minIdx = idx;
          resourceReady = resourceVector[idx];
        }
      }
    }

    start = std::max(start, resourceReady);

    // std::cerr << "resource count: " << resourceVector.size() << "\n";

    // std::cerr << "start: " << start << "\n";

    // std::cerr << "resourceReady: " << resourceReady << "\n";

    int64_t end = start;
    int64_t block_free = start;
    // Assign endpoint based on cpu or accelerator.
    if (cpu) {
      end += ModuleScheduler::getBasicBlockLatencyCpu(*LT, BB);
      // std::cerr << "cpu latency: " <<
      // ModuleScheduler::get_basic_block_latency_cpu(*LT, BB) << "\n";
    } else if (AssumePipelining) {
      int pipeline_latency = (int)AssumePipelining;
      end += ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB);
      block_free +=
          std::min(pipeline_latency,
                   ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB));
      // std::cerr << "acc latency: " <<
      // ModuleScheduler::get_basic_block_latency_accelerator(*LT, BB) << "\n";
    } else {
      end += ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB);
      block_free = end;
      // std::cerr << "acc latency: " <<
      // ModuleScheduler::get_basic_block_latency_accelerator(*LT, BB) << "\n";
    }

    // std::cerr << "end: " << end << "\n";
    // std::cerr << "block_free: " << block_free << "\n";

    // update the occupied resource with the new end cycle
    if (cpu) {
      cpuCycle = end;
    } else {
      resourceVector[minIdx] = block_free;
    }

    // std::cerr << "Schedule vertex: " << graph[v].basicblock->getName().str()
    // <<
    //  " start: " << start << " end: " << end << "\n";
    graph[v].setStart(start, tid);
    graph[v].setEnd(end, tid);

    // std::cerr << "VERTEX [" << v << "] START: " <<
    // (*graph_ref)[v].get_start() << " END: " << (*graph_ref)[v].get_end() <<
    // "\n";

    // keep track of last cycle as seen by scheduler
    lastCycle = std::max(lastCycle, end);

    // Mark up children as visited.
    auto oPair = boost::out_edges(v, graph);
    auto oi = oPair.first;
    auto oe = oPair.second;
    for (; oi != oe; oi++) {
      TraceGraph::vertex_descriptor s = boost::target(*oi, graph);
      if (graph[s].getStart(tid) == -1) {
        // we can now schedule this one.
        schedulableBB.push(s);
      } else {
        graph[s].setStart(graph[s].getStart(tid) + 1, tid);
      }
    }
    // std::cerr << "LastCycle: " << *lastCycle_ref << "\n";
  }

  return lastCycle;
}

// Function: schedule_with_resource_constraints_global
// Return: latency of execution of trace
// This function will use the execution trace graph generated previously
// and the resource constraints embedded in the IR as metadata to determine
// the latency of the particular function call instance represented by this
// execution trace
int64_t AdvisorAnalysis::schedule_with_resource_constraints_global(
    TraceGraphList::iterator graph_it,
    std::unordered_map<BasicBlock *, std::vector<unsigned>> *resourceTable,
    int tid) {
  DEBUG(*outputLog << __func__ << "\n");

  TraceGraph graph = *graph_it;
  // perform the scheduling with resource considerations

  // use hash table to keep track of resources available
  // the key is the basicblock resource
  // each key indexes into a vector of unsigned integers
  // the number of elements in the vector correspond to the
  // number of available resources of that basic block
  // the vector contains unsigned ints which represent the cycle
  // at which the resource next becomes available
  // the bool of the pair in the value is the CPU resource flag
  // if set to true, no additional hardware is required
  // however, a global value is used to keep track of the cpu
  // idleness

  // reset the cpu free cycle global!
  int64_t cpuCycle = 1;

  int64_t lastCycle = 1;

  // build a queue of schedulable BBs from the task graph. In reality, we should
  // build this once and encode it
  // in the graph datatype as a pointer list. But for now, we can waste time.
  std::queue<TraceGraph::vertex_descriptor> schedulableBB;
  TraceGraph::vertex_iterator vi, ve;

  // set the vertices up with zero values for this tid.
  for (boost::tie(vi, ve) = vertices(graph); vi != ve; vi++) {
    // Mark node as unscheduled, with a count of its parent
    // dependencies. Once a node hits zero, it can be scheduled.
    // this gives us the O(V+E) runtime we want.
    auto degree = boost::in_degree(*vi, graph);
    if (degree == 0) {
      graph[*vi].setStart(0, tid);
      schedulableBB.push(*vi);
    } else {
      // Is this in the graph?
      graph[*vi].setStart(-1 * degree, tid);
    }
  }

  while (!schedulableBB.empty()) {

    TraceGraph::vertex_descriptor v = schedulableBB.front();

    // std::cerr << "\nScheduleBB: v " << v << " " <<
    // graph[v].basicblock->getName().str() << "\n";

    schedulableBB.pop();

    assert((graph[v].getStart(tid) == 0) || (graph[v].getStart(tid) == -1));

    // if we changed how we handle the vector and made it an array, we could do
    // much better.

    // find the latest finishing parent
    // if no parent, start at 0
    int64_t start = -1;

    // Probably we can refactor this to remove the loop. This might help us.
    TraceGraph::in_edge_iterator ii, ie;
    for (boost::tie(ii, ie) = boost::in_edges(v, graph); ii != ie; ii++) {
      TraceGraph::vertex_descriptor s = boost::source(*ii, graph);
      // TraceGraph_vertex_descriptor t = boost::target(*ii, (graph));
      // std::cerr << (graph)[s].ID << " -> " << (graph)[t].ID << "\n";
      int64_t transitionDelay =
          (int)boost::get(boost::edge_weight_t(), graph, *ii);

      start = std::max(start, graph[s].getEnd(tid) + transitionDelay);
      // std::cerr << "NEW START: " << start << "\n";
    }
    start += 1;
    // std::cerr << "deps ready: " << start << "\n";

    BasicBlock *BB = graph[v].basicblock;

    // this differs from the maximal parallelism configuration scheduling
    // in that it also considers resource requirement
    // Above here
    // first sort the vector
    auto search = resourceTable->find(BB);
    // assert(search != resourceTable.end());
    // DEBUG(if (search == resourceTable.end()) {
    //	// if not found, could mean that either
    //	// a) basic block to be executed on cpu
    //	// b) resource table not initialized properly o.o
    //	std::cerr << "Basic block " << (*graph_ref)[v].name << " not found in
    // resource table.\n";
    //	assert(0);
    //  })

    int64_t resourceReady = UINT_MAX;
    unsigned minIdx;

    // should we do something with start. It may be that there will be a
    // unit we could use before hand, but we will instead use an earlier
    // available unit.
    std::vector<unsigned> &resourceVector = search->second;
    bool cpu = (resourceVector.size() == 0);

    if (cpu) { // cpu resource flag
      resourceReady = cpuCycle;
    } else {
      // find the minimum index
      for (unsigned idx = 0; idx < resourceVector.size(); idx++) {
        if (resourceVector[idx] < resourceReady) {
          minIdx = idx;
          resourceReady = resourceVector[idx];
        }
      }
    }

    start = std::max(start, resourceReady);

    // std::cerr << "start: " << start << "\n";

    // std::cerr << "resource count: " << resourceVector.size() << "\n";

    // std::cerr << "resourceReady: " << resourceReady << "\n";

    int64_t end = start;
    int64_t block_free = start;
    // Assign endpoint based on cpu or accelerator.
    if (cpu) {
      end += ModuleScheduler::getBasicBlockLatencyCpu(*LT, BB);
      // std::cerr << "cpu latency: " <<
      // ModuleScheduler::get_basic_block_latency_cpu(*LT, BB) << "\n";
    } else if (AssumePipelining) {
      int pipeline_latency = (int)AssumePipelining;
      end += ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB);
      block_free +=
          std::min(pipeline_latency,
                   ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB));
      // std::cerr << "acc latency: " <<
      // ModuleScheduler::get_basic_block_latency_accelerator(*LT, BB) << "\n";
    } else {
      end += ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB);
      block_free = end;
      // std::cerr << "acc latency: " <<
      // ModuleScheduler::get_basic_block_latency_accelerator(*LT, BB) << "\n";
    }

    // std::cerr << "end: " << end << "\n";
    // std::cerr << "block_free: " << block_free << "\n";

    // update the occupied resource with the new end cycle
    if (cpu) {
      cpuCycle = end;
    } else {
      resourceVector[minIdx] = block_free;
    }

    // std::cerr << "Schedule vertex: " << graph[v].basicblock->getName().str()
    // <<
    // " start: " << start << " end: " << end << "\n";
    graph[v].setStart(start, tid);
    graph[v].setEnd(end, tid);

    // std::cerr << "VERTEX [" << v << "] START: " << (graph)[v].get_start(tid)
    // << " END: " << (graph)[v].get_end(tid) << "\n";

    // keep track of last cycle as seen by scheduler
    lastCycle = std::max(lastCycle, end);
    // std::cerr << "LastCycle: " << lastCycle<< "\n";

    // Mark up children as visited.
    auto oPair = boost::out_edges(v, graph);
    auto oi = oPair.first;
    auto oe = oPair.second;
    for (; oi != oe; oi++) {
      TraceGraph::vertex_descriptor s = boost::target(*oi, graph);
      if (graph[s].getStart(tid) == -1) {
        // we can now schedule this one.
        schedulableBB.push(s);
      } else {
        graph[s].setStart(graph[s].getStart(tid) + 1, tid);
      }
    }
  }
  // std::cerr << " final LastCycle: " << lastCycle<< "\n";

  return lastCycle;
}

uint64_t AdvisorAnalysis::schedule_without_resource_constraints(
    TraceGraphList::iterator graph_it, Function *F,
    std::unordered_map<BasicBlock *, std::vector<unsigned>> *resourceTable) {
  DEBUG(*outputLog << __func__ << "\n");

  TraceGraph graph = *graph_it;
  // perform the scheduling with resource considerations

  // use hash table to keep track of resources available
  // the key is the basicblock resource
  // each key indexes into a vector of unsigned integers
  // the number of elements in the vector correspond to the
  // number of available resources of that basic block
  // the vector contains unsigned ints which represent the cycle
  // at which the resource next becomes available
  // the bool of the pair in the value is the CPU resource flag
  // if set to true, no additional hardware is required
  // however, a global value is used to keep track of the cpu
  // idleness

  int64_t lastCycle = 1;

  // build a queue of schedulable BBs from the task graph. In reality, we should
  // build this once and encode it
  // in the graph datatype as a pointer list. But for now, we can waste time.
  std::queue<TraceGraph::vertex_descriptor> schedulableBB;
  TraceGraph::vertex_iterator vi, ve;

  // set the vertices up with zero values for this tid.
  for (boost::tie(vi, ve) = vertices(graph); vi != ve; vi++) {
    // Mark node as unscheduled, with a count of its parent
    // dependencies. Once a node hits zero, it can be scheduled.
    // this gives us the O(V+E) runtime we want.
    auto degree = boost::in_degree(*vi, graph);
    if (degree == 0) {
      graph[*vi].setStart(0, SINGLE_THREAD_TID);
      BasicBlock *thisBB = graph[*vi].basicblock;
      BasicBlock *BB = find_basicblock_by_name(thisBB->getName());
      if (BB == NULL) {
        // now that we are ignoring 'dangling' basic blocks in
        // process_basic_block(), this case should not occur
        DEBUG(*outputLog << "WARNING bb " << thisBB->getName()
                         << " does not belong to " << F->getName() << "\n");
        assert(0);
        // continue;
      }
      schedulableBB.push(*vi);
    } else {
      // Is this in the graph?
      graph[*vi].setStart(-1 * degree, SINGLE_THREAD_TID);
    }
  }

  while (!schedulableBB.empty()) {

    TraceGraph::vertex_descriptor v = schedulableBB.front();

    // std::cerr << "\nScheduleBB: v " << v << " " <<
    // graph[v].basicblock->getName().str() << "\n";

    schedulableBB.pop();

    assert((graph[v].getStart(SINGLE_THREAD_TID) == 0) ||
           (graph[v].getStart(SINGLE_THREAD_TID) == -1));

    // if we changed how we handle the vector and made it an array, we could do
    // much better.

    // find the latest finishing parent
    // if no parent, start at 0
    int64_t start = -1;

    // Probably we can refactor this to remove the loop. This might help us.
    TraceGraph::in_edge_iterator ii, ie;
    for (boost::tie(ii, ie) = boost::in_edges(v, graph); ii != ie; ii++) {
      TraceGraph::vertex_descriptor s = boost::source(*ii, graph);

      start = std::max(start, (int64_t)graph[s].getEnd(SINGLE_THREAD_TID));
    }
    start += 1;
    // std::cerr << "start: " << start << "\n";

    BasicBlock *BB = graph[v].basicblock;

    // this differs from the maximal parallelism configuration scheduling
    // in that it also considers resource requirement
    // Above here
    // first sort the vector
    auto search = resourceTable->find(BB);

    // assert(search != resourceTable.end());
    // DEBUG(if (search == resourceTable.end()) {
    //	// if not found, could mean that either
    //	// a) basic block to be executed on cpu
    //	// b) resource table not initialized properly o.o
    //	std::cerr << "Basic block " << (*graph_ref)[v].name << " not found in
    // resource table.\n";
    //	assert(0);
    //  })

    int64_t resourceReady = UINT_MAX;
    unsigned minIdx;

    // should we do something with start. It may be that there will be a
    // unit we could use before hand, but we will instead use an earlier
    // available unit.

    std::vector<unsigned> &resourceVector = search->second;
    // find the minimum index
    for (unsigned idx = 0; idx < resourceVector.size(); idx++) {
      if (resourceVector[idx] < resourceReady) {
        minIdx = idx;
        resourceReady = resourceVector[idx];
      }
    }

    // std::cerr << "resource count: " << resourceVector.size() << "\n";
    // std::cerr << "resourceReady: " << resourceReady << "\n";

    // If there is no resource available, we will create a new one.
    if (start < resourceReady) {
      // std::cerr << "Adding resource, total is: " << resourceVector.size() + 1
      // << " \n";
      minIdx = resourceVector.size();
      resourceVector.push_back((unsigned)start);
      resourceReady = start; // we actually have the resource now.
    }

    start = std::max(resourceReady, start);

    int64_t end = start;
    int64_t block_free = start;
    // Assign endpoint based on cpu or accelerator.
    if (AssumePipelining) {
      int pipeline_latency = (int)AssumePipelining;
      end += ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB);
      block_free +=
          std::min(pipeline_latency,
                   ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB));
    } else {
      end += ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB);
      // std::cerr << "acc latency: " <<
      // ModuleScheduler::get_basic_block_latency_accelerator(*LT, BB) << "\n";
      block_free = end;
    }

    // std::cerr << "end: " << end << "\n";
    // std::cerr << "block_free: " << block_free << "\n";

    // update the occupied resource with the new end cycle
    resourceVector[minIdx] = block_free;

    // std::cerr << "Schedule vertex: " << graph[v].basicblock->getName().str()
    // <<
    //  " start: " << start << " end: " << end << "\n";
    graph[v].setMinStart(start);
    graph[v].setMinEnd(end);
    graph[v].setStart(start, SINGLE_THREAD_TID);
    graph[v].setEnd(end, SINGLE_THREAD_TID);

    // std::cerr << "VERTEX [" << v << "] START: " <<
    // (*graph_ref)[v].get_start() << " END: " << (*graph_ref)[v].get_end() <<
    // "\n";

    // keep track of last cycle as seen by scheduler
    lastCycle = std::max(lastCycle, end);

    // Mark up children as visited.
    auto oPair = boost::out_edges(v, graph);
    auto oi = oPair.first;
    auto oe = oPair.second;
    for (; oi != oe; oi++) {
      TraceGraph::vertex_descriptor s = boost::target(*oi, graph);
      if (graph[s].getStart(SINGLE_THREAD_TID) == -1) {
        // we can now schedule this one.
        schedulableBB.push(s);
      } else {
        graph[s].setStart(graph[s].getStart(SINGLE_THREAD_TID) + 1,
                          SINGLE_THREAD_TID);
      }
    }
  }

  // std::cerr << "LastCycle: " << lastCycle << "\n";

  return lastCycle;
}

uint64_t AdvisorAnalysis::schedule_without_resource_constraints_global(
    TraceGraphList::iterator graph_it,
    std::unordered_map<BasicBlock *, std::vector<unsigned>> *resourceTable) {
  DEBUG(*outputLog << __func__ << "\n");

  TraceGraph graph = *graph_it;
  // perform the scheduling with resource considerations

  // use hash table to keep track of resources available
  // the key is the basicblock resource
  // each key indexes into a vector of unsigned integers
  // the number of elements in the vector correspond to the
  // number of available resources of that basic block
  // the vector contains unsigned ints which represent the cycle
  // at which the resource next becomes available
  // the bool of the pair in the value is the CPU resource flag
  // if set to true, no additional hardware is required
  // however, a global value is used to keep track of the cpu
  // idleness

  int64_t lastCycle = 1;

  // build a queue of schedulable BBs from the task graph. In reality, we should
  // build this once and encode it
  // in the graph datatype as a pointer list. But for now, we can waste time.
  std::queue<TraceGraph::vertex_descriptor> schedulableBB;
  TraceGraph::vertex_iterator vi, ve;

  // set the vertices up with zero values for this tid.
  for (boost::tie(vi, ve) = vertices(graph); vi != ve; vi++) {
    // Mark node as unscheduled, with a count of its parent
    // dependencies. Once a node hits zero, it can be scheduled.
    // this gives us the O(V+E) runtime we want.
    auto degree = boost::in_degree(*vi, graph);
    if (degree == 0) {
      graph[*vi].setStart(0, SINGLE_THREAD_TID);
      // BasicBlock *thisBB = graph[*vi].basicblock;
      schedulableBB.push(*vi);
    } else {
      // Is this in the graph?
      graph[*vi].setStart(-1 * degree, SINGLE_THREAD_TID);
    }
  }

  while (!schedulableBB.empty()) {

    TraceGraph::vertex_descriptor v = schedulableBB.front();

    // std::cerr << "\nScheduleBB: v " << v << " " <<
    // graph[v].basicblock->getName().str() << "\n";

    schedulableBB.pop();

    assert((graph[v].getStart(SINGLE_THREAD_TID) == 0) ||
           (graph[v].getStart(SINGLE_THREAD_TID) == -1));

    // if we changed how we handle the vector and made it an array, we could do
    // much better.

    // find the latest finishing parent
    // if no parent, start at 0
    int64_t start = -1;

    // Probably we can refactor this to remove the loop. This might help us.
    TraceGraph::in_edge_iterator ii, ie;
    for (boost::tie(ii, ie) = boost::in_edges(v, graph); ii != ie; ii++) {
      TraceGraph::vertex_descriptor s = boost::source(*ii, graph);

      start = std::max(start, (int64_t)graph[s].getEnd(SINGLE_THREAD_TID));
    }
    start += 1;
    // std::cerr << "start: " << start << "\n";

    BasicBlock *BB = graph[v].basicblock;

    // this differs from the maximal parallelism configuration scheduling
    // in that it also considers resource requirement
    // Above here
    // first sort the vector
    auto search = resourceTable->find(BB);

    // assert(search != resourceTable.end());
    // DEBUG(if (search == resourceTable.end()) {
    //	// if not found, could mean that either
    //	// a) basic block to be executed on cpu
    //	// b) resource table not initialized properly o.o
    //	std::cerr << "Basic block " << (*graph_ref)[v].name << " not found in
    // resource table.\n";
    //	assert(0);
    //  })

    int64_t resourceReady = UINT_MAX;
    unsigned minIdx;

    // should we do something with start. It may be that there will be a
    // unit we could use before hand, but we will instead use an earlier
    // available unit.

    std::vector<unsigned> &resourceVector = search->second;
    // find the minimum index
    for (unsigned idx = 0; idx < resourceVector.size(); idx++) {
      if (resourceVector[idx] < resourceReady) {
        minIdx = idx;
        resourceReady = resourceVector[idx];
      }
    }

    // std::cerr << "resource count: " << resourceVector.size() << "\n";
    // std::cerr << "resourceReady: " << resourceReady << "\n";

    // If there is no resource available, we will create a new one.
    if (start < resourceReady) {
      // std::cerr << "Adding resource, total is: " << resourceVector.size() + 1
      // << " \n";
      minIdx = resourceVector.size();
      resourceVector.push_back((unsigned)start);
      resourceReady = start; // we actually have the resource now.
    }

    start = std::max(resourceReady, start);

    int64_t end = start;
    int64_t block_free = start;
    // Assign endpoint based on cpu or accelerator.
    if (AssumePipelining) {
      int pipeline_latency = (int)AssumePipelining;
      end += ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB);
      block_free +=
          std::min(pipeline_latency,
                   ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB));
    } else {
      end += ModuleScheduler::getBasicBlockLatencyAccelerator(*LT, BB);
      // std::cerr << "acc latency: " <<
      // ModuleScheduler::get_basic_block_latency_accelerator(*LT, BB) << "\n";
      block_free = end;
    }

    // std::cerr << "end: " << end << "\n";
    // std::cerr << "block_free: " << block_free << "\n";

    // update the occupied resource with the new end cycle
    resourceVector[minIdx] = block_free;

    // std::cerr << "Schedule vertex: " << graph[v].basicblock->getName().str()
    // <<
    // " start: " << start << " end: " << end << "\n";
    graph[v].setMinStart(start);
    graph[v].setMinEnd(end);
    graph[v].setStart(start, SINGLE_THREAD_TID);
    graph[v].setEnd(end, SINGLE_THREAD_TID);

    // std::cerr << "VERTEX [" << v << "] START: " <<
    // (*graph_ref)[v].get_start() << " END: " << (*graph_ref)[v].get_end() <<
    // "\n";

    // keep track of last cycle as seen by scheduler
    lastCycle = std::max(lastCycle, end);

    // Mark up children as visited.
    auto oPair = boost::out_edges(v, graph);
    auto oi = oPair.first;
    auto oe = oPair.second;
    for (; oi != oe; oi++) {
      TraceGraph::vertex_descriptor s = boost::target(*oi, graph);
      if (graph[s].getStart(SINGLE_THREAD_TID) == -1) {
        // we can now schedule this one.
        schedulableBB.push(s);
      } else {
        graph[s].setStart(graph[s].getStart(SINGLE_THREAD_TID) + 1,
                          SINGLE_THREAD_TID);
      }
    }
  }

  // std::cerr << "LastCycle: " << lastCycle << "\n";

  return lastCycle;
}

uint64_t AdvisorAnalysis::schedule_cpu(TraceGraphList::iterator graph_it,
                                       Function *F) {
  DEBUG(*outputLog << __func__ << "\n");

  TraceGraph graph = *graph_it;
  // perform the scheduling with resource considerations

  // use hash table to keep track of resources available
  // the key is the basicblock resource
  // each key indexes into a vector of unsigned integers
  // the number of elements in the vector correspond to the
  // number of available resources of that basic block
  // the vector contains unsigned ints which represent the cycle
  // at which the resource next becomes available
  // the bool of the pair in the value is the CPU resource flag
  // if set to true, no additional hardware is required
  // however, a global value is used to keep track of the cpu
  // idleness

  // reset the cpu free cycle global!
  int64_t cpuCycle = 1;

  int64_t lastCycle = 1;

  // build a queue of schedulable BBs from the task graph. In reality, we should
  // build this once and encode it
  // in the graph datatype as a pointer list. But for now, we can waste time.
  std::queue<TraceGraph::vertex_descriptor> schedulableBB;
  TraceGraph::vertex_iterator vi, ve;

  // set the vertices up with zero values for this tid.
  for (boost::tie(vi, ve) = vertices(graph); vi != ve; vi++) {
    // Mark node as unscheduled, with a count of its parent
    // dependencies. Once a node hits zero, it can be scheduled.
    // this gives us the O(V+E) runtime we want.
    auto degree = boost::in_degree(*vi, graph);
    if (degree == 0) {
      graph[*vi].setStart(0, SINGLE_THREAD_TID);
      schedulableBB.push(*vi);
    } else {
      // Is this in the graph?
      graph[*vi].setStart(-1 * degree, SINGLE_THREAD_TID);
    }
  }

  while (!schedulableBB.empty()) {

    TraceGraph::vertex_descriptor v = schedulableBB.front();

    schedulableBB.pop();

    assert((graph[v].getStart(SINGLE_THREAD_TID) == 0) ||
           (graph[v].getStart(SINGLE_THREAD_TID) == -1));

    // std::cerr << "ScheduleBB: " << graph[v].basicblock->getName().str() <<
    // "\n";

    // if we changed how we handle the vector and made it an array, we could do
    // much better.

    // find the latest finishing parent
    // if no parent, start at 0
    int64_t start = -1;

    // Probably we can refactor this to remove the loop. This might help us.
    TraceGraph::in_edge_iterator ii, ie;
    for (boost::tie(ii, ie) = boost::in_edges(v, graph); ii != ie; ii++) {
      TraceGraph::vertex_descriptor s = boost::source(*ii, graph);
      start = std::max(start, (int64_t)graph[s].getEnd(SINGLE_THREAD_TID));
    }
    start += 1;
    // std::cerr << "deps ready: " << start << "\n";

    BasicBlock *BB = graph[v].basicblock;
    BasicBlock *searchBB = find_basicblock_by_name(BB->getName());
    if (searchBB == NULL) {
      // now that we are ignoring 'dangling' basic blocks in
      // process_basic_block(), this case should not occur
      DEBUG(*outputLog << "WARNING bb " << BB->getName()
                       << " does not belong to " << F->getName() << "\n");
      assert(0);
      // continue;
    }

    start = std::max(cpuCycle, start);

    // std::cerr << "start: " << start << "\n";

    // std::cerr << "resourceReady: " << cpuCycle << "\n";

    int64_t end = start;

    // Assign endpoint based on cpu or accelerator.
    end += ModuleScheduler::getBasicBlockLatencyCpu(*LT, BB);
    cpuCycle = end;

    // std::cerr << "end: " << end << "\n";

    graph[v].setStart(start, SINGLE_THREAD_TID);
    graph[v].setEnd(end, SINGLE_THREAD_TID);

    // keep track of last cycle as seen by scheduler
    lastCycle = std::max(lastCycle, end);

    // Mark up children as visited.
    auto oPair = boost::out_edges(v, graph);
    auto oi = oPair.first;
    auto oe = oPair.second;
    for (; oi != oe; oi++) {
      TraceGraph::vertex_descriptor s = boost::target(*oi, graph);
      if (graph[s].getStart(SINGLE_THREAD_TID) == -1) {
        // we can now schedule this one.
        schedulableBB.push(s);
      } else {
        graph[s].setStart(graph[s].getStart(SINGLE_THREAD_TID) + 1,
                          SINGLE_THREAD_TID);
      }
    }
  }

  return lastCycle;
}

uint64_t
AdvisorAnalysis::schedule_cpu_global(TraceGraphList::iterator graph_it) {
  DEBUG(*outputLog << __func__ << "\n");

  TraceGraph graph = *graph_it;
  // perform the scheduling with resource considerations

  // use hash table to keep track of resources available
  // the key is the basicblock resource
  // each key indexes into a vector of unsigned integers
  // the number of elements in the vector correspond to the
  // number of available resources of that basic block
  // the vector contains unsigned ints which represent the cycle
  // at which the resource next becomes available
  // the bool of the pair in the value is the CPU resource flag
  // if set to true, no additional hardware is required
  // however, a global value is used to keep track of the cpu
  // idleness

  // reset the cpu free cycle global!
  int64_t cpuCycle = 1;

  int64_t lastCycle = 1;

  // build a queue of schedulable BBs from the task graph. In reality, we should
  // build this once and encode it
  // in the graph datatype as a pointer list. But for now, we can waste time.
  std::queue<TraceGraph::vertex_descriptor> schedulableBB;
  TraceGraph::vertex_iterator vi, ve;

  // set the vertices up with zero values for this tid.
  for (boost::tie(vi, ve) = vertices(graph); vi != ve; vi++) {
    // Mark node as unscheduled, with a count of its parent
    // dependencies. Once a node hits zero, it can be scheduled.
    // this gives us the O(V+E) runtime we want.
    auto degree = boost::in_degree(*vi, graph);
    if (degree == 0) {
      graph[*vi].setStart(0, SINGLE_THREAD_TID);
      schedulableBB.push(*vi);
    } else {
      // Is this in the graph?
      graph[*vi].setStart(-1 * degree, SINGLE_THREAD_TID);
    }
  }

  while (!schedulableBB.empty()) {

    auto v = schedulableBB.front();

    schedulableBB.pop();

    assert((graph[v].getStart(SINGLE_THREAD_TID) == 0) ||
           (graph[v].getStart(SINGLE_THREAD_TID) == -1));

    // std::cerr << "ScheduleBB: " << graph[v].basicblock->getName().str() <<
    // "\n";

    // if we changed how we handle the vector and made it an array, we could do
    // much better.

    // find the latest finishing parent
    // if no parent, start at 0
    int64_t start = -1;

    // Probably we can refactor this to remove the loop. This might help us.
    TraceGraph::in_edge_iterator ii, ie;
    for (boost::tie(ii, ie) = boost::in_edges(v, graph); ii != ie; ii++) {
      auto s = boost::source(*ii, graph);
      start = std::max(start, (int64_t)graph[s].getEnd(SINGLE_THREAD_TID));
    }
    start += 1;
    // std::cerr << "deps ready: " << start << "\n";

    BasicBlock *BB = graph[v].basicblock;

    start = std::max(cpuCycle, start);

    // std::cerr << "start: " << start << "\n";

    // std::cerr << "resourceReady: " << cpuCycle << "\n";

    int64_t end = start;

    // Assign endpoint based on cpu or accelerator.
    end += ModuleScheduler::getBasicBlockLatencyCpu(*LT, BB);
    cpuCycle = end;

    // std::cerr << "end: " << end << "\n";

    graph[v].setStart(start, SINGLE_THREAD_TID);
    graph[v].setEnd(end, SINGLE_THREAD_TID);

    // keep track of last cycle as seen by scheduler
    lastCycle = std::max(lastCycle, end);

    // Mark up children as visited.
    auto oPair = boost::out_edges(v, graph);
    auto oi = oPair.first;
    auto oe = oPair.second;
    for (; oi != oe; oi++) {
      auto s = boost::target(*oi, graph);
      if (graph[s].getStart(SINGLE_THREAD_TID) == -1) {
        // we can now schedule this one.
        schedulableBB.push(s);
      } else {
        graph[s].setStart(graph[s].getStart(SINGLE_THREAD_TID) + 1,
                          SINGLE_THREAD_TID);
      }
    }
  }

  return lastCycle;
}

// Function: find_root_vertices
// Finds all vertices with in degree 0 -- root of subgraph/tree
void AdvisorAnalysis::find_root_vertices(
    std::vector<TraceGraph::vertex_descriptor> &roots,
    TraceGraphList::iterator graph_it) {
  TraceGraph graph = *graph_it;
  TraceGraph::vertex_iterator vi, ve;
  for (boost::tie(vi, ve) = vertices(graph); vi != ve; vi++) {
    if (boost::in_degree(*vi, graph) == 0) {
      roots.push_back(*vi);
    }
  }
}

// Function: set_basic_block_instance_count
// Set the basic block metadata to denote the number of basic block instances
// needed
void AdvisorAnalysis::set_basic_block_instance_count(BasicBlock *BB,
                                                     int value) {
  bbInstanceCounts[BB] = value;
  // apply across the threadpool
  set_all_thread_pool_basic_block_instance_counts(BB, value);
}

void AdvisorAnalysis::set_all_thread_pool_basic_block_instance_counts(
    BasicBlock *BB, int value) {
  for (auto it = threadPoolInstanceCounts.begin();
       it != threadPoolInstanceCounts.end(); it++) {
    (*it).second->at(BB) = value;
  }
}

// Resizes the thread pool resource tables after a gradient step
void AdvisorAnalysis::adjust_all_thread_pool_resource_tables(BasicBlock *BB,
                                                             int value) {
  for (auto it = threadPoolResourceTables.begin();
       it != threadPoolResourceTables.end(); it++) {
    // if we went cpu only, set the first member to true
    (*it).second->at(BB).resize(value);
  }
}

void AdvisorAnalysis::set_thread_pool_basic_block_instance_count(BasicBlock *BB,
                                                                 int value) {
  threadPoolInstanceCounts.find(BB)->second->find(BB)->second = value;
}

int AdvisorAnalysis::get_thread_pool_basic_block_instance_count(
    BasicBlock *BB) {
  return threadPoolInstanceCounts.find(BB)->second->find(BB)->second;
}

int AdvisorAnalysis::get_basic_block_instance_count(BasicBlock *BB) {
  return bbInstanceCounts[BB];
}

// Function: decrement_basic_block_instance_count
// Return: false if decrement not successful
// Modify basic block metadata to denote the number of basic block instances
// needed
bool AdvisorAnalysis::decrement_basic_block_instance_count(BasicBlock *BB) {
  int repFactor = get_basic_block_instance_count(BB);
  if (repFactor <=
      0) { // 0 represents CPU execution, anything above 0 means HW accel
    return false;
  }

  set_basic_block_instance_count(BB, repFactor - 1);
  return true;
}

bool AdvisorAnalysis::decrement_thread_pool_basic_block_instance_count(
    BasicBlock *BB) {
  int repFactor = get_thread_pool_basic_block_instance_count(BB);
  if (repFactor <=
      0) { // 0 represents CPU execution, anything above 0 means HW accel
    return false;
  }

  set_thread_pool_basic_block_instance_count(BB, repFactor - 1);
  return true;
}

// Function: increment_basic_block_instance_count
// Return: false if decrement not successful
// Modify basic block metadata to denote the number of basic block instances
// needed
bool AdvisorAnalysis::increment_basic_block_instance_count(BasicBlock *BB) {
  int repFactor = get_basic_block_instance_count(BB);

  set_basic_block_instance_count(BB, repFactor + 1);

  return true;
}

bool AdvisorAnalysis::increment_thread_pool_basic_block_instance_count(
    BasicBlock *BB) {
  int repFactor = get_thread_pool_basic_block_instance_count(BB);

  set_thread_pool_basic_block_instance_count(BB, repFactor + 1);

  return true;
}

// Function: update_transition
void AdvisorAnalysis::update_transition(BasicBlock *BB) {

  // if successful, update the transition
  // this is dumb and inefficient, but just do this for now
  if (PerFunction) {
    Function *F = BB->getParent();
    for (auto fIt = executionGraph[F].begin(); fIt != executionGraph[F].end();
         fIt++) {
      update_transition_delay(fIt);
    }
  } else {
    auto fIt = globalTraceGraph;
    update_transition_delay(fIt);
  }
}

// Function: decrement_basic_block_instance_count_and_update_transition
// Return: false if decrement not successful
bool AdvisorAnalysis::
    decrement_basic_block_instance_count_and_update_transition(BasicBlock *BB) {
  // decrement
  if (!decrement_basic_block_instance_count(BB)) {
    return false;
  }

  // if successful, update the transition
  // this is dumb and inefficient, but just do this for now
  if (PerFunction) {
    Function *F = BB->getParent();
    for (auto fIt = executionGraph[F].begin(); fIt != executionGraph[F].end();
         fIt++) {
      update_transition_delay(fIt);
    }
  } else {
    auto fIt = globalTraceGraph;
    update_transition_delay(fIt);
  }

  return true;
}

bool AdvisorAnalysis::decrease_basic_block_instance_count_and_update_transition(
    std::unordered_map<BasicBlock *, int> &removeBBs) {

  std::unordered_map<Function *, int> updateFunctions;

  for (auto it = removeBBs.begin(); it != removeBBs.end(); it++) {
    BasicBlock *block = it->first;
    int count = it->second;

    int origCount = get_basic_block_instance_count(block);

    int newCount = std::max(0, origCount - count);

    if (newCount == 0) {
      updateFunctions[block->getParent()] = 0;
    }

    // update both the thread pools and the main instance count.
    set_all_thread_pool_basic_block_instance_counts(block, newCount);
    adjust_all_thread_pool_resource_tables(block, newCount);
    set_basic_block_instance_count(block, newCount);
  }

  // If we removed all instances of any block, then update the transition delays
  for (auto it = updateFunctions.begin(); it != updateFunctions.end(); it++) {
    for (auto fIt = executionGraph[it->first].begin();
         fIt != executionGraph[it->first].end(); fIt++) {
      update_transition_delay(fIt);
    }
  }

  return true;
}

// Function: increment_basic_block_instance_count_and_update_transition
// Return: false if increment not successful
bool AdvisorAnalysis::
    increment_basic_block_instance_count_and_update_transition(BasicBlock *BB) {
  // decrement
  if (!increment_basic_block_instance_count(BB)) {
    return false;
  }

  // if successful, update the transition
  // this is dumb and inefficient, but just do this for now
  if (PerFunction) {
    Function *F = BB->getParent();
    for (auto fIt = executionGraph[F].begin(); fIt != executionGraph[F].end();
         fIt++) {
      update_transition_delay(fIt);
    }
  } else {
    auto fIt = globalTraceGraph;
    update_transition_delay(fIt);
  }

  return true;
}

void AdvisorAnalysis::
    decrement_all_basic_block_instance_count_and_update_transition(
        Function *F) {
  for (auto &BB : *F) {
    while (decrement_basic_block_instance_count(&BB))
      ;
  }

  // this is dumb and inefficient, but just do this for now
  for (auto fIt = executionGraph[F].begin(); fIt != executionGraph[F].end();
       fIt++) {
    update_transition_delay(fIt);
  }
}

void AdvisorAnalysis::
    decrement_all_basic_block_instance_count_and_update_transition_global(
        Module &M) {
  for (auto &F : M) {
    for (auto &BB : F) {
      while (decrement_basic_block_instance_count(&BB))
        ;
    }
  }

  auto fIt = globalTraceGraph;
  update_transition_delay(fIt);
}
/*
// Function: get_basic_block_instance_count
// Return: the number of instances of this basic block from metadata
static int AdvisorAnalysis::get_basic_block_instance_count(BasicBlock *BB) {
        assert(BB);
        std::string MDName = "FPGA_ADVISOR_REPLICATION_FACTOR_";
        MDName += BB->getName().str();
        MDNode *M = BB->getTerminator()->getMetadata(MDName);

        int repFactor = -1;

        assert(M);
        assert(M->getOperand(0));

        std::string repFactorStr =
cast<MDString>(M->getOperand(0))->getString().str();
        repFactor = stoi(repFactorStr);

        // *outputLog << __func__ << " metadata: ";
        //BB->getTerminator()->print(*outputLog);
        // *outputLog << " replication factor: " << repFactor << "\n";

        return repFactor;
}
*/

// Function: initialize_resource_table
// The resource table represents the resources needed for this program
// the resources we need to consider are:
// HW logic: represented by individual basic blocks
// CPU: represented by a flag
// other??
// FIXME: integrate the cpu
void AdvisorAnalysis::initialize_resource_table(
    Function *F,
    std::unordered_map<BasicBlock *, std::vector<unsigned>> *resourceTable,
    bool cpuOnly) {
  for (auto &BB : *F) {
    int repFactor = get_basic_block_instance_count(&BB);
    if (repFactor < 0) {
      continue;
    }

    if (cpuOnly) {
      // cpu
      std::vector<unsigned> resourceVector(0);
      resourceTable->insert(std::make_pair(&BB, resourceVector));
      DEBUG(*outputLog << "Created entry in resource table for basic block: "
                       << BB.getName() << " using cpu resources.\n");
    } else {
      // fpga
      // create a vector
      std::vector<unsigned> resourceVector(repFactor, 0);
      resourceTable->insert(std::make_pair(&BB, resourceVector));

      DEBUG(*outputLog << "Created entry in resource table for basic block: "
                       << BB.getName() << " with " << repFactor
                       << " entries.\n");
    }
  }
}

// Function: initialize_resource_table
// The resource table represents the resources needed for this program
// the resources we need to consider are:
// HW logic: represented by individual basic blocks
// CPU: represented by a flag
// other??
// FIXME: integrate the cpu
void AdvisorAnalysis::initialize_resource_table_global(
    Module &M,
    std::unordered_map<BasicBlock *, std::vector<unsigned>> *resourceTable,
    bool cpuOnly) {
  for (auto &F : M) {
    for (auto &BB : F) {
      int repFactor = get_basic_block_instance_count(&BB);
      if (repFactor < 0) {
        continue;
      }

      if (cpuOnly) {
        // cpu
        std::vector<unsigned> resourceVector(0);
        resourceTable->insert(std::make_pair(&BB, resourceVector));
        DEBUG(*outputLog << "Created entry in resource table for basic block: "
                         << BB.getName() << " using cpu resources.\n");
      } else {
        // fpga
        // create a vector
        std::vector<unsigned> resourceVector(repFactor, 0);
        resourceTable->insert(std::make_pair(&BB, resourceVector));

        DEBUG(*outputLog << "Created entry in resource table for basic block: "
                         << BB.getName() << " with " << repFactor
                         << " entries.\n");
      }
    }
  }
}

// Function: get_area_requirement
// Return: a unitless value representing the area 'cost' of a design
// I'm sure this will need a lot of calibration...
unsigned AdvisorAnalysis::get_area_requirement(Function *F) {
  // baseline area required for cpu
  // int area = 1000;
  int area = 0;
  for (auto &BB : *F) {
    int areaBB = ModuleAreaEstimator::getBasicBlockArea(*AT, &BB);
    int repFactor = get_basic_block_instance_count(&BB);
    area += areaBB * repFactor;
  }
  return area;
}
unsigned AdvisorAnalysis::get_area_requirement_global(Module &M) {
  int area = 0;
  for (auto &F : M) {
    for (auto &BB : F) {
      int areaBB = ModuleAreaEstimator::getBasicBlockArea(*AT, &BB);
      int repFactor = get_basic_block_instance_count(&BB);
      area += areaBB * repFactor;
    }
  }
  return area;
}

// Function: update_transition_delay_basic_block
// updates the trace execution graph edge weights
/*void
   AdvisorAnalysis::update_transition_delay_basic_block(TraceGraphList_iterator
   graph) {
        //std::cerr <<
   ">>>>>>>>>>=======================================================\n";
        TraceGraph_edge_iterator ei, ee;
        // look at each edge
        for (boost::tie(ei, ee) = edges(*graph); ei != ee; ei++) {
                TraceGraph_vertex_descriptor s = boost::source(*ei, *graph);
                TraceGraph_vertex_descriptor t = boost::target(*ei, *graph);
                bool sHwExec = (0 <
   get_basic_block_instance_count((*graph)[s].basicblock));
                bool tHwExec = (0 <
   get_basic_block_instance_count((*graph)[t].basicblock));
                //std::cerr << "TRANSITION DELAY [" << s << "] -> [" << t <<
   "]\n";;
                //std::cerr << "Source CPU: " << sHwExec << " Target CPU: " <<
   tHwExec << "\n";
                // add edge weight <=> transition delay when crossing a hw/cpu
   boundary
                unsigned delay = 0;
                if (sHwExec ^ tHwExec) {
                        //std::cerr << "Transition cpu<->fpga\n";
                        bool CPUToHW = true;
                        if (sHwExec == true) {
                                // fpga -> cpu
                                CPUToHW = false;
                        }
                        // currently just returns 100
                        delay = get_transition_delay((*graph)[s].basicblock,
   (*graph)[t].basicblock, CPUToHW);
                } else {
                        // should have no transition penalty, double make sure
                        delay = 0;
                }
                boost::put(boost::edge_weight_t(), *graph, *ei, delay);
        }
        //std::cerr <<
   "<<<<<<<<<<=======================================================\n";
        }*/

// Function: update_transition_delay
// updates the trace execution graph edge weights
void AdvisorAnalysis::update_transition_delay(TraceGraphList::iterator graph) {
  // std::cerr <<
  // ">>>>>>>>>>=======================================================\n";
  TraceGraph::edge_iterator ei, ee;
  // look at each edge
  for (boost::tie(ei, ee) = edges(*graph); ei != ee; ei++) {
    auto s = boost::source(*ei, *graph);
    auto t = boost::target(*ei, *graph);
    bool sHwExec = (0 < get_basic_block_instance_count((*graph)[s].basicblock));
    bool tHwExec = (0 < get_basic_block_instance_count((*graph)[t].basicblock));
    // std::cerr << "TRANSITION DELAY [" << s << "] -> [" << t << "]\n";;
    // std::cerr << "Source CPU: " << sHwExec << " Target CPU: " << tHwExec <<
    // "\n";
    // add edge weight <=> transition delay when crossing a hw/cpu boundary
    unsigned delay = 0;
    if (sHwExec ^ tHwExec) {
      // std::cerr << "Transition cpu<->fpga\n";
      bool CPUToHW = true;
      if (sHwExec == true) {
        // fpga -> cpu
        CPUToHW = false;
      }
      // currently just returns 100
      delay = get_transition_delay((*graph)[s].basicblock,
                                   (*graph)[t].basicblock, CPUToHW);
    } else {
      // should have no transition penalty, double make sure
      delay = 0;
    }
    boost::put(boost::edge_weight_t(), *graph, *ei, delay);
  }
  // std::cerr <<
  // "<<<<<<<<<<=======================================================\n";
}

// Function: get_transition_delay
// Return: an unsigned int representing the transitional delay between switching
// from either
// fpga to cpu, or cpu to fpga
unsigned AdvisorAnalysis::get_transition_delay(BasicBlock *source,
                                               BasicBlock *target,
                                               bool CPUToHW) {
  unsigned delay = 100; // some baseline delay

  if (UserTransitionDelay > 0) {
    delay = UserTransitionDelay;
  }

  // need to do something here...
  // the delay shouldn't be constant?
  return delay;
}

void AdvisorAnalysis::print_basic_block_configuration(Function *F,
                                                      raw_ostream *out) {
  *out << "Basic Block Configuration:\n";
  for (auto &BB : *F) {
    int repFactor = get_basic_block_instance_count(&BB);
    *out << BB.getName() << " function " << BB.getParent()->getName().str()
         << "\t[" << repFactor << "]\n";
  }
}

int AdvisorAnalysis::get_total_basic_block_instances(Function *F) {
  int total = 0;
  for (auto &BB : *F) {
    total += get_basic_block_instance_count(&BB);
  }
  return total;
}

int AdvisorAnalysis::get_total_basic_block_instances_global(Module &M) {
  int total = 0;
  for (auto &F : M) {
    for (auto &BB : F) {
      total += get_basic_block_instance_count(&BB);
    }
  }
  return total;
}

bool AdvisorAnalysis::prune_basic_block_configuration_to_device_area(
    Function *F) {

  for (auto &BB : *F) {
    int areaBB = ModuleAreaEstimator::getBasicBlockArea(*AT, &BB);
    int repFactor = get_basic_block_instance_count(&BB);
    int maxBBCount = areaConstraint / areaBB;
    // Lower repFactor to the maximum for the target FPGA.
    repFactor = std::min(maxBBCount, repFactor);
    set_basic_block_instance_count(&BB, repFactor);
  }

  return true;
}

bool AdvisorAnalysis::prune_basic_block_configuration_to_device_area_global(
    Module &M) {
  for (auto &F : M) {
#if 0 // FIXME bring this back
          if(!functionInTrace(&F)) {
               // "Function not seen in the incoming trace"
               continue;
          }
#endif
    for (auto &BB : F) {
      int areaBB = ModuleAreaEstimator::getBasicBlockArea(*AT, &BB);
      int repFactor = get_basic_block_instance_count(&BB);
      int maxBBCount = areaConstraint / areaBB;
      // Lower repFactor to the maximum for the target FPGA.
      repFactor = std::min(maxBBCount, repFactor);
      set_basic_block_instance_count(&BB, repFactor);
    }
  }
  return true;
}

void AdvisorAnalysis::dumpImplementationCounts(Function *F) {
  for (auto &BB : *F) {
    int repFactor = get_basic_block_instance_count(&BB);
    Instruction *I = BB.getTerminator();
    std::stringstream ss;

    const DebugLoc &DL = I->getDebugLoc();
    if (!DL) {
      ss << "nofile:0";
    } else {
      unsigned Lin = DL.getLine();
      DIScope *Scope = cast<DIScope>(DL.getScope());
      StringRef File = Scope->getFilename();
      ss << File.str() << ":" << std::dec << Lin;
    }

    if (repFactor > 0) {
      std::cerr << "Implementation for block : " << BB.getName().str()
                << " function " << BB.getParent()->getName().str()
                << "():" << ss.str() << " (area: "
                << ModuleAreaEstimator::getBasicBlockArea(*AT, &BB)
                << ") count is " << repFactor << std::endl;
    }
  }
}

void AdvisorAnalysis::dumpBlockCounts(Function *F, unsigned cpuLatency = 0) {
  std::unordered_map<BasicBlock *, unsigned> blockCounts;
  TraceGraph::vertex_iterator vi, ve;

  for (auto fIt = executionGraph[F].begin(); fIt != executionGraph[F].end();
       fIt++) {
    TraceGraph graph = *fIt;
    // set the vertices up with zero values for this tid.
    for (boost::tie(vi, ve) = vertices(graph); vi != ve; vi++) {
      auto BB = graph[*vi].basicblock;
      BasicBlock *searchBB = find_basicblock_by_name(BB->getName());
      if (searchBB == NULL) {
        // now that we are ignoring 'dangling' basic blocks in
        // process_basic_block(), this case should not occur
        DEBUG(*outputLog << "WARNING bb " << BB->getName()
                         << " does not belong to " << F->getName() << "\n");
        assert(0);
        // continue;
      }
      if (blockCounts.find(BB) != blockCounts.end()) {
        blockCounts[BB] = blockCounts[BB] + 1;
      } else {
        blockCounts[BB] = 1;
      }
    }
  }

  // Dump block counts
  for (auto blockIt = blockCounts.begin(); blockIt != blockCounts.end();
       blockIt++) {
    auto BB = (*blockIt).first;
    auto count = (*blockIt).second;
    uint64_t totalCycles =
        count * (uint64_t)ModuleScheduler::getBasicBlockLatencyCpu(*LT, BB);
    std::cerr << "Basic block: " << BB->getName().str() << " function "
              << BB->getParent()->getName().str() << " count: " << count
              << " cpu latency: " << totalCycles;
    if (cpuLatency != 0) {
      std::cerr << " fraction of total latency: "
                << ((double)(totalCycles) / ((double)cpuLatency));
    }
    std::cerr << std::endl;
  }
}

void AdvisorAnalysis::dumpBlockCountsGlobal(unsigned cpuLatency = 0) {
  std::unordered_map<BasicBlock *, unsigned> blockCounts;
  TraceGraph::vertex_iterator vi, ve;

  TraceGraph graph = *globalTraceGraph;
  // set the vertices up with zero values for this tid.
  for (boost::tie(vi, ve) = vertices(graph); vi != ve; vi++) {
    auto BB = graph[*vi].basicblock;
    if (blockCounts.find(BB) != blockCounts.end()) {
      blockCounts[BB] = blockCounts[BB] + 1;
    } else {
      blockCounts[BB] = 1;
    }
  }

  // Dump block counts
  for (auto blockIt = blockCounts.begin(); blockIt != blockCounts.end();
       blockIt++) {
    auto BB = (*blockIt).first;
    auto count = (*blockIt).second;
    uint64_t totalCycles =
        count * (uint64_t)ModuleScheduler::getBasicBlockLatencyCpu(*LT, BB);
    std::cerr << "Basic block: " << BB->getName().str() << " function "
              << BB->getParent()->getName().str() << " count: " << count
              << " cpu latency: " << totalCycles;
    if (cpuLatency != 0) {
      std::cerr << " fraction of total latency: "
                << ((double)(totalCycles) / ((double)cpuLatency));
    }
    std::cerr << std::endl;
  }
}

void AdvisorAnalysis::print_optimal_configuration_for_all_calls(Function *F) {
  // std::cerr << "PRINTOUT OPTIMAL CONFIGURATION\n";
  int callNum = 0;
  for (TraceGraphList::iterator fIt = executionGraph[F].begin();
       fIt != executionGraph[F].end(); fIt++) {

    callNum++;
    std::string outfileName(F->getName().str() + "." + std::to_string(callNum) +
                            ".final.dot");
    TraceGraphVertexWriter<TraceGraph> vpw(*fIt, this);
    TraceGraphEdgeWriter<TraceGraph> epw(*fIt);
    std::ofstream outfile(outfileName);
    boost::write_graphviz(outfile, *fIt, vpw, epw);
  }
}

bool AdvisorAnalysis::getDependenceGraphFromFile(std::string fileName,
                                                 DepGraph **DG,
                                                 bool isGlobal) {
  DepGraph *depGraph = NULL;
  depGraph = (!isGlobal) ? new DepGraph : *DG;

  ifstream fin;
  fin.open(fileName.c_str());

  // file not found
  if (!fin.good()) return false;

  std::string line;
  const char *delim = " ";
  while (std::getline(fin, line)) {
    std::vector<char> rawLine(line.size()+1);
    strncpy(&rawLine[0], line.c_str(), line.length());
    rawLine[line.length()] = '\0';
    
    char *token = std::strtok(&rawLine[0], delim);
    if (token != NULL) {
      if (strcmp(token, "vertex") == 0) {
        token = std::strtok(NULL, delim);
        std::string bbString(token);

        token = std::strtok(NULL, delim);
        std::string vString(token);

        BasicBlock *BB = find_basicblock_by_name(bbString);

        // add vertex
        auto currVertex = boost::add_vertex(*depGraph);
        (*depGraph)[currVertex] = BB;
        BlockMap[BB] = currVertex;
      }
      else if (strcmp(token, "edge") == 0) {
        token = std::strtok(NULL, delim);
        int source = std::atoi(token);

        token = std::strtok(NULL, delim);
        int target = std::atoi(token);

        token = std::strtok(NULL, delim);
        bool trueDep = (std::atoi(token) == 1);

        boost::add_edge(source, target, trueDep, *depGraph);
      }
      else {
        assert(0 && "Invalid input in graph file!");
      }
    }
    else {
      assert(0 && "Error reading line from graph file!");
    }
  }

  if (!isGlobal) {
    *DG = depGraph;
  }

  return true;
}

bool AdvisorAnalysis::isBBDependenceTrue(BasicBlock *BB1,
                                         BasicBlock *BB2,
                                         DepGraph &DG) {
  auto bb1 = BlockMap[BB1];
  auto bb2 = BlockMap[BB2];

  // get the edge
  std::pair<DepGraph::edge_descriptor, bool> ep = boost::edge(bb1, bb2, DG);

  if (ep.second) {
    return boost::get(true_dependence_t(), DG, ep.first);
  }
  
  return false; // such edge does not exist
}

// Function: modify_resource_requirement
// This function will use the gradient descent method to reduce the resource
// requirements
// for the program
void AdvisorAnalysis::modify_resource_requirement(
    Function *F, TraceGraphList::iterator graph_it) {
  // add code here...
}

// this is not super optimal due to things like false sharing. But it is easier
// to code.
BBSchedElem::BBSchedElem()
    : cycStart(UseThreads, 0), cycEnd(UseThreads, 0), minCycStart(-1) {}

void ScheduleVisitor::discover_vertex(TraceGraph::vertex_descriptor v,
                                      const TraceGraph &graph) {
  // find the latest finishing parent
  // if no parent, start at 0
  int start = -1;
  TraceGraph::in_edge_iterator ii, ie;
  for (boost::tie(ii, ie) = boost::in_edges(v, graph); ii != ie; ii++) {
    start = std::max(start, graph[boost::source(*ii, graph)].minCycEnd);
  }
  start += 1;

  int end = start;
  BasicBlock *BB = graph[v].basicblock;

  if (AssumePipelining) {
    int pipeline_latency = (int)AssumePipelining;
    end += std::min(pipeline_latency,
                    ModuleScheduler::getBasicBlockLatencyAccelerator(LT, BB));
  } else {
    end += ModuleScheduler::getBasicBlockLatencyAccelerator(LT, BB);
  }

  // std::cerr << "Schedule vertex: (" << v << ") " <<
  // graph[v].basicblock->getName().str() <<
  //			" start: " << start << " end: " << end << "\n";
  (*graph_ref)[v].setMinStart(start);
  (*graph_ref)[v].setMinEnd(end);

  // These two are really not necessary.
  (*graph_ref)[v].setStart(start, tid);
  (*graph_ref)[v].setEnd(end, tid);

  // keep track of the last cycle as seen by the scheduler
  *lastCycle_ref = std::max(*lastCycle_ref, end);
  // std::cerr << "LastCycle: " << *lastCycle_ref << "\n";
}

void ConstrainedScheduleVisitor::discover_vertex(
    TraceGraph::vertex_descriptor v, const TraceGraph &graph) {
  // find the latest finishing parent
  // if no parent, start at 0
  int64_t start = -1;
  TraceGraph::in_edge_iterator ii, ie;
  for (boost::tie(ii, ie) = boost::in_edges(v, (*graph_ref)); ii != ie; ii++) {
    auto s = boost::source(*ii, (*graph_ref));
    // TraceGraph_vertex_descriptor t = boost::target(*ii, (*graph_ref));
    // std::cerr << (*graph_ref)[s].ID << " -> " << (*graph_ref)[t].ID << "\n";
    int64_t transitionDelay =
        (int)boost::get(boost::edge_weight_t(), (*graph_ref), *ii);

    // std::cerr << "MINIMUM END CYCLE FOR EDGE: " << (*graph_ref)[s].get_end()
    // << "\n";
    // std::cerr << "TRANSITION DELAY: " << transitionDelay << "\n";
    // std::cerr << "MAX START: " << start << "\n";

    start = std::max(start, (*graph_ref)[s].getEnd(tid) + transitionDelay);
    // std::cerr << "NEW START: " << start << "\n";
  }
  start += 1;

  BasicBlock *BB = (*graph_ref)[v].basicblock;

  // this differs from the maximal parallelism configuration scheduling
  // in that it also considers resource requirement

  // first sort the vector
  auto search = resourceTable->find(BB);
  // assert(search != resourceTable.end());
  // DEBUG(if (search == resourceTable.end()) {
  //	// if not found, could mean that either
  //	// a) basic block to be executed on cpu
  //	// b) resource table not initialized properly o.o
  //	std::cerr << "Basic block " << (*graph_ref)[v].name << " not found in
  // resource table.\n";
  //	assert(0);
  //  })

  int64_t resourceReady = UINT_MAX;
  unsigned minIdx;

  // should we do something with start. It may be that there will be a
  // unit we could use before hand, but we will instead use an earlier available
  // unit.

  std::vector<unsigned> &resourceVector = search->second;
  bool cpu = (resourceVector.size() == 0);

  if (cpu) { // cpu resource flag
    resourceReady = *cpuCycle_ref;
  } else {
    // find the minimum index
    for (unsigned idx = 0; idx < resourceVector.size(); idx++) {
      if (resourceVector[idx] < resourceReady) {
        minIdx = idx;
        resourceReady = resourceVector[idx];
      }
    }
  }

  start = std::max(start, resourceReady);

  int64_t end = start;
  int64_t block_free = start;

  // Assign endpoint based on cpu or accelerator.
  if (cpu) {
    end += ModuleScheduler::getBasicBlockLatencyCpu(LT, BB);
  } else if (AssumePipelining) {
    int pipeline_latency = (int)AssumePipelining;
    end += ModuleScheduler::getBasicBlockLatencyAccelerator(LT, BB);
    block_free +=
        std::min(pipeline_latency,
                 ModuleScheduler::getBasicBlockLatencyAccelerator(LT, BB));
  } else {
    end += ModuleScheduler::getBasicBlockLatencyAccelerator(LT, BB);
    block_free += end;
  }

  // update the occupied resource with the new end cycle
  if (cpu) {
    *cpuCycle_ref = end;
  } else {
    resourceVector[minIdx] = block_free;
  }

  // std::cerr << "Schedule vertex: " << graph[v].basicblock->getName().str() <<
  //  " start: " << start << " end: " << end << "\n";
  (*graph_ref)[v].setStart(start, tid);
  (*graph_ref)[v].setEnd(end, tid);

  // std::cerr << "VERTEX [" << v << "] START: " << (*graph_ref)[v].get_start()
  // << " END: " << (*graph_ref)[v].get_end() << "\n";

  // keep track of last cycle as seen by scheduler
  *lastCycle_ref = std::max(*lastCycle_ref, end);
  // std::cerr << "LastCycle: " << *lastCycle_ref << "\n";
}

char AdvisorAnalysis::ID = 0;
static RegisterPass<AdvisorAnalysis> T("fpga-advisor-analysis",
                                       "FPGA-Advisor Analysis Pass -- to be "
                                       "executed after instrumentation and "
                                       "program run",
                                       false, false);

char ModuleScheduler::ID = 0;
static RegisterPass<ModuleScheduler>
    Z("module-scheduler", "FPGA-Advisor Analysis Module Scheduler Pass", false,
      true);

char ModuleAreaEstimator::ID = 0;
static RegisterPass<ModuleAreaEstimator>
    Y("module-area-estimator",
      "FPGA-Advisor Analysis Module Area Estimator Pass", false, true);

void *ModuleAreaEstimator::analyzerLibHandle;
int (*ModuleAreaEstimator::getBlockArea)(BasicBlock *BB);
bool ModuleAreaEstimator::useDefault = true;

void *ModuleScheduler::analyzerLibHandle;
int (*ModuleScheduler::getBlockLatency)(BasicBlock *BB);
int (*ModuleScheduler::getBlockII)(BasicBlock *BB);
bool ModuleScheduler::useDefault = true;
