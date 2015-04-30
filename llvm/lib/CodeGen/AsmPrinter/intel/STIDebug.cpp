//===-- STIDebug.cpp - Symbol And Type Info -------*- C++ -*--===//
//
//===----------------------------------------------------------------------===//
//
// This file contains support for writing symbol and type information
// compatible with Visual Studio.
//
//===----------------------------------------------------------------------===//

#include "STIDebug.h"
#include "STI.h"
#include "STIIR.h"
#include "pdbInterface.h"
#include "../DbgValueHistoryCalculator.h"
#include "llvm/ADT/PointerUnion.h" // dyn_cast
#include "llvm/ADT/StringRef.h"
#include "llvm/ADT/Triple.h"
#include "llvm/CodeGen/AsmPrinter.h"
#include "llvm/CodeGen/LexicalScopes.h"
#include "llvm/CodeGen/MachineFunction.h"
#include "llvm/CodeGen/MachineModuleInfo.h"
#include "llvm/IR/Constants.h"
#include "llvm/IR/DebugInfo.h"
#include "llvm/MC/MCSection.h"
#include "llvm/MC/MCStreamer.h"
#include "llvm/MC/MCSymbol.h"
#include "llvm/Target/TargetLoweringObjectFile.h"
#include "llvm/Target/TargetFrameLowering.h"
#include "llvm/Target/TargetMachine.h"
#include "llvm/Target/TargetSubtargetInfo.h"

#include <map>
#include <vector>

using namespace llvm;

//===----------------------------------------------------------------------===//
// Helper Routines
//===----------------------------------------------------------------------===//

static int16_t getPaddedSize(const int16_t num) {
  static const int16_t padding = 4;
  static const int16_t paddingInc = padding - 1;
  static const int16_t paddingMask = ~paddingInc;
  return (num + paddingInc) & paddingMask;
}

static void getFullFileName(const DIScope scope, std::string &path) {
  path = (scope.getDirectory() + Twine("\\") + scope.getFilename()).str();
  std::replace(path.begin(), path.end(), '/', '\\');
  size_t index = 0;
  while ((index = path.find("\\\\", index)) != std::string::npos) {
    path.erase(index, 1);
  }
}

static std::string getRealName(std::string name) {
  std::string prefix = ".?AV"; //".?AU"
  std::string sufix = "@";
  std::string realName = sufix;

  while (std::size_t pos = name.find("::")) {
    if (pos == std::string::npos) {
      realName = (Twine(prefix) + Twine(name.substr(0, pos)) + Twine(sufix) +
                  Twine(realName)).str();
      break;
    }
    realName =
        (Twine(name.substr(0, pos)) + Twine(sufix) + Twine(realName)).str();
    name = name.substr(pos + 2);
  }
  return realName;
}

static bool isStaticMethod(StringRef linkageName) {
  // FIXME: this is a temporary WA to partial demangle gcc linkageName
  size_t pos = linkageName.find("@@");
  if (pos != StringRef::npos) {
    switch (linkageName[pos + 2]) {
    case 'T':
    case 'S':
    case 'K':
    case 'L':
    case 'C':
    case 'D':
      return true;
    }
  }
  return false;
}

static unsigned getFunctionAttribute(const DISubprogram SP,
                                     const DICompositeType llvmParentType,
                                     bool introduced) {
  unsigned attribute = 0;
  unsigned virtuality = SP.getVirtuality();

  if (SP.isProtected())
    attribute = attribute | STI_ACCESS_PRIVATE;
  else if (SP.isPrivate())
    attribute = attribute | STI_ACCESS_PROTECT;
  else if (SP.isPublic())
    attribute = attribute | STI_ACCESS_PUBLIC;
  // Otherwise C++ member and base classes are considered public.
  else if (llvmParentType.getTag() == dwarf::DW_TAG_class_type)
    attribute = attribute | STI_ACCESS_PRIVATE;
  else
    attribute = attribute | STI_ACCESS_PUBLIC;

  if (SP.isArtificial()) {
    attribute = attribute | STI_COMPGENX;
  }

  switch (virtuality) {
  case dwarf::DW_VIRTUALITY_none:
    break;
  case dwarf::DW_VIRTUALITY_virtual:
    if (introduced) {
      attribute = attribute | STI_MPROP_INTR_VRT;
    } else {
      attribute = attribute | STI_MPROP_VIRTUAL;
    }
    break;
  case dwarf::DW_VIRTUALITY_pure_virtual:
    if (introduced) {
      attribute = attribute | STI_MPROP_PURE_INTR_VRT;
    } else {
      attribute = attribute | STI_MPROP_PURE_VRT;
    }
    break;
  default:
    assert(!"unhandled virtuality case");
    break;
  }

  if (isStaticMethod(SP.getLinkageName())) {
    attribute = attribute | STI_MPROP_STATIC;
  }

  return attribute;
}

static unsigned getTypeAttribute(const DIDerivedType llvmType,
                                 const DICompositeType llvmParentType) {
  unsigned attribute = 0;

  if (llvmType.isProtected())
    attribute = attribute | STI_ACCESS_PRIVATE;
  else if (llvmType.isPrivate())
    attribute = attribute | STI_ACCESS_PROTECT;
  else if (llvmType.isPublic())
    attribute = attribute | STI_ACCESS_PUBLIC;
  // Otherwise C++ member and base classes are considered public.
  else if (llvmParentType.getTag() == dwarf::DW_TAG_class_type)
    attribute = attribute | STI_ACCESS_PRIVATE;
  else
    attribute = attribute | STI_ACCESS_PUBLIC;

  if (llvmType.isArtificial()) {
    attribute = attribute | STI_COMPGENX;
  }

  if (llvmType.isStaticMember()) {
    attribute = attribute | STI_MPROP_STATIC;
  }

  return attribute;
}

static bool isIndirectExpression(DIExpression Expr) {
  if (!Expr || (Expr.getNumElements() == 0)) {
    return false;
  }

  if (Expr.getNumElements() != 1) {
    // Looking for DW_OP_deref expression only.
    return false;
  }

  DIExpression::iterator I = Expr.begin();
  DIExpression::iterator E = Expr.end();

  for (; I != E; ++I) {
    switch (*I) {
    case dwarf::DW_OP_bit_piece:
    case dwarf::DW_OP_plus:
      return false;
    case dwarf::DW_OP_deref:
      return true;
    default:
      llvm_unreachable("unhandled opcode found in DIExpression");
    }
  }
  return false;
}

//===----------------------------------------------------------------------===//
// Printing/Debugging Routines
//===----------------------------------------------------------------------===//

static StringRef toString(STISubsectionID subsectionID) {
  StringRef string;

  switch (subsectionID) {
#define X(KIND, VALUE)                                                         \
  case (KIND):                                                                 \
    string = #KIND;                                                            \
    break;
    STI_SUBSECTION_KINDS
#undef X
  default:
    string = "<invalid subsection kind>";
    break;
  }

  return string;
}

static StringRef toString(STIMachineID machineID) {
  StringRef string;

  switch (machineID) {
#define X(KIND, VALUE)                                                         \
  case (KIND):                                                                 \
    string = #KIND;                                                            \
    break;
    STI_MACHINE_KINDS
#undef X
  default:
    string = "<invalid machine kind>";
    break;
  }

  return string;
}

static StringRef toString(STISymbolID symbolID) {
  StringRef string;

  switch (symbolID) {
#define X(KIND, VALUE)                                                         \
  case (KIND):                                                                 \
    string = #KIND;                                                            \
    break;
    X(S_OBJNAME,        0x0000) // FIXME: define these in STI.h with values
    X(S_COMPILE3,       0x0000)
    X(S_GPROC32_ID,     0x0000)
    X(S_LPROC32_ID,     0x0000)
    X(S_FRAMEPROC,      0x0000)
    X(S_BLOCK32,        0x0000)
    X(S_REGREL32,       0x0000)
    X(S_REGISTER,       0x0000)
    X(S_BPREL32,        0x0000)
    X(S_LDATA32,        0x0000)
    X(S_GDATA32,        0x0000)
    X(S_PROC_ID_END,    0x0000)
    X(S_CONSTANT,       0x0000)
#undef X
  default:
    string = "<invalid symbol kind>";
    break;
  }

  return string;
}

//===----------------------------------------------------------------------===//
// toMachineID(architecture)
//===----------------------------------------------------------------------===//

static STIMachineID toMachineID(Triple::ArchType architecture) {
  STIMachineID machineID;

  switch (architecture) {
#define MAP(ARCH, MACHINE)                                                     \
  case (Triple::ARCH):                                                         \
    machineID = MACHINE;                                                       \
    break
    MAP(x86,    STI_MACHINE_INTEL_PENTIUM_III);
    MAP(x86_64, STI_MACHINE_INTEL64);
#undef MAP
  default:
    assert(!"Architecture cannot be mapped to an STI machine type!");
    break;
  }

  return machineID;
}

//===----------------------------------------------------------------------===//
// STITypeInfo
//===----------------------------------------------------------------------===//

typedef std::vector<STIType *> STITypeTable;

//===----------------------------------------------------------------------===//
// STITypeMap
//===----------------------------------------------------------------------===//
typedef DenseMap<const MDNode *, STIType *> TypeMap;
typedef DenseMap<const STIType *, TypeMap> TypeScopedMap;

typedef DenseMap<const MachineInstr *, MCSymbol *> LabelMap;

struct ClassInfo {
  typedef std::vector<const MDNode *> BaseClassList;
  struct VBaseClassInfo {
    const MDNode *llvmInheritance;
    unsigned vbIndex;
    bool indirect;

    VBaseClassInfo() : llvmInheritance(nullptr), vbIndex(0), indirect(false) {}

    VBaseClassInfo(const MDNode *N, unsigned I, bool InDir)
        : llvmInheritance(N), vbIndex(I), indirect(InDir) {}
  };
  // llvmClassType -> {llvmInheritance, vbIndex, indirect}
  typedef MapVector<const MDNode *, VBaseClassInfo> VBaseClassList;
  // [<llvmMemberType, baseOffset>]
  typedef std::vector<std::pair<const MDNode *, unsigned> > MemberList;
  // methodName -> [<llvmSubprogram, introduced>]
  typedef std::map<StringRef, std::vector<std::pair<const MDNode *, bool> > >
      MethodsMap;
  // methodName -> [llvmSubprogram]
  typedef std::map<StringRef, std::vector<const MDNode *> > VMethodsMap;

  // non-virtual base classes
  BaseClassList baseClasses;
  // virtual base classes (direct and indirect).
  VBaseClassList vBaseClasses;
  // offset of virtual base pointer
  int vbpOffset;
  // virtual function table (only if have introduced virtual methods)
  const MDNode *vFuncTab;
  // direct members
  MemberList members;
  // direct methods (gathered by name), for each function: (introduced?)
  MethodsMap methods;
  // virtual methods (gathered by name), for DTOR use "~" name.
  VMethodsMap vMethods;
  // FIXME: add support to: CONSTRUCTOR, OVERLOAD, OVERLOADED ASSIGNMENT, etc.
  // Class has Constructor
  bool hasCTOR;
  // Class has Destructor
  bool hasDTOR;
  // Number of virtual methods (length of virtual function table)
  unsigned vMethodsCount;

  ClassInfo()
      : vbpOffset(~0), vFuncTab(nullptr), hasCTOR(false), hasDTOR(false),
        vMethodsCount(0) {}
};

//===----------------------------------------------------------------------===//
// STIWriter
//===----------------------------------------------------------------------===//

class STIWriter {
public:
  virtual void emitInt8(int32_t value) = 0;
  virtual void emitInt16(int32_t value) = 0;
  virtual void emitInt32(int32_t value) = 0;
  virtual void emitString(StringRef string) = 0;
  virtual void emitBytes(size_t size, const char* data) = 0;
  virtual void emitFill(size_t size, const uint8_t byte) = 0;
  virtual void emitComment(StringRef comment) = 0;
  virtual void emitLabel(MCSymbol *symbol) = 0;
  virtual void emitValue(const MCExpr *value, unsigned int sizeInBytes) = 0;

  virtual void typeBegin(const STIType* type) = 0;
  virtual void typeEnd(const STIType* type) = 0;

  virtual ~STIWriter();

protected:
  STIWriter();
};

STIWriter::STIWriter() {
}

STIWriter::~STIWriter() {
}

//===----------------------------------------------------------------------===//
// STIAsmWriter
//===----------------------------------------------------------------------===//

class STIAsmWriter : public STIWriter {
public:
  STIAsmWriter(AsmPrinter* asmPrinter);
  virtual ~STIAsmWriter();

  static STIAsmWriter* create(AsmPrinter* asmPrinter);

  AsmPrinter* ASM() const;

  virtual void emitInt8(int32_t value);
  virtual void emitInt16(int32_t value);
  virtual void emitInt32(int32_t value);
  virtual void emitString(StringRef string);
  virtual void emitBytes(size_t size, const char* data);
  virtual void emitFill(size_t size, const uint8_t byte);
  virtual void emitComment(StringRef comment);
  virtual void emitLabel(MCSymbol *symbol);
  virtual void emitValue(const MCExpr *value, unsigned int sizeInBytes);

  virtual void typeBegin(const STIType* type);
  virtual void typeEnd(const STIType* type);

private:
  AsmPrinter* _asmPrinter;
};

STIAsmWriter::STIAsmWriter(AsmPrinter* asmPrinter) :
    STIWriter   (),
    _asmPrinter (asmPrinter) {
}

STIAsmWriter::~STIAsmWriter() {
}

STIAsmWriter* STIAsmWriter::create(AsmPrinter* asmPrinter) {
  return new STIAsmWriter(asmPrinter);
}

AsmPrinter* STIAsmWriter::ASM() const {
  return _asmPrinter;
}

void STIAsmWriter::emitInt8(int32_t value) {
  ASM()->EmitInt8(value);
}

void STIAsmWriter::emitInt16(int32_t value) {
  ASM()->EmitInt16(value);
}

void STIAsmWriter::emitInt32(int32_t value) {
  ASM()->EmitInt32(value);
}

void STIAsmWriter::emitString(StringRef string) {
  ASM()->OutStreamer.EmitBytes(string);
  ASM()->EmitInt8(0);
}

void STIAsmWriter::emitBytes(size_t size, const char* data) {
  ASM()->OutStreamer.EmitBytes(StringRef(data, size));
}

void STIAsmWriter::emitFill(size_t size, const uint8_t byte) {
  ASM()->OutStreamer.EmitFill(size, byte);
}

void STIAsmWriter::emitComment(StringRef comment) {
  ASM()->OutStreamer.AddComment(comment);
}

void STIAsmWriter::emitLabel(MCSymbol *symbol) {
  ASM()->OutStreamer.EmitLabel(symbol);
}

void STIAsmWriter::emitValue(const MCExpr *value, unsigned int sizeInBytes) {
  ASM()->OutStreamer.EmitValue(value, sizeInBytes);
}

void STIAsmWriter::typeBegin(const STIType* type) {
}

void STIAsmWriter::typeEnd(const STIType* type) {
}

//===----------------------------------------------------------------------===//
// STIPdbWriter
//===----------------------------------------------------------------------===//

class STIPdbWriter : public STIWriter {
public:
  STIPdbWriter();
  virtual ~STIPdbWriter();

  static STIPdbWriter* create();

  virtual void emitInt8(int32_t value);
  virtual void emitInt16(int32_t value);
  virtual void emitInt32(int32_t value);
  virtual void emitString(StringRef string);
  virtual void emitBytes(size_t size, const char* data);
  virtual void emitFill(size_t size, const uint8_t byte);
  virtual void emitComment(StringRef comment);
  virtual void emitLabel(MCSymbol *symbol);
  virtual void emitValue(const MCExpr *value, unsigned int sizeInBytes);

  virtual void typeBegin(const STIType* type);
  virtual void typeEnd(const STIType* type);

private:
  std::vector<char> _buffer;
};

STIPdbWriter::STIPdbWriter() :
    STIWriter   (),
    _buffer     () {
}

STIPdbWriter::~STIPdbWriter() {
}

STIPdbWriter* STIPdbWriter::create() {
  return new STIPdbWriter();
}

void STIPdbWriter::emitInt8(int32_t value) {
  emitBytes(1, reinterpret_cast<const char*>(&value));
}

void STIPdbWriter::emitInt16(int32_t value) {
  emitBytes(2, reinterpret_cast<const char*>(&value));
}

void STIPdbWriter::emitInt32(int32_t value) {
  emitBytes(4, reinterpret_cast<const char*>(&value));
}

void STIPdbWriter::emitString(StringRef string) {
  _buffer.insert(_buffer.end(), string.begin(), string.end());
  _buffer.push_back('\0');
}

void STIPdbWriter::emitBytes(size_t size, const char* data) {
  _buffer.insert(_buffer.end(), data, data + size);
}

void STIPdbWriter::emitFill(size_t size, const uint8_t byte) {
  // Fill bytes are not emitted to the PDB writer.
}

void STIPdbWriter::emitComment(StringRef comment) {
  // Comments are not emitted to the PDB writer.
}

void STIPdbWriter::emitLabel(MCSymbol *symbol) {
  // Labels are not emitted to the PDB writer.
}

void STIPdbWriter::emitValue(const MCExpr *value, unsigned int sizeInBytes) {
  // This is currently only used for emitting label diffs, which are not used
  // when writing type information to the PDB writer.
}

void STIPdbWriter::typeBegin(const STIType* type) {
  assert(_buffer.size() == 0);
}

void STIPdbWriter::typeEnd(const STIType* type) {
  unsigned long index;

  // Buffer must minimally contain a type length.
  assert(_buffer.size() > 2);

  pdb_write_type(_buffer.data(), &index);

  const_cast<STIType *>(type)->setIndex(index);

  _buffer.clear();
}

//===----------------------------------------------------------------------===//
// STIDebugImpl
//===----------------------------------------------------------------------===//

class STIDebugImpl : public STIDebug {
private:
  typedef DenseMap<const Function *, STISymbolProcedure *> FunctionMap;
  typedef DenseMap<const MDNode *, STIScope *> STIScopeMap;
  typedef DenseMap<const MDNode *, ClassInfo *> ClassInfoMap;
  typedef DenseMap<const MDNode *, std::string> StringNameMap;

  AsmPrinter *_asmPrinter;
  STISymbolProcedure *_currentProcedure;
  DbgValueHistoryMap _valueHistory;
  FunctionMap _functionMap;
  STISymbolTable _symbolTable;
  STITypeTable _typeTable;
  STIStringTable _stringTable;
  STIChecksumTable _checksumTable;
  STIScopeMap _ScopeMap;
  TypeScopedMap _typeMap;
  TypeMap _dclTypeMap;
  STIType *_voidType;
  STIType *_vbpType;
  unsigned int _blockNumber;
  LexicalScopes _lexicalScopes;
  LabelMap _labelsBeforeInsn;
  LabelMap _labelsAfterInsn;
  const MachineInstr *_curMI;
  STISubsection *_currentSubsection;
  unsigned _ptrSizeInBits;
  ClassInfoMap _classInfoMap;
  StringNameMap _stringNameMap;
  unsigned _uniqueNameCounter;
  std::vector<char> _pdbBuff;
  bool _usePDB;
  STIWriter* _writer;

