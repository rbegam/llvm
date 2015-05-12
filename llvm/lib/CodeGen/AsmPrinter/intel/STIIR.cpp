//===-- STIIR.cpp - Symbol And Type Info -------*- C++ -*--===//
//
//===----------------------------------------------------------------------===//
//
// This file contains data structures for symbol and type information entries.
//
//===----------------------------------------------------------------------===//

#include "STIIR.h"

using namespace llvm;

//===----------------------------------------------------------------------===//
// STIObject
//===----------------------------------------------------------------------===//

uint32_t STIObject::_countUniqueID = 0;

STIObject::STIObject(STIObjectKind kind)
    : _kind(kind), _uniqueID(++_countUniqueID) {}

STIObject::~STIObject() {}

STIObjectKind STIObject::getKind() const { return _kind; }

//===----------------------------------------------------------------------===//
// STILocation
//===----------------------------------------------------------------------===//

STILocation::STILocation(STISymbolID symbolID, STIRegID regnum, int offset,
                         MCSymbol *label)
    : STIObject(STI_OBJECT_KIND_LOCATION), _symbolID(symbolID), _regnum(regnum),
      _offset(offset), _label(label) {}

STILocation::~STILocation() {}

STILocation *STILocation::createRegisterOffset(STIRegID regnum, int offset) {
  return new STILocation(S_REGREL32, regnum, offset, nullptr);
}

STILocation *STILocation::createRegister(STIRegID regnum) {
  return new STILocation(S_REGISTER, regnum, 0, nullptr);
}

STILocation *STILocation::createOffset(int offset) {
  return new STILocation(S_BPREL32, STI_REGISTER_NONE, offset, nullptr);
}

STILocation *STILocation::createGlobalSegmentedOffset(MCSymbol *label) {
  return new STILocation(S_GDATA32, STI_REGISTER_NONE, 0, label);
}

STILocation *STILocation::createLocalSegmentedOffset(MCSymbol *label) {
  return new STILocation(S_LDATA32, STI_REGISTER_NONE, 0, label);
}

STISymbolID STILocation::getSymbolID() const { return _symbolID; }

void STILocation::setSymbolID(STISymbolID symbolID) { _symbolID = symbolID; }

STIRegID STILocation::getReg() const { return _regnum; }

void STILocation::setReg(STIRegID regnum) { _regnum = regnum; }

int STILocation::getOffset() const { return _offset; }

void STILocation::setOffset(int offset) { _offset = offset; }

MCSymbol *STILocation::getLabel() const { return _label; }

void STILocation::setLabel(MCSymbol *label) { _label = label; }

//===----------------------------------------------------------------------===//
// STIStringEntry
//===----------------------------------------------------------------------===//

STIStringEntry::STIStringEntry() : _string(), _offset(0) {}

STIStringEntry::~STIStringEntry() {}

STIStringEntry *STIStringEntry::create() { return new STIStringEntry(); }

StringRef STIStringEntry::getString() const { return _string; }

void STIStringEntry::setString(StringRef string) { _string = string; }

STIStringEntry::Offset STIStringEntry::getOffset() const { return _offset; }

void STIStringEntry::setOffset(Offset offset) { _offset = offset; }

//===----------------------------------------------------------------------===//
// STIStringTable
//===----------------------------------------------------------------------===//

STIStringTable::STIStringTable() : _entries() {
  // The first entry is always the empty string.
  append("");
}

STIStringTable::~STIStringTable() {
  for (STIStringEntry *entry : getEntries()) {
    delete entry;
  }
}

STIStringEntry *STIStringTable::lookup(StringRef string) {
  STIStringEntry *match = nullptr;

  // Inefficient, but the set of strings recorded here is small.
  for (STIStringEntry *entry : getEntries()) {
    if (entry->getString() == string) {
      match = entry;
    }
  }

  return match;
}

STIStringEntry *STIStringTable::append(StringRef string) {
  STIStringEntry *entry;

  entry = STIStringEntry::create();
  entry->setString(string);
  _entries.push_back(entry);

  return entry;
}

STIStringTable::EntryList &STIStringTable::getEntries() { return _entries; }

const STIStringTable::EntryList &STIStringTable::getEntries() const {
  return _entries;
}

STIStringEntry *STIStringTable::find(StringRef string) {
  STIStringEntry *entry;

  entry = lookup(string);
  if (entry == nullptr) {
    entry = append(string);
  }

  return entry;
}

//===----------------------------------------------------------------------===//
// STIChecksumEntry
//===----------------------------------------------------------------------===//

STIChecksumEntry::STIChecksumEntry()
    : _stringEntry(nullptr), _offset(0),
      _type(STI_FILECHECKSUM_ENTRY_TYPE_NONE), _checksum(nullptr) {}

STIChecksumEntry::~STIChecksumEntry() {}

STIChecksumEntry *STIChecksumEntry::create() { return new STIChecksumEntry(); }

StringRef STIChecksumEntry::getFilename() const {
  return _stringEntry->getString();
}

STIStringEntry *STIChecksumEntry::getStringEntry() const {
  return _stringEntry;
}

void STIChecksumEntry::setStringEntry(STIStringEntry *stringEntry) {
  _stringEntry = stringEntry;
}

STIChecksumEntry::Offset STIChecksumEntry::getOffset() const { return _offset; }

void STIChecksumEntry::setOffset(Offset offset) { _offset = offset; }

STIChecksumEntry::Type STIChecksumEntry::getType() const { return _type; }

void STIChecksumEntry::setType(Type type) { _type = type; }

STIChecksumEntry::Checksum STIChecksumEntry::getChecksum() const {
  return _checksum;
}

void STIChecksumEntry::setChecksum(Checksum checksum) { _checksum = checksum; }

size_t STIChecksumEntry::getChecksumSize() const {
  uint32_t size;
  Type type;

  type = getType();
  switch (type) {
#define MAP(TYPE, SIZE)                                                        \
  case TYPE:                                                                   \
    size = SIZE;                                                               \
    break
    MAP(STI_FILECHECKSUM_ENTRY_TYPE_NONE, 0);
    MAP(STI_FILECHECKSUM_ENTRY_TYPE_MD5, 16);
    MAP(STI_FILECHECKSUM_ENTRY_TYPE_SHA1, 20);
#undef MAP
  default:
    assert(type != type); // unrecognized checksum type
    break;
  }

  return size;
}

