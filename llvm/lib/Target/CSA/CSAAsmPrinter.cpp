//===-- CSAAsmPrinter.cpp - CSA LLVM assembly writer ----------------------===//
//
// Copyright (C) 2017-2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive
// property of Intel Corporation and may not be disclosed, examined
// or reproduced in whole or in part without explicit written authorization
// from the company.
//
//===----------------------------------------------------------------------===//
//
// This file contains a printer that converts from our internal representation
// of machine-dependent LLVM code to the CSA assembly language.
//
//===----------------------------------------------------------------------===//

#include "CSA.h"
#include "CSAInstrInfo.h"
#include "CSAMCInstLower.h"
#include "CSATargetMachine.h"
#include "InstPrinter/CSAInstPrinter.h"
#include "llvm/BinaryFormat/ELF.h"
#include "llvm/Bitcode/CSASaveRawBC.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/MachineConstantPool.h"
#include "llvm/CodeGen/MachineFunctionPass.h"
#include "llvm/CodeGen/MachineInstr.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/CodeGen/MachineRegisterInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/IR/DerivedTypes.h"
#include "llvm/IR/Mangler.h"
#include "llvm/IR/Module.h"
#include "llvm/MC/MCAsmInfo.h"
#include "llvm/MC/MCInst.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Support/Path.h"
#include "llvm/Support/TargetRegistry.h"
#include "llvm/Support/raw_ostream.h"
#include <fstream>
#include <sstream>

using namespace llvm;

#define DEBUG_TYPE "asm-printer"

static cl::opt<bool>
  EmitLineNumbers("csa-emit-line-numbers", cl::Hidden,
                  cl::desc("CSA Specific: Emit Line numbers even without -G"),
                  cl::init(true));

static cl::opt<bool>
  InterleaveSrc("csa-emit-src", cl::ZeroOrMore, cl::Hidden,
                cl::desc("CSA Specific: Emit source line in asm file"),
                cl::init(false));

static cl::opt<bool>
  StrictTermination("csa-strict-term", cl::Hidden,
                    cl::desc("CSA Specific: Turn on strict termination mode"),
                    cl::init(false));

static cl::opt<bool>
  ImplicitLicDefs("csa-implicit-lics", cl::Hidden,
                  cl::desc("CSA Specific: Define LICs implicitly"),
                  cl::init(false));

static cl::opt<bool>
  EmitRegNames("csa-print-lic-names", cl::Hidden,
               cl::desc("CSA Specific: Print pretty names for LICs"),
               cl::init(false));

namespace {
class LineReader {
private:
  unsigned theCurLine;
  std::ifstream fstr;
  char buff[512];
  std::string theFileName;
  SmallVector<unsigned, 32> lineOffset;

public:
  LineReader(std::string filename) {
    theCurLine = 0;
    fstr.open(filename.c_str());
    theFileName = filename;
  }
  std::string fileName() { return theFileName; }
  ~LineReader() { fstr.close(); }
  std::string readLine(unsigned lineNum) {
    if (lineNum < theCurLine) {
      theCurLine = 0;
      fstr.seekg(0, std::ios::beg);
    }
    while (theCurLine < lineNum) {
      fstr.getline(buff, 500);
      theCurLine++;
    }
    return buff;
  }
};

class CSAAsmPrinter : public AsmPrinter {
  const Function *F;
  const MachineRegisterInfo *MRI;
  DebugLoc prevDebugLoc;
  bool ignoreLoc(const MachineInstr &);
  LineReader *reader;
  // To record filename to ID mapping
  bool doInitialization(Module &M) override;
  void emitLineNumberAsDotLoc(const MachineInstr &);
  void emitSrcInText(StringRef filename, unsigned line);
  LineReader *getReader(std::string);
  void emitParamList(const Function *);
  void emitReturnVal(const Function *);

  void writeAsmLine(const char *);

public:
  CSAAsmPrinter(TargetMachine &TM, std::unique_ptr<MCStreamer> Streamer)
      : AsmPrinter(TM, std::move(Streamer)), reader() {}

  StringRef getPassName() const override { return "CSA Assembly Printer"; }

  void printOperand(const MachineInstr *MI, int OpNum, raw_ostream &O);
  bool PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                       unsigned AsmVariant, const char *ExtraCode,
                       raw_ostream &O) override;

  void EmitStartOfAsmFile(Module &) override;
  void EmitEndOfAsmFile(Module &) override;

  void EmitFunctionEntryLabel() override;
  void EmitFunctionBodyStart() override;
  void EmitFunctionBodyEnd() override;
  void EmitInstruction(const MachineInstr *MI) override;

