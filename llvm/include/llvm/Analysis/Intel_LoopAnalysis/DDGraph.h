//===------- DDTest.h - Provides Data Dependence Analysis -*-- C++ --*-----===//
//
// Copyright (C) 2015 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//  This file contains common data structures for DD.
//  Somewhat misleadingly, it does not contain ddgraph itself, but rather its
//  parent class which contains the implementation of graph logic. This could
//  be ripped out to make a general graph in the future.
//
//  The other data structures are needed to implement dd tests, such as
//  DirectionVector. This is a common location to DDAnalysis and DDTest as both
//  need these defs, and we don't want those two including each other.
//
//  //TODO: in light of above comments, rename this?
//===----------------------------------------------------------------------===//
#ifndef INTEL_LOOPANALYSIS_DDGRAPH
#define INTEL_LOOPANALYSIS_DDGRAPH

#include <vector>
#include <map>
#include <list>
#include "llvm/Support/Debug.h"
#include "llvm/IR/Intel_LoopIR/CanonExpr.h"
#include "llvm/IR/Intel_LoopIR/RegDDRef.h"

namespace llvm {
namespace loopopt {
class HLNode;

struct DirectionVector {
  enum class Direction : unsigned char {
    UNINIT = 0,
    GT = 1,
    EQ = 2,
    GE = 3, // 0x1 || 0x2
    LT = 4,
    LG = 5, // 0x1 || 0x4
    LE = 6, // 0x4 || 0x2
    ALL = 7,
  };
  Direction getDVAtLevel(int Level) { return Elements[Level - 1]; }
  DirectionVector() {
    for (unsigned int i = 0; i < MaxLoopNestLevel; ++i)
      Elements[i] = Direction::UNINIT;
  }
  void setDVAtLevel(Direction NewDirection, int Level) {
    Elements[Level - 1] = NewDirection;
  }

  const std::string elementAsChar(Direction Dir) const {
    switch (Dir) {
    case Direction::UNINIT:
      return "0";
    case Direction::GT:
      return ">";
    case Direction::EQ:
      return "=";
    case Direction::GE:
      return ">=";
    case Direction::LT:
      return "<";
    case Direction::LG:
      return "<>";
    case Direction::LE:
      return "<=";
    case Direction::ALL:
      return "*";
    default:
      return "?";
    }
  }

  void print(raw_ostream &OS) const {
    OS << "[ ";
    for (unsigned int i = 0; i < MaxLoopNestLevel; ++i) {
      if (i > 0 && Elements[i] == Direction::UNINIT)
        break;
      OS << elementAsChar(Elements[i]) << " ";
    }
    OS << "]"
       << "\n";
  }

  void dump() const { print(dbgs()); }

private:
  Direction Elements[MaxLoopNestLevel];
};

// TODO move to another location to make a generic graph?
// TODO doc req
// This was meant to be the basis of a general graph class
// focused on fast iteration at the cost of slow modification
// and extra memory
template <class GraphNode, class GraphEdge> class HIRGraph {

public:
  // Don't let others modify edges. We can only remove or add
  // edges
  typename std::vector<GraphEdge>::const_iterator
  incoming_edges_begin(GraphNode *Node) {
    return inEdges[Node].cbegin();
  }
  typename std::vector<GraphEdge>::const_iterator
  incoming_edges_end(GraphNode *Node) {
    return inEdges[Node].cend();
  }
  typename std::vector<GraphEdge>::const_iterator
  outgoing_edges_begin(GraphNode *Node) {
    return outEdges[Node].cbegin();
  }
  typename std::vector<GraphEdge>::const_iterator
  outgoing_edges_end(GraphNode *Node) {
    return outEdges[Node].cend();
  }

  void addEdge(GraphEdge E) {
    inEdges[E.getSink()].push_back(E);
    outEdges[E.getSrc()].push_back(E);
  }

  void removeEdge(GraphEdge E) {
    // TODO
  }

  void print(raw_ostream &OS) const {
    for (auto I = outEdges.begin(), E = outEdges.end(); I != E; ++I) {
      std::vector<GraphEdge> edges = I->second;
      for (auto EIt = edges.begin(), EdgesEnd = edges.end(); EIt != EdgesEnd;
           ++EIt) {
        EIt->print(OS);
      }
    }
  }

  void dump() const { print(dbgs()); }

  void clear() {
    inEdges.clear();
    outEdges.clear();
  }

private:
  GraphNode *CurNode;

  // It is assumed the common operation is to iterate over inc/out edges
  // As such, we keep edge vectors for each node, with each edge stored
  // (as a struct vs ptr) twice; once in inEdges and once in outEdges
  std::map<GraphNode *, std::vector<GraphEdge>> inEdges;
  std::map<GraphNode *, std::vector<GraphEdge>> outEdges;
};

class DDEdge {
private:
  enum class DepType : unsigned char {
    OUTPUT,
    INPUT,
    ANTI,
    FLOW,
  };
  DDRef *Src;
  DDRef *Sink;
  DirectionVector DV;

public:
  DDEdge() { Src = Sink = nullptr; }
  DDEdge(DDRef *SrcRef, DDRef *SinkRef, DirectionVector DirV)
      : Src(SrcRef), Sink(SinkRef), DV(DirV) {}

  DepType getEdgeType() const {
    RegDDRef *SrcRef = dyn_cast<RegDDRef>(Src);
    RegDDRef *SinkRef = dyn_cast<RegDDRef>(Sink);
    bool SrcIsLval = (SrcRef && SrcRef->isLval()) ? true : false;
    bool SinkIsLval = (SinkRef && SinkRef->isLval()) ? true : false;

    if (SrcIsLval) {
      if (SinkIsLval)
        return DepType::OUTPUT;
      return DepType::FLOW;
    } else {
      if (SinkIsLval)
        return DepType::ANTI;
      return DepType::INPUT;
    }
  }

  DDRef *getSrc() { return Src; }
  DDRef *getSink() { return Sink; }
  friend llvm::raw_ostream &operator<<(llvm::raw_ostream &out, DepType value) {
    const char *s = 0;
    switch (value) {
    case DepType::OUTPUT:
      s = "OUTPUT";
      break;
    case DepType::ANTI:
      s = "ANTI";
      break;
    case DepType::FLOW:
      s = "FLOW";
      break;
    case DepType::INPUT:
      s = "INPUT";
      break;
    }

    return out << s;
  }

  void print(raw_ostream &OS) const {
    formatted_raw_ostream FOS(OS);
    FOS << Src->getHLDDNode()->getNumber() << ":";
    FOS << Sink->getHLDDNode()->getNumber() << " ";
    Src->print(FOS);
    FOS << " --> ";
    Sink->print(FOS);
    FOS << " ";
    FOS << getEdgeType();
    FOS << " ";
    DV.print(FOS);
    FOS << " \n";
    // todo
  }

  void dump() const { print(dbgs()); }
};

// TODO better name?
// Specific graph type for dd is our "general" graph with
// nodes being ddrefs and edges being ddedge
typedef HIRGraph<DDRef, DDEdge> DDGraphTy;
}
}

#endif