//===----------------------------------------------------------------------===//
// STIChecksumTable
//===----------------------------------------------------------------------===//

STIChecksumTable::STIChecksumTable() : _entries(), _map() {}

STIChecksumTable::~STIChecksumTable() {
  for (STIChecksumEntry *entry : getEntries()) {
    delete entry;
  }
}

STIChecksumTable::EntryList &STIChecksumTable::getEntries() { return _entries; }

const STIChecksumTable::EntryList &STIChecksumTable::getEntries() const {
  return _entries;
}

STIChecksumEntry *
STIChecksumTable::findEntry(const STIStringEntry *string) const {
  auto itr = _map.find(string);
  if (itr != _map.end()) {
    return itr->second;
  }
  return nullptr;
}

void STIChecksumTable::append(STIStringEntry *string, STIChecksumEntry *entry) {
  _entries.push_back(entry);
  _map.insert(std::make_pair(string, entry));
}

//===----------------------------------------------------------------------===//
// STILineEntry
//
// A line table entry correlating a machine instruction to a source line.
//
//===----------------------------------------------------------------------===//

STILineEntry::STILineEntry() : _label(nullptr), _line() {}

STILineEntry::~STILineEntry() {}

STILineEntry *STILineEntry::create() { return new STILineEntry(); }

const MCSymbol *STILineEntry::getLabel() const { return _label; }

void STILineEntry::setLabel(const MCSymbol *const label) { _label = label; }

uint32_t STILineEntry::getLineNumStart() const { return _line._lineNumStart; }

void STILineEntry::setLineNumStart(const uint32_t lineNumStart) {
  _line._lineNumStart = lineNumStart;
}

uint32_t STILineEntry::getDeltaLineEnd() const { return _line._deltaLineEnd; }

void STILineEntry::setDeltaLineEnd(const uint32_t deltaLineEnd) {
  _line._deltaLineEnd = deltaLineEnd;
}

bool STILineEntry::getStatementEnd() const { return _line._fStatement; }

void STILineEntry::setStatementEnd(const bool statementEnd) {
  _line._fStatement = statementEnd;
}

//===----------------------------------------------------------------------===//
// STILineBlock
//
// A block of line table entries which all have a common source file.
//
//===----------------------------------------------------------------------===//

STILineBlock::STILineBlock() : _checksumEntry(nullptr), _lineEntries() {}

STILineBlock::~STILineBlock() {
  for (const STILineEntry *entry : getLines()) {
    delete entry;
  }
}

STILineBlock *STILineBlock::create() { return new STILineBlock(); }

StringRef STILineBlock::getFilename() const {
  return _checksumEntry->getFilename();
}

STIChecksumEntry *STILineBlock::getChecksumEntry() const {
  return _checksumEntry;
}

void STILineBlock::setChecksumEntry(STIChecksumEntry *checksumEntry) {
  _checksumEntry = checksumEntry;
}

STILineBlock::LineEntries &STILineBlock::getLines() { return _lineEntries; }

const STILineBlock::LineEntries &STILineBlock::getLines() const {
  return _lineEntries;
}

size_t STILineBlock::getLineCount() const { return _lineEntries.size(); }

void STILineBlock::appendLine(STILineEntry *entry) {
  _lineEntries.push_back(entry);
}

//===----------------------------------------------------------------------===//
// STILineSlice
//
// A segment of the machine instruction to source line correlation specific to
// a single procedure.
//
//===----------------------------------------------------------------------===//

STILineSlice::STILineSlice() : _function(nullptr), _blocks() {}

STILineSlice::~STILineSlice() {
  for (const STILineBlock *block : getBlocks()) {
    delete block;
  }
}

STILineSlice *STILineSlice::create() { return new STILineSlice(); }

Function *STILineSlice::getFunction() const { return _function; }

void STILineSlice::setFunction(Function *function) { _function = function; }

STILineSlice::BlockList &STILineSlice::getBlocks() { return _blocks; }

const STILineSlice::BlockList &STILineSlice::getBlocks() const {
  return _blocks;
}

void STILineSlice::appendBlock(STILineBlock *block) {
  _blocks.push_back(block);
}

//===----------------------------------------------------------------------===//
// STINumeric
//===----------------------------------------------------------------------===//

STINumeric* STINumeric::create(LeafID leafID, size_t size, const char* data) {
  return new STINumeric(leafID, size, data);
}

STINumeric::STINumeric(LeafID leafID, size_t size, const char* data)
  : _leafID     (leafID),
    _size       (size),
    _data       () {
  char* dest;
  if (size > sizeof(_data)) {
    dest = _data = new char [size];
  } else {
    dest = reinterpret_cast<char*>(&_data);
  }
  std::copy_n(data, size, dest);
}

STINumeric::~STINumeric() {
  if (_size > sizeof(_data)) {
    delete [] _data;
  }
}

STINumeric::LeafID STINumeric::getLeafID() const {
  return _leafID;
}

size_t STINumeric::getSize() const {
  return _size;
}

const char* STINumeric::getData() const {
  const char* data;
  if (_size > sizeof(_data)) {
    data = _data;
  } else {
    data = reinterpret_cast<const char*>(&_data);
  }
  return data;
}

//===----------------------------------------------------------------------===//
// STISymbol
//===----------------------------------------------------------------------===//

STISymbol::STISymbol(STIObjectKind kind) : STIObject(kind) {}

STISymbol::~STISymbol() {}

//===----------------------------------------------------------------------===//
// STISymbolModule
//===----------------------------------------------------------------------===//

STISymbolModule *STISymbolModule::create(const Module *module) {
  STISymbolModule *symbol = new STISymbolModule();

  symbol->setSignatureID(STI_SIGNATURE_LATEST);

  return symbol;
}

STISymbolModule::STISymbolModule()
    : STISymbol(STI_OBJECT_KIND_SYMBOL_MODULE), _path(), _compileUnits() {}

STISymbolModule::~STISymbolModule() {
  for (const STISymbolCompileUnit *unit : *getCompileUnits()) {
    delete unit;
  }
}