  // Maps from a type identifier to the actual MDNode.
  DITypeIdentifierMap TypeIdentifierMap;

public:
  STIDebugImpl(AsmPrinter *asmPrinter);
  virtual ~STIDebugImpl();

  void setSymbolSize(const MCSymbol *Symbol, uint64_t size);
  void beginModule();
  void endModule();
  void beginFunction(const MachineFunction *MF);
  void endFunction(const MachineFunction *MF);
  void beginInstruction(const MachineInstr *MI);
  void endInstruction();

protected:
  AsmPrinter *ASM();
  const AsmPrinter *ASM() const;
  MachineModuleInfo *MMI() const;
  const Module *getModule();
  const TargetRegisterInfo *getTargetRegisterInfo();
  STISymbolTable *getSymbolTable();
  const STISymbolTable *getSymbolTable() const;
  STITypeTable *getTypeTable();
  const STITypeTable *getTypeTable() const;
  STIStringTable *getStringTable();
  const STIStringTable *getStringTable() const;
  STIChecksumTable *getChecksumTable();
  const STIChecksumTable *getChecksumTable() const;
  bool hasScope(const MDNode *llvmNode) const;
  STIScope *getScope(const MDNode *llvmNode);
  void addScope(const MDNode *llvmNode, STIScope *object);
  TypeScopedMap *getTypeMap();
  const TypeScopedMap *getTypeMap() const;
  TypeMap *getDclTypeMap();
  const TypeMap *getDclTypeMap() const;
  ClassInfoMap *getClassInfoMap();
  const ClassInfoMap *getClassInfoMap() const;
  StringNameMap *getStringNameMap();
  const StringNameMap *getStringNameMap() const;
  STIWriter* writer() const;
  void setWriter(STIWriter* writer);

  std::string getUniqueName();

  STISymbolCompileUnit *getCompileUnit() { // FIXME:
    STISymbolModule *module =
        static_cast<STISymbolModule *>(getSymbolTable()->getRoot());
    STISymbolCompileUnit *compileUnit = module->getCompileUnits()->back();
    return compileUnit;
  }

  /// \brief Return the TypeIdentifierMap.
  const DITypeIdentifierMap &getTypeIdentifierMap() const;

  STIScope *getOrCreateScope(const DIScope llvmScope);
  std::string getScopeFullName(const DIScope llvmScope, StringRef name,
                               bool useClassName = false);
  STIType *getClassScope(const DIScope llvmScope);

  STIRegID toSTIRegID(unsigned int regnum) const;
  STISymbolVariable *createSymbolVariable(const DIVariable DIV,
                                          unsigned int frameIndex,
                                          const MachineInstr *DVInsn = nullptr);

  STISymbolProcedure *getCurrentProcedure() const;
  void setCurrentProcedure(STISymbolProcedure *procedure);

  void setSymbolModule(STISymbolModule *module);
  STISymbolModule *getSymbolModule() const;

  size_t getPaddingSize(const STIChecksumEntry *entry) const;

  void clearValueHistory();

  void collectModuleInfo();
  void collectGlobalVariableInfo(DICompileUnit CU);
  void collectRoutineInfo();

  ClassInfo &collectClassInfo(const DICompositeType llvmType);
  void collectClassInfoFromInheritance(ClassInfo &info,
                                       const DIDerivedType inherTy,
                                       bool &finalizedOffset);
  void collectMemberInfo(ClassInfo &info, const DIDerivedType DDTy);
  bool isEqualVMethodPrototype(DISubroutineType typeA, DISubroutineType typeB);

  DIType getUnqualifiedDIType(DIType ditype);

  STINumeric* createNumericUnsignedInt(const uint64_t value);
  STINumeric* createNumericSignedInt(const int64_t value);
  STINumeric* createNumericAPInt(const DIType ditype, const APInt& value);
  STINumeric* createNumericAPFloat(const DIType ditype, const APFloat& value);

  STISymbolProcedure *getOrCreateSymbolProcedure(const DISubprogram &SP);
  STISymbolBlock *createSymbolBlock(const DILexicalBlock &LB);
  STIChecksumEntry *getOrCreateChecksum(StringRef path);

  STIType *createType(const DIType llvmType, STIType *classType = nullptr,
                      bool isStatic = false);
  STIType *createTypeBasic(const DIBasicType llvmType);
  STIType *createTypePointer(const DIDerivedType llvmType);
  STIType *createTypeModifier(const DIDerivedType llvmType);
  STIType *createTypeArray(const DICompositeType llvmType);
  STIType *createTypeStructure(const DICompositeType llvmType,
                               bool isDcl = false);
  STIType *createTypeEnumeration(const DICompositeType llvmType);
  STIType *createTypeSubroutine(const DISubroutineType llvmType,
                                STIType *classType = nullptr,
                                bool isStatic = false);
  uint64_t getBaseTypeSize(DIDerivedType Ty) const;

  STIType *getVoidType() const;
  STIType *getVbpType() const;
  unsigned getPointerSizeInBits() const;

  STIType *createSymbolUserDefined(const DIDerivedType llvmType);

  void layout();
  void emit();

  // Used with _typeIdentifierMap for type resolution, not clear why?
  template <typename T> T resolve(DIRef<T> ref) const;

  size_t numericLength(const STINumeric* numeric) const;

  // Routines for emitting atomic data.
  void emitAlign(unsigned int byteAlignment) const;
  void emitPadding(unsigned int padByteCount) const;
  void emitInt8(int value) const;
  void emitInt16(int value) const;
  void emitInt32(int value) const;
  void emitString(StringRef string) const;
  void emitComment(StringRef comment) const;
  void emitValue(const MCExpr *expr, unsigned int byteSize) const;
  void emitLabel(MCSymbol *symbol) const;
  void emitLabelDiff(const MCSymbol *begin, const MCSymbol *end) const;
  void emitSymbolID(const STISymbolID symbolID) const;
  void emitBytes(const char *data, size_t size) const;
  void emitFill(size_t size, const uint8_t byte) const;
  void emitSecRel32(MCSymbol *symbol) const;
  void emitSectionIndex(MCSymbol *symbol) const;
  void emitNumeric(const uint32_t num) const;
  void emitNumeric(const STINumeric* numeric) const;

  // Routines for emitting atomic PDB data.
  void typeBegin(const STIType* type) const;
  void typeEnd(const STIType* type) const;
  std::string getPDBFullPath() const;
  bool usePDB() const;

  // Routines for emitting sections.
  void emitSectionBegin(const MCSection *section) const;

  // Routines for emitting subsections.
  void emitSubsectionBegin(STISubsection *subsection) const;
  void emitSubsectionEnd(STISubsection *subsection) const;
  void emitSubsection(STISubsectionID id) const;
  void closeSubsection() const;

  // Routines for emitting the .debug$S section.
  void emitSymbols() const;
  void emitStringTable() const;
  void emitStringEntry(const STIStringEntry *entry) const;
  void emitChecksumTable() const;
  void emitChecksumEntry(const STIChecksumEntry *entry) const;
  void emitLineEntry(const STISymbolProcedure *procedure,
                     const STILineEntry *entry) const;
  void emitLineBlock(const STISymbolProcedure *procedure,
                     const STILineBlock *block) const;
  void emitLineSlice(const STISymbolProcedure *procedure) const;
  void walkSymbol(const STISymbol *symbol) const;
  void emitSymbolModule(const STISymbolModule *module) const;
  void emitSymbolCompileUnit(const STISymbolCompileUnit *compileUnit) const;
  void emitSymbolConstant(const STISymbolConstant *symbol) const;
  void emitSymbolProcedure(const STISymbolProcedure *procedure) const;
  void emitSymbolProcedureEnd() const;
  void emitSymbolFrameProc(const STISymbolFrameProc *frame) const;
  void emitSymbolBlock(const STISymbolBlock *block) const;
  void emitSymbolScopeEnd() const;
  void emitSymbolVariable(const STISymbolVariable *variable) const;
  void emitSymbolUserDefined(const STISymbolUserDefined *type) const;

  // Routines for emiting the .debug$T section.
  void emitTypes() const;
  void emitTypesSignature() const;
  void emitTypesPDBTypeServer() const;
  void emitTypesPDBBegin(STIWriter** savedWriter) const;
  void emitTypesPDBEnd(STIWriter** savedWriter) const;
  void emitTypesTable() const;
  void emitType(const STIType *type) const;
  void emitTypeBasic(const STITypeBasic *type) const;
  void emitTypeModifier(const STITypeModifier *type) const;
  void emitTypePointer(const STITypePointer *type) const;
  void emitTypeArray(const STITypeArray *type) const;
  void emitTypeStructure(const STITypeStructure *type) const;
  void emitTypeEnumeration(const STITypeEnumeration *type) const;
  void emitTypeVShape(const STITypeVShape *type) const;
  void emitTypeBitfield(const STITypeBitfield *type) const;
  void emitTypeMethodList(const STITypeMethodList *type) const;
  void emitTypeFieldList(const STITypeFieldList *type) const;
  void emitTypeFunctionID(const STITypeFunctionID *type) const;
  void emitTypeProcedure(const STITypeProcedure *type) const;
  void emitTypeArgumentList(const STITypeArgumentList *type) const;
  void emitTypeServer(const STITypeServer *type) const;

  // Routines for creating labels.
  MCSymbol *createFuncLabel(const char *name) const;
  MCSymbol *createBlockLabel(const char *name);
};

//===----------------------------------------------------------------------===//
// STIDebugImpl Public Routines
//===----------------------------------------------------------------------===//

STIDebugImpl::STIDebugImpl(AsmPrinter *asmPrinter) :
    STIDebug            (),
    _asmPrinter         (asmPrinter),
    _currentProcedure   (nullptr),
    _valueHistory       (),
    _functionMap        (),
    _symbolTable        (),
    _typeTable          (),
    _stringTable        (),
    _checksumTable      (),
    _ScopeMap           (),
    _typeMap            (),
    _voidType           (nullptr),
    _vbpType            (nullptr),
    _blockNumber        (0),
    _lexicalScopes      (),
    _labelsBeforeInsn   (),
    _labelsAfterInsn    (),
    _curMI              (nullptr),
    _currentSubsection  (nullptr),
    _ptrSizeInBits      (0),
    _classInfoMap       (),
    _stringNameMap      (),
    _uniqueNameCounter  (0),
    _pdbBuff            (),
    _usePDB             (false),
    _writer             (STIAsmWriter::create(asmPrinter)) {
  // If module doesn't have named metadata anchors or COFF debug section
  // is not available, skip any debug info related stuff.
  if (!MMI()->getModule()->getNamedMetadata("llvm.dbg.cu") ||
      !ASM()->getObjFileLowering().getCOFFDebugSymbolsSection())
    return;

  _ptrSizeInBits = getModule()->getDataLayout().getPointerSizeInBits();

  beginModule();
}

STIDebugImpl::~STIDebugImpl() {
  for (STIType *type : *getTypeTable()) {
    delete type;
  }
  for (auto entry : *getClassInfoMap()) {
    delete entry.second;
  }
  delete _writer;
}

void STIDebugImpl::setSymbolSize(const MCSymbol *Symbol, uint64_t size) {}

void STIDebugImpl::beginModule() {
  _usePDB = false; //FIXME: initialize _usePDB
  if (usePDB()) {
    pdb_set_default_dll_name("mspdb110.dll");
    if (!pdb_open("vc110.pdb") ) {
      _usePDB = false;
    }
  }

  // Collect all of the initial module information.
  collectModuleInfo();

  // Tell MMI to make the debug information available.
  MMI()->setDebugInfoAvailability(true);
}

void STIDebugImpl::endModule() {
  if (!MMI()->hasDebugInfo())
    return;

  layout();
  emit();

  if (usePDB()) {
    pdb_close();
  }
}

void STIDebugImpl::beginFunction(const MachineFunction *MF) {
  if (!MMI()->hasDebugInfo())
    return;

  STISymbolProcedure *procedure;

  _lexicalScopes.initialize(*MF);

  // if (_lexicalScopes.empty())
  //  return;

  // Locate the symbol for this function.
  procedure = _functionMap.find(MF->getFunction())
                  ->second; // FIXME: validate function exist in the map
  procedure->setLabelBegin(createFuncLabel("fbeg"));
  procedure->setLabelEnd(createFuncLabel("fend"));

  // Emit the label marking the beginning of the procedure.
  emitLabel(procedure->getLabelBegin());

  // Record this as the current procedure.
  setCurrentProcedure(procedure);

  calculateDbgValueHistory(MF, getTargetRegisterInfo(), _valueHistory);
}

void STIDebugImpl::endFunction(const MachineFunction *MF) {
  if (!MMI()->hasDebugInfo())
    return;

  STISymbolProcedure *procedure = getCurrentProcedure();

  // Emit the label marking the end of the procedure.
  emitLabel(procedure->getLabelEnd());

  // Collect information about this routine.
  collectRoutineInfo();

  clearValueHistory();
}

void STIDebugImpl::clearValueHistory() { _valueHistory.clear(); }

void STIDebugImpl::beginInstruction(const MachineInstr *MI) {
  DebugLoc location = MI->getDebugLoc();
  STISymbolProcedure *procedure;
  MDNode *node;
  MCSymbol *label;
  std::string path;
  uint32_t line;
  STIChecksumEntry *checksum;
  STILineSlice *slice;
  STILineBlock *block;
  STILineEntry *entry;

  assert(_curMI == nullptr);

  if (MI->isDebugValue()) {
    return;
  }

  _curMI = MI;

  if (location == DebugLoc()) {
    label = MMI()->getContext().CreateTempSymbol();
    emitLabel(label);

    // Handle Scope
    _labelsBeforeInsn.insert(std::make_pair(_curMI, label));
    return;
  }

  procedure = getCurrentProcedure();
  slice = procedure->getLineSlice();
  line = location.getLine();

  node = location.getScope(ASM()->MF->getFunction()->getContext());
  DIScope scope(node);
  getFullFileName(scope, path);

  label = MMI()->getContext().CreateTempSymbol();
  emitLabel(label);

  if (slice->getBlocks().size() == 0 ||
      slice->getBlocks().back()->getFilename() != path) {
    checksum = getOrCreateChecksum(path);

    block = STILineBlock::create();
    block->setChecksumEntry(checksum);

    // We don't get source correlation information for the prologue and
    // epilogue.  Visual Studio requires source correlation for the very
    // first instruction in the routine or it thinks there is no debug
    // information available and steps over the routine.  The following
    // code is a hack to correlate the prologue with the first line
    // number which occurs in the routine.  This should be fixed in LLVM
    // to propagate the source correlation for the opening curly brace.
    //
    if (slice->getBlocks().size() == 0) {
      entry = STILineEntry::create();
      entry->setLabel(procedure->getLabelBegin());
      entry->setLineNumStart(procedure->getScopeLineNumber());
      block->appendLine(entry);
    }

    slice->appendBlock(block);
  }

  block = slice->getBlocks().back();

  entry = block->getLines().empty() ? nullptr : block->getLines().back();
  if (line != 0 && (entry == nullptr || entry->getLineNumStart() != line)) {
    entry = STILineEntry::create();
    entry->setLabel(label);
    entry->setLineNumStart(line);

    block->appendLine(entry);
  }

  if (!MI->getFlag(MachineInstr::FrameSetup) &&
      procedure->getLabelPrologEnd() == nullptr) {
    procedure->setLabelPrologEnd(label);
  }

  // Handle Scope
  _labelsBeforeInsn.insert(std::make_pair(_curMI, label));
}

void STIDebugImpl::endInstruction() {
  MCSymbol *label;

  if (_curMI == nullptr) {
    return;
  }

  label = MMI()->getContext().CreateTempSymbol();
  emitLabel(label);

  // Handle Scope
  _labelsAfterInsn.insert(std::make_pair(_curMI, label));

  _curMI = nullptr;
}

STISymbolProcedure *STIDebugImpl::getCurrentProcedure() const {
  return _currentProcedure;
}

void STIDebugImpl::setCurrentProcedure(STISymbolProcedure *procedure) {
  _currentProcedure = procedure;
}

AsmPrinter *STIDebugImpl::ASM() { return _asmPrinter; }

const AsmPrinter *STIDebugImpl::ASM() const { return _asmPrinter; }

MachineModuleInfo *STIDebugImpl::MMI() const { return ASM()->MMI; }

const Module *STIDebugImpl::getModule() { return MMI()->getModule(); }

const TargetRegisterInfo *STIDebugImpl::getTargetRegisterInfo() {
  return ASM()->TM.getSubtargetImpl()->getRegisterInfo();
}

STISymbolTable *STIDebugImpl::getSymbolTable() { return &_symbolTable; }

const STISymbolTable *STIDebugImpl::getSymbolTable() const {
  return &_symbolTable;
}

STITypeTable *STIDebugImpl::getTypeTable() { return &_typeTable; }

const STITypeTable *STIDebugImpl::getTypeTable() const { return &_typeTable; }

STIStringTable *STIDebugImpl::getStringTable() { return &_stringTable; }

const STIStringTable *STIDebugImpl::getStringTable() const {
  return &_stringTable;
}

STIChecksumTable *STIDebugImpl::getChecksumTable() { return &_checksumTable; }

const STIChecksumTable *STIDebugImpl::getChecksumTable() const {
  return &_checksumTable;
}

bool STIDebugImpl::hasScope(const MDNode *llvmNode) const {
  return _ScopeMap.count(llvmNode);
}

STIScope *STIDebugImpl::getScope(const MDNode *llvmNode) {
  assert(hasScope(llvmNode) && "LLVM node has no STI object mapped yet!");
  return _ScopeMap[llvmNode];
}

void STIDebugImpl::addScope(const MDNode *llvmNode, STIScope *object) {
  assert(!hasScope(llvmNode) && "LLVM node already mapped to STI object!");
  _ScopeMap[llvmNode] = object;
}

TypeScopedMap *STIDebugImpl::getTypeMap() { return &_typeMap; }

const TypeScopedMap *STIDebugImpl::getTypeMap() const { return &_typeMap; }

TypeMap *STIDebugImpl::getDclTypeMap() { return &_dclTypeMap; }

const TypeMap *STIDebugImpl::getDclTypeMap() const { return &_dclTypeMap; }

STIDebugImpl::ClassInfoMap *STIDebugImpl::getClassInfoMap() {
  return &_classInfoMap;
}

const STIDebugImpl::ClassInfoMap *STIDebugImpl::getClassInfoMap() const {
  return &_classInfoMap;
}

STIDebugImpl::StringNameMap *STIDebugImpl::getStringNameMap() {
  return &_stringNameMap;
}

const STIDebugImpl::StringNameMap *STIDebugImpl::getStringNameMap() const {
  return &_stringNameMap;
}

STIWriter* STIDebugImpl::writer() const {
  return _writer;
}

void STIDebugImpl::setWriter(STIWriter* writer) {
  _writer = writer;
}

std::string STIDebugImpl::getUniqueName() {
  return (Twine("<unnamed-tag>") + Twine(_uniqueNameCounter++)).str();
}

const DITypeIdentifierMap &STIDebugImpl::getTypeIdentifierMap() const {
  return TypeIdentifierMap;
}