  void EmitCsaCodeSection();
};
} // end of anonymous namespace

void CSAAsmPrinter::printOperand(const MachineInstr *MI, int OpNum,
                                 raw_ostream &O) {
  const MachineOperand &MO = MI->getOperand(OpNum);

  switch (MO.getType()) {
  case MachineOperand::MO_Register:
    O << "%" << CSAInstPrinter::getRegisterName(MO.getReg());
    break;

  case MachineOperand::MO_Immediate:
    O << MO.getImm();
    break;

  case MachineOperand::MO_MachineBasicBlock:
    O << *MO.getMBB()->getSymbol();
    break;

  case MachineOperand::MO_GlobalAddress:
    O << *getSymbol(MO.getGlobal());
    break;

  case MachineOperand::MO_BlockAddress: {
    MCSymbol *BA = GetBlockAddressSymbol(MO.getBlockAddress());
    O << BA->getName();
    break;
  }

  case MachineOperand::MO_ExternalSymbol:
    O << *GetExternalSymbolSymbol(MO.getSymbolName());
    break;

  case MachineOperand::MO_JumpTableIndex:
    O << MAI->getPrivateGlobalPrefix() << "JTI" << getFunctionNumber() << '_'
      << MO.getIndex();
    break;

  case MachineOperand::MO_ConstantPoolIndex:
    O << MAI->getPrivateGlobalPrefix() << "CPI" << getFunctionNumber() << '_'
      << MO.getIndex();
    return;

  default:
    llvm_unreachable("<unknown operand type>");
  }
}

// PrintAsmOperand - Print out an operand for an inline asm expression.
bool CSAAsmPrinter::PrintAsmOperand(const MachineInstr *MI, unsigned OpNo,
                                    unsigned /*AsmVariant*/,
                                    const char *ExtraCode, raw_ostream &O) {
  // Does this asm operand have a single letter operand modifier?
  if (ExtraCode && ExtraCode[0]) {
    if (ExtraCode[1])
      return true; // Unknown modifier.

    switch (ExtraCode[0]) {
    default:
      return true; // Unknown modifier.
    }
  }
  printOperand(MI, OpNo, O);
  return false;
}

bool CSAAsmPrinter::ignoreLoc(const MachineInstr &MI) {
  switch (MI.getOpcode()) {
  default:
    return false;
    // May be desirable to avoid CSA-specific MachineInstrs
  }
}

bool CSAAsmPrinter::doInitialization(Module &M) {
  bool result = AsmPrinter::doInitialization(M);

  // Emit module-level inline asm if it exists.
  if (!M.getModuleInlineAsm().empty()) {
    OutStreamer->AddComment("Start of file scope inline assembly");
    OutStreamer->AddBlankLine();
    OutStreamer->EmitRawText(StringRef(M.getModuleInlineAsm()));
    OutStreamer->AddBlankLine();
    OutStreamer->AddComment("End of file scope inline assembly");
    OutStreamer->AddBlankLine();
  }

  return result;
}

void CSAAsmPrinter::emitLineNumberAsDotLoc(const MachineInstr &MI) {
  if (!EmitLineNumbers)
    return;
  if (ignoreLoc(MI))
    return;

  DebugLoc curLoc = MI.getDebugLoc();

  if (!prevDebugLoc && !curLoc)
    return;

  if (prevDebugLoc == curLoc)
    return;

  prevDebugLoc = curLoc;

  if (!curLoc)
    return;

  auto *Scope = cast_or_null<DIScope>(curLoc.getScope());
  if (!Scope)
    return;

  StringRef fileName(Scope->getFilename());
  StringRef dirName(Scope->getDirectory());

  // Emit the line from the source file.
  if (InterleaveSrc)
    this->emitSrcInText(fileName.str(), curLoc.getLine());

  std::stringstream temp;

  //
  // EmitDwarfFileDirective() returns the file ID for the given
  // file path.  It will only emit the file directive once
  // for each file.
  //
  unsigned FileNo = OutStreamer->EmitDwarfFileDirective(0, dirName, fileName);

  if (FileNo == 0)
    return;

  temp << "\t.loc " << FileNo << " " << curLoc.getLine()
       << " " << curLoc.getCol();
  OutStreamer->EmitRawText(Twine(temp.str().c_str()));
}

void CSAAsmPrinter::emitSrcInText(StringRef filename, unsigned line) {
  std::stringstream temp;
  LineReader *reader = this->getReader(filename.str());
  temp << "\n#";
  temp << filename.str();
  temp << ":";
  temp << line;
  temp << " ";
  temp << reader->readLine(line);
  temp << "\n";
  this->OutStreamer->EmitRawText(Twine(temp.str()));
}