STISignatureID STISymbolModule::getSignatureID() const { return _signatureID; }

StringRef STISymbolModule::getPath() const { return _path; }

void STISymbolModule::setSignatureID(STISignatureID signatureID) {
  _signatureID = signatureID;
}

void STISymbolModule::setPath(StringRef path) { _path = path; }

void STISymbolModule::add(STISymbolCompileUnit *compileUnit) {
  _compileUnits.push_back(compileUnit);
}

const STISymbolModule::CompileUnitList *
STISymbolModule::getCompileUnits() const {
  return &_compileUnits;
}

//===----------------------------------------------------------------------===//
// STISymbolCompileUnit
//===----------------------------------------------------------------------===//

STISymbolCompileUnit *
STISymbolCompileUnit::create(const DICompileUnit compileUnit) {
  STISymbolCompileUnit *symbol;

  symbol = new STISymbolCompileUnit();

  return symbol;
}

STISymbolCompileUnit::STISymbolCompileUnit()
    : STISymbol(STI_OBJECT_KIND_SYMBOL_COMPILE_UNIT), _machineID(), _producer(),
      _scope(STIScope::create(this)) {}

STISymbolCompileUnit::~STISymbolCompileUnit() { delete _scope; }

STIMachineID STISymbolCompileUnit::getMachineID() const { return _machineID; }

void STISymbolCompileUnit::setMachineID(STIMachineID machineID) {
  _machineID = machineID;
}

StringRef STISymbolCompileUnit::getProducer() const { return _producer; }

void STISymbolCompileUnit::setProducer(StringRef producer) {
  _producer = producer;
}

STIScope *STISymbolCompileUnit::getScope() const { return _scope; }

//===----------------------------------------------------------------------===//
// STISymbolProcedure
//===----------------------------------------------------------------------===//

STISymbolProcedure::STISymbolProcedure()
    : STISymbol(STI_OBJECT_KIND_SYMBOL_PROCEDURE), _symbolID(S_GPROC32_ID),
      _name(), _type(nullptr), _scope(STIScope::create(this)),
      _labelBegin(nullptr), _labelEnd(nullptr), _labelPrologEnd(nullptr),
      _lineSlice(STILineSlice::create()), _scopeLineNumber(0),
      _frame(nullptr) {}

STISymbolProcedure::~STISymbolProcedure() {
  delete _scope;
  delete _lineSlice;
  delete _frame;
}

STISymbolProcedure *STISymbolProcedure::create() {
  return new STISymbolProcedure();
}

STISymbolID STISymbolProcedure::getSymbolID() const { return _symbolID; }

void STISymbolProcedure::setSymbolID(STISymbolID symbolID) {
  _symbolID = symbolID;
}

StringRef STISymbolProcedure::getName() const { return _name; }

void STISymbolProcedure::setName(StringRef name) { _name = name; }

STIType *STISymbolProcedure::getType() const { return _type; }

void STISymbolProcedure::setType(STIType *type) { _type = type; }

STIScope *STISymbolProcedure::getScope() const { return _scope; }

void STISymbolProcedure::setScope(STIScope *scope) { _scope = scope; }

MCSymbol *STISymbolProcedure::getLabelBegin() const { return _labelBegin; }

void STISymbolProcedure::setLabelBegin(MCSymbol *labelBegin) {
  _labelBegin = labelBegin;
}

MCSymbol *STISymbolProcedure::getLabelEnd() const { return _labelEnd; }

void STISymbolProcedure::setLabelEnd(MCSymbol *labelEnd) {
  _labelEnd = labelEnd;
}

MCSymbol *STISymbolProcedure::getLabelPrologEnd() const {
  return _labelPrologEnd;
}

void STISymbolProcedure::setLabelPrologEnd(MCSymbol *labelPrologEnd) {
  _labelPrologEnd = labelPrologEnd;
}

STILineSlice *STISymbolProcedure::getLineSlice() { return _lineSlice; }

const STILineSlice *STISymbolProcedure::getLineSlice() const {
  return _lineSlice;
}

unsigned STISymbolProcedure::getScopeLineNumber() const {
  return _scopeLineNumber;
}

void STISymbolProcedure::setScopeLineNumber(unsigned line) {
  _scopeLineNumber = line;
}

STISymbolFrameProc *STISymbolProcedure::getFrame() const { return _frame; }

void STISymbolProcedure::setFrame(STISymbolFrameProc *frame) { _frame = frame; }

//===----------------------------------------------------------------------===//
// STISymbolFrameProc
//===----------------------------------------------------------------------===//

STISymbolFrameProc::STISymbolFrameProc()
    : STISymbol(STI_OBJECT_KIND_SYMBOL_FRAMEPROC), _procedure(nullptr) {}

STISymbolFrameProc::~STISymbolFrameProc() {}

STISymbolFrameProc *STISymbolFrameProc::create() {
  return new STISymbolFrameProc();
}

STISymbolProcedure *STISymbolFrameProc::getProcedure() const {
  return _procedure;
}

void STISymbolFrameProc::setProcedure(STISymbolProcedure *procedure) {
  _procedure = procedure;
}

//===----------------------------------------------------------------------===//
// STISymbolBlock
//===----------------------------------------------------------------------===//

STISymbolBlock::STISymbolBlock()
    : STISymbol(STI_OBJECT_KIND_SYMBOL_BLOCK), _name(),
      _scope(STIScope::create(this)), _labelBegin(nullptr), _labelEnd(nullptr),
      _procedure(nullptr) {}

STISymbolBlock::~STISymbolBlock() { delete _scope; }

STISymbolBlock *STISymbolBlock::create() { return new STISymbolBlock(); }

StringRef STISymbolBlock::getName() const { return _name; }

void STISymbolBlock::setName(StringRef name) { _name = name; }

STIScope *STISymbolBlock::getScope() const { return _scope; }

void STISymbolBlock::setScope(STIScope *scope) { _scope = scope; }

MCSymbol *STISymbolBlock::getLabelBegin() const { return _labelBegin; }

void STISymbolBlock::setLabelBegin(MCSymbol *labelBegin) {
  _labelBegin = labelBegin;
}

MCSymbol *STISymbolBlock::getLabelEnd() const { return _labelEnd; }