STIRegID STIDebugImpl::toSTIRegID(unsigned int llvmID) const {
  STIRegID stiID;

  switch (llvmID) {
#define MAP(LLVMID, STIID)                                                     \
  case LLVMID:                                                                 \
    stiID = STIID;                                                             \
    break
    // FIXME: register mapping correct?
    MAP(0x13, STI_REGISTER_EAX);
    MAP(0x14, STI_REGISTER_EBP);
    MAP(0x15, STI_REGISTER_EBX);
    MAP(0x16, STI_REGISTER_ECX);
    MAP(0x17, STI_REGISTER_EDI);
    MAP(0x18, STI_REGISTER_EDX);
    // MAP(0x1a,   STI_REGISTER_EIP);
    // MAP(0x1b,   STI_REGISTER_EIZ);
    MAP(0x1d, STI_REGISTER_ESI);
    MAP(0x1e, STI_REGISTER_ESP);

    MAP(0x23, STI_REGISTER_RAX);
    MAP(0x24, STI_REGISTER_RBP);
    MAP(0x25, STI_REGISTER_RBX);
    MAP(0x26, STI_REGISTER_RCX);
    MAP(0x27, STI_REGISTER_RDI);
    MAP(0x28, STI_REGISTER_RDX);
    // MAP(0x29,   STI_REGISTER_RIP);
    // MAP(0x2a,   STI_REGISTER_RIZ);
    MAP(0x2b, STI_REGISTER_RSI);
    MAP(0x2c, STI_REGISTER_RSP);

    MAP(0x6a, STI_REGISTER_R8);
    MAP(0x6b, STI_REGISTER_R9);
    MAP(0x6c, STI_REGISTER_R10);
    MAP(0x6d, STI_REGISTER_R11);
    MAP(0x6e, STI_REGISTER_R12);
    MAP(0x6f, STI_REGISTER_R13);
    MAP(0x70, STI_REGISTER_R14);
    MAP(0x71, STI_REGISTER_R15);

    MAP(0xda, STI_REGISTER_R8B);
    MAP(0xdb, STI_REGISTER_R9B);
    MAP(0xdc, STI_REGISTER_R10B);
    MAP(0xdd, STI_REGISTER_R11B);
    MAP(0xde, STI_REGISTER_R12B);
    MAP(0xdf, STI_REGISTER_R13B);
    MAP(0xe0, STI_REGISTER_R14B);
    MAP(0xe1, STI_REGISTER_R15B);

    MAP(0xe2, STI_REGISTER_R8W);
    MAP(0xe3, STI_REGISTER_R9W);
    MAP(0xe4, STI_REGISTER_R10W);
    MAP(0xe5, STI_REGISTER_R11W);
    MAP(0xe6, STI_REGISTER_R12W);
    MAP(0xe7, STI_REGISTER_R13W);
    MAP(0xe8, STI_REGISTER_R14W);
    MAP(0xe9, STI_REGISTER_R15W);

    MAP(0xea, STI_REGISTER_R8D);
    MAP(0xeb, STI_REGISTER_R9D);
    MAP(0xec, STI_REGISTER_R10D);
    MAP(0xed, STI_REGISTER_R11D);
    MAP(0xee, STI_REGISTER_R12D);
    MAP(0xef, STI_REGISTER_R13D);
    MAP(0xf0, STI_REGISTER_R14D);
    MAP(0xf1, STI_REGISTER_R15D);

    MAP(0x7a, STI_REGISTER_XMM0);
    MAP(0x7b, STI_REGISTER_XMM1);
    MAP(0x7c, STI_REGISTER_XMM2);
    MAP(0x7d, STI_REGISTER_XMM3);
    MAP(0x7e, STI_REGISTER_XMM4);
    MAP(0x7f, STI_REGISTER_XMM5);
    MAP(0x80, STI_REGISTER_XMM6);
    MAP(0x81, STI_REGISTER_XMM7);
#undef MAP
  default:
    assert(llvmID != llvmID); // unrecognized llvm register number
    break;
  }

  return stiID;
}

#define PRIMITIVE_TYPE_MAPPINGS                                                \
  X(dwarf::DW_ATE_address, 4, T_32PVOID, T_32PVOID)                            \
  X(dwarf::DW_ATE_boolean, 1, T_BOOL08, T_BOOL08)                              \
  X(dwarf::DW_ATE_boolean, 2, T_BOOL16, T_BOOL16)                              \
  X(dwarf::DW_ATE_boolean, 4, T_BOOL32, T_BOOL32)                              \
  X(dwarf::DW_ATE_boolean, 8, T_BOOL64, T_BOOL64)                              \
  X(dwarf::DW_ATE_complex_float, 4, T_CPLX32, T_CPLX32)                        \
  X(dwarf::DW_ATE_complex_float, 8, T_CPLX64, T_CPLX64)                        \
  X(dwarf::DW_ATE_complex_float, 10, T_CPLX80, T_CPLX80)                       \
  X(dwarf::DW_ATE_complex_float, 16, T_CPLX128, T_CPLX128)                     \
  X(dwarf::DW_ATE_float, 4, T_REAL32, T_REAL32)                                \
  X(dwarf::DW_ATE_float, 6, T_REAL48, T_REAL48)                                \
  X(dwarf::DW_ATE_float, 8, T_REAL64, T_REAL64)                                \
  X(dwarf::DW_ATE_float, 10, T_REAL80, T_REAL80)                               \
  X(dwarf::DW_ATE_float, 16, T_REAL128, T_REAL128)                             \
  X(dwarf::DW_ATE_decimal_float, 4, T_REAL32, T_REAL32)                        \
  X(dwarf::DW_ATE_decimal_float, 6, T_REAL48, T_REAL48)                        \
  X(dwarf::DW_ATE_decimal_float, 8, T_REAL64, T_REAL64)                        \
  X(dwarf::DW_ATE_decimal_float, 10, T_REAL80, T_REAL80)                       \
  X(dwarf::DW_ATE_decimal_float, 16, T_REAL128, T_REAL128)                     \
  X(dwarf::DW_ATE_signed, 1, T_CHAR, T_CHAR)                                   \
  X(dwarf::DW_ATE_signed, 2, T_SHORT, T_SHORT)                                 \
  X(dwarf::DW_ATE_signed, 4, T_INT4, T_LONG)                                   \
  X(dwarf::DW_ATE_signed, 8, T_QUAD, T_QUAD)                                   \
  X(dwarf::DW_ATE_signed_char, 1, T_CHAR, T_CHAR)                              \
  X(dwarf::DW_ATE_unsigned, 1, T_UCHAR, T_UCHAR)                               \
  X(dwarf::DW_ATE_unsigned, 2, T_USHORT, T_USHORT)                             \
  X(dwarf::DW_ATE_unsigned, 4, T_UINT4, T_ULONG)                               \
  X(dwarf::DW_ATE_unsigned, 8, T_UQUAD, T_UQUAD)                               \
  X(dwarf::DW_ATE_unsigned_char, 1, T_UCHAR, T_UCHAR)
// FIXME: dwarf::DW_ATE_imaginary_float
// FIXME: dwarf::DW_ATE_packed_decimal
// FIXME: dwarf::DW_ATE_numeric_string
// FIXME: dwarf::DW_ATE_edited
// FIXME: dwarf::DW_ATE_signed_fixed
// FIXME: dwarf::DW_ATE_unsigned_fixed
// FIXME: dwarf::DW_ATE_UTF

static STITypeBasic::Primitive
toPrimitive(dwarf::TypeKind encoding, uint32_t byteSize,
            bool isLong) // FIXME: improve this implementation
{
  STITypeBasic::Primitive primitive;

// FIXME: Algorithm is not efficient.
#define X(ENCODING, BYTESIZE, PRIMITIVE, PRIMITIVE2)                           \
  if (encoding == ENCODING && byteSize == BYTESIZE) {                          \
    primitive = (isLong) ? PRIMITIVE2 : PRIMITIVE;                             \
  } else
  PRIMITIVE_TYPE_MAPPINGS
#undef X
  { primitive = T_NOTYPE; }

  return primitive;
}

STIType *STIDebugImpl::createTypeBasic(const DIBasicType llvmType) {
  unsigned int encoding = llvmType.getEncoding();
  dwarf::TypeKind typeKind = static_cast<dwarf::TypeKind>(encoding);
  uint64_t sizeInBytes = llvmType.getSizeInBits() >> 3;
  STITypeBasic *type;
  bool isLong = false;

  if (llvmType.getName().count("long")) {
    isLong = true;
  }

  type = STITypeBasic::create();
  type->setPrimitive(toPrimitive(typeKind, sizeInBytes, isLong));
  type->setSizeInBits(llvmType.getSizeInBits());

  return type;
}

template <typename T> T STIDebugImpl::resolve(DIRef<T> ref) const {
  return ref.resolve(getTypeIdentifierMap());
}

STIType *STIDebugImpl::createTypePointer(const DIDerivedType llvmType) {
  STITypePointer *type;
  STIType *classType = nullptr;
  STIType *pointerTo = nullptr;
  unsigned int sizeInBits = llvmType.getSizeInBits();
  bool isReference = llvmType.getTag() == dwarf::DW_TAG_reference_type ||
                     llvmType.getTag() == dwarf::DW_TAG_rvalue_reference_type;

  STITypePointer::PTMType ptrToMemberType = STITypePointer::PTM_NONE;

  DIType derivedType = resolve(llvmType.getTypeDerivedFrom());
  pointerTo = createType(derivedType);

  if (llvmType.getTag() == dwarf::DW_TAG_ptr_to_member_type) {
    classType = createType(resolve(llvmType.getClassType()));
    if (resolve(llvmType.getTypeDerivedFrom()).isSubroutineType()) {
      ptrToMemberType = STITypePointer::PTM_METHOD;
    } else {
      ptrToMemberType = STITypePointer::PTM_DATA;
    }
  }

  type = STITypePointer::create();

  type->setPointerTo(pointerTo);

  if (sizeInBits == 0) {
    sizeInBits = getPointerSizeInBits();
  }
  type->setSizeInBits(sizeInBits);
  type->setContainingClass(classType);
  type->setIsReference(isReference);
  type->setPtrToMemberType(ptrToMemberType);

  return type;
}

STIType *STIDebugImpl::createTypeModifier(const DIDerivedType llvmType) {
  STITypeModifier *type;
  STIType *qualifiedType;

  qualifiedType = createType(resolve(llvmType.getTypeDerivedFrom()));

  type = STITypeModifier::create();
  type->setQualifiedType(qualifiedType);
  type->setIsConstant(llvmType.getTag() == dwarf::DW_TAG_const_type);
  type->setIsVolatile(llvmType.getTag() == dwarf::DW_TAG_volatile_type);
  type->setIsUnaligned(false);
  type->setSizeInBits(qualifiedType->getSizeInBits());

  return type;
}

STIType *STIDebugImpl::createSymbolUserDefined(const DIDerivedType llvmType) {
  STISymbolUserDefined *symbol;
  STIType *userDefinedType;

  DIType derivedType = resolve(llvmType.getTypeDerivedFrom());
  userDefinedType = createType(derivedType);

  if (userDefinedType->getKind() == STI_OBJECT_KIND_TYPE_STRUCTURE) {
    STITypeStructure *pType = static_cast<STITypeStructure *>(userDefinedType);
    auto stringMap = const_cast<STIDebugImpl *>(this)->getStringNameMap();
    if (stringMap->count(derivedType)) {
      (*stringMap)[llvmType] = llvmType.getName();
      pType->setName(llvmType.getName());
    }
  }

  if (userDefinedType->getKind() == STI_OBJECT_KIND_TYPE_ENUMERATION) {
    STITypeEnumeration *pType =
        static_cast<STITypeEnumeration *>(userDefinedType);
    pType->setName(llvmType.getName());
  }

  symbol = STISymbolUserDefined::create();
  symbol->setDefinedType(userDefinedType);
  symbol->setName(llvmType.getName());

  getOrCreateScope(resolve(llvmType.getContext()))->add(symbol);

  return userDefinedType;
}

STIType *STIDebugImpl::createTypeArray(const DICompositeType llvmType) {
  STITypeArray *type = nullptr;
  STIType *elementType;
  bool undefinedSubrange = false;

  elementType = createType(resolve(llvmType.getTypeDerivedFrom()));

  // Add subranges to array type.
  DIArray Elements = llvmType.getElements();
  uint32_t elementLength = elementType->getSizeInBits() >> 3;
  for (int i = Elements.getNumElements() - 1; i >= 0; --i) {
    DIDescriptor Element = Elements.getElement(i);
    if (Element.getTag() != dwarf::DW_TAG_subrange_type) {
      assert(false && "Can array have element that is not of a subrange type?");
      continue;
    }
    DISubrange SR = DISubrange(Element);
    int64_t LowerBound = SR.getLo();
    int64_t DefaultLowerBound = 0; // FIXME : default bound
    int64_t Count = SR.getCount();

    assert(LowerBound == DefaultLowerBound && "TODO: fix default bound check");

    if (Count == -1) {
      // FIXME: this is a WA solution until solving dynamic array boundary.
      Count = 1;
      undefinedSubrange = true;
    }

    type = STITypeArray::create();
    type->setElementType(elementType);
    type->setLength(createNumericUnsignedInt(elementLength * Count));

    elementType = type;
    elementLength *= Count;

    if (i != 0) {
      // FIXME
      const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(type);
    }
  }

  assert((undefinedSubrange ||
         elementLength == (llvmType.getSizeInBits() >> 3)) &&
         "mismatch: bad array subrange sizes");

  type->setName(llvmType.getName());
  type->setSizeInBits(llvmType.getSizeInBits());

  return type;
}

/// If this type is derived from a base type then return base type size.
uint64_t STIDebugImpl::getBaseTypeSize(DIDerivedType Ty) const {
  unsigned Tag = Ty.getTag();

  if (Tag != dwarf::DW_TAG_member && Tag != dwarf::DW_TAG_typedef &&
      Tag != dwarf::DW_TAG_const_type && Tag != dwarf::DW_TAG_volatile_type &&
      Tag != dwarf::DW_TAG_restrict_type)
    return Ty.getSizeInBits();

  DIType BaseType = resolve(Ty.getTypeDerivedFrom());

  // If this type is not derived from any type or the type is a declaration then
  // take conservative approach.
  if (!BaseType.isValid() || BaseType.isForwardDecl())
    return Ty.getSizeInBits();

  // If this is a derived type, go ahead and get the base type, unless it's a
  // reference then it's just the size of the field. Pointer types have no need
  // of this since they're a different type of qualification on the type.
  if (BaseType.getTag() == dwarf::DW_TAG_reference_type ||
      BaseType.getTag() == dwarf::DW_TAG_rvalue_reference_type)
    return Ty.getSizeInBits();

  if (BaseType.isDerivedType())
    return getBaseTypeSize(DIDerivedType(BaseType));

  return BaseType.getSizeInBits();
}

bool STIDebugImpl::isEqualVMethodPrototype(DISubroutineType typeA,
                                           DISubroutineType typeB) {
  DITypeArray ElementsA = typeA.getTypeArray();
  DITypeArray ElementsB = typeB.getTypeArray();

  if (ElementsA.getNumElements() != ElementsB.getNumElements()) {
    return false;
  }

  assert(ElementsA.getNumElements() >= 2 && "non-trevial method");

  for (unsigned i = 2, N = ElementsA.getNumElements(); i < N; ++i) {
    const DIType ElementA = resolve(ElementsA.getElement(i));
    const DIType ElementB = resolve(ElementsB.getElement(i));
    if (ElementA != ElementB) {
      return false;
    }
  }
  return true;
}

void STIDebugImpl::collectClassInfoFromInheritance(ClassInfo &info,
                                                   const DIDerivedType inherTy,
                                                   bool &finalizedOffset) {
  bool isVirtual = inherTy.isVirtual();

  DICompositeType DDTy = DICompositeType(resolve(inherTy.getTypeDerivedFrom()));
  ClassInfo &inherInfo = collectClassInfo(DDTy);

  for (auto &itr : inherInfo.vBaseClasses) {
    if (!info.vBaseClasses.count(itr.first)) {
      int vbIndex = info.vBaseClasses.size() + 1;
      info.vBaseClasses[itr.first] = ClassInfo::VBaseClassInfo(
          itr.second.llvmInheritance, vbIndex, true /*indirect*/);
    }
  }

  if (isVirtual) {
    auto vbClass = info.vBaseClasses.find(DDTy);
    if (vbClass != info.vBaseClasses.end()) {
      vbClass->second.indirect = false;
    } else {
      int vbIndex = info.vBaseClasses.size() + 1;
      info.vBaseClasses[DDTy] =
          ClassInfo::VBaseClassInfo(inherTy, vbIndex, false /*indirect*/);
    }
  } else {
    if (!finalizedOffset) {
      if (inherInfo.vBaseClasses.size()) {
        finalizedOffset = true;
        info.vbpOffset = (DDTy.getOffsetInBits() >> 3) + inherInfo.vbpOffset;
        info.vMethodsCount = inherInfo.vMethodsCount;
      } else {
        info.vbpOffset = (DDTy.getOffsetInBits() + DDTy.getSizeInBits()) >> 3;
      }
    }
    info.baseClasses.push_back(inherTy);
  }

  // append "inherInfo.vMethods" to "info.vMethods"
  for (auto &itr : inherInfo.vMethods) {
    StringRef methodName = itr.first;
    auto &vMethodsDst = info.vMethods[methodName];

    for (unsigned i = 0, Ni = itr.second.size(); i < Ni; ++i) {
      DISubroutineType SPTy(itr.second[i]);
      bool found = false;
      for (unsigned j = 0, Nj = vMethodsDst.size(); j < Nj; ++j) {
        if (isEqualVMethodPrototype(DISubroutineType(vMethodsDst[j]), SPTy)) {
          // virtual method is not introduced.
          found = true;
          break;
        }
      }
      if (!found) {
        vMethodsDst.push_back(SPTy);
      }
    }
  }
}

void STIDebugImpl::collectMemberInfo(ClassInfo &info,
                                     const DIDerivedType DDTy) {
  if (!DDTy.getName().empty()) {
    info.members.push_back(std::make_pair(DDTy, 0));
    return;
  }
  // Member with no name, must be nested structure/union, collects its memebers
  assert((DDTy.getOffsetInBits() % 8) == 0 && "Unnamed bitfield member!");
  unsigned offset = DDTy.getOffsetInBits() >> 3;
  const DIType Ty = resolve(DDTy.getTypeDerivedFrom());
  assert(Ty.isCompositeType() && "Expects structure or union type");
  const DICompositeType DCTy(Ty);
  ClassInfo &nestedInfo = collectClassInfo(DCTy);
  ClassInfo::MemberList &members = nestedInfo.members;
  for (unsigned i = 0, e = members.size(); i != e; ++i) {
    auto itr = members[i];
    info.members.push_back(std::make_pair(itr.first, itr.second + offset));
  }
  //TODO: do we need to create the type of the unnamed member?
  //(void)createType(Ty);
}

