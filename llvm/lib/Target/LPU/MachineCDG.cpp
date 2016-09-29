//===- MachineCDG.cpp ---------------------*- C++ -*-===//
//
// This file defines the ControlDependenceGraph class, which allows fast and 
// efficient control dependence queries. It is based on Ferrante et al's "The 
// Program Dependence Graph and Its Use in Optimization."
//
//===----------------------------------------------------------------------===//
#include "LPU.h"
#include "MachineCDG.h"

#include "llvm/Analysis/DOTGraphTraitsPass.h"
#include "llvm/ADT/PostOrderIterator.h"
#include "llvm/Target/TargetInstrInfo.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineBasicBlock.h"
#include "llvm/Support/Debug.h"
#include "llvm/Support/raw_ostream.h"
#include "LPUInstrInfo.h"
#include <deque>
#include <set>

using namespace llvm;

//  Because of the namespace-related syntax limitations of gcc, we need
//  To hoist init out of namespace blocks. 
char ControlDependenceGraph::ID = 0;
//declare ControlDependenceGraph Pass
INITIALIZE_PASS(ControlDependenceGraph, "machine-cdg",
  "Machine Control Dependence Graph Construction", true, true)


#define DEBUG_TYPE "lpu-cdg-pass"
namespace llvm {

void ControlDependenceNode::addTrue(ControlDependenceNode *Child) {
  TrueChildren.insert(Child);
}

void ControlDependenceNode::addFalse(ControlDependenceNode *Child) {
  FalseChildren.insert(Child);
}

void ControlDependenceNode::addOther(ControlDependenceNode *Child) {
  OtherChildren.insert(Child);
}

void ControlDependenceNode::addParent(ControlDependenceNode *Parent) {
  assert(std::find(Parent->begin(), Parent->end(), this) != Parent->end()
  	 && "Must be a child before adding the parent!");
  Parents.insert(Parent);
}

void ControlDependenceNode::removeTrue(ControlDependenceNode *Child) {
  node_iterator CN = TrueChildren.find(Child);
  if (CN != TrueChildren.end())
    TrueChildren.erase(CN);
}

void ControlDependenceNode::removeFalse(ControlDependenceNode *Child) {
  node_iterator CN = FalseChildren.find(Child);
  if (CN != FalseChildren.end())
    FalseChildren.erase(CN);
}

void ControlDependenceNode::removeOther(ControlDependenceNode *Child) {
  node_iterator CN = OtherChildren.find(Child);
  if (CN != OtherChildren.end())
    OtherChildren.erase(CN);
}

void ControlDependenceNode::removeParent(ControlDependenceNode *Parent) {
  node_iterator PN = Parents.find(Parent);
  if (PN != Parents.end())
    Parents.erase(PN);
}

const ControlDependenceNode *ControlDependenceNode::enclosingRegion() const {
  if (this->isRegion()) {
    return this;
  } else {
    assert(this->Parents.size() == 1);
    const ControlDependenceNode *region = *this->Parents.begin();
    assert(region->isRegion());
    return region;
  }
}

#if 0
bool ControlDependenceNode::isLatchNode() {
  if (isChild(this)) {
    //assert a cycle is properly formed
    assert(isParent(this));
    return true;
  }
  return false;
}
#endif

ControlDependenceNode::EdgeType
ControlDependenceGraphBase::getEdgeType(MachineBasicBlock *A, MachineBasicBlock *B) {
  SmallVector<MachineOperand, 4> Cond; // For AnalyzeBranch.
  Cond.clear();
  MachineBasicBlock *TBB = nullptr, *FBB = nullptr; // For AnalyzeBranch
  assert(A->isSuccessor(B) && "Asking for edge type between unconnected basic blocks!");
  if (TII->AnalyzeBranch(*A, TBB, FBB, Cond)) { 
    //no branch, just fall through
    return ControlDependenceNode::OTHER;
  } else if (!FBB && Cond.empty()) {
    //unconditional jump
    return ControlDependenceNode::OTHER;
  } else if (!FBB && !Cond.empty() && TBB) { 
    //branch followed by a fall through
		if (TBB == B) {
			if (A->getFirstTerminator()->getOpcode() == LPU::BT)
				return ControlDependenceNode::TRUE;
			else
				return ControlDependenceNode::FALSE;
		} else {
			if (A->getFirstTerminator()->getOpcode() == LPU::BT)
				return ControlDependenceNode::FALSE;	
			else 
				return ControlDependenceNode::TRUE;
		}
  } else if (TBB && !Cond.empty() && FBB) {
    // a two-way branch
		if (TBB == B) {
			if (A->getFirstTerminator()->getOpcode() == LPU::BT) {
				return ControlDependenceNode::TRUE;
			} else {
				return ControlDependenceNode::FALSE;
			}
		} else {
			if (A->getFirstTerminator()->getOpcode() == LPU::BT) {
				return ControlDependenceNode::FALSE;
			}	else {
				return ControlDependenceNode::TRUE;
			}
		}
  } else {
    assert(false && "unexpected case");
  }
}

#if 0
ControlDependenceNode* ControlDependenceGraphBase::getLatchParent(ControlDependenceNode* anode) {
  if (anode->getNumParents() == 0) return NULL;
  for (ControlDependenceNode::node_iterator pnode = anode->parent_begin(), pend = anode->parent_end(); pnode != pend; ++pnode) {
    ControlDependenceNode* pcdn = *pnode;
    if (pcdn->isLatchNode()) {
      MachineDomTreeNode* ppdt = thisPDT->getNode(pcdn->getBlock());
      MachineDomTreeNode* npdt = thisPDT->getNode(anode->getBlock());
      if (npdt->getIDom() == ppdt) {
        //parent is latch, and an immediate post dominator
        return pcdn;
      }
    }
  }
  return NULL;
}
#endif 

#if 0
//return the first non latch parent found or NULL
ControlDependenceNode* ControlDependenceGraphBase::getNonLatchParent(ControlDependenceNode* anode, bool oneAndOnly=false) {
  ControlDependenceNode* pcdn = nullptr;
  if (anode->getNumParents() == 0) return pcdn;
  for (ControlDependenceNode::node_iterator pnode = anode->parent_begin(), pend = anode->parent_end(); pnode != pend; ++pnode) {
    if (!(*pnode)->isLatchNode()) {
      if (oneAndOnly && pcdn) {
        assert(false && "CDG node has more than one if parent");
      }
      pcdn = *pnode;
    }
  }
  return pcdn;
}
#endif 

void ControlDependenceGraphBase::computeDependencies(MachineFunction &F, MachinePostDominatorTree &pdt) {
  root = new ControlDependenceNode();
  nodes.insert(root);
  for (MachineFunction::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
    ControlDependenceNode *bn = new ControlDependenceNode(BB);
    nodes.insert(bn);
    bb2cdg[BB] = bn;
    cdg2bb[bn] = BB;
  }

  for (MachineFunction::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
    MachineBasicBlock *A = BB;
    ControlDependenceNode *AN = bb2cdg[A];

    for (MachineBasicBlock::succ_iterator succ = A->succ_begin(), end = A->succ_end(); succ != end; ++succ) {
      MachineBasicBlock *B = *succ;
      assert(A && B);
      if (A == B || !pdt.dominates(B, A)) {
        MachineBasicBlock *L = pdt.findNearestCommonDominator(A, B);
        ControlDependenceNode::EdgeType type = ControlDependenceGraphBase::getEdgeType(A, B);
        if (A == L) {
          switch (type) {
          case ControlDependenceNode::TRUE:
            AN->addTrue(AN); break;
          case ControlDependenceNode::FALSE:
            AN->addFalse(AN); break;
          case ControlDependenceNode::OTHER:
            AN->addOther(AN); break;
          }
          AN->addParent(AN);
        }
        for (MachineDomTreeNode *cur = pdt[B]; cur && cur != pdt[L]; cur = cur->getIDom()) {
          ControlDependenceNode *CN = bb2cdg[cur->getBlock()];
          switch (type) {
          case ControlDependenceNode::TRUE:
            AN->addTrue(CN); break;
          case ControlDependenceNode::FALSE:
            AN->addFalse(CN); break;
          case ControlDependenceNode::OTHER:
            AN->addOther(CN); break;
          }
          assert(CN);
          CN->addParent(AN);
        }
      }
    }
  }

  // ENTRY -> START
  for (MachineDomTreeNode *cur = pdt[&F.front()]; cur; cur = cur->getIDom()) {
    if (cur->getBlock()) {
      ControlDependenceNode *CN = bb2cdg[cur->getBlock()];
      assert(CN);
      root->addOther(CN); CN->addParent(root);
    }
  }
}

void ControlDependenceGraphBase::insertRegions(MachinePostDominatorTree &pdt) {
  typedef po_iterator<MachinePostDominatorTree*> po_pdt_iterator;  
  typedef std::pair<ControlDependenceNode::EdgeType, ControlDependenceNode *> cd_type;
  typedef std::set<cd_type> cd_set_type;
  typedef std::map<cd_set_type, ControlDependenceNode *> cd_map_type;

  cd_map_type cdMap;
  cd_set_type initCDs;
  initCDs.insert(std::make_pair(ControlDependenceNode::OTHER, root));
  cdMap.insert(std::make_pair(initCDs,root));

  for (po_pdt_iterator DTN = po_pdt_iterator::begin(&pdt), END = po_pdt_iterator::end(&pdt);
       DTN != END; ++DTN) {
    if (!DTN->getBlock())
      continue;

    ControlDependenceNode *node = bb2cdg[DTN->getBlock()];
    assert(node);

    cd_set_type cds;
    for (ControlDependenceNode::node_iterator P = node->Parents.begin(), E = node->Parents.end(); P != E; ++P) {
      ControlDependenceNode *parent = *P;
      if (parent->TrueChildren.find(node) != parent->TrueChildren.end())
        cds.insert(std::make_pair(ControlDependenceNode::TRUE, parent));
      if (parent->FalseChildren.find(node) != parent->FalseChildren.end())
        cds.insert(std::make_pair(ControlDependenceNode::FALSE, parent));
      if (parent->OtherChildren.find(node) != parent->OtherChildren.end())
        cds.insert(std::make_pair(ControlDependenceNode::OTHER, parent));
    }

    cd_map_type::iterator CDEntry = cdMap.find(cds);
    ControlDependenceNode *region;
    if (CDEntry == cdMap.end()) {
      region = new ControlDependenceNode();
      nodes.insert(region);
      cdMap.insert(std::make_pair(cds,region));
      for (cd_set_type::iterator CD = cds.begin(), CDEnd = cds.end(); CD != CDEnd; ++CD) {
        switch (CD->first) {
        case ControlDependenceNode::TRUE:
          CD->second->addTrue(region);
          break;
        case ControlDependenceNode::FALSE:
          CD->second->addFalse(region);
          break;
        case ControlDependenceNode::OTHER:
          CD->second->addOther(region);
          break;
        }
        region->addParent(CD->second);
      }
    } else {
      region = CDEntry->second;
    }
    for (cd_set_type::iterator CD = cds.begin(), CDEnd = cds.end(); CD != CDEnd; ++CD) {
      switch (CD->first) {
      case ControlDependenceNode::TRUE:
        CD->second->removeTrue(node);
        break;
      case ControlDependenceNode::FALSE:
        CD->second->removeFalse(node);
        break;
      case ControlDependenceNode::OTHER:
        CD->second->removeOther(node);
        break;
      }
      region->addOther(node);
      node->addParent(region);
      node->removeParent(CD->second);
    }
  }

  // Make sure that each node has at most one true or false edge
  for (std::set<ControlDependenceNode *>::iterator N = nodes.begin(), E = nodes.end();
       N != E; ++N) {
    ControlDependenceNode *node = *N;
    assert(node);
    if (node->isRegion())
      continue;

    // Fix too many true nodes
    if (node->TrueChildren.size() > 1) {
      ControlDependenceNode *region = new ControlDependenceNode();
      nodes.insert(region);
      ControlDependenceNode::node_iterator C = node->true_begin(), CE = node->true_end();
      while (C != CE) {
        ControlDependenceNode *child = *C;
        ++C;
        assert(node);
        assert(child);
        assert(region);
        region->addOther(child);
        child->addParent(region);
        child->removeParent(node);
        node->removeTrue(child);

      }
      node->addTrue(region);
      region->addParent(node);
    }

    // Fix too many false nodes
    if (node->FalseChildren.size() > 1) {
      ControlDependenceNode *region = new ControlDependenceNode();
      nodes.insert(region);
      ControlDependenceNode::node_iterator C = node->false_begin(), CE = node->false_end();
      while (C != CE) {
        ControlDependenceNode *child = *C;
        ++C;
        region->addOther(child);
        child->addParent(region);
        child->removeParent(node);
        node->removeFalse(child);
      }
      node->addFalse(region);
      region->addParent(node);
    }
  }
}

void ControlDependenceGraphBase::graphForFunction(MachineFunction &F, MachinePostDominatorTree &pdt) {
  computeDependencies(F,pdt);
  //insertRegions(pdt);
  regionsForGraph(F, pdt);

  dumpRegions();

}

//base on "compact representaions for control dependence, by Cytron, Ferrante, Sarkar"
//ControlDependenceNode is the link between these ADT:
//ControlDependenceNode => MachineBasicBlock
//ControlDependenceNode => Region
//The original paper actually compute the week region, this algorithms enhance it to compute a strong region 
//if the loop latch has exit edge, as most LLVM loops do,or it is a while loop
void ControlDependenceGraphBase::regionsForGraph(MachineFunction &F, MachinePostDominatorTree &pdt) {
  typedef po_iterator<MachinePostDominatorTree*> po_pdt_iterator;
  DenseMap<MachineBasicBlock *, CDGRegion *> mbb2rgn;
  CDGRegion* rootRegion = new CDGRegion;
  //regions[0] = rootRegion;
  regions.push_back(rootRegion);
  rootRegion->NewRegion = 0;
  unsigned NumRegions = 0;
  //first, add all CDG nodes into region 0, by postorder traversal of the pdt, so that
  //RTAIL(0)==STOP; and postdominator of any node X is linked into the list somewhere AFTER X
  for (po_pdt_iterator DTN = po_pdt_iterator::begin(&pdt), END = po_pdt_iterator::end(&pdt);
    DTN != END; ++DTN) {
    if (!DTN->getBlock())
      continue;
    ControlDependenceNode *node = bb2cdg[DTN->getBlock()];
    rootRegion->nodes.insert(node);
    cdg2rgn[node] = rootRegion;
  }

  for (MachineFunction::iterator BB = F.begin(), E = F.end(); BB != E; ++BB) {
    MachineBasicBlock *A = BB;
    for (MachineBasicBlock::succ_iterator succ = A->succ_begin(), end = A->succ_end(); succ != end; ++succ) {
      MachineBasicBlock *B = *succ;
      assert(A && B);
      unsigned T = NumRegions;
      if (A == B || !pdt.dominates(B, A)) {
        MachineDomTreeNode *Y= pdt.getNode(B);
        MachineDomTreeNode *StartDN = Y;
        MachineBasicBlock *L = pdt.findNearestCommonDominator(A, B);
        MachineBasicBlock *loopLatch = NULL;
        if (A == L) {
          loopLatch = A;
        }
        MachineDomTreeNode *EndDN = pdt.getNode(A)->getIDom();
        while (Y != EndDN) {
          MachineBasicBlock *YB = Y->getBlock();
          CDGRegion *YR = cdg2rgn[bb2cdg[YB]];
          //RHEAD
          ControlDependenceNode *YRHdr = YR->nodes[0];
          MachineBasicBlock *YRHdrBB = cdg2bb[YRHdr];
          MachineDomTreeNode *YRHdrDN = pdt.getNode(YRHdrBB);
          //RTAIL
          ControlDependenceNode *YRTail = YR->nodes.back();
          MachineBasicBlock *YRTailBB = cdg2bb[YRTail];
          MachineDomTreeNode *YRTailDN = pdt.getNode(YRTailBB);
          bool isYBtwnStartEnd = pdt.dominates(YRHdrDN, StartDN) &&
                                 pdt.properlyDominates(EndDN, YRTailDN);
		  if (!isYBtwnStartEnd || loopLatch && Y->getBlock() == loopLatch && YR->nodes.size() > 1) {
			//modification to the original paper: latch node need to be in a seperate region by itself
			if (YR->NewRegion <= T || loopLatch && Y->getBlock() == loopLatch && YR->nodes.size() > 1) {
			  NumRegions++;
              CDGRegion *splitRgn = new CDGRegion();
              //regions[NumRegions] = splitRgn;
              regions.push_back(splitRgn);
              //YR's splited new region has region# NumRegions in regions list
              YR->NewRegion = NumRegions;
              //splitRgn's new region is itself -- not splited yet
              splitRgn->NewRegion = NumRegions;
            }
            ControlDependenceNode *YCN = bb2cdg[YB];
            //delete Y from YR
            YR->nodes.remove(YCN);
            //add Y at tail of the new region
            regions[YR->NewRegion]->nodes.insert(YCN); 
            //denote Y is in a new region now
            cdg2rgn[bb2cdg[YB]] = regions[YR->NewRegion];
          }
          Y = Y->getIDom();
        } //end of while
      } //end of if(A == B ...
    } //end of edge AB's end point
  }//end of for(A
}

void ControlDependenceGraphBase::dumpRegions() {
  for (unsigned i = 0; i < regions.size(); i++) {
    CDGRegion *r = regions[i];
    DEBUG(errs() << "Region" << i << ": ");
    for (SetVector<ControlDependenceNode *>::iterator N = r->nodes.begin(), E = r->nodes.end();
      N != E; ++N) {
      ControlDependenceNode *node = *N;
      assert(node);
      DEBUG(errs() << "BB" << cdg2bb[node]->getNumber() << ", ");
    }
    DEBUG(errs() << "\n");
  }
}

bool ControlDependenceGraphBase::controls(MachineBasicBlock *A, MachineBasicBlock *B) const {
  const ControlDependenceNode *n = getNode(B);
  assert(n && "Basic block not in control dependence graph!");
  while (n->getNumParents() == 1) {
    n = *n->parent_begin();
    if (n->getBlock() == A)
      return true;
  }
  return false;
}

bool ControlDependenceGraphBase::influences(MachineBasicBlock *A, MachineBasicBlock *B) const {
  const ControlDependenceNode *n = getNode(B);
  assert(n && "Basic block not in control dependence graph!");

  std::deque<ControlDependenceNode *> worklist;
  worklist.insert(worklist.end(), n->parent_begin(), n->parent_end());

  while (!worklist.empty()) {
    n = worklist.front();
    worklist.pop_front();
    if (n->getBlock() == A) return true;
    worklist.insert(worklist.end(), n->parent_begin(), n->parent_end());
  }

  return false;
}




const ControlDependenceNode *ControlDependenceGraphBase::enclosingRegion(MachineBasicBlock *BB) const {
  if (const ControlDependenceNode *node = this->getNode(BB)) {
    return node->enclosingRegion();
  } else {
    return NULL;
  }
}

ControlDependenceGraph::ControlDependenceGraph() : MachineFunctionPass(ID), ControlDependenceGraphBase() {
  initializeControlDependenceGraphPass(*PassRegistry::getPassRegistry());
}


void ControlDependenceGraph::writeDotGraph(StringRef fname) {
  std::string Filename = fname.str() + "_CDG" + ".dot";
  std::error_code EC;

  DEBUG(errs() << "Writing '" << Filename << "'...");

  raw_fd_ostream File(Filename, EC, sys::fs::F_Text);
  GraphWriter<ControlDependenceGraph *> gwr(File, this, false);
  gwr.writeGraph();

  Filename = fname.str() + "_CFG" + ".dot";
  raw_fd_ostream File1(Filename, EC, sys::fs::F_Text);
  GraphWriter<MachineFunction *> gwr1(File1, thisMF, false);
  gwr1.writeGraph();

  MachinePostDominatorTree &pdt = getAnalysis<MachinePostDominatorTree>();
  Filename = fname.str() + "_PDT" + ".dot";
  raw_fd_ostream File2(Filename, EC, sys::fs::F_Text);
  GraphWriter<MachinePostDominatorTree *> gwr2(File2, &pdt, false);
  gwr2.writeGraph();

  MachineDominatorTree &dt = getAnalysis<MachineDominatorTree>();
  Filename = fname.str() + "_DT" + ".dot";
  raw_fd_ostream File3(Filename, EC, sys::fs::F_Text);
  GraphWriter<MachineDominatorTree *> gwr3(File3, &dt, false);
  gwr3.writeGraph();
}

} // namespace llvm


MachineFunctionPass *llvm::createControlDepenceGraph() {
  return new ControlDependenceGraph();
}