void STISymbolBlock::setLabelEnd(MCSymbol *labelEnd) { _labelEnd = labelEnd; }

STISymbolProcedure *STISymbolBlock::getProcedure() const { return _procedure; }

void STISymbolBlock::setProcedure(STISymbolProcedure *procedure) {
  _procedure = procedure;
}

//===----------------------------------------------------------------------===//
// STISymbolConstant
//===----------------------------------------------------------------------===//

STISymbolConstant *STISymbolConstant::create() {
  return new STISymbolConstant();
}

STISymbolConstant::STISymbolConstant()
    : STISymbol(STI_OBJECT_KIND_SYMBOL_CONSTANT),
      _name     (),
      _type     (nullptr),
      _value    (nullptr) {
}

STISymbolConstant::~STISymbolConstant() {
  delete _value;
}

StringRef STISymbolConstant::getName() const {
  return _name;
}

void STISymbolConstant::setName(StringRef name) {
  _name = name;
}

const STINumeric* STISymbolConstant::getValue() const {
  return _value;
}

void STISymbolConstant::setValue(const STINumeric* value) {
  _value = value;
}

STIType *STISymbolConstant::getType() const {
  return _type;
}

void STISymbolConstant::setType(STIType *type) {
    _type = type;
}

//===----------------------------------------------------------------------===//
// STISymbolVariable
//===----------------------------------------------------------------------===//

STISymbolVariable *STISymbolVariable::create() {
  return new STISymbolVariable();
}

STISymbolVariable::STISymbolVariable()
    : STISymbol(STI_OBJECT_KIND_SYMBOL_VARIABLE), _location(nullptr) {}

STISymbolVariable::~STISymbolVariable() { delete _location; }

StringRef STISymbolVariable::getName() const { return _name; }

void STISymbolVariable::setName(StringRef name) { _name = name; }

STILocation *STISymbolVariable::getLocation() const { return _location; }

void STISymbolVariable::setLocation(STILocation *location) {
  _location = location;
}

STIType *STISymbolVariable::getType() const { return _type; }

void STISymbolVariable::setType(STIType *type) { _type = type; }

//===----------------------------------------------------------------------===//
// STIType
//===----------------------------------------------------------------------===//

STIType::STIType(STIObjectKind kind)
    : STIObject(kind), _index(T_NOTYPE), _sizeInBits(0) {}

STIType::~STIType() {}

STITypeIndex STIType::getIndex() const { return _index; }

void STIType::setIndex(STITypeIndex index) { _index = index; }

uint32_t STIType::getSizeInBits() const { return _sizeInBits; }

void STIType::setSizeInBits(uint32_t sizeInBits) { _sizeInBits = sizeInBits; }

//===----------------------------------------------------------------------===//
// STITypeBasic
//===----------------------------------------------------------------------===//

STITypeBasic::STITypeBasic()
    : STIType(STI_OBJECT_KIND_TYPE_BASIC), _primitive(T_NOTYPE) {}

STITypeBasic::~STITypeBasic() {}

STITypeBasic *STITypeBasic::create() { return new STITypeBasic(); }

STITypeBasic::Primitive STITypeBasic::getPrimitive() const {
  return _primitive;
}

void STITypeBasic::setPrimitive(Primitive primitive) { _primitive = primitive; }

//===----------------------------------------------------------------------===//
// STITypeModifier
//===----------------------------------------------------------------------===//

STITypeModifier::STITypeModifier()
    : STIType(STI_OBJECT_KIND_TYPE_MODIFIER), _qualifiedType(nullptr),
      _isConstant(false), _isVolatile(false), _isUnaligned(false) {}

STITypeModifier::~STITypeModifier() {}

STITypeModifier *STITypeModifier::create() { return new STITypeModifier(); }

STIType *STITypeModifier::getQualifiedType() const { return _qualifiedType; }

void STITypeModifier::setQualifiedType(STIType *qualifiedType) {
  _qualifiedType = qualifiedType;
}

bool STITypeModifier::isConstant() const { return _isConstant; }

void STITypeModifier::setIsConstant(bool isConstant) {
  _isConstant = isConstant;
}

bool STITypeModifier::isVolatile() const { return _isVolatile; }

void STITypeModifier::setIsVolatile(bool isVolatile) {
  _isVolatile = isVolatile;
}

bool STITypeModifier::isUnaligned() const { return _isUnaligned; }

void STITypeModifier::setIsUnaligned(bool isUnaligned) {
  _isUnaligned = isUnaligned;
}

//===----------------------------------------------------------------------===//
// STITypePointer
//===----------------------------------------------------------------------===//

STITypePointer::STITypePointer()
    : STIType(STI_OBJECT_KIND_TYPE_POINTER), _pointerTo(nullptr),
      _containingClass(nullptr), _isReference(false),
      _ptrToMemberType(STITypePointer::PTM_NONE), _isConstant(false) {}

STITypePointer::~STITypePointer() {}

STITypePointer *STITypePointer::create() { return new STITypePointer(); }

STIType *STITypePointer::getPointerTo() const { return _pointerTo; }

void STITypePointer::setPointerTo(STIType *pointerTo) {
  _pointerTo = pointerTo;
}

STIType *STITypePointer::getContainingClass() const { return _containingClass; }

void STITypePointer::setContainingClass(STIType *classType) {
  _containingClass = classType;
}

bool STITypePointer::isReference() const { return _isReference; }

void STITypePointer::setIsReference(bool isReference) {
  _isReference = isReference;
}

STITypePointer::PTMType STITypePointer::getPtrToMemberType() const {
  return _ptrToMemberType;
}

void
STITypePointer::setPtrToMemberType(STITypePointer::PTMType ptrToMemberType) {
  _ptrToMemberType = ptrToMemberType;
}

bool STITypePointer::isConstant() const { return _isConstant; }

void STITypePointer::setIsConstant(bool isConst) { _isConstant = isConst; }

//===----------------------------------------------------------------------===//
// STITypeArray
//===----------------------------------------------------------------------===//

STITypeArray::STITypeArray() :
    STIType     (STI_OBJECT_KIND_TYPE_ARRAY),
    _elementType(nullptr),
    _length     (nullptr) {
}