LineReader *CSAAsmPrinter::getReader(std::string filename) {
  if (!reader) {
    reader = new LineReader(filename);
  }

  if (reader->fileName() != filename) {
    delete reader;
    reader = new LineReader(filename);
  }

  return reader;
}

void CSAAsmPrinter::emitParamList(const Function *F) {
  SmallString<128> Str;
  raw_svector_ostream O(Str);
  const TargetLowering *TLI =
    MF->getSubtarget<CSASubtarget>().getTargetLowering();
  Function::const_arg_iterator I, E;
  MVT thePointerTy = TLI->getPointerTy(MF->getDataLayout());

  // Stride through parameters, putting out a .param {type} .reg %r{num}
  // This is a hack mostly taken from NVPTX.  This assumes successive
  // parameters go to successive registers, starting with the initial
  // value of paramReg.  This may be too simplistic for longer term.
  int paramReg = 2;  // Params start in R2 - see CSACallingConv.td
  int lastReg  = 17; // Params end (inclusive) in R17 - see CSACallingConv.td
  bool first   = true;
  for (I = F->arg_begin(), E = F->arg_end(); I != E && paramReg <= lastReg;
       ++I, paramReg++) {
    Type *Ty            = I->getType();
    unsigned sz         = 0;
    std::string typeStr = ".i";
    if (isa<IntegerType>(Ty)) {
      sz = cast<IntegerType>(Ty)->getBitWidth();
    } else if (Ty->isFloatingPointTy()) {
      sz = Ty->getPrimitiveSizeInBits();
    } else if (isa<PointerType>(Ty)) {
      sz = thePointerTy.getSizeInBits();
    } else {
      sz = Ty->getPrimitiveSizeInBits();
    }
    if (!first) {
      O << '\n';
    }
    O << CSAInstPrinter::WrapCsaAsmLinePrefix();
    O << "\t.param .reg " << typeStr << sz << " %r" << paramReg;
    O << CSAInstPrinter::WrapCsaAsmLineSuffix();
    first = false;
  }
  if (!first)
    OutStreamer->EmitRawText(O.str());
}

void CSAAsmPrinter::emitReturnVal(const Function *F) {
  SmallString<128> Str;
  raw_svector_ostream O(Str);
  const TargetLowering *TLI =
    MF->getSubtarget<CSASubtarget>().getTargetLowering();

  Type *Ty = F->getReturnType();

  if (Ty->getTypeID() == Type::VoidTyID)
    return;

  O << CSAInstPrinter::WrapCsaAsmLinePrefix();
  O << "\t.result .reg";

  if (Ty->isFloatingPointTy() || Ty->isIntegerTy()) {
    unsigned size = 0;
    if (const IntegerType *ITy = dyn_cast<IntegerType>(Ty)) {
      size = ITy->getBitWidth();
      O << " .i" << size;
    } else {
      assert(Ty->isFloatingPointTy() && "Floating point type expected here");
      size = Ty->getPrimitiveSizeInBits();
      O << " .i" << size;
    }

  } else if (isa<PointerType>(Ty)) {
    O << " .i" << TLI->getPointerTy(MF->getDataLayout()).getSizeInBits();
  } else if ((Ty->getTypeID() == Type::StructTyID) || isa<VectorType>(Ty)) {
    llvm_unreachable("NYI: aggregate result");
  } else
    llvm_unreachable("Unknown return type");

  // Hack: For now, we simply go with the standard return register.
  // (Should really use the allocation.)
  O << " %r0";
  O << CSAInstPrinter::WrapCsaAsmLineSuffix();

  OutStreamer->EmitRawText(O.str());
}

void CSAAsmPrinter::writeAsmLine(const char *text) {
  SmallString<128> Str;
  raw_svector_ostream O(Str);

  O << CSAInstPrinter::WrapCsaAsmLinePrefix();
  O << text;
  O << CSAInstPrinter::WrapCsaAsmLineSuffix();
  OutStreamer->EmitRawText(O.str());
}

void CSAAsmPrinter::EmitCsaCodeSection() {
  // The .section directive for an ELF object as a name and 3 optional,
  // comma separated parts as detailed at
  // https://sourceware.org/binutils/docs/as/Section.html
  //
  // The CSA code section uses the following:
  //
  // Name: ".csa.code". I may want to append the module name.
  //
  // Flag values:
  // - a - Section is allocatable - Which tells us very little. The ELF
  //       docs expand this to explain that SHF_ALLOC means that the
  //       section occupies memory during process execution
  // - S - Section contains zero terminated strings
  //
  // Type: "@progbits" - section contains data
  OutStreamer->EmitRawText("\t.section\t\".csa.code\",\"aS\",@progbits");
}