ClassInfo &STIDebugImpl::collectClassInfo(const DICompositeType llvmType) {
  auto *CIM = getClassInfoMap();
  auto itr = CIM->find(llvmType);
  if (itr != CIM->end()) {
    return *itr->second;
  }

  CIM->insert(std::make_pair(llvmType, new ClassInfo()));
  ClassInfo &info = *(CIM->find(llvmType)->second);

  std::string constructorName = llvmType.getName();
  std::string destructorName = (Twine("~") + Twine(llvmType.getName())).str();
  std::string virtualTableName =
      (Twine("_vptr$") + Twine(llvmType.getName())).str();

  bool finalizedOffset = false;

  // Add elements to structure type.
  DIArray Elements = llvmType.getElements();
  for (unsigned i = 0, N = Elements.getNumElements(); i < N; ++i) {
    const DIDescriptor Element = Elements.getElement(i);
    if (Element.isSubprogram()) {
      // FIXME: implement this case
      // getOrCreateSubprogramDIE(DISubprogram(Element));
      const DISubprogram subprogram(Element);
      StringRef methodName = subprogram.getName();
      info.methods[methodName].push_back(
          std::make_pair(subprogram, true /*introduced*/));

      if (methodName == constructorName)
        info.hasCTOR = true;
      if (methodName == destructorName)
        info.hasDTOR = true;

    } else if (Element.isDerivedType()) {
      const DIDerivedType DDTy(Element);
      if (DDTy.getTag() == dwarf::DW_TAG_friend) {
        // FIXME: implement this case
        // DIE &ElemDie = createAndAddDIE(dwarf::DW_TAG_friend, Buffer);
        // addType(ElemDie, resolve(DDTy.getTypeDerivedFrom()),
        //        dwarf::DW_AT_friend);
        assert(!"FIXME: implement this case");
      } else {
        if (DDTy.getName() == virtualTableName) {
          assert(!info.vFuncTab && "Class has more than one virtual table.");
          info.vFuncTab = DDTy;
        } else if (DDTy.getTag() == dwarf::DW_TAG_inheritance) {
          collectClassInfoFromInheritance(info, DDTy, finalizedOffset);
        } else {
          collectMemberInfo(info, DDTy);
        }
      }
    }
  }
  bool hasVFuncTab = false;
  for (auto &itr : info.methods) {
    StringRef methodName = itr.first;
    if (methodName == destructorName) {
      methodName = "~";
    }

    auto &vMethods = info.vMethods[methodName];
    for (unsigned i = 0, Ni = itr.second.size(); i < Ni; ++i) {
      auto &methodInfo = itr.second[i];
      DISubprogram subprogram(methodInfo.first);

      if (subprogram.getVirtuality() == dwarf::DW_VIRTUALITY_none) {
        // non-virtual method, nothing to update. Just skip it.
        continue;
      }
      DISubroutineType SPTy(subprogram.getType());

      for (unsigned j = 0, Nj = vMethods.size(); j < Nj; ++j) {
        if (isEqualVMethodPrototype(DISubroutineType(vMethods[j]), SPTy)) {
          // virtual method is not introduced.
          methodInfo.second = false;
        }
      }
      if (methodInfo.second) {
        // an introduced virtual function, update counter and add to vMethods.
        info.vMethodsCount++;
        vMethods.push_back(SPTy);
        hasVFuncTab = true;
      }
    }
  }

  if (!hasVFuncTab) {
    info.vFuncTab = nullptr;
  }

  if (info.vBaseClasses.size() > 0 && info.vbpOffset < 0) {
    if (info.vFuncTab) {
      // Class has virtual function pointer, add pointer size.
      info.vbpOffset = getPointerSizeInBits() >> 3;
    } else {
      info.vbpOffset = 0;
    }
  }

  return info;
}

STIType *STIDebugImpl::createTypeStructure(const DICompositeType llvmType,
                                           bool isDcl) {
  STITypeFieldList *fieldType = nullptr;
  int16_t prop = 0;
  int32_t size = 0;
  STITypeVShape *vshapeType = nullptr;
  STITypePointer *virtualTableType = nullptr;
  STITypeStructure *type = nullptr;

  if (llvmType.isForwardDecl()) {
    isDcl = true;
  }

  if (!llvmType.getName().empty()) {
    STIType *classType = getClassScope(resolve(llvmType.getContext()));
    if (classType) {
      assert(classType->getKind() == STI_OBJECT_KIND_TYPE_STRUCTURE &&
             "unknown containing type");
      prop = prop | PROP_ISNESTED;
      STITypeStructure *pType = static_cast<STITypeStructure *>(classType);
      pType->setProperty(pType->getProperty() | PROP_CNESTED);
    }
  }

  if (isDcl) {
    prop = prop | PROP_FWDREF;
  } else {
    STIType *dclType = createType(llvmType); // Force creating a declaration.
    fieldType = STITypeFieldList::create();

    ClassInfo &info = collectClassInfo(llvmType);

    if (info.hasCTOR) {
      prop = prop | PROP_CTOR;
    }

    // Create base classes
    ClassInfo::BaseClassList &baseClasses = info.baseClasses;
    for (unsigned i = 0, e = baseClasses.size(); i != e; ++i) {
      const DIDerivedType inheritance = DIDerivedType(baseClasses[i]);

      STITypeBaseClass *bClass = STITypeBaseClass::create();
      bClass->setAttribute(getTypeAttribute(inheritance, llvmType));
      bClass->setType(createType(resolve(inheritance.getTypeDerivedFrom())));
      bClass->setOffset(
              createNumericUnsignedInt(inheritance.getOffsetInBits() >> 3));

      fieldType->getBaseClasses().push_back(bClass);
    }

    // Create virtual base classes
    for (auto &itr : info.vBaseClasses) {
      const DIDerivedType inheritance =
          DIDerivedType(itr.second.llvmInheritance);
      unsigned vbIndex = itr.second.vbIndex;
      bool indirect = itr.second.indirect;

      STITypeVBaseClass *vbClass = STITypeVBaseClass::create(indirect);
      vbClass->setAttribute(getTypeAttribute(inheritance, llvmType));
      vbClass->setType(createType(resolve(inheritance.getTypeDerivedFrom())));
      vbClass->setVbpType(getVbpType());
      vbClass->setVbpOffset(createNumericSignedInt(info.vbpOffset));
      vbClass->setVbIndex(createNumericUnsignedInt(vbIndex));

      fieldType->getVBaseClasses().push_back(vbClass);
    }

    // Create members
    ClassInfo::MemberList &members = info.members;
    for (unsigned i = 0, e = members.size(); i != e; ++i) {
      auto itr = members[i];
      const DIDerivedType llvmMember = DIDerivedType(itr.first);

      STITypeMember *member = STITypeMember::create();

      STIType *memberBaseType =
          createType(resolve(llvmMember.getTypeDerivedFrom()));

      if (llvmMember.isStaticMember()) {
        member->setIsStatic(true);
        member->setAttribute(getTypeAttribute(llvmMember, llvmType));
        member->setType(memberBaseType);
        member->setName(llvmMember.getName());

        fieldType->getMembers().push_back(member);
        continue;
      }

      // TODO: move the member size calculation to a helper function.
      uint64_t Size = llvmMember.getSizeInBits();
      uint64_t FieldSize = getBaseTypeSize(llvmMember);
      uint64_t OffsetInBytes = itr.second;

      if (Size != FieldSize) {
        STITypeBitfield *bitfieldType = STITypeBitfield::create();

        uint64_t Offset = llvmMember.getOffsetInBits();
        uint64_t AlignMask = ~(llvmMember.getAlignInBits() - 1);
        uint64_t HiMark = (Offset + FieldSize) & AlignMask;
        uint64_t FieldOffset = (HiMark - FieldSize);
        Offset -= FieldOffset;

        // Maybe we need to work from the other end.
        // if (ASM()->getDataLayout().isLittleEndian())
        //  Offset = FieldSize - (Offset + Size);

        bitfieldType->setOffset(Offset);
        bitfieldType->setSize(Size);
        bitfieldType->setType(memberBaseType);

        const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
            bitfieldType); // FIXME

        OffsetInBytes += FieldOffset >> 3;
        memberBaseType = bitfieldType;
      } else {
        // This is not a bitfield.
        OffsetInBytes += llvmMember.getOffsetInBits() >> 3;
      }

      member->setAttribute(getTypeAttribute(llvmMember, llvmType));
      member->setType(memberBaseType);
      member->setOffset(createNumericUnsignedInt(OffsetInBytes));
      member->setName(llvmMember.getName());

      fieldType->getMembers().push_back(member);
    }

    // Create methods
    for (auto &itr : info.methods) {
      unsigned overloadedCount = itr.second.size();
      assert(overloadedCount > 0 && "Empty methods map entry");
      if (overloadedCount == 1) {
        auto &methodInfo = itr.second[0];
        DISubprogram subprogram = DISubprogram(methodInfo.first);
        bool introduced = methodInfo.second;

        bool isStatic = isStaticMethod(subprogram.getLinkageName());

        unsigned attribute =
            getFunctionAttribute(subprogram, llvmType, introduced);
        STIType *methodtype =
            createType(resolve(subprogram.getType().operator DITypeRef()),
                       dclType, isStatic);

        unsigned virtuality = subprogram.getVirtuality();
        unsigned virtualIndex = subprogram.getVirtualIndex();

        // Create LF_METHOD entry
        STITypeOneMethod *method = STITypeOneMethod::create();

        method->setAttribute(attribute);
        method->setType(methodtype);
        if (introduced) {
          method->setVirtuality(virtuality);
          method->setVirtualIndex(virtualIndex);
        }
        method->setName(itr.first);

        fieldType->getOneMethods().push_back(method);
      } else {
        // Create LF_METHODLIST entry
        STITypeMethodList *methodList = STITypeMethodList::create();
        for (unsigned i = 0; i < overloadedCount; ++i) {
          auto &methodInfo = itr.second[i];
          DISubprogram subprogram = DISubprogram(methodInfo.first);
          bool introduced = methodInfo.second;

          bool isStatic = isStaticMethod(subprogram.getLinkageName());

          unsigned attribute =
              getFunctionAttribute(subprogram, llvmType, introduced);
          STIType *methodtype =
              createType(resolve(subprogram.getType().operator DITypeRef()),
                         dclType, isStatic);

          unsigned virtuality = subprogram.getVirtuality();
          unsigned virtualIndex = subprogram.getVirtualIndex();

          STITypeMethodListEntry *entry = STITypeMethodListEntry::create();

          entry->setAttribute(attribute);
          entry->setType(methodtype);
          if (introduced) {
            entry->setVirtuality(virtuality);
            entry->setVirtualIndex(virtualIndex);
          }

          methodList->getList().push_back(entry);
        }

        const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
            methodList); // FIXME

        // Create LF_METHOD entry
        STITypeMethod *method = STITypeMethod::create();

        method->setCount(overloadedCount);
        method->setList(methodList);
        method->setName(itr.first);

        fieldType->getMethods().push_back(method);
      }
    }

    if (info.vMethodsCount) {
      vshapeType = STITypeVShape::create();
      vshapeType->setCount(info.vMethodsCount);

      const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
          vshapeType); // FIXME

      if (info.vFuncTab) {
        // Create VFUNCTAB
        virtualTableType = STITypePointer::create();
        virtualTableType->setSizeInBits(getPointerSizeInBits());

        STITypeVFuncTab *vFuncTab = STITypeVFuncTab::create();

        virtualTableType->setPointerTo(vshapeType);
        vFuncTab->setType(virtualTableType);

        const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
            virtualTableType); // FIXME

        fieldType->setVFuncTab(vFuncTab);
      }
    }

    const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
        fieldType); // FIXME

    size = (uint32_t)(llvmType.getSizeInBits() >> 3);
  }
#if 0
    DICompositeType ContainingType(resolve(CTy.getContainingType()));
    if (ContainingType)
      addDIEEntry(Buffer, dwarf::DW_AT_containing_type,
                  *getOrCreateTypeDIE(ContainingType));

    // Add template parameters to a class, structure or union types.
    // FIXME: The support isn't in the metadata for this yet.
    if (Tag == dwarf::DW_TAG_class_type ||
        Tag == dwarf::DW_TAG_structure_type || Tag == dwarf::DW_TAG_union_type)
      addTemplateParams(Buffer, CTy.getTemplateParams());
#endif

  type = STITypeStructure::create();

  switch (llvmType.getTag()) {
#define X(TAG, TYPE)                                                           \
  case dwarf::TAG:                                                             \
    type->setLeaf(TYPE);                                                       \
    break
    X(DW_TAG_class_type, LF_CLASS);
    X(DW_TAG_structure_type, LF_STRUCTURE);
    X(DW_TAG_union_type, LF_UNION);
#undef X
  default:
    assert(!"Unknown structure type");
  }

  std::string fullCLassName =
      getScopeFullName(resolve(llvmType.getContext()), llvmType.getName());

  if (fullCLassName.empty()) {
    auto stringMap = const_cast<STIDebugImpl *>(this)->getStringNameMap();
    if (!stringMap->count(llvmType)) {
      stringMap->insert(std::make_pair(llvmType, getUniqueName()));
    }
    fullCLassName = stringMap->find(llvmType)->second;
  }

  type->setCount(isDcl ? 0 : llvmType.getElements().getNumElements());

  type->setProperty(prop); //  FIXME: property

  type->setFieldType(fieldType);

  // type->setDerivedType(STIType* derivedType);
  type->setVShapeType(vshapeType);

  type->setSize(createNumericSignedInt(size));

  type->setName(fullCLassName);

  type->setSizeInBits(llvmType.getSizeInBits());

  if (!isDcl && !llvmType.getName().empty()) {
    STISymbolUserDefined *symbol = STISymbolUserDefined::create();
    symbol->setDefinedType(type);
    symbol->setName(fullCLassName);

    getOrCreateScope(resolve(llvmType.getContext()))->add(symbol);
  }

  return type;
}

STIType *STIDebugImpl::createTypeEnumeration(const DICompositeType llvmType) {
  STITypeEnumeration *type;
  STITypeFieldList *fieldType = nullptr;
  STIType *elementType = nullptr;
  unsigned elementCount = 0;
  bool isDcl = false;
  int16_t prop = 0;

  if (llvmType.isForwardDecl()) {
    isDcl = true;
  }

  STIType *classType = getClassScope(resolve(llvmType.getContext()));
  if (classType) {
    assert(classType->getKind() == STI_OBJECT_KIND_TYPE_STRUCTURE &&
           "unknown containing type");
    prop = prop | PROP_ISNESTED;
    STITypeStructure *pType = static_cast<STITypeStructure *>(classType);
    pType->setProperty(pType->getProperty() | PROP_CNESTED);
  }

  if (isDcl) {
    prop = prop | PROP_FWDREF;
  } else {

    elementType = createType(resolve(llvmType.getTypeDerivedFrom()));

    DIArray Elements = llvmType.getElements();
    elementCount = Elements.getNumElements();

    fieldType = STITypeFieldList::create();

    // Add enumerators to enumeration type.
    for (unsigned i = 0; i < elementCount; ++i) {
      DIEnumerator Enum(Elements.getElement(i));
      if (!Enum.isEnumerator()) {
        assert(!"enumeration element is not an enumerator!");
        continue;
      }
      STITypeEnumerator *enumeratorType = STITypeEnumerator::create();

      uint16_t attribute = 0;

      attribute = attribute | STI_ACCESS_PUBLIC;

      enumeratorType->setAttribute(attribute); // FIXME: attribute
      enumeratorType->setValue(createNumericSignedInt(Enum.getEnumValue()));
      enumeratorType->setName(Enum.getName());

      fieldType->getEnumerators().push_back(enumeratorType);
    }

    const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
        fieldType); // FIXME
  }

  type = STITypeEnumeration::create();

  type->setCount(elementCount); // TODO: is this right?

  type->setProperty(prop); //  FIXME: property

  type->setElementType(elementType);

  type->setFieldType(fieldType);

  type->setName(llvmType.getName());

  type->setSizeInBits(llvmType.getSizeInBits());

  return type;
}

STIType *STIDebugImpl::createTypeSubroutine(const DISubroutineType llvmType,
                                            STIType *classType, bool isStatic) {
  STITypeProcedure *procedureType;
  STITypeArgumentList *argListType;
  int callingConvention = NEAR_C; // FIXME:

  procedureType = STITypeProcedure::create();
  argListType = STITypeArgumentList::create();

  procedureType->setCallingConvention(callingConvention);
  procedureType->setArgumentList(argListType);

  // Add return type. A void return won't have a type.
  DITypeArray Elements = llvmType.getTypeArray();
  DIType RTy(resolve(Elements.getElement(0)));
  procedureType->setReturnType(createType(RTy));

  unsigned firstArgIndex = 1;
  if (classType) {
    // This is a member function, initialize: classType, thisType, thisAdjust
    procedureType->setClassType(classType);
    if (!isStatic) {
      assert(Elements.getNumElements() >= 2 &&
             "Expect at least return value and 'this' argument");
      procedureType->setThisType(createType(resolve(Elements.getElement(1))));
      firstArgIndex = 2;
      procedureType->setThisAdjust(0); // FIXME:
    }
  }

  // Function
  procedureType->setParamCount(Elements.getNumElements() - firstArgIndex);
  for (unsigned i = firstArgIndex, N = Elements.getNumElements(); i < N; ++i) {
    DIType Ty = resolve(Elements.getElement(i));
    if (!Ty) {
      assert(i == N - 1 && "Unspecified parameter must be the last argument");
      // FIXME: handle variadic function argument
      // createAndAddDIE(dwarf::DW_TAG_unspecified_parameters, Buffer);
      procedureType->setParamCount(Elements.getNumElements() - 2);
      argListType->getArgumentList()->push_back(nullptr);
    } else {
      argListType->getArgumentList()->push_back(createType(Ty));
      // if (DIType(Ty).isArtificial())
      //  addFlag(Arg, dwarf::DW_AT_artificial);
    }
  }

#if 0
    bool isPrototyped = true;
    if (Elements.getNumElements() == 2 &&
        !Elements.getElement(1))
      isPrototyped = false;

    // Add prototype flag if we're dealing with a C language and the
    // function has been prototyped.
    uint16_t Language = getLanguage();
    if (isPrototyped &&
        (Language == dwarf::DW_LANG_C89 || Language == dwarf::DW_LANG_C99 ||
         Language == dwarf::DW_LANG_ObjC))
      addFlag(Buffer, dwarf::DW_AT_prototyped);

    if (CTy.isLValueReference())
      addFlag(Buffer, dwarf::DW_AT_reference);

    if (CTy.isRValueReference())
      addFlag(Buffer, dwarf::DW_AT_rvalue_reference);
#endif

  const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
      argListType); // FIXME

  return procedureType;
}

STIType *STIDebugImpl::getVoidType() const {
  if (_voidType == nullptr) {
    STITypeBasic *voidType = STITypeBasic::create();
    voidType->setPrimitive(T_VOID);
    const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
        voidType);                                          // FIXME
    const_cast<STIDebugImpl *>(this)->_voidType = voidType; // FIXME
  }
  return _voidType;
}