STITypeArray::~STITypeArray() {
  delete _length;
}

STITypeArray *STITypeArray::create() { return new STITypeArray(); }

STIType *STITypeArray::getElementType() const { return _elementType; }

void STITypeArray::setElementType(STIType *elementType) {
  _elementType = elementType;
}

StringRef STITypeArray::getName() const {
  return _name;
}

void STITypeArray::setName(StringRef name) {
  _name = name;
}

const STINumeric* STITypeArray::getLength() const {
  return _length;
}

void STITypeArray::setLength(const STINumeric* length) {
  _length = length;
}

//===----------------------------------------------------------------------===//
// STITypeBitfield
//===----------------------------------------------------------------------===//

STITypeBitfield::STITypeBitfield()
    : STIType(STI_OBJECT_KIND_TYPE_BITFIELD), _type(nullptr), _offset(~0),
      _size(0) {}

STITypeBitfield::~STITypeBitfield() {}

STITypeBitfield *STITypeBitfield::create() { return new STITypeBitfield(); }

STIType *STITypeBitfield::getType() const { return _type; }

void STITypeBitfield::setType(STIType *type) { _type = type; }

uint32_t STITypeBitfield::getOffset() const { return _offset; }

void STITypeBitfield::setOffset(uint32_t offset) { _offset = offset; }

uint32_t STITypeBitfield::getSize() const { return _size; }

void STITypeBitfield::setSize(uint32_t size) { _size = size; }

//===----------------------------------------------------------------------===//
// STITypeMember
//===----------------------------------------------------------------------===//

STITypeMember::STITypeMember() :
    _attribute  (0),
    _type       (nullptr),
    _offset     (nullptr),
    _isStatic   (false) {
}

STITypeMember::~STITypeMember() {
  delete _offset;
}

STITypeMember *STITypeMember::create() { return new STITypeMember(); }

uint16_t STITypeMember::getAttribute() const { return _attribute; }

void STITypeMember::setAttribute(uint16_t attr) { _attribute = attr; }

STIType *STITypeMember::getType() const { return _type; }

void STITypeMember::setType(STIType *type) { _type = type; }

const STINumeric *STITypeMember::getOffset() const {
  return _offset;
}

void STITypeMember::setOffset(const STINumeric *offset) {
  _offset = offset;
}

StringRef STITypeMember::getName() const { return _name; }

void STITypeMember::setName(StringRef name) { _name = name; }

bool STITypeMember::isStatic() const { return _isStatic; }

void STITypeMember::setIsStatic(bool isStatic) { _isStatic = isStatic; }

//===----------------------------------------------------------------------===//
// STITypeMethodListEntry
//===----------------------------------------------------------------------===//

STITypeMethodListEntry::STITypeMethodListEntry()
    : _attribute(0), _type(nullptr), _virtuality(0), _virtualIndex(0) {}

STITypeMethodListEntry::~STITypeMethodListEntry() {}

STITypeMethodListEntry *STITypeMethodListEntry::create() {
  return new STITypeMethodListEntry();
}

uint16_t STITypeMethodListEntry::getAttribute() const { return _attribute; }

void STITypeMethodListEntry::setAttribute(uint16_t attr) { _attribute = attr; }

STIType *STITypeMethodListEntry::getType() const { return _type; }

void STITypeMethodListEntry::setType(STIType *type) { _type = type; }

int STITypeMethodListEntry::getVirtuality() const { return _virtuality; }

void STITypeMethodListEntry::setVirtuality(int virtuality) {
  _virtuality = virtuality;
}

int STITypeMethodListEntry::getVirtualIndex() const { return _virtualIndex; }

void STITypeMethodListEntry::setVirtualIndex(int virtualIndex) {
  _virtualIndex = virtualIndex;
}

//===----------------------------------------------------------------------===//
// STITypeMethodList
//===----------------------------------------------------------------------===//

STITypeMethodList::STITypeMethodList()
    : STIType(STI_OBJECT_KIND_TYPE_METHOD_LIST) {}

STITypeMethodList::~STITypeMethodList() {
  for (STITypeMethodListEntry *entry : getList()) {
    delete entry;
  }
}

STITypeMethodList *STITypeMethodList::create() {
  return new STITypeMethodList();
}

uint32_t STITypeMethodList::getMethodsCount() const {
  return _methodList.size();
}

const STITypeMethodList::STIMethodTypeList &STITypeMethodList::getList() const {
  return _methodList;
}

STITypeMethodList::STIMethodTypeList &STITypeMethodList::getList() {
  return _methodList;
}

//===----------------------------------------------------------------------===//
// STITypeMethod
//===----------------------------------------------------------------------===//

STITypeMethod::STITypeMethod() : _count(0), _methodList(nullptr) {}

STITypeMethod::~STITypeMethod() {}

STITypeMethod *STITypeMethod::create() { return new STITypeMethod(); }

int STITypeMethod::getCount() const { return _count; }

void STITypeMethod::setCount(int count) { _count = count; }

STIType *STITypeMethod::getList() const { return _methodList; }

void STITypeMethod::setList(STIType *methodList) { _methodList = methodList; }

StringRef STITypeMethod::getName() const { return _name; }

void STITypeMethod::setName(StringRef name) { _name = name; }

//===----------------------------------------------------------------------===//
// STITypeOneMethod
//===----------------------------------------------------------------------===//

STITypeOneMethod::STITypeOneMethod()
    : _attribute(0), _type(nullptr), _virtuality(0), _virtualIndex(0) {}

STITypeOneMethod::~STITypeOneMethod() {}

STITypeOneMethod *STITypeOneMethod::create() { return new STITypeOneMethod(); }

uint16_t STITypeOneMethod::getAttribute() const { return _attribute; }

void STITypeOneMethod::setAttribute(uint16_t attr) { _attribute = attr; }

STIType *STITypeOneMethod::getType() const { return _type; }

void STITypeOneMethod::setType(STIType *type) { _type = type; }

int STITypeOneMethod::getVirtuality() const { return _virtuality; }

void STITypeOneMethod::setVirtuality(int virtuality) {
  _virtuality = virtuality;
}

int STITypeOneMethod::getVirtualIndex() const { return _virtualIndex; }