void CSAAsmPrinter::EmitStartOfAsmFile(Module &M) {

  /* Disabled 2016/3/31.  Long term, we should only put this out if it
   * is not autounit.  The theory is if the compiler has done tailoring
   * for a specific target, that should be reflected in the file.
   */
  SmallString<128> Str;
  raw_svector_ostream O(Str);
  const CSATargetMachine *CSATM = static_cast<const CSATargetMachine *>(&TM);
  assert(CSATM && CSATM->getSubtargetImpl());
  O << CSAInstPrinter::WrapCsaAsmLinePrefix();
  O << "\t# .processor "; // note - commented out...
  O << CSATM->getSubtargetImpl()->csaName();
  O << CSAInstPrinter::WrapCsaAsmLineSuffix();
  OutStreamer->EmitRawText(O.str());

  writeAsmLine("\t.version 0,6,0");
  // This should probably be replaced by code to handle externs
  writeAsmLine("\t.set implicitextern");
  if (not StrictTermination)
    writeAsmLine("\t.set relaxed");
  if (ImplicitLicDefs)
    writeAsmLine("\t.set implicit");
  writeAsmLine("\t.unit sxu");
}

void CSAAsmPrinter::EmitEndOfAsmFile(Module &M) {

  if (CSAInstPrinter::WrapCsaAsm()) {
    // Add the terminating null for the .csa section. Note
    // that we are NOT using SwitchSection because then we'll
    // fight with the AmsPrinter::EmitFunctionHeader
    EmitCsaCodeSection();
    OutStreamer->EmitRawText("\t.asciz \"\"");

    // Dump the raw IR to the file as data. We want this information
    // loaded into the address space, so we're giving it the "a" flag
    auto *SRB = getAnalysisIfAvailable<CSASaveRawBC>();
    assert(SRB && "CSASaveRawBC should always be available!");

    const std::string &rawBC = SRB->getRawBC();
    OutStreamer->EmitRawText("\t.section\t\".csa.bc.data\",\"a\",@progbits");
    OutStreamer->EmitRawText(".csa.bc.start:");

    for (size_t i = 0; i < rawBC.size(); ++i) {
      OutStreamer->EmitIntValue(rawBC[i], 1);
    }
    OutStreamer->EmitRawText(".csa.bc.end:");

    // Finish the file with a data structure entry containing
    // the bounds of the IR for this file. The linker will
    // concatenate the data in the .csa.bc.data and .csa.bc.bounds
    // sections, and we'll need to bounds information to allow us
    // to write the individual bitcode files to disk so they can be
    // concatenated by llvm-link
    OutStreamer->EmitRawText("\t.section\t\".csa.bc.bounds\",\"a\",@progbits");
    OutStreamer->EmitRawText("\t.quad\t.csa.bc.start");
    OutStreamer->EmitRawText("\t.quad\t.csa.bc.end\n");
  }
}