STIType *STIDebugImpl::getVbpType() const {
  if (_vbpType == nullptr) {
    STITypePointer *vbpType = STITypePointer::create();
    STITypeModifier *constInt4Type = STITypeModifier::create();
    STITypeBasic *int4Type = STITypeBasic::create();
    int4Type->setPrimitive(T_INT4);
    constInt4Type->setQualifiedType(int4Type);
    constInt4Type->setIsConstant(true);
    vbpType->setPointerTo(constInt4Type);
    vbpType->setSizeInBits(getPointerSizeInBits());
    const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
        int4Type); // FIXME
    const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
        constInt4Type); // FIXME
    const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
        vbpType);                                         // FIXME
    const_cast<STIDebugImpl *>(this)->_vbpType = vbpType; // FIXME
  }
  return _vbpType;
}

unsigned STIDebugImpl::getPointerSizeInBits() const { return _ptrSizeInBits; }

STIType *STIDebugImpl::createType(const DIType llvmType, STIType *classType,
                                  bool isStatic) {
  STIType *type;
  unsigned int tag;

  if (llvmType == nullptr) {
    return getVoidType();
  }

  TypeMap *dclTM = const_cast<STIDebugImpl *>(this)->getDclTypeMap(); // FIXME
  TypeMap::iterator dclItr = dclTM->find(llvmType);
  TypeMap &TM1 =
      (*const_cast<STIDebugImpl *>(this)->getTypeMap())[classType]; // FIXME
  TypeMap::iterator itr = TM1.find(llvmType);

  if (itr != TM1.end()) {
    if (itr->second != nullptr) {
      return itr->second;
    }

    if (dclItr != dclTM->end()) {
      if (dclItr->second != nullptr) {
        return dclItr->second;
      }
    }

    dclTM->insert(std::make_pair(llvmType, nullptr)); // FIXME

    switch (llvmType.getTag()) {
#define X(TAG, HANDLER, TYPE)                                                  \
  case dwarf::TAG:                                                             \
    type = HANDLER(static_cast<TYPE>(llvmType), true);                         \
    dclItr = dclTM->find(llvmType);                                            \
    assert(dclItr != dclTM->end() && "Type should be in map by now!");         \
    if (dclItr->second == nullptr) {                                           \
      dclItr->second = type;                                                   \
      const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(type);       \
    }                                                                          \
    if (dclItr->second != type) {                                              \
      /* need to delete type! */                                               \
      delete type;                                                             \
    }                                                                          \
    return dclItr->second;

      X(DW_TAG_class_type, createTypeStructure, DICompositeType);
      X(DW_TAG_structure_type, createTypeStructure, DICompositeType);
      X(DW_TAG_union_type, createTypeStructure, DICompositeType);
#undef X
    default:
      break;
    }
  } else {
    TM1.insert(std::make_pair(llvmType, nullptr)); // FIXME
  }

  tag = llvmType.getTag();
  switch (tag) {
#define X(TAG, HANDLER, TYPE)                                                  \
  case dwarf::TAG:                                                             \
    type = HANDLER(static_cast<TYPE>(llvmType));                               \
    break
#define X1(TAG, HANDLER, TYPE)                                                 \
  case dwarf::TAG:                                                             \
    type = HANDLER(static_cast<TYPE>(llvmType), classType, isStatic);          \
    break
    X(DW_TAG_array_type, createTypeArray, DICompositeType);
    X(DW_TAG_class_type, createTypeStructure, DICompositeType);
    X(DW_TAG_structure_type, createTypeStructure, DICompositeType);
    X(DW_TAG_union_type, createTypeStructure, DICompositeType);
    X(DW_TAG_enumeration_type, createTypeEnumeration, DICompositeType);
    X(DW_TAG_base_type, createTypeBasic, DIBasicType);
    X(DW_TAG_pointer_type, createTypePointer, DIDerivedType);
    X(DW_TAG_reference_type, createTypePointer, DIDerivedType);
    X(DW_TAG_rvalue_reference_type, createTypePointer, DIDerivedType);
    X(DW_TAG_unspecified_type, createTypePointer, DIDerivedType);
    X(DW_TAG_ptr_to_member_type, createTypePointer, DIDerivedType);
    X(DW_TAG_const_type, createTypeModifier, DIDerivedType);
    X(DW_TAG_volatile_type, createTypeModifier, DIDerivedType);
    X(DW_TAG_typedef, createSymbolUserDefined, DIDerivedType);
    X1(DW_TAG_subroutine_type, createTypeSubroutine, DISubroutineType);
#undef X
#undef X1
  default:
    assert(tag != tag); // unhandled type tag!
    break;
  }

  TypeMap &TM2 = (*const_cast<STIDebugImpl *>(this)->getTypeMap())[classType];
  itr = TM2.find(llvmType); // FIXME
  assert(itr != TM2.end() && "Type should be in map by now!");
  if (itr->second == nullptr) {
    itr->second = type;
    if (tag != dwarf::DW_TAG_typedef) {
      const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
          type); // FIXME
    }
  }
  if (itr->second != type) {
    // need to delete type!
    // Howver, type of typedef is already added to deferent in the type table.
    if (tag != dwarf::DW_TAG_typedef) {
      delete type;
    }
  }

  return itr->second;
}

STIScope *STIDebugImpl::getOrCreateScope(const DIScope llvmScope) {
  STIScope* scope = nullptr;
  if (!llvmScope || llvmScope.isFile() || llvmScope.isCompileUnit()) {
    scope = getCompileUnit()->getScope();
  } else if (llvmScope.isType()) {
    scope = getOrCreateScope(resolve(DIType(llvmScope).getContext()));
  } else if (llvmScope.isNameSpace()) {
    // scope = getOrCreateNameSpace(DINameSpace(llvmScope));
    scope = getOrCreateScope(DINameSpace(llvmScope).getContext());
  } else if (llvmScope.isSubprogram()) {
    STISymbolProcedure *proc =
      getOrCreateSymbolProcedure(DISubprogram(llvmScope));
    if (proc != nullptr) {
      scope = proc->getScope();
    } else {
      //FIXME: WA to prevent build from crashing!
      scope = getCompileUnit()->getScope();
    }
  } else if (hasScope(llvmScope)) {
    scope = getScope(llvmScope);
  } else if (llvmScope.isLexicalBlockFile()) {
    // Must check "isLexicalBlockFile()" before "isLexicalBlock()"
    // It appears this is currently only used for DWARF discriminators.
    // Otherwise it is just another lexical scope.
    STISymbolBlock* block =
        createSymbolBlock(DILexicalBlockFile(llvmScope).getScope());
    scope = block->getScope();
    addScope(llvmScope, scope);
  } else if (llvmScope.isLexicalBlock()) {
    STISymbolBlock *block = createSymbolBlock(DILexicalBlock(llvmScope));
    scope = block->getScope();
    addScope(llvmScope, scope);
  }

  assert(scope != nullptr);  // Callers assume a valid scope is returned.

  return scope;
}

std::string STIDebugImpl::getScopeFullName(const DIScope llvmScope,
                                           StringRef name, bool useClassName) {
  if (!llvmScope || llvmScope.isFile() || name.empty())
    return name;
  if (llvmScope.isType()) {
    if (DIType(llvmScope).getName().empty()) {
      return name;
    }
    std::string scopedName =
        (Twine(DIType(llvmScope).getName()) + "::" + Twine(name)).str();
    return getScopeFullName(resolve(DIType(llvmScope).getContext()),
                            scopedName);
  }
  if (llvmScope.isNameSpace()) {
    StringRef nsName = DINameSpace(llvmScope).getName();
    if (nsName.empty()) {
      nsName = "`anonymous namespace'";
    }
    std::string scopedName = (Twine(nsName) + "::" + Twine(name)).str();
    return getScopeFullName(DINameSpace(llvmScope).getContext(), scopedName);
  }
  if (llvmScope.isSubprogram()) {
    // TODO: should we assert here?
    return name;
  }
  return name;
}

STIType *STIDebugImpl::getClassScope(const DIScope llvmScope) {
  if (!llvmScope || llvmScope.isFile())
    return nullptr;
  if (llvmScope.isType()) {
    return createType(DIType(llvmScope));
  }
  if (llvmScope.isNameSpace()) {
    return nullptr;
  }
  if (llvmScope.isSubprogram()) {
    return nullptr;
  }
  return nullptr;
}

STISymbolVariable *STIDebugImpl::createSymbolVariable(
    const DIVariable DIV, unsigned int frameIndex, const MachineInstr *DVInsn) {
  STISymbolVariable *variable;
  STILocation *location = nullptr;

  variable = STISymbolVariable::create();
  variable->setName(DIV.getName());
  variable->setType(createType(resolve(DIV.getType())));

  if (frameIndex != ~0U) {
    unsigned int regnum;
    int offset;

    const TargetFrameLowering *TFL =
        ASM()->TM.getSubtargetImpl()->getFrameLowering();

    regnum = 0;
    offset = TFL->getFrameIndexReference(*ASM()->MF, frameIndex, regnum);

    location = STILocation::createRegisterOffset(toSTIRegID(regnum), offset);

  } else {
    assert(DVInsn && "Unknown location");
    assert(DVInsn->getNumOperands() == 3 || DVInsn->getNumOperands() == 4);
    // TODO: handle the case DVInsn->getNumOperands() == 4
    bool indirect = isIndirectExpression(DVInsn->getDebugExpression());
    if (DVInsn->getOperand(0).isReg()) {
      const MachineOperand RegOp = DVInsn->getOperand(0);
      // If the second operand is an immediate, this is an indirect value.
      if (DVInsn->getOperand(1).isImm()) {
        if (RegOp.getReg() == 0) {
          location = STILocation::createOffset(DVInsn->getOperand(1).getImm());
        } else {
          location = STILocation::createRegisterOffset(
              toSTIRegID(RegOp.getReg()), DVInsn->getOperand(1).getImm());
        }
      } else if (indirect) {
        location =
            STILocation::createRegisterOffset(toSTIRegID(RegOp.getReg()), 0);
      } else if (RegOp.getReg()) {
        location = STILocation::createRegister(toSTIRegID(RegOp.getReg()));
      }
    } else if (DVInsn->getOperand(0).isImm()) {
      //assert(!"FIXME: support this case");
      // addConstantValue(*VariableDie, DVInsn->getOperand(0), DV.getType());
    } else if (DVInsn->getOperand(0).isFPImm()) {
      //assert(!"FIXME: support this case");
      // addConstantFPValue(*VariableDie, DVInsn->getOperand(0));
    } else if (DVInsn->getOperand(0).isCImm()) {
      //assert(!"FIXME: support this case");
      // addConstantValue(*VariableDie, DVInsn->getOperand(0).getCImm(),
      //                 DV.getType());
    }
  }

  variable->setLocation(location);

  return variable;
}

STISymbolProcedure *
STIDebugImpl::getOrCreateSymbolProcedure(const DISubprogram &SP) {
  Function *pFunc = SP.getFunction();
  if (pFunc == nullptr)
    return nullptr;
  assert(pFunc && "LLVM subprogram has no LLVM function");
  if (_functionMap.count(pFunc)) {
    // Function is already created
    return _functionMap[pFunc];
  }

  STIType *classType = getClassScope(resolve(SP.getContext()));
  bool isStatic = isStaticMethod(SP.getLinkageName());
  STIType *procedureType = createType(
      resolve(SP.getType().operator DITypeRef()), classType, isStatic);
  STITypeFunctionID *funcIDType = STITypeFunctionID::create();

  funcIDType->setType(procedureType);
  funcIDType->setParentScope(nullptr); // FIXME
  funcIDType->setParentClassType(classType);
  funcIDType->setName(SP.getName());

  STISymbolFrameProc *frame = STISymbolFrameProc::create();

  STISymbolProcedure *procedure;
  procedure = STISymbolProcedure::create();
  procedure->setName(
      getScopeFullName(resolve(SP.getContext()), SP.getName(), true));
#if 1
  // FIXME: This is WA till ntobjanl tool is updated.
  procedure->setType(procedureType);
  procedure->setSymbolID(SP.isLocalToUnit() ? S_LPROC32 : S_GPROC32);
#else
  procedure->setType(funcIDType);
  procedure->setSymbolID(SP.isLocalToUnit() ? S_LPROC32_ID : S_GPROC32_ID);
#endif
  procedure->getLineSlice()->setFunction(SP.getFunction());
  procedure->setScopeLineNumber(SP.getScopeLineNumber());
  procedure->setFrame(frame);

  frame->setProcedure(procedure);

  getOrCreateScope(resolve(SP.getContext()))
      ->add(procedure); // FIXME: inline function!?

  const_cast<STIDebugImpl *>(this)->getTypeTable()->push_back(
      funcIDType); // FIXME

  _functionMap.insert(std::make_pair(pFunc, procedure));
  return procedure;
}

STISymbolBlock *STIDebugImpl::createSymbolBlock(const DILexicalBlock &LB) {
  STISymbolBlock *block;
  block = STISymbolBlock::create();

  LexicalScope *Scope = _lexicalScopes.findLexicalScope(LB);
  const SmallVectorImpl<InsnRange> &Ranges = Scope->getRanges();
  assert(!Ranges.empty() && "Handle Block with empty range ");
  // assert(Ranges.size() == 1 && "Handle Block with more than one range");
  // TODO: handle Ranges.size() != 1

  const MachineInstr *BInst = Ranges.front().first;
  const MachineInstr *EInst = Ranges.front().second;

  assert(_labelsBeforeInsn[BInst] && "empty range begin location");
  assert(_labelsAfterInsn[EInst] && "empty range end location");
  // FIXME: emit block labels correctly
  block->setLabelBegin(_labelsBeforeInsn[BInst]);
  block->setLabelEnd(_labelsAfterInsn[EInst]);
  block->setName(LB.getName());

  getOrCreateScope(LB.getContext())->add(block);

  DIScope FuncScope = LB.getContext();
  while (FuncScope.isLexicalBlock()) {
    FuncScope = DILexicalBlock(FuncScope).getContext();
  }
  assert(FuncScope.isSubprogram() &&
         "Failed to reach function scope of a lexical block");
  block->setProcedure(getOrCreateSymbolProcedure(DISubprogram(FuncScope)));

  return block;
}

STIChecksumEntry *STIDebugImpl::getOrCreateChecksum(StringRef path) {
  STIStringEntry *string = _stringTable.find(strdup(path.str().c_str()));
  STIChecksumEntry *checksum = _checksumTable.findEntry(string);

  if (checksum == nullptr) {
    checksum = STIChecksumEntry::create();
    checksum->setStringEntry(string);
    checksum->setType(STIChecksumEntry::STI_FILECHECKSUM_ENTRY_TYPE_NONE);
    checksum->setChecksum(nullptr);
    _checksumTable.append(string, checksum);
  }
  return checksum;
}

//===----------------------------------------------------------------------===//
// getUnqualifiedDIType(ditype)
//
// Returns the specified ditype after stripping the const/volatile qualifiers.
//
//===----------------------------------------------------------------------===//

DIType STIDebugImpl::getUnqualifiedDIType(DIType ditype) {
  uint16_t tag;

  while (ditype.isDerivedType()) {
    DIDerivedType derivedType (ditype);
    tag = derivedType.getTag();
    if (tag != dwarf::DW_TAG_const_type && tag != dwarf::DW_TAG_volatile_type) {
      break;
    }
    ditype = resolve(derivedType.getTypeDerivedFrom());
  }

  return ditype;
}

//===----------------------------------------------------------------------===//
// createNumericUnsignedInt(value)
//
// Creates a numeric leaf representing the specified unsigned integer value.
//
//===----------------------------------------------------------------------===//

STINumeric* STIDebugImpl::createNumericUnsignedInt(const uint64_t value)
{
  STINumeric*           numeric;
  STINumeric::LeafID    leafID;
  size_t                size;
  const char*           data = reinterpret_cast<const char*>(&value);

  if (isUInt<8>(value)) {
    leafID = LF_CHAR;
    size   = 1;
  } else if (isUInt<16>(value)) {
    leafID = LF_USHORT;
    size   = 2;
  } else if (isUInt<32>(value)) {
    leafID = LF_ULONG;
    size   = 4;
  } else {
    leafID = LF_UQUADWORD;
    size   = 8;
  }

  // For small unsigned integers we don't need to encode the leaf identifier.
  //
  if (leafID == LF_CHAR || (leafID == LF_USHORT && value < LF_NUMERIC)) {
    leafID = LF_INTEL_NONE; // No leaf identifier.
  }

  numeric = STINumeric::create(leafID, size, data);

  return numeric;
}

//===----------------------------------------------------------------------===//
// createNumericSignedInt(value)
//
// Creates a numeric leaf representing the specified signed integer value.
//
//===----------------------------------------------------------------------===//

STINumeric* STIDebugImpl::createNumericSignedInt(const int64_t value)
{
  STINumeric*           numeric;
  STINumeric::LeafID    leafID;
  size_t                size;
  const char*           data = reinterpret_cast<const char*>(&value);

  // Non-negative signed values are encoded as unsigned values.
  //
  if (value > 0) {
    return createNumericUnsignedInt(static_cast<uint64_t>(value));
  }

  // Adjust encoded size based on value.
  //
  if (isInt<8>(value)) {
    leafID = LF_CHAR;
    size   = 1;
  } else if (isInt<16>(value)) {
    leafID = LF_SHORT;
    size   = 2;
  } else if (isInt<32>(value)) {
    leafID = LF_LONG;
    size   = 4;
  } else {
    leafID = LF_QUADWORD;
    size   = 8;
  }

  numeric = STINumeric::create(leafID, size, data);

  return numeric;
}

//===----------------------------------------------------------------------===//
// createNumericAPInt(ditype, value)
//
// Returns a numeric value with one of the following encodings:
//   * LF_USHORT
//   * LF_ULONG
//   * LF_UQUADWORD
//   * LF_CHAR
//   * LF_SHORT
//   * LF_LONG
//   * LF_QUADWORD
//
//===----------------------------------------------------------------------===//

STINumeric* STIDebugImpl::createNumericAPInt(
        const DIType ditype,
        const APInt& value) {
  STINumeric*   numeric;
  DIType        unqualifiedDIType;

  // It's not clear how we would encode an arbitrary length integer more
  // than 64-bits long in the STI debug information format, so we ignore
  // them altogether here.
  //
  if (value.getBitWidth() > 64) {
    return nullptr;
  }

  unqualifiedDIType = getUnqualifiedDIType(ditype);

  // We don't currently handle constant values for non-basic types.
  //
  if (!unqualifiedDIType.isBasicType()) {
    return nullptr;
  }

  DIBasicType dibasic (unqualifiedDIType);
  unsigned    encoding = dibasic.getEncoding();

  switch (encoding) {
  case dwarf::DW_ATE_boolean:
  case dwarf::DW_ATE_unsigned_char:
  case dwarf::DW_ATE_unsigned:
    numeric = createNumericUnsignedInt(value.getZExtValue());
    break;

  case dwarf::DW_ATE_signed_char:
  case dwarf::DW_ATE_signed:
    numeric = createNumericSignedInt(value.getSExtValue());
    break;

  default:
    numeric = nullptr;
    break;
  }

  return numeric;
}