void STITypeOneMethod::setVirtualIndex(int virtualIndex) {
  _virtualIndex = virtualIndex;
}

StringRef STITypeOneMethod::getName() const { return _name; }

void STITypeOneMethod::setName(StringRef name) { _name = name; }

//===----------------------------------------------------------------------===//
// STITypeEnumerator
//===----------------------------------------------------------------------===//

STITypeEnumerator::STITypeEnumerator() :
    _attribute  (0),
    _value      (nullptr) {
}

STITypeEnumerator::~STITypeEnumerator() {
  delete _value;
}

STITypeEnumerator *STITypeEnumerator::create() {
  return new STITypeEnumerator();
}

uint16_t STITypeEnumerator::getAttribute() const { return _attribute; }

void STITypeEnumerator::setAttribute(uint16_t attr) { _attribute = attr; }

const STINumeric *STITypeEnumerator::getValue() const {
  return _value;
}

void STITypeEnumerator::setValue(const STINumeric *value) {
  _value = value;
}

StringRef STITypeEnumerator::getName() const { return _name; }

void STITypeEnumerator::setName(StringRef name) { _name = name; }

//===----------------------------------------------------------------------===//
// STITypeBaseClass
//===----------------------------------------------------------------------===//

STITypeBaseClass::STITypeBaseClass() :
    _attribute  (0),
    _type       (nullptr),
    _offset     (nullptr) {
}

STITypeBaseClass::~STITypeBaseClass() {
  delete _offset;
}

STITypeBaseClass *STITypeBaseClass::create() { return new STITypeBaseClass(); }

uint16_t STITypeBaseClass::getAttribute() const { return _attribute; }

void STITypeBaseClass::setAttribute(uint16_t attr) { _attribute = attr; }

STIType *STITypeBaseClass::getType() const { return _type; }

void STITypeBaseClass::setType(STIType *type) { _type = type; }

const STINumeric *STITypeBaseClass::getOffset() const {
  return _offset;
}

void STITypeBaseClass::setOffset(const STINumeric *offset) {
  _offset = offset;
}

//===----------------------------------------------------------------------===//
// STITypeVBaseClass
//===----------------------------------------------------------------------===//

STITypeVBaseClass::STITypeVBaseClass(bool indirect) :
    _attribute  (0),
    _type       (nullptr),
    _vbpType    (nullptr),
    _vbpOffset  (nullptr),
    _vbIndex    (nullptr) {
  _symbolID = indirect ? LF_IVBCLASS : LF_VBCLASS;
}

STITypeVBaseClass::~STITypeVBaseClass() {
  delete _vbpOffset;
  delete _vbIndex;
}

STITypeVBaseClass *STITypeVBaseClass::create(bool indirect) {
  return new STITypeVBaseClass(indirect);
}

STISymbolID STITypeVBaseClass::getSymbolID() const { return _symbolID; }

uint16_t STITypeVBaseClass::getAttribute() const { return _attribute; }

void STITypeVBaseClass::setAttribute(uint16_t attr) { _attribute = attr; }

STIType *STITypeVBaseClass::getType() const { return _type; }

void STITypeVBaseClass::setType(STIType *type) { _type = type; }

STIType *STITypeVBaseClass::getVbpType() const { return _vbpType; }

void STITypeVBaseClass::setVbpType(STIType *type) { _vbpType = type; }

const STINumeric * STITypeVBaseClass::getVbpOffset() const {
  return _vbpOffset;
}

void STITypeVBaseClass::setVbpOffset(const STINumeric *offset) {
  _vbpOffset = offset;
}

const STINumeric * STITypeVBaseClass::getVbIndex() const {
  return _vbIndex;
}

void STITypeVBaseClass::setVbIndex(const STINumeric *index) {
  _vbIndex = index;
}

//===----------------------------------------------------------------------===//
// STITypeVFuncTab
//===----------------------------------------------------------------------===//

STITypeVFuncTab::STITypeVFuncTab() : _type(nullptr) {}

STITypeVFuncTab::~STITypeVFuncTab() {}

STITypeVFuncTab *STITypeVFuncTab::create() { return new STITypeVFuncTab(); }

STIType *STITypeVFuncTab::getType() const { return _type; }

void STITypeVFuncTab::setType(STIType *type) { _type = type; }

//===----------------------------------------------------------------------===//
// STITypeFieldList
//===----------------------------------------------------------------------===//

STITypeFieldList::STITypeFieldList()
    : STIType(STI_OBJECT_KIND_TYPE_FIELD_LIST), _vFuncTab(nullptr) {}

STITypeFieldList::~STITypeFieldList() {
  for (STITypeBaseClass *baseClass : getBaseClasses()) {
    delete baseClass;
  }

  for (STITypeVBaseClass *vBaseClass : getVBaseClasses()) {
    delete vBaseClass;
  }

  const STITypeVFuncTab *vFuncTab = getVFuncTab();
  delete vFuncTab;

  for (STITypeMember *member : getMembers()) {
    delete member;
  }

  for (STITypeMethod *method : getMethods()) {
    delete method;
  }

  for (STITypeOneMethod *method : getOneMethods()) {
    delete method;
  }

  for (STITypeEnumerator *enumerator : getEnumerators()) {
    delete enumerator;
  }
}

STITypeFieldList *STITypeFieldList::create() { return new STITypeFieldList(); }

STITypeFieldList::STITypeBaseClassList &STITypeFieldList::getBaseClasses() {
  return _baseClasses;
}

const STITypeFieldList::STITypeBaseClassList &
STITypeFieldList::getBaseClasses() const {
  return _baseClasses;
}

STITypeFieldList::STITypeVBaseClassList &STITypeFieldList::getVBaseClasses() {
  return _vBaseClasses;
}

const STITypeFieldList::STITypeVBaseClassList &
STITypeFieldList::getVBaseClasses() const {
  return _vBaseClasses;
}

const STITypeVFuncTab *STITypeFieldList::getVFuncTab() const {
  return _vFuncTab;
}

void STITypeFieldList::setVFuncTab(STITypeVFuncTab *vFuncTab) {
  _vFuncTab = vFuncTab;
}

STITypeFieldList::STITypeMemberList &STITypeFieldList::getMembers() {
  return _members;
}