void CSAAsmPrinter::EmitFunctionEntryLabel() {
  SmallString<128> Str;
  raw_svector_ostream O(Str);

  // Set up
  MRI = &MF->getRegInfo();
  F   = MF->getFunction();

  //
  // CMPLRS-49165: set compilation directory DWARF emission.
  //
  // With -fdwarf-directory-asm (default in ICX) and unset compilation
  // directory EmitDwarfFileDirective will use new syntax for assembly
  // .file directory:
  //     .file 1 "directory" "file"
  //
  // Neither standard 'as' nor CSA simulator can handle this.
  //
  // If we set the compilation directory, and the file being compiled
  // is located in the compilation folder, then the old syntax will be used.
  // At the same time, even if we set the compilation directory,
  // the new syntax will be used in cases, when the file is not
  // in the compilation directory.  So the general fix is to use
  // -fno-dwarf-directory-asm - see CMPLRS-49173.
  //
  // I think setting the compilation directory is the right thing to do
  // anyway.
  //
  auto *SubProgram = MF->getFunction()->getSubprogram();
  if (SubProgram &&
      SubProgram->getUnit()->getEmissionKind() != DICompileUnit::NoDebug) {
    MCDwarfLineTable &Table = OutStreamer->getContext().getMCDwarfLineTable(0);
    Table.setCompilationDir(SubProgram->getUnit()->getDirectory());
  }

  // If we're wrapping the CSA assembly we need to create our own
  // global symbol declaration
  if (CSAInstPrinter::WrapCsaAsm()) {
    // The global symbol needs a value. As long as we're using the simulator,
    // we find entries by name, so point to the name. But be sure it doesn't
    // interrupt the string we're building in the .csa.code section
    O << "\n\t.section\t.rodata.str1.16,\"aMS\",@progbits,1\n";
    O << *CurrentFnSym << ":\n";
    O << "\t.asciz\t"
      << "\"" << *CurrentFnSym << "\"\n\n";
    O << "\t.section\t\".csa.code\",\"aS\",@progbits\n";

    O << CSAInstPrinter::WrapCsaAsmLinePrefix();
    O << "\t.globl\t" << *CurrentFnSym;
    O << CSAInstPrinter::WrapCsaAsmLineSuffix();
    O << "\n";
  }
  O << CSAInstPrinter::WrapCsaAsmLinePrefix();
  O << "\t.entry\t" << *CurrentFnSym;
  O << CSAInstPrinter::WrapCsaAsmLineSuffix();
  O << "\n";
  // For now, assume control flow (sequential) entry
  O << CSAInstPrinter::WrapCsaAsmLinePrefix();
  O << *CurrentFnSym << ":";
  O << CSAInstPrinter::WrapCsaAsmLineSuffix();
  OutStreamer->EmitRawText(O.str());

  // Start a scope for this routine to localize the LIC names
  // For now, this includes parameters and results
  writeAsmLine("{");

  emitReturnVal(F);

  emitParamList(F);
}

void CSAAsmPrinter::EmitFunctionBodyStart() {
  //  const MachineRegisterInfo *MRI;
  MRI                                = &MF->getRegInfo();
  const CSAMachineFunctionInfo *LMFI = MF->getInfo<CSAMachineFunctionInfo>();

  if (not ImplicitLicDefs) {
    auto printRegister = [&](unsigned reg, StringRef name) {
      SmallString<128> Str;
      raw_svector_ostream O(Str);
      O << CSAInstPrinter::WrapCsaAsmLinePrefix();
      O << "\t.lic";
      if (unsigned depth = LMFI->getLICDepth(reg)) {
        O << "@" << depth;
      }
      O << " .i" << LMFI->getLICSize(reg) << " ";
      O << "%" << name;
      O << CSAInstPrinter::WrapCsaAsmLineSuffix();
      OutStreamer->EmitRawText(O.str());
    };

    // Generate declarations for each LIC by looping over the LIC classes,
    // and over each lic in the class, outputting a decl if needed.
    // Note: If we start allowing parameters and results in LICs for
    // HybridDataFlow, this may need to be revisited to make sure they
    // are in order.
    for (TargetRegisterClass::iterator ri = CSA::ANYCRegClass.begin();
         ri != CSA::ANYCRegClass.end(); ++ri) {
      MCPhysReg reg = *ri;
      // A decl is needed if we allocated this LIC and it is has a
      // using/defining instruction. (Sometimes all such instructions are
      // cleaned up by DIE.)
      if (reg != CSA::IGN && reg != CSA::NA && !MRI->reg_empty(reg)) {
        StringRef name = CSAInstPrinter::getRegisterName(reg);
        printRegister(reg, name);
      }
    }
    for (unsigned index = 0, e = MRI->getNumVirtRegs(); index != e; ++index) {
      unsigned vreg = TargetRegisterInfo::index2VirtReg(index);
      if (!MRI->reg_empty(vreg)) {
        StringRef name = LMFI->getLICName(vreg);
        if (!EmitRegNames || name.empty()) {
          LMFI->setLICName(vreg, Twine("cv") + Twine(LMFI->getLICSize(vreg)) +
                                   "_" + Twine(index));
        }
        printRegister(vreg, LMFI->getLICName(vreg));
      }
    }
  }
}

void CSAAsmPrinter::EmitFunctionBodyEnd() { writeAsmLine("}"); }

void CSAAsmPrinter::EmitInstruction(const MachineInstr *MI) {
  CSAMCInstLower MCInstLowering(OutContext, *this);
  emitLineNumberAsDotLoc(*MI);
  MCInst TmpInst;
  MCInstLowering.Lower(MI, TmpInst);
  EmitToStreamer(*OutStreamer, TmpInst);
}

// Force static initialization.
extern "C" void LLVMInitializeCSAAsmPrinter() {
  RegisterAsmPrinter<CSAAsmPrinter> X(getTheCSATarget());
}