//===----------------------------------------------------------------------===//
// createNumericAPFloat(ditype, value)
//
// Returns a numeric value with one of the following encodings:
//   * LF_REAL32
//   * LF_REAL48
//   * LF_REAL64
//   * LF_REAL80
//   * LF_REAL128
//
// NOTE: Although cvdump can correctly dump floating point constants, the
//       Microsoft compiler (cl) doesn't produce these for global variables
//       and Visual Studio can't properly display them.
//
//===----------------------------------------------------------------------===//

STINumeric* STIDebugImpl::createNumericAPFloat(
        const DIType   ditype,
        const APFloat& value) {
  STINumeric*           numeric;
  STINumeric::LeafID    leafID;
  DIType                unqualifiedDIType;
  const char*           data;
  size_t                size;                   // size of data in bytes

  unqualifiedDIType = getUnqualifiedDIType(ditype);

  // We don't currently handle constant values for non-basic types.
  //
  if (!unqualifiedDIType.isBasicType()) {
    return nullptr;
  }

  // Convert bit size to byte size.  Round up partial bytes (1 bit => 1 byte).
  //
  // NOTE: It looks like the bitcast may be losing some precision, but this is
  //       the same way the rest of the compiler acquires the byte sequence.
  //
  data = reinterpret_cast<const char*>(value.bitcastToAPInt().getRawData());

  DIBasicType           dibasic (unqualifiedDIType);
  const fltSemantics&   semantics = value.getSemantics();

  if (&semantics == &APFloat::IEEEsingle) {
    leafID = LF_REAL32;
    size   = 4;
  } else if (&semantics == &APFloat::IEEEdouble) {
    leafID = LF_REAL64;
    size   = 8;
  } else if (&semantics == &APFloat::x87DoubleExtended) {
    leafID = LF_REAL80;
    size   = 10;
  } else if (&semantics == &APFloat::IEEEquad) {
    leafID = LF_REAL128;
    size   = 16;
  } else {
    // Not Yet Supported:
    //   * IEEEhalf:
    //   * PPCDoubleDouble:
    //   * Bogus:
    return nullptr;
  }

  // Create the numeric value encoding.
  //
  numeric = STINumeric::create(leafID, size, data);

  return numeric;
}

//===----------------------------------------------------------------------===//
// collectGlobalVariableInfo(CU)
//
// Iterates over all of the global variables in specified compilation unit and
// generates debug information entries for them.
//
//===----------------------------------------------------------------------===//

void STIDebugImpl::collectGlobalVariableInfo(DICompileUnit CU) {
  DIArray           DIGVs = CU.getGlobalVariables();

  for (unsigned int I = 0, E = DIGVs.getNumElements(); I < E; ++I) {
    DIGlobalVariable DIGV (DIGVs.getElement(I));

    if (GlobalVariable* global = DIGV.getGlobal()) {
      STISymbolVariable* variable;

      MCSymbol *label = ASM()->getSymbol(global);

      STILocation *location = DIGV.isLocalToUnit()
                            ? STILocation::createLocalSegmentedOffset(label)
                            : STILocation::createGlobalSegmentedOffset(label);

      DIScope context = DIGV.getContext();
      if (DIDerivedType SDMDecl = DIGV.getStaticDataMemberDeclaration()) {
        context = resolve(SDMDecl.getContext());
        assert(SDMDecl.isStaticMember() && "Expected static member decl");
        assert(DIGV.isDefinition());
      }

      variable = STISymbolVariable::create();
      variable->setName(getScopeFullName(context, DIGV.getName(), true));
      variable->setType(createType(resolve(DIGV.getType())));
      variable->setLocation(location);

      getOrCreateScope(DIGV.getContext())->add(variable);

      std::string path;
      getFullFileName(context, path);
      (void)getOrCreateChecksum(path);  // FIXME:  Do not check every variable!

    } else if (Constant* constant = DIGV.getConstant()) {
      STISymbolConstant* symbol;
      DIScope            discope = DIGV.getContext();
      DIType             ditype  = resolve(DIGV.getType());
      STINumeric*        numeric;

      // Translate the different constant types into a STINumeric object.
      //
      if (ConstantInt* CI = dyn_cast<ConstantInt>(constant)) {
        numeric = createNumericAPInt(ditype, CI->getValue());

      } else if (ConstantFP* CFP = dyn_cast<ConstantFP>(constant)) {
        numeric = createNumericAPFloat(ditype, CFP->getValueAPF());

      } else {
        // Possible unsupported numeric encodings:
        //   * LF_COMPLEX32
        //   * LF_COMPLEX64
        //   * LF_COMPLEX80
        //   * LF_COMPLEX128
        //   * LF_VARSTRING
        //   * LF_OCTWORD
        //   * LF_UOCTWORD
        //   * LF_DECIMAL
        //   * LF_UTFSTRING
        //
        numeric = nullptr;
      }

      // If we can't calculate the constant value, then we don't emit anything.
      // Skip to the next entry.
      //
      if (numeric == nullptr) {
          continue;
      }

      // Create a symbolic constant using the type and numeric value.
      //
      symbol = STISymbolConstant::create();
      symbol->setName(getScopeFullName(discope, DIGV.getName(), true));
      symbol->setType(createType(ditype));
      symbol->setValue(numeric);

      getOrCreateScope(discope)->add(symbol);
    }
  }
}

void STIDebugImpl::collectModuleInfo() {
  const Module *M = getModule();
  STISymbolModule *module;

  module = STISymbolModule::create(M);
  getSymbolTable()->setRoot(module);

  NamedMDNode *CU_Nodes = M->getNamedMetadata("llvm.dbg.cu");
  if (!CU_Nodes)
    return;

  TypeIdentifierMap = generateDITypeIdentifierMap(CU_Nodes);

  for (const MDNode *node : CU_Nodes->operands()) {
    DICompileUnit CU(node);
    STISymbolCompileUnit *compileUnit;

    compileUnit = STISymbolCompileUnit::create(CU);
    compileUnit->setProducer(CU.getProducer());
    compileUnit->setMachineID(
        toMachineID(Triple(ASM()->getTargetTriple()).getArch()));
    module->add(compileUnit);

    collectGlobalVariableInfo(CU);

    DIArray SPs = CU.getSubprograms();
    for (unsigned int i = 0, e = SPs.getNumElements(); i < e; ++i) {
      DISubprogram SP(SPs.getElement(i));

      getOrCreateSymbolProcedure(SP);
    }
  }
}

void STIDebugImpl::collectRoutineInfo() {
  typedef MachineModuleInfo::VariableDbgInfo VariableDbgInfo;
  typedef DbgValueHistoryMap::InstrRanges InstrRanges;
  typedef std::pair<const MDNode *, InstrRanges> VariableHistoryInfo;

  STISymbolVariable *variable;

  std::set<const MDNode *> processed;

  for (const VariableDbgInfo &info : MMI()->getVariableDbgInfo()) {
    DIVariable DIV(info.Var);

    if (processed.count(DIV))
      continue;

    // Ignore this variable if we can't identify the scope it belongs to.
    // This prevents us from crashing later when we try to insert the variable
    // into the scope.
    //
    if (!_lexicalScopes.findLexicalScope(DIV.getContext())) {
      continue;
    }

    variable = createSymbolVariable(DIV, info.Slot);
    getOrCreateScope(DIV.getContext())->add(variable, DIV.getArgNumber());

    processed.insert(DIV);
  }

  for (const VariableHistoryInfo &info : _valueHistory) {
    const MDNode *node = info.first;
    const DbgValueHistoryMap::InstrRanges &Ranges = info.second;
    DIVariable DIV(node);

    if (processed.count(DIV))
      continue;

    const MachineInstr *MInsn = Ranges.front().first;
    variable = createSymbolVariable(DIV, ~0, MInsn); // FIXME: params
    getOrCreateScope(DIV.getContext())->add(variable, DIV.getArgNumber());

    processed.insert(DIV);
  }
}

void STIDebugImpl::layout() {
  uint16_t nextTypeIndex = 0x1000;
  for (STIType *type : *getTypeTable()) {
    switch (type->getKind()) {
    case STI_OBJECT_KIND_TYPE_BASIC:
      type->setIndex(static_cast<STITypeBasic *>(type)->getPrimitive());
      continue;
    case STI_OBJECT_KIND_TYPE_POINTER: {
      STITypePointer *pType = static_cast<STITypePointer *>(type);
      STIType *pPointerTo = pType->getPointerTo();
      if (pPointerTo->getKind() == STI_OBJECT_KIND_TYPE_BASIC) {
        STITypeBasic *pBasicType = static_cast<STITypeBasic *>(pPointerTo);
        switch (pBasicType->getPrimitive()) {
        // TODO: Add more cases!
        case T_CHAR:
          type->setIndex(T_64PRCHAR);
          continue;
        }
      }
    }
    default:
      type->setIndex(nextTypeIndex++);
      break;
    }
  }

  uint32_t nextStringOffset = 0;
  for (STIStringEntry *entry : getStringTable()->getEntries()) {
    entry->setOffset(nextStringOffset);
    nextStringOffset += entry->getString().size() + 1;
  }

  uint32_t nextChecksumOffset = 0;
  for (STIChecksumEntry *entry : getChecksumTable()->getEntries()) {
    entry->setOffset(nextChecksumOffset);
    nextChecksumOffset += 6 + entry->getChecksumSize() + getPaddingSize(entry);
  }
}

void STIDebugImpl::emit() {
  emitTypes();          // Emits the .debug$S section.
  emitSymbols();        // Emits the .debug$T section.
}

void STIDebugImpl::emitSymbolID(const STISymbolID symbolID) const {
  emitComment(toString(symbolID));
  emitInt16(symbolID);
}

void STIDebugImpl::emitSubsectionBegin(STISubsection *subsection) const {
  STISubsectionID id = subsection->getID();

  // Create the beginning and ending labels for this subsection.
  subsection->setBegin(MMI()->getContext().CreateTempSymbol());
  subsection->setEnd(MMI()->getContext().CreateTempSymbol());

  // Subsections are 4-byte aligned.
  emitAlign(4);

  // Each subsection begins with an identifier for the type of subsection.
  emitComment(toString(id));
  emitInt32(id);

  // Followed by the subsection length.  The end label is emitted later.
  emitComment("length");
  emitLabelDiff(subsection->getBegin(), subsection->getEnd());

  // Mark the beginning of the subsection which contributes to the length.
  emitLabel(subsection->getBegin());
}

void STIDebugImpl::emitSubsectionEnd(STISubsection *subsection) const {
  // Mark the end of the subsection which contributes to the length.
  emitLabel(subsection->getEnd());
}

void STIDebugImpl::closeSubsection() const {
  if (_currentSubsection != nullptr) {
    emitSubsectionEnd(_currentSubsection);
    delete _currentSubsection;
    const_cast<STIDebugImpl *>(this)->_currentSubsection = nullptr;
  }
}

void STIDebugImpl::emitSubsection(STISubsectionID id) const {
  // If trying to change subsection to same subsection do nothing.
  if (_currentSubsection != nullptr && _currentSubsection->getID() == id) {
    return;
  }

  closeSubsection();

  const_cast<STIDebugImpl *>(this)->_currentSubsection = new STISubsection(id);
  emitSubsectionBegin(_currentSubsection);
}

MCSymbol *STIDebugImpl::createFuncLabel(const char *name) const {
  return ASM()->GetTempSymbol(name, ASM()->getFunctionNumber());
}

MCSymbol *STIDebugImpl::createBlockLabel(const char *name) {
  return ASM()->GetTempSymbol(name, _blockNumber++);
}

void STIDebugImpl::emitAlign(unsigned int byteAlignment) const {
  ASM()->OutStreamer.EmitValueToAlignment(byteAlignment);
}

void STIDebugImpl::typeBegin(const STIType* type) const {
  writer()->typeBegin(type);
}

void STIDebugImpl::typeEnd(const STIType* type) const {
  writer()->typeEnd(type);
}

void STIDebugImpl::emitInt8(int value) const {
  writer()->emitInt8(value);
}

void STIDebugImpl::emitInt16(int value) const {
  writer()->emitInt16(value);
}

void STIDebugImpl::emitInt32(int value) const {
  writer()->emitInt32(value);
}

void STIDebugImpl::emitString(StringRef string) const {
  writer()->emitString(string);
}

void STIDebugImpl::emitBytes(const char *data, size_t size) const {
  writer()->emitBytes(size, data);
}

void STIDebugImpl::emitFill(size_t size, const uint8_t byte) const {
  writer()->emitFill(size, byte);
}

void STIDebugImpl::emitComment(StringRef comment) const {
  writer()->emitComment(comment);
}

void STIDebugImpl::emitLabel(MCSymbol *symbol) const {
  writer()->emitLabel(symbol);
}

void STIDebugImpl::emitValue(const MCExpr *value,
                             unsigned int sizeInBytes) const {
  writer()->emitValue(value, sizeInBytes);
}

void STIDebugImpl::emitPadding(unsigned int padByteCount) const {
  static const int paddingArray[16] = {
      LF_PAD0,  LF_PAD1,  LF_PAD2,  LF_PAD3,
      LF_PAD4,  LF_PAD5,  LF_PAD6,  LF_PAD7,
      LF_PAD8,  LF_PAD9,  LF_PAD10, LF_PAD11,
      LF_PAD12, LF_PAD13, LF_PAD14, LF_PAD15};

  for (unsigned int i = padByteCount; i > 0; --i) {
    writer()->emitInt8(paddingArray[i]);
  }
}

void STIDebugImpl::emitLabelDiff(const MCSymbol *begin,
                                 const MCSymbol *end) const {
  MCContext &context = ASM()->OutStreamer.getContext();
  const MCExpr *bExpr;
  const MCExpr *eExpr;
  const MCExpr *delta;

  bExpr = MCSymbolRefExpr::Create(begin, MCSymbolRefExpr::VK_None, context);
  eExpr = MCSymbolRefExpr::Create(end, MCSymbolRefExpr::VK_None, context);
  delta = MCBinaryExpr::Create(MCBinaryExpr::Sub, eExpr, bExpr, context);

  emitValue(delta, 4);
}

void STIDebugImpl::emitSecRel32(MCSymbol *symbol) const {
  ASM()->OutStreamer.EmitCOFFSecRel32(symbol);
}

void STIDebugImpl::emitSectionIndex(MCSymbol *symbol) const {
  ASM()->OutStreamer.EmitCOFFSectionIndex(symbol);
}

void STIDebugImpl::emitNumeric(const uint32_t num) const {
  if (num < LF_NUMERIC) {
    emitInt16(num);
  } else if (num < (LF_NUMERIC << 1)) {
    emitInt16(LF_USHORT);
    emitInt16(num);
  } else {
    emitInt16(LF_ULONG);
    emitInt32(num);
  }
}

bool STIDebugImpl::usePDB() const {
  return _usePDB;
}

std::string STIDebugImpl::getPDBFullPath() const {
  char *path = pdb_get_path();
  std::string pdbName = (Twine(path) + Twine("\\vc110.pdb")).str();
  free(path);
  return pdbName;
}

void STIDebugImpl::emitSymbolModule(const STISymbolModule *module) const {
  STISignatureID signatureID = module->getSignatureID();
  StringRef path = module->getPath();
  const int length = 7 + path.size();

  emitInt16(length);
  emitSymbolID(S_OBJNAME);
  emitInt32(signatureID);
  emitString(path);
}

class STICompile3Flags {
private:
  union FlagsUnion {
    int32_t raw;
    struct {
      uint32_t language : 8;        // Source Language
      uint32_t fEC : 1;             // Edit and Continue
      uint32_t fNoDbgInfo : 1;      // Not compiled with debug
      uint32_t fLTCG : 1;           // Link-time code generation
      uint32_t fNoDataAlign : 1;    // Global data alignment
      uint32_t fManagedPresent : 1; // Managed code/data
      uint32_t fSecurityChecks : 1; // Security Checks (/GS)
      uint32_t fHotPatch : 1;       // Hotpatch Support (/hotpatch)
      uint32_t fCVTCIL : 1;         // CVTCIL
      uint32_t fMSILModule : 1;     // MSIL
      uint32_t padding : 15;        // reserved bits
    } field;
  };
  typedef union FlagsUnion Flags;

  Flags _flags;

public:
  STICompile3Flags() {
    _flags.raw = 0;
    _flags.field.language = STI_C_PLUS_PLUS;
  }

  operator int32_t() const { return _flags.raw; }
};

void STIDebugImpl::emitSymbolCompileUnit(
    const STISymbolCompileUnit *compileUnit) const {
  STISymbolID symbolID = S_COMPILE3;
  STICompile3Flags flags;
  STIMachineID machine = compileUnit->getMachineID();
  int verFEMajor;
  int verFEMinor;
  int verFEBuild;
  int verFEQFE;
  int verMajor;
  int verMinor;
  int verBuild;
  int verQFE;
  StringRef producer;

  verFEMajor = 0x0001;
  verFEMinor = 0x0002;
  verFEBuild = 0x0003;
  verFEQFE = 0x0004;
  verMajor = 0x0005;
  verMinor = 0x0006;
  verBuild = 0x0007;
  verQFE = 0x0008;

  producer = compileUnit->getProducer();

  emitInt16(25 + producer.size()); // record length
  emitSymbolID(symbolID);
  emitInt32(flags);
  emitComment(toString(machine));
  emitInt16(machine);
  emitInt16(verFEMajor);
  emitInt16(verFEMinor);
  emitInt16(verFEBuild);
  emitInt16(verFEQFE);
  emitInt16(verMajor);
  emitInt16(verMinor);
  emitInt16(verBuild);
  emitInt16(verQFE);
  emitString(producer);
}

class STIProcedureFlags {
public:
  operator int() { return 0; } // FIXME
};

void
STIDebugImpl::emitSymbolProcedure(const STISymbolProcedure *procedure) const {
  int length;
  STISymbolID symbolID = procedure->getSymbolID();
  int pParent = 0;
  int pEnd = 0;
  int pNext = 0;
  const MCSymbol *labelBegin = procedure->getLabelBegin();
  const MCSymbol *labelEnd = procedure->getLabelEnd();
  const MCSymbol *labelPrologEnd = procedure->getLabelPrologEnd();
  int debugEnd = 0;
  int procType = procedure->getType()->getIndex();
  STIProcedureFlags flags;
  StringRef name = procedure->getName();

  // Is not this equal to: labelBegin?
  const STILineSlice *slice = procedure->getLineSlice();
  Function *function = slice->getFunction(); // FIXME
  MCSymbol *functionLabel = ASM()->getSymbol(function);

  length = 37 + name.size() + 1;

  emitInt16(length); // record length
  emitSymbolID(symbolID);
  emitInt32(pParent);
  emitInt32(pEnd);
  emitInt32(pNext);
  emitLabelDiff(labelBegin, labelEnd);
  emitLabelDiff(labelBegin, labelPrologEnd);
  emitInt32(debugEnd);
  emitInt32(procType);
  emitSecRel32(functionLabel);
  emitSectionIndex(functionLabel);
  emitInt8(flags);
  emitString(name);
}

void STIDebugImpl::emitSymbolProcedureEnd() const {
  emitInt16(2);
  emitSymbolID(S_PROC_ID_END);
}

class STIFrameProcFlags {
public:
  operator int() { return 0x14000; } // FIXME
};