const STITypeFieldList::STITypeMemberList &
STITypeFieldList::getMembers() const {
  return _members;
}

STITypeFieldList::STITypeMethodsList &STITypeFieldList::getMethods() {
  return _methods;
}

const STITypeFieldList::STITypeMethodsList &
STITypeFieldList::getMethods() const {
  return _methods;
}

STITypeFieldList::STITypeOneMethodList &STITypeFieldList::getOneMethods() {
  return _oneMethods;
}

const STITypeFieldList::STITypeOneMethodList &
STITypeFieldList::getOneMethods() const {
  return _oneMethods;
}

STITypeFieldList::STITypeEnumeratorList &STITypeFieldList::getEnumerators() {
  return _enumerators;
}

const STITypeFieldList::STITypeEnumeratorList &
STITypeFieldList::getEnumerators() const {
  return _enumerators;
}

//===----------------------------------------------------------------------===//
// STITypeStructure
//===----------------------------------------------------------------------===//

STITypeStructure::STITypeStructure() :
    STIType     (STI_OBJECT_KIND_TYPE_STRUCTURE),
    _leaf       (0),
    _count      (0),
    _property   (0),
    _fieldType  (nullptr),
    _derivedType(nullptr),
    _vshapeType (nullptr),
    _size       (nullptr) {
}

STITypeStructure::~STITypeStructure() {
  delete _size;
}

STITypeStructure *STITypeStructure::create() { return new STITypeStructure(); }

uint16_t STITypeStructure::getLeaf() const { return _leaf; }

void STITypeStructure::setLeaf(uint16_t leaf) { _leaf = leaf; }

uint16_t STITypeStructure::getCount() const { return _count; }

void STITypeStructure::setCount(uint16_t count) { _count = count; }

uint16_t STITypeStructure::getProperty() const { return _property; }

void STITypeStructure::setProperty(uint16_t prop) { _property = prop; }

STIType *STITypeStructure::getFieldType() const { return _fieldType; }

void STITypeStructure::setFieldType(STIType *fieldType) {
  _fieldType = fieldType;
}

STIType *STITypeStructure::getDerivedType() const { return _derivedType; }

void STITypeStructure::setDerivedType(STIType *derivedType) {
  _derivedType = derivedType;
}

STIType *STITypeStructure::getVShapeType() const { return _vshapeType; }

void STITypeStructure::setVShapeType(STIType *vshapeType) {
  _vshapeType = vshapeType;
}

const STINumeric *STITypeStructure::getSize() const {
  return _size;
}

void STITypeStructure::setSize(const STINumeric *size) {
  _size = size;
}

StringRef STITypeStructure::getName() const { return _name; }

void STITypeStructure::setName(StringRef name) { _name = name; }

//===----------------------------------------------------------------------===//
// STITypeEnumeration
//===----------------------------------------------------------------------===//

STITypeEnumeration::STITypeEnumeration()
    : STIType(STI_OBJECT_KIND_TYPE_ENUMERATION), _count(0), _property(0),
      _elementType(nullptr), _fieldType(nullptr) {}

STITypeEnumeration::~STITypeEnumeration() {}

STITypeEnumeration *STITypeEnumeration::create() {
  return new STITypeEnumeration();
}

uint16_t STITypeEnumeration::getCount() const { return _count; }

void STITypeEnumeration::setCount(uint16_t count) { _count = count; }

uint16_t STITypeEnumeration::getProperty() const { return _property; }

void STITypeEnumeration::setProperty(uint16_t prop) { _property = prop; }

STIType *STITypeEnumeration::getElementType() const { return _elementType; }

void STITypeEnumeration::setElementType(STIType *elementType) {
  _elementType = elementType;
}

STIType *STITypeEnumeration::getFieldType() const { return _fieldType; }

void STITypeEnumeration::setFieldType(STIType *fieldType) {
  _fieldType = fieldType;
}

StringRef STITypeEnumeration::getName() const { return _name; }

void STITypeEnumeration::setName(StringRef name) { _name = name; }

//===----------------------------------------------------------------------===//
// STITypeVShape
//===----------------------------------------------------------------------===//

STITypeVShape::STITypeVShape()
    : STIType(STI_OBJECT_KIND_TYPE_VSHAPE), _count(0) {}

STITypeVShape::~STITypeVShape() {}

STITypeVShape *STITypeVShape::create() { return new STITypeVShape(); }

uint16_t STITypeVShape::getCount() const { return _count; }

void STITypeVShape::setCount(uint16_t count) { _count = count; }

//===----------------------------------------------------------------------===//
// STITypeFunctionID
//===----------------------------------------------------------------------===//

STITypeFunctionID::STITypeFunctionID()
    : STIType(STI_OBJECT_KIND_TYPE_FUNCTION_ID), _type(nullptr),
      _parentScope(nullptr) {}

STITypeFunctionID::~STITypeFunctionID() {}

STITypeFunctionID *STITypeFunctionID::create() {
  return new STITypeFunctionID();
}

STIType *STITypeFunctionID::getType() const { return _type; }

void STITypeFunctionID::setType(STIType *type) { _type = type; }

STIType *STITypeFunctionID::getParentScope() const { return _parentScope; }

void STITypeFunctionID::setParentScope(STIType *parentScope) {
  _parentScope = parentScope;
}

STIType *STITypeFunctionID::getParentClassType() const {
  return _parentClassType;
}

void STITypeFunctionID::setParentClassType(STIType *parentClassType) {
  _parentClassType = parentClassType;
}

StringRef STITypeFunctionID::getName() const { return _name; }

void STITypeFunctionID::setName(StringRef name) { _name = name; }

//===----------------------------------------------------------------------===//
// STITypeProcedure
//===----------------------------------------------------------------------===//

STITypeProcedure::STITypeProcedure()
    : STIType(STI_OBJECT_KIND_TYPE_PROCEDURE), _returnType(nullptr),
      _classType(nullptr), _thisType(nullptr), _callingConvention(0),
      _paramCount(0), _argumentList(nullptr), _thisAdjust(0) {}

STITypeProcedure::~STITypeProcedure() {}

STITypeProcedure *STITypeProcedure::create() { return new STITypeProcedure(); }

STIType *STITypeProcedure::getReturnType() const { return _returnType; }

void STITypeProcedure::setReturnType(STIType *returnType) {
  _returnType = returnType;
}

STIType *STITypeProcedure::getClassType() const { return _classType; }

void STITypeProcedure::setClassType(STIType *classType) {
  _classType = classType;
}

STIType *STITypeProcedure::getThisType() const { return _thisType; }

void STITypeProcedure::setThisType(STIType *thisType) { _thisType = thisType; }

int STITypeProcedure::getCallingConvention() const {
  return _callingConvention;
}

void STITypeProcedure::setCallingConvention(int callingConvention) {
  _callingConvention = callingConvention;
}

uint16_t STITypeProcedure::getParamCount() const { return _paramCount; }

void STITypeProcedure::setParamCount(uint16_t paramCount) {
  _paramCount = paramCount;
}

STIType *STITypeProcedure::getArgumentList() const { return _argumentList; }

void STITypeProcedure::setArgumentList(STIType *argumentList) {
  _argumentList = argumentList;
}

int STITypeProcedure::getThisAdjust() const { return _thisAdjust; }

void STITypeProcedure::setThisAdjust(int thisAdjust) {
  _thisAdjust = thisAdjust;
}

//===----------------------------------------------------------------------===//
// STITypeArgumentList
//===----------------------------------------------------------------------===//

STITypeArgumentList::STITypeArgumentList()
    : STIType(STI_OBJECT_KIND_TYPE_ARGUMENT_LIST), _argumentList() {}

STITypeArgumentList::~STITypeArgumentList() {}

STITypeArgumentList *STITypeArgumentList::create() {
  return new STITypeArgumentList();
}

uint32_t STITypeArgumentList::getArgumentCount() const {
  return _argumentList.size();
}

const STITypeArgumentList::STIArgTypeList *
STITypeArgumentList::getArgumentList() const {
  return &_argumentList;
}

STITypeArgumentList::STIArgTypeList *STITypeArgumentList::getArgumentList() {
  return &_argumentList;
}

//===----------------------------------------------------------------------===//
// STITypeServer
//===----------------------------------------------------------------------===//

STITypeServer::STITypeServer()
    : STIType(STI_OBJECT_KIND_TYPE_SERVER), _pdbFullName() {}

STITypeServer::~STITypeServer() {}

STITypeServer *STITypeServer::create() { return new STITypeServer(); }

StringRef STITypeServer::getPDBFullName() const { return _pdbFullName; }

void STITypeServer::setPDBFullName(StringRef name) { _pdbFullName = name; }

//===----------------------------------------------------------------------===//
// STISymbolUserDefined
//===----------------------------------------------------------------------===//

STISymbolUserDefined::STISymbolUserDefined()
    : STISymbol(STI_OBJECT_KIND_SYMBOL_USER_DEFINED), _definedType(nullptr) {}

STISymbolUserDefined::~STISymbolUserDefined() {}

STISymbolUserDefined *STISymbolUserDefined::create() {
  return new STISymbolUserDefined();
}

STIType *STISymbolUserDefined::getDefinedType() const { return _definedType; }

void STISymbolUserDefined::setDefinedType(STIType *definedType) {
  _definedType = definedType;
}

StringRef STISymbolUserDefined::getName() const { return _name; }

void STISymbolUserDefined::setName(StringRef name) { _name = name; }

//===----------------------------------------------------------------------===//
// STIScope
//===----------------------------------------------------------------------===//

STIScope::STIScope(STISymbol *symbol)
    : STIObject(STI_OBJECT_KIND_SCOPE), _parent(nullptr), _symbol(symbol),
      _objects() {}

STIScope::~STIScope() {
  for (auto entry : getObjects()) {
    delete entry.second;
  }
}

STIScope *STIScope::create(STISymbol *symbol) { return new STIScope(symbol); }

STIScope *STIScope::getParent() const { return _parent; }

void STIScope::setParent(STIScope *parent) { _parent = parent; }

STISymbol *STIScope::getSymbol() const { return _symbol; }

void STIScope::setSymbol(STISymbol *symbol) { _symbol = symbol; }

void STIScope::add(STIObject *object, unsigned ArgNum) {
  if (ArgNum) {
    // Keep all parameters in order at the start of the variable list to ensure
    // function types are correct (no out-of-order parameters)
    //
    // This could be improved by only doing it for optimized builds (unoptimized
    // builds have the right order to begin with), searching from the back (this
    // would catch the unoptimized case quickly), or doing a binary search
    // rather than linear search.
    auto I = _objects.begin();
    while (I != _objects.end()) {
      unsigned CurNum = (*I).first;
      // A local (non-parameter) variable has been found, insert immediately
      // before it.
      if (CurNum == 0)
        break;
      // A later indexed parameter has been found, insert immediately before it.
      if (CurNum > ArgNum)
        break;

      //assert((CurNum != ArgNum) &&
      //       "Duplicate argument for top level (non-inlined) function");
      ++I;
    }
    _objects.insert(I, std::pair<unsigned, STIObject *>(ArgNum, object));
    return;
  }

  _objects.push_back(std::pair<unsigned, STIObject *>(ArgNum, object));
}

const STIScope::ObjectList &STIScope::getObjects() const { return _objects; }

//===----------------------------------------------------------------------===//
// STISubsection
//===----------------------------------------------------------------------===//

STISubsection::STISubsection(STISubsectionID id)
    : _id(id), _begin(nullptr), _end(nullptr) {}

STISubsection::~STISubsection() {}

STISubsectionID STISubsection::getID() const { return _id; }

MCSymbol *STISubsection::getBegin() const { return _begin; }

void STISubsection::setBegin(MCSymbol *begin) { _begin = begin; }

MCSymbol *STISubsection::getEnd() const { return _end; }

void STISubsection::setEnd(MCSymbol *end) { _end = end; }

//===----------------------------------------------------------------------===//
// STISymbolTable
//===----------------------------------------------------------------------===//

STISymbolTable::STISymbolTable() : _root(nullptr) {}

STISymbolTable::~STISymbolTable() { delete _root; }

STISymbol *STISymbolTable::getRoot() const { return _root; }

void STISymbolTable::setRoot(STISymbol *root) { _root = root; }