void STIDebugImpl::emitSymbolFrameProc(const STISymbolFrameProc *frame) const {
  int length = 28;
  STISymbolID symbolID = S_FRAMEPROC;
  STISymbolProcedure *procedure = frame->getProcedure();

  STIFrameProcFlags flags;

  // Is not this equal to: labelBegin?
  const STILineSlice *slice = procedure->getLineSlice();
  Function *function = slice->getFunction(); // FIXME
  MCSymbol *functionLabel = ASM()->getSymbol(function);

  emitInt16(length); // record length
  emitSymbolID(symbolID);
  emitInt32(0);                    // cbFrame
  emitInt32(0);                    // cbPad
  emitInt32(0);                    // offPad
  emitInt32(0);                    // cbSaveRegs
  emitSecRel32(functionLabel);     // offExHdlr
  emitSectionIndex(functionLabel); // sectExHdlr
  emitInt32(flags);                // flags
}

void STIDebugImpl::emitSymbolBlock(const STISymbolBlock *block) const {
  int length;
  STISymbolID symbolID;
  int pParent = 0;
  int pEnd = 0;
  MCSymbol *labelBegin = block->getLabelBegin();
  MCSymbol *labelEnd = block->getLabelEnd();
  StringRef name = block->getName();
  STISymbolProcedure *procedure = block->getProcedure();

  const STILineSlice *slice = procedure->getLineSlice();
  Function *function = slice->getFunction();
  MCSymbol *functionLabel = ASM()->getSymbol(function);
  // MCSymbol*           functionLabel   = procedure->getLabelBegin();

  symbolID = S_BLOCK32; // FIXME
  length = 20 + name.size() + 1;

  emitInt16(length); // record length
  emitSymbolID(symbolID);
  emitInt32(pParent);
  emitInt32(pEnd);
  emitLabelDiff(labelBegin, labelEnd);
  emitSecRel32(labelBegin);
  emitSectionIndex(functionLabel);
  emitString(name);
}

void STIDebugImpl::emitSymbolScopeEnd() const {
  emitInt16(2);
  emitSymbolID(S_END);
}

//===----------------------------------------------------------------------===//
// numericLength(numeric)
//
// Returns the encoded length, in bytes, of the specified numeric leaf.
//
// NOTE: The minimum encoded size of the leaf must be two bytes long.
//
//===----------------------------------------------------------------------===//

size_t STIDebugImpl::numericLength(const STINumeric* numeric) const {
  return std::max<size_t>(
          (numeric->getLeafID() != LF_INTEL_NONE ? 2 : 0) + numeric->getSize(),
          2);
}

//===----------------------------------------------------------------------===//
// emitNumeric(numeric)
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitNumeric(const STINumeric* numeric) const {
  const STINumeric::LeafID leafID = numeric->getLeafID();

  // Emit the leafID if this numeric encoding requires one.  Unsigned values
  // less than LF_NUMERIC (0x8000) do not require one.
  //
  if (leafID != LF_INTEL_NONE) {
    emitInt16(leafID);
  }

  // Emit the numeric value.
  //
  emitBytes(numeric->getData(), numeric->getSize());

  // The minimal field width of a numeric leaf is two bytes.  If the numeric
  // doesn't require a leaf identifier and only requires one byte then we need
  // to pad the value with a zero byte.
  //
  if (leafID == LF_INTEL_NONE && numeric->getSize() == 1) {
    emitInt8(0x00);
  }
}

//===----------------------------------------------------------------------===//
// emitSymbolConstant(symbol)
//
// Emits an entry for a constant symbol.
//
// For example, this source ...
//   +---------------------------------------------------------------------+
//   | const int N = 100;                                                  |
//   +---------------------------------------------------------------------+
//
// ... should create the following debug information symbol:
//   +---------------------------------------------------------------------+
//   | (0001A8) S_CONSTANT: Type:             0x10BC, Value: 100, N        |
//   +---------------------------------------------------------------------+
//
// The format of the S_CONSTANT symbol record is:
//   +----+----+--------+- - - - - - - -+- - - - - - -+
//   |2   |2   |4       |*              |*            |
//   +----+----+--------+- - - - - - - -+- - - - - - -+
//    ^    ^    ^        ^               ^
//    |    |    |        |               `-- name
//    |    |    |        `-- value
//    |    |    `-- typeIndex
//    |    `-- symbolID (S_CONSTANT or S_MANCONSTANT)
//    `-- length
//
// NOTE: The minimum size of the value field is two bytes.
//
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitSymbolConstant(const STISymbolConstant *symbol) const {
  const STISymbolID symbolID    = S_CONSTANT; // S_MANCONSTANT not implemented
  int               length;
  StringRef         name        = symbol->getName();
  const STIType*    type        = symbol->getType();
  const STINumeric* value       = symbol->getValue();
  uint32_t          typeIndex   = type->getIndex();
  
  // Calculate the length in bytes of the symbolic constant, not including the
  // length field itself.
  //
  length = 2 + 4 + numericLength(value) + name.size() + 1;

  // Emit each field in the symbolic constant entry.
  //
  emitInt16     (length);
  emitSymbolID  (symbolID);
  emitInt32     (typeIndex);
  emitNumeric   (value);
  emitString    (name);
}

void STIDebugImpl::emitSymbolVariable(const STISymbolVariable *variable) const {
  if (variable->getLocation() == nullptr) {
    //assert(!"Variable with no location");
    return;
  }
  STISymbolID symbolID = variable->getLocation()->getSymbolID();
  uint32_t type = variable->getType()->getIndex();
  int reg = variable->getLocation()->getReg();
  int offset = variable->getLocation()->getOffset();
  MCSymbol *label = variable->getLocation()->getLabel();
  StringRef name = variable->getName();
  int length;

  switch (symbolID) {
  case S_REGREL32:
    length = 12 + name.size() + 1;
    emitInt16(length);
    emitSymbolID(symbolID);
    emitInt32(offset);
    emitInt32(type);
    emitInt16(reg);
    emitString(name);
    break;

  case S_REGISTER:
    length = 8 + name.size() + 1;
    emitInt16(length);
    emitSymbolID(symbolID);
    emitInt32(type);
    emitInt16(reg);
    emitString(name);
    break;

  case S_BPREL32:
    length = 10 + name.size() + 1;
    emitInt16(length);
    emitSymbolID(symbolID);
    emitInt32(offset);
    emitInt32(type);
    emitString(name);
    break;

  case S_LDATA32:
  case S_GDATA32:
    length = 12 + name.size() + 1;
    emitInt16(length);
    emitSymbolID(symbolID);
    emitInt32(type);
    emitSecRel32(label);
    emitSectionIndex(label);
    emitString(name);
    break;

  default:
    assert(symbolID != symbolID); // invalid variable symbol id
    break;
  }
}

//===----------------------------------------------------------------------===//
// emitLineEntry(entry)
//
//   +--------+--------+
//   |4       |4       |
//   +--------+--------+
//    ^        ^
//    |        `-- CV_Line
//    `-- offset
//
//
// The bit encoding for the CV_Line entry is:
//
//   32 31             24
//   +--+--------------+--------------------------------------------+
//   |  |              |                                            |
//   +--+--------------+--------------------------------------------+
//    ^  ^              ^
//    |  |              `-- linenumStart
//    |  `-- deltaLineEnd
//    `-- fStatement
//
//===----------------------------------------------------------------------===//

class STILineEntryEncoding {
private:
  union {
    int32_t raw;
    struct {
      uint32_t lineNumStart : 24;
      uint32_t deltaLineEnd : 7;
      uint32_t fStatement : 1;
    } field;
  } _encoding;

public:
  STILineEntryEncoding(const STILineEntry *entry);
  ~STILineEntryEncoding();
  operator int16_t() const;
};

STILineEntryEncoding::STILineEntryEncoding(const STILineEntry *entry) {
  _encoding.raw = 0;
  _encoding.field.lineNumStart = entry->getLineNumStart();
  _encoding.field.deltaLineEnd = entry->getDeltaLineEnd();
  _encoding.field.fStatement = entry->getStatementEnd();
}

STILineEntryEncoding::~STILineEntryEncoding() {}

STILineEntryEncoding::operator int16_t() const { return _encoding.raw; }

void STIDebugImpl::emitLineEntry(const STISymbolProcedure *procedure,
                                 const STILineEntry *entry) const {
  STILineEntryEncoding encodedEntry(entry);

  emitLabelDiff(procedure->getLabelBegin(), entry->getLabel());
  emitInt32(encodedEntry);
}

//===----------------------------------------------------------------------===//
// emitLineBlock(block)
//
//   +--------+--------+--------+- - - - - - - - -
//   |4       |4       |4       |  ... lines
//   +--------+--------+--------+- - - - - - - - -
//    ^        ^        ^
//    |        |        `-- cbFileBlock
//    |        `-- clines
//    `-- offFile
//
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitLineBlock(const STISymbolProcedure *procedure,
                                 const STILineBlock *block) const {
  MCSymbol *labelBegin = MMI()->getContext().CreateTempSymbol();
  MCSymbol *labelEnd = MMI()->getContext().CreateTempSymbol();

  emitLabel(labelBegin);
  emitInt32(block->getChecksumEntry()->getOffset()); // offFile
  emitInt32(block->getLineCount());                  // cLines
  emitLabelDiff(labelBegin, labelEnd);               // cbFileBlock

  for (const STILineEntry *entry : block->getLines()) {
    emitLineEntry(procedure, entry);
  }

  emitLabel(labelEnd);
}

//===----------------------------------------------------------------------===//
// emitLineSlice(slice)
//
// The lines subsection begins with a header identifying the function
// associated with the slice of the line table being emitted:
//
//   +--------+----+----+--------+- - - - - - - -
//   |4       |2   |2   |4       |  ... blocks
//   +--------+----+----+--------+- - - - - - - -
//    ^        ^    ^    ^
//    |        |    |    `-- cbCon
//    |        |    `-- flags
//    |        `-- section
//    `-- secrel
//
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitLineSlice(const STISymbolProcedure *procedure) const {
  const STILineSlice *slice = procedure->getLineSlice();
  Function *function = slice->getFunction();
  MCSymbol *functionLabel = ASM()->getSymbol(function);

  emitSubsection(STI_SUBSECTION_LINES);

  emitSecRel32(functionLabel);
  emitSectionIndex(functionLabel);
  emitInt16(0); // FIXME: flags values?
  emitLabelDiff(procedure->getLabelBegin(), procedure->getLabelEnd());

  for (const STILineBlock *block : slice->getBlocks()) {
    emitLineBlock(procedure, block);
  }
}

void STIDebugImpl::walkSymbol(const STISymbol *symbol) const {
  STIObjectKind kind;

  kind = symbol->getKind();
  switch (kind) {
  case STI_OBJECT_KIND_SYMBOL_MODULE: {
    const STISymbolModule *module;
    module = static_cast<const STISymbolModule *>(symbol);
    emitSubsection(STI_SUBSECTION_SYMBOLS);
    emitSymbolModule(module);
    for (const STISymbolCompileUnit *unit : *module->getCompileUnits()) {
      walkSymbol(unit);
    }
  } break;

  case STI_OBJECT_KIND_SYMBOL_COMPILE_UNIT: {
    const STISymbolCompileUnit *compileUnit;
    compileUnit = static_cast<const STISymbolCompileUnit *>(symbol);
    emitSubsection(STI_SUBSECTION_SYMBOLS);
    emitSymbolCompileUnit(compileUnit);
    for (const auto &object : compileUnit->getScope()->getObjects()) {
      walkSymbol(static_cast<const STISymbol *>(object.second)); // FIXME: cast
    }
  } break;

  case STI_OBJECT_KIND_SYMBOL_PROCEDURE: {
    const STISymbolProcedure *procedure;

    procedure = static_cast<const STISymbolProcedure *>(symbol);
    emitSubsection(STI_SUBSECTION_SYMBOLS);
    emitSymbolProcedure(procedure);
    emitSymbolFrameProc(procedure->getFrame());
    for (const auto &object : procedure->getScope()->getObjects()) {
      walkSymbol(static_cast<const STISymbol *>(object.second));
    }
    emitSymbolProcedureEnd();
    emitLineSlice(procedure);
  } break;

  case STI_OBJECT_KIND_SYMBOL_BLOCK: {
    const STISymbolBlock *block;

    block = static_cast<const STISymbolBlock *>(symbol);
    bool emptyBlock = true;
    for (const auto &object : block->getScope()->getObjects()) {
      if (object.second->getKind() != STI_OBJECT_KIND_SYMBOL_BLOCK) {
        emptyBlock = false;
      }
    }
    emitSubsection(STI_SUBSECTION_SYMBOLS);
    if (!emptyBlock)
      emitSymbolBlock(block);
    for (const auto &object : block->getScope()->getObjects()) {
      walkSymbol(static_cast<const STISymbol *>(object.second));
    }
    if (!emptyBlock)
      emitSymbolScopeEnd();
  } break;

  case STI_OBJECT_KIND_SYMBOL_VARIABLE: {
    const STISymbolVariable *variable;
    variable = static_cast<const STISymbolVariable *>(symbol);
    emitSubsection(STI_SUBSECTION_SYMBOLS);
    emitSymbolVariable(variable);
  } break;

  case STI_OBJECT_KIND_SYMBOL_CONSTANT: {
    const STISymbolConstant *constant;
    constant = static_cast<const STISymbolConstant*>(symbol);
    emitSubsection(STI_SUBSECTION_SYMBOLS);
    emitSymbolConstant(constant);
  } break;

  case STI_OBJECT_KIND_SYMBOL_USER_DEFINED: {
    const STISymbolUserDefined *userDefined;
    userDefined = static_cast<const STISymbolUserDefined *>(symbol);
    emitSubsection(STI_SUBSECTION_SYMBOLS);
    emitSymbolUserDefined(userDefined);
  } break;

  default:
    assert(kind != kind); // unrecognized symbol kind!
    break;
  }
}

void STIDebugImpl::emitSectionBegin(const MCSection *section) const {
  ASM()->OutStreamer.SwitchSection(section);
}

//===----------------------------------------------------------------------===//
// emitSymbols()
//
// Emits the .debug$S section.
//
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitSymbols() const {
  emitSectionBegin(ASM()->getObjFileLowering().getCOFFDebugSymbolsSection());
  emitComment("Symbols Section Signature");
  emitInt32(STI_SECTION_SIGNATURE_CV7);
  walkSymbol(getSymbolTable()->getRoot());
  emitChecksumTable();
  emitStringTable();
  closeSubsection();
  emitAlign(4);
}

void STIDebugImpl::emitTypeBasic(const STITypeBasic *type) const {
  // Primitive Types Are Predefined And Not Emitted
}

class STITypeModifierAttributes {
private:
  union {
    int16_t raw;
    struct {
      uint16_t _const : 1;
      uint16_t _volatile : 1;
      uint16_t _unaligned_ : 1;
      uint16_t _reserved : 13;
    } field;
  } _attributes;

public:
  STITypeModifierAttributes(const STITypeModifier *const type) {
    _attributes.raw = 0;
    _attributes.field._const = type->isConstant();
    _attributes.field._volatile = type->isVolatile();
    _attributes.field._unaligned_ = type->isUnaligned();
  }

  ~STITypeModifierAttributes() {}

  operator int16_t() const { return _attributes.raw; }
};

void STIDebugImpl::emitTypeModifier(const STITypeModifier *type) const {
  STITypeModifierAttributes attributes(type);
  const STIType *qualifiedType = type->getQualifiedType();
  const int16_t length = 8;

  typeBegin (type);
  emitInt16 (length);
  emitInt16 (LF_MODIFIER);
  emitInt32 (qualifiedType->getIndex());
  emitInt16 (attributes);
  typeEnd   (type);
}

class STITypePointerAttributes {
private:
  union {
    int32_t raw;
    struct {
      uint32_t _ptrtype : 5;
      uint32_t _ptrmode : 3;
      uint32_t _isflat32 : 1;
      uint32_t _volatile : 1;
      uint32_t _const : 1;
      uint32_t _unaligned_ : 1;
      uint32_t _restrict_ : 1;
      uint32_t _reserved1 : 3;
      uint32_t _unknownField : 1;
      uint32_t _reserved2 : 15;
    } field;
  } _attributes;

public:
  STITypePointerAttributes(const STITypePointer *type) {
    _attributes.raw = 0;
    if (type->getSizeInBits() == 64) {
      _attributes.field._ptrtype = ATTR_PTRTYPE_64;
      _attributes.field._unknownField = 1; // Necessary to get "Size: 8"
    } else {
      _attributes.field._ptrtype = ATTR_PTRTYPE_NEAR32;
    }

    if (type->isReference()) {
      _attributes.raw = _attributes.raw | ATTR_PTRMODE_REFERENCE;
    }

    switch (type->getPtrToMemberType()) {
    case STITypePointer::PTM_NONE:
      break;
    case STITypePointer::PTM_DATA:
      _attributes.raw = _attributes.raw | ATTR_PTRMODE_DATAMB;
      break;
    case STITypePointer::PTM_METHOD:
      _attributes.raw = _attributes.raw | ATTR_PTRMODE_METHOD;
      break;
    }

    if (type->isConstant()) {
      _attributes.field._const = true;
    }
  }

  ~STITypePointerAttributes() {}

  operator int32_t() const { return _attributes.raw; }
};

void STIDebugImpl::emitTypePointer(const STITypePointer *type) const {
  STITypePointerAttributes attributes(type);
  const STIType *pointerTo = type->getPointerTo();
  const STIType *classType = type->getContainingClass();
  const int16_t length = 10 + (classType ? 6 : 0);
  int format = 0;

  switch (type->getPtrToMemberType()) {
  case STITypePointer::PTM_NONE:
    break;
  case STITypePointer::PTM_DATA:
    format = FORMAT_16_DATA_NO_VMETHOD_NO_VBASE;
    break;
  case STITypePointer::PTM_METHOD:
    format = FORMAT_16_NEAR_METHOD_NO_VBASE_SADDR;
    break;
  }

  typeBegin (type);
  emitInt16 (length);
  emitInt16 (LF_POINTER);
  emitInt32 (pointerTo->getIndex());
  emitInt32 (attributes);
  // emit*    (variant);  // emitted based on pointer type
  if (classType) {
    emitInt32(classType->getIndex());
    emitInt16(format);
  }
  typeEnd   (type);
}

void STIDebugImpl::emitTypeArray(const STITypeArray *type) const {
  const STIType *elementType = type->getElementType();
  StringRef name = type->getName();
  const STINumeric* arrayLength = type->getLength();
  const int16_t length = 10 + numericLength(arrayLength) + name.size() + 1;

  typeBegin     (type);
  emitInt16     (length);
  emitInt16     (LF_ARRAY);
  emitInt32     (elementType->getIndex());
  emitInt32     (T_ULONG);
  emitNumeric   (arrayLength);
  emitString    (name);
  typeEnd       (type);
}

void STIDebugImpl::emitTypeStructure(const STITypeStructure *type) const {
  const uint16_t leaf = type->getLeaf();
  bool isUnion = (leaf == LF_UNION);
  const uint16_t count = type->getCount();
  const uint16_t prop = type->getProperty();
  const STIType *fieldType = type->getFieldType();
  const STIType *derivedType = type->getDerivedType();
  const STIType *vshapeType = type->getVShapeType();
  const STINumeric *size = type->getSize();
  std::string name = type->getName();

  assert(!name.empty() && "empty stucture name!");

  std::string realName = getRealName(name);

  const int16_t length = (isUnion ? 10 : 18) + numericLength(size) +
                         name.size() + 1 + realName.size() + 1;

  typeBegin     (type);
  emitInt16     (length);
  emitInt16     (leaf);
  emitInt16     (count);
  emitInt16     (prop | PROP_REALNAME);
  emitInt32     (fieldType ? fieldType->getIndex() : 0);
  if (!isUnion) {
    emitInt32   (derivedType ? derivedType->getIndex() : 0);
    emitInt32   (vshapeType ? vshapeType->getIndex() : 0);
  }
  emitNumeric   (size);
  emitString    (name);
  emitString    (realName);
  typeEnd       (type);
}

void STIDebugImpl::emitTypeEnumeration(const STITypeEnumeration *type) const {
  const uint16_t count = type->getCount();
  const uint16_t prop = type->getProperty();
  const STIType *elementType = type->getElementType();
  const STIType *fieldType = type->getFieldType();
  StringRef name = type->getName();
  const int16_t length = 14 + name.size() + 1;

  typeBegin     (type);
  emitInt16     (length);
  emitInt16     (LF_ENUM);
  emitInt16     (count);
  emitInt16     (prop);
  emitInt32     (elementType ? elementType->getIndex() : 0);
  emitInt32     (fieldType ? fieldType->getIndex() : 0);
  emitString    (name);
  typeEnd       (type);
}

void STIDebugImpl::emitTypeVShape(const STITypeVShape *type) const {
  const uint16_t count = type->getCount();
  const int16_t length = 4 + 4 * count;

  typeBegin     (type);
  emitInt16     (length);
  emitInt16     (LF_VTSHAPE);
  emitInt16     (count);
  for (unsigned i = 0; i < count; ++i) {
    emitInt32   (CV_VFTS_NEAR32);
  }
  typeEnd       (type);
}

void STIDebugImpl::emitTypeBitfield(const STITypeBitfield *type) const {
  const uint32_t offset = type->getOffset();
  const uint32_t size = type->getSize();
  const STIType *memberType = type->getType();
  const int16_t length = 10;

  typeBegin     (type);
  emitInt16     (length);
  emitInt16     (LF_BITFIELD);
  emitInt32     (memberType ? memberType->getIndex() : 0);
  emitInt8      (size);
  emitInt8      (offset);
  emitPadding   (2);
  typeEnd       (type);
}

void STIDebugImpl::emitTypeMethodList(const STITypeMethodList *type) const {
  uint16_t length = 2;

  for (STITypeMethodListEntry *method : type->getList()) {
    bool isVirtual = method->getVirtuality();
    length += 8 + (isVirtual ? 4 : 0);
  }

  typeBegin     (type);
  emitInt16     (length);
  emitInt16     (LF_MLIST);

  for (STITypeMethodListEntry *method : type->getList()) {
    uint16_t attribute = method->getAttribute();
    const STIType *methodType = method->getType();
    bool isVirtual = method->getVirtuality();
    uint32_t virualIndex = method->getVirtualIndex();

    emitInt16   (attribute);
    emitInt16   (0); // 0-Padding
    emitInt32   (methodType ? methodType->getIndex() : 0);
    if (isVirtual)
      emitInt32 (virualIndex);
  }
  typeEnd       (type);
}

void STIDebugImpl::emitTypeFieldList(const STITypeFieldList *type) const {
  uint16_t length = 2;

  const STITypeVFuncTab *vFuncTab = type->getVFuncTab();

  for (STITypeBaseClass *baseClass : type->getBaseClasses()) {
    const STINumeric *offset = baseClass->getOffset();
    int16_t baseClassLength = 8 + numericLength(offset);
    length += getPaddedSize(baseClassLength);
  }

  for (STITypeVBaseClass *vBaseClass : type->getVBaseClasses()) {
    const STINumeric *offset = vBaseClass->getVbpOffset();
    const STINumeric *index  = vBaseClass->getVbIndex();
    int16_t vBaseClassLength =
        12 + numericLength(offset) + numericLength(index);
    length += getPaddedSize(vBaseClassLength);
  }

  if (vFuncTab) {
    length += 8;
  }

  for (STITypeMember *member : type->getMembers()) {
    const STINumeric *offset = member->getOffset();
    StringRef name = member->getName();
    bool isStatic = member->isStatic();
    int16_t memberLength =
        8 + (isStatic ? 0 : numericLength(offset)) + name.size() + 1;
    length += getPaddedSize(memberLength);
  }

  for (STITypeMethod *method : type->getMethods()) {
    StringRef name = method->getName();
    int16_t methodLength = 8 + name.size() + 1;
    length += getPaddedSize(methodLength);
  }

  for (STITypeOneMethod *method : type->getOneMethods()) {
    StringRef name = method->getName();
    bool isVirtual = method->getVirtuality();
    int16_t methodLength = 8 + (isVirtual ? 4 : 0) + name.size() + 1;
    length += getPaddedSize(methodLength);
  }

  for (STITypeEnumerator *enumerator : type->getEnumerators()) {
    const STINumeric *value = enumerator->getValue();
    StringRef name = enumerator->getName();
    int16_t enumeratorLength = 4 + numericLength(value) + name.size() + 1;
    length += getPaddedSize(enumeratorLength);
  }

  typeBegin(type);
  emitInt16     (length);
  emitInt16     (LF_FIELDLIST);

  for (STITypeBaseClass *baseClass : type->getBaseClasses()) {
    uint16_t attribute = baseClass->getAttribute();
    const STIType *baseClassType = baseClass->getType();
    const STINumeric *offset = baseClass->getOffset();
    int16_t baseClassLength = 8 + numericLength(offset);
    int16_t paddedSize = getPaddedSize(baseClassLength);

    emitInt16   (LF_BCLASS);
    emitInt16   (attribute);
    emitInt32   (baseClassType ? baseClassType->getIndex() : 0);
    emitNumeric (offset);
    emitPadding (paddedSize - baseClassLength);
  }

  for (STITypeVBaseClass *vBaseClass : type->getVBaseClasses()) {
    STISymbolID symbolID = vBaseClass->getSymbolID();
    uint16_t attribute = vBaseClass->getAttribute();
    const STIType *vBaseClassType = vBaseClass->getType();
    const STIType *vbpType = vBaseClass->getVbpType();
    const STINumeric *offset = vBaseClass->getVbpOffset();
    const STINumeric *index  = vBaseClass->getVbIndex();
    int16_t vBaseClassLength =
        12 + numericLength(offset) + numericLength(index);
    int16_t paddedSize = getPaddedSize(vBaseClassLength);

    emitInt16   (symbolID);
    emitInt16   (attribute);
    emitInt32   (vBaseClassType ? vBaseClassType->getIndex() : 0);
    emitInt32   (vbpType ? vbpType->getIndex() : 0);
    emitNumeric (offset);
    emitNumeric (index);
    emitPadding (paddedSize - vBaseClassLength);
  }

  if (vFuncTab) {
    const STIType *vptrType = vFuncTab->getType();

    emitInt16   (LF_VFUNCTAB);
    emitInt16   (0); // 0-Padding
    emitInt32   (vptrType ? vptrType->getIndex() : 0);
  }

  for (STITypeMember *member : type->getMembers()) {
    uint16_t attribute = member->getAttribute();
    const STIType *memberType = member->getType();
    const STINumeric *offset = member->getOffset();
    StringRef name = member->getName();
    bool isStatic = member->isStatic();
    int16_t memberLength =
        8 + (isStatic ? 0 : numericLength(offset)) + name.size() + 1;
    int16_t paddedSize = getPaddedSize(memberLength);

    emitInt16   (isStatic ? LF_STMEMBER : LF_MEMBER);
    emitInt16   (attribute);
    emitInt32   (memberType ? memberType->getIndex() : 0);
    if (!isStatic) {
      emitNumeric(offset);
    }
    emitString  (name);
    emitPadding (paddedSize - memberLength);
  }

  for (STITypeMethod *method : type->getMethods()) {
    uint16_t count = method->getCount();
    const STIType *methodListType = method->getList();
    StringRef name = method->getName();
    int16_t methodLength = 8 + name.size() + 1;
    int16_t paddedSize = getPaddedSize(methodLength);

    emitInt16   (LF_METHOD);
    emitInt16   (count);
    emitInt32   (methodListType ? methodListType->getIndex() : 0);
    emitString  (name);
    emitPadding (paddedSize - methodLength);
  }

  for (STITypeOneMethod *method : type->getOneMethods()) {
    uint16_t attribute = method->getAttribute();
    const STIType *methodType = method->getType();
    bool isVirtual = method->getVirtuality();
    uint32_t virualIndex = method->getVirtualIndex();
    StringRef name = method->getName();
    int16_t methodLength = 8 + (isVirtual ? 4 : 0) + name.size() + 1;
    int16_t paddedSize = getPaddedSize(methodLength);

    emitInt16   (LF_ONEMETHOD);
    emitInt16   (attribute);
    emitInt32   (methodType ? methodType->getIndex() : 0);
    if (isVirtual)
      emitInt32 (virualIndex);
    emitString  (name);
    emitPadding (paddedSize - methodLength);
  }

  for (STITypeEnumerator *enumerator : type->getEnumerators()) {
    uint16_t attribute = enumerator->getAttribute();
    const STINumeric *value = enumerator->getValue();
    StringRef name = enumerator->getName();
    int16_t enumeratorLength = 4 + numericLength(value) + name.size() + 1;
    int16_t paddedSize = getPaddedSize(enumeratorLength);

    emitInt16   (LF_ENUMERATE);
    emitInt16   (attribute);
    emitNumeric(value);
    emitString  (name);
    emitPadding (paddedSize - enumeratorLength);
  }
  typeEnd(type);
}

void STIDebugImpl::emitTypeFunctionID(const STITypeFunctionID *type) const {
  StringRef name = type->getName();
  const STIType *funcType = type->getType();
  const STIType *parentClassType = type->getParentClassType();
  const STIType *parentScope =
      parentClassType ? parentClassType : type->getParentScope();
  STISymbolID symbolID = parentClassType ? LF_MFUNC_ID : LF_FUNC_ID;
  uint16_t length = 10 + name.size() + 1;
  uint16_t paddedLength = getPaddedSize(length);

  typeBegin     (type);
  emitInt16     (paddedLength);
  emitInt16     (symbolID);
  emitInt32     (parentScope ? parentScope->getIndex() : 0);
  emitInt32     (funcType ? funcType->getIndex() : 0);
  emitString    (name);
  emitPadding   (paddedLength - length);
  typeEnd       (type);
}

void STIDebugImpl::emitTypeProcedure(const STITypeProcedure *type) const {
  const STIType *returnType = type->getReturnType();
  const STIType *classType = type->getClassType();
  const STIType *thisType = type->getThisType();
  int callingConvention = type->getCallingConvention();
  uint16_t paramCount = type->getParamCount();
  const STIType *argumentList = type->getArgumentList();
  int thisAdjust = type->getThisAdjust();
  STISymbolID symbolID = classType ? LF_MFUNCTION : LF_PROCEDURE;
  uint16_t length = classType ? 26 : 14;

  typeBegin     (type);
  emitInt16     (length);
  emitInt16     (symbolID);
  emitInt32     (returnType ? returnType->getIndex() : 0);
  if (classType) {
    emitInt32   (classType ? classType->getIndex() : 0);
    emitInt32   (thisType ? thisType->getIndex() : 0);
  }
  emitInt8      (callingConvention);
  emitInt8      (0); // reserved
  emitInt16     (paramCount);
  emitInt32     (argumentList ? argumentList->getIndex() : 0);
  if (classType) {
    emitInt32   (thisAdjust);
  }
  typeEnd       (type);
}

void STIDebugImpl::emitTypeArgumentList(const STITypeArgumentList *type) const {
  uint32_t argumentCount = type->getArgumentCount();
  const STITypeTable *argumentList = type->getArgumentList();
  uint16_t length = 6 + 4 * argumentCount;

  typeBegin     (type);
  emitInt16     (length);
  emitInt16     (LF_ARGLIST);
  emitInt32     (argumentCount);
  for (const STIType *argemntType : *argumentList) {
    emitInt32   (argemntType ? argemntType->getIndex() : 0);
  }
  typeEnd       (type);
}

void STIDebugImpl::emitTypeServer(const STITypeServer *type) const {
  const size_t MAX_BUFF_LENGTH = 32;
  unsigned char signature[MAX_BUFF_LENGTH];
  unsigned char age[MAX_BUFF_LENGTH];
  StringRef name = type->getPDBFullName();
  size_t signatureLen = pdb_get_signature(signature, MAX_BUFF_LENGTH);
  size_t ageLen = pdb_get_age(age, MAX_BUFF_LENGTH);
  uint16_t length = 2 + signatureLen + ageLen + name.size() + 1;

  emitInt16 (length);
  emitInt16 (LF_TYPESERVER2);
  for (size_t i=0; i < signatureLen; ++i) {
    emitInt8(signature[i]);
  }
  for (size_t i=0; i < ageLen; ++i) {
    emitInt8(age[i]);
  }
  emitString(name);
}

void STIDebugImpl::emitType(const STIType *type) const {
  STIObjectKind kind;

  if (type->getIndex() < 0x1000) {
    // TODO: add a comment!
    return;
  }

  kind = type->getKind();
  switch (kind) {
#define X(KIND, HANDLER, TYPE)                                                 \
  case STI_OBJECT_KIND_TYPE_##KIND:                                            \
    HANDLER(static_cast<const TYPE *>(type));                                  \
    break
    X(BASIC,            emitTypeBasic,          STITypeBasic);
    X(MODIFIER,         emitTypeModifier,       STITypeModifier);
    X(POINTER,          emitTypePointer,        STITypePointer);
    X(ARRAY,            emitTypeArray,          STITypeArray);
    X(STRUCTURE,        emitTypeStructure,      STITypeStructure);
    X(ENUMERATION,      emitTypeEnumeration,    STITypeEnumeration);
    X(VSHAPE,           emitTypeVShape,         STITypeVShape);
    X(BITFIELD,         emitTypeBitfield,       STITypeBitfield);
    X(METHOD_LIST,      emitTypeMethodList,     STITypeMethodList);
    X(FIELD_LIST,       emitTypeFieldList,      STITypeFieldList);
    X(FUNCTION_ID,      emitTypeFunctionID,     STITypeFunctionID);
    X(PROCEDURE,        emitTypeProcedure,      STITypeProcedure);
    X(ARGUMENT_LIST,    emitTypeArgumentList,   STITypeArgumentList);
    X(SERVER,           emitTypeServer,         STITypeServer);
#undef X
  default:
    assert(kind != kind); // invalid type kind
    break;
  }
}

//===----------------------------------------------------------------------===//
// emitTypesSignature()
//
// Emits the type signature at the beginning of the .debug$T section which
// identifies the version number of the types information.
//
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitTypesSignature() const {
  emitComment("Types Section Signature");
  emitInt32(STI_SIGNATURE_LATEST);
}

//===----------------------------------------------------------------------===//
// emitTypesPDBTypeServer()
//
// When emitting type information to a PDB, this routine emits a LF_TYPESERVER
// record into the object file.
//
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitTypesPDBTypeServer() const {
  STITypeServer* typeServer;

  // The LF_TYPESERVER entry is only emitted if the type information is emitted
  // to a PDB.
  if (!usePDB()) {
    return;
  }

  typeServer = STITypeServer::create();
  typeServer->setPDBFullName(getPDBFullPath());
  emitTypeServer(typeServer);
  delete typeServer;
}

//===----------------------------------------------------------------------===//
// emitTypesPDBBegin(savedWriter)
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitTypesPDBBegin(STIWriter** savedWriter) const {
  STIWriter* pdbWriter;

  if (!usePDB()) {
    return;
  }

  pdbWriter = STIPdbWriter::create();
  *savedWriter = writer();
  const_cast<STIDebugImpl*>(this)->setWriter(pdbWriter);
}

//===----------------------------------------------------------------------===//
// emitTypesPDBEnd(savedWriter)
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitTypesPDBEnd(STIWriter** savedWriter) const {
  STIWriter* pdbWriter;

  if (!usePDB()) {
    return;
  }

  pdbWriter = const_cast<STIDebugImpl*>(this)->writer();
  const_cast<STIDebugImpl*>(this)->setWriter(*savedWriter);
  delete pdbWriter;
}

//===----------------------------------------------------------------------===//
// emitTypesTable()
//
// Emits all of the types from the types table, in order.
//
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitTypesTable() const {
  for (const STIType *type : *getTypeTable()) {
    emitType(type);
  }
}

//===----------------------------------------------------------------------===//
// emitTypes()
//
// Emits the .debug$T section.
//
//===----------------------------------------------------------------------===//

void STIDebugImpl::emitTypes() const {
  STIWriter* savedWriter = nullptr;

  emitSectionBegin(ASM()->getObjFileLowering().getCOFFDebugTypesSection());
  emitTypesSignature();
  emitTypesPDBTypeServer();
  emitTypesPDBBegin(&savedWriter);
  emitTypesTable();
  emitTypesPDBEnd(&savedWriter);
}

void STIDebugImpl::emitSymbolUserDefined(
    const STISymbolUserDefined *userDefined) const {
  const STIType *definedType = userDefined->getDefinedType();
  StringRef name = userDefined->getName();
  const int16_t length = 6 + name.size() + 1;

  emitInt16(length);
  emitInt16(S_UDT);
  emitInt32(definedType->getIndex());
  emitString(name);
}

void STIDebugImpl::emitStringEntry(const STIStringEntry *entry) const {
  emitString(entry->getString());
}

void STIDebugImpl::emitStringTable() const {
  emitSubsection(STI_SUBSECTION_STRINGTABLE);

  for (const STIStringEntry *entry : getStringTable()->getEntries()) {
    emitStringEntry(entry);
  }
}

void STIDebugImpl::emitChecksumTable() const {
  emitSubsection(STI_SUBSECTION_FILECHKSMS);

  for (const STIChecksumEntry *entry : getChecksumTable()->getEntries()) {
    emitChecksumEntry(entry);
  }
}

size_t STIDebugImpl::getPaddingSize(const STIChecksumEntry *entry) const {
  return 4 - ((6 + entry->getChecksumSize()) % 4);
}

void STIDebugImpl::emitChecksumEntry(const STIChecksumEntry *entry) const {
  emitInt32(entry->getStringEntry()->getOffset());
  emitInt8(entry->getChecksumSize());
  emitInt8(entry->getType());
  emitBytes(entry->getChecksum(), entry->getChecksumSize());
  emitFill(getPaddingSize(entry), 0x00);
}

//===----------------------------------------------------------------------===//
// STIDebug Routines
//===----------------------------------------------------------------------===//

STIDebug::STIDebug() {}

STIDebug::~STIDebug() {}

STIDebug *STIDebug::create(AsmPrinter *Asm) { return new STIDebugImpl(Asm); }
