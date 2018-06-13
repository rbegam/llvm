//===---- DTransOptBase.h - Common base classes for DTrans Transforms --==//
//
// Copyright (C) 2018 Intel Corporation. All rights reserved.
//
// The information and source code contained herein is the exclusive property
// of Intel Corporation and may not be disclosed, examined or reproduced in
// whole or in part without explicit written authorization from the company.
//
//===----------------------------------------------------------------------===//
//
// This file declares the base classes for DTrans transformations that provide
// the common functionality needed for rewriting dependent data types and
// functions which change as the result of DTrans modifying a structure
// definition. Transformations should derive from the DTransOptBase class to
// get the needed common functionality.
//
//===----------------------------------------------------------------------===//

#if !INTEL_INCLUDE_DTRANS
#error DTrans.h include in an non-INTEL_INCLUDE_DTRANS build.
#endif

#ifndef INTEL_OPTIONALCOMPONENTS_INTEL_DTRANS_TRANSFORMS_DTRANSOPTBASE_H
#define INTEL_OPTIONALCOMPONENTS_INTEL_DTRANS_TRANSFORMS_DTRANSOPTBASE_H

#include "llvm/ADT/DenseMap.h"
#include "llvm/ADT/SetVector.h"
#include "llvm/IR/Module.h"
#include "llvm/Transforms/Utils/ValueMapper.h"

namespace llvm {

class BinaryOperator;

namespace dtrans {
class CallInfo;
}

class DTransAnalysisInfo;

// This class handles the remapping of structure types from old to new types
// during the transformation and cloning of functions for DTrans.
//
// Clients should first populate the old type to new type mapping for types the
// transformation needs to replace with the \b addTypeMapping() method. New
// types that derive from the type mapping can be then be computed using the
// \b computeReplacementType() method.
//
// For example: if %struct.t1 is to be replaced with %struct.xyz_trans.t1,
// then a call should be made to:
//   addTypeMapping(%struct.t1, %struct.xyx_trans.t1);
// If the transformation then needs to know what the replacement for the array
// type "[5 x %struct.t1**]" or the function type "void (%struct.t1*)*" should
// be, a call to \b computeReplacementType() can be made.
//
//
// After all type mappings for the structures being modified are added, the
// \b setAllTypeMappingsAdded() method must be called, which will allow the
// \b remapType() routine to be used to compute and cache results.
//
class DTransTypeRemapper : public ValueMapTypeRemapper {
public:
  DTransTypeRemapper() : AllTypeMappingsAdded(false) {}

  DTransTypeRemapper(const DTransTypeRemapper &) = delete;
  DTransTypeRemapper &operator=(const DTransTypeRemapper &) = delete;

  // Return the type to use for \p SrcTy.
  //
  // If the type is not being changed, then \p SrcTy will be returned. Otherwise
  // the replacement type will be returned.
  //
  // This method caches the results for subsequent lookups, and may only be
  // used after all the base types being replaced have been populated via
  // the \b addTypeMapping() method.
  virtual llvm::Type *remapType(llvm::Type *SrcTy) override;

  // Add a type \p SrcTy that needs to be remapped to \p DestTy.
  void addTypeMapping(llvm::Type *SrcTy, llvm::Type *DestTy);

  // Indicate that all structure types that DTrans needs to rewrite have
  // been added.
  void setAllTypeMappingsAdded() { AllTypeMappingsAdded = true; }

  // Check if the \p SrcTy type has a mapping in the type list
  bool hasRemappedType(llvm::Type *SrcTy) const;

  // Return the type mapping for \p SrcTy type, if there is one. If there is
  // not one, return nullptr. This differs from \b remapType, in that it will
  // not create and cache a new type mapping for \p SrcTy.
  llvm::Type *lookupTypeMapping(llvm::Type *SrcTy) const;

  // Return the cached result for a type mapping for \p SrcTy type, if the type
  // has been evaluated previously. Otherwise, return nullptr.
  llvm::Type *lookupCachedTypeMapping(llvm::Type *SrcTy) const;

  // Compute the replacement type for \p SrcTy based on the SrcTypeToNewType
  // mapping. If the type needs to be replaced, return the type to be used. If
  // the type should not be replaced, return nullptr.
  llvm::Type *computeReplacementType(llvm::Type *SrcTy) const;

private:
  // Mapping from original type to the replacement type.
  DenseMap<llvm::Type *, llvm::Type *> SrcTypeToNewType;

  // During the remapping process, a cache is built up to avoid repeated
  // computations on complex types that have been determine to need or not need
  // to be replaced.
  DenseMap<llvm::Type *, llvm::Type *> RemapSrcToDestTypeCache;

  // This indicates the client has added all the structure types the
  // transformation needs to replace.
  bool AllTypeMappingsAdded;
};

// This is a base class the specific transformations derive from to
// implement specific transformations. This class provides the basic framework
// for driving the transformation and handling the common functionality for
// transforming dependent data types.
//
// This class handles:
// - The identification of dependent data types
// - The construction of new data types for the dependent types
// - The replacement of global variables with types being changed
// - The cloning of functions that have arguments or return values with types
//   that are being modified.
class DTransOptBase {
public:
  // Data structure to use for mapping one type to another type.
  using TypeToTypeMap = DenseMap<llvm::Type *, llvm::Type *>;

  // \param DTInfo DTrans Analysis Result
  // \param Context llvm context for the module
  // \param DL Module's data layout
  // \param DepTypePrefix Optional string to prefix structure names of rewritten
  // dependent types
  // \param TypeRemapper Class that will perform type mapping from old types
  // to new types
  // \param Materializer Optional class that works with ValueMapper to create
  // new Values during type remapping
  DTransOptBase(DTransAnalysisInfo &DTInfo, LLVMContext &Context,
                const DataLayout &DL, StringRef DepTypePrefix,
                DTransTypeRemapper *TypeRemapper,
                ValueMaterializer *Materializer = nullptr)
      : DTInfo(DTInfo), Context(Context), DL(DL), DepTypePrefix(DepTypePrefix),
        TypeRemapper(TypeRemapper), Materializer(Materializer) {}

  DTransOptBase(const DTransOptBase &) = delete;
  DTransOptBase &operator=(const DTransOptBase &) = delete;

  virtual ~DTransOptBase() {}

  // The main routine the drives the entire process. Returns 'true' if changes
  // are made to the module.
  //
  // The flow and interaction with the derived classes is:
  //  1. Child class prepares opaque types for new types: (prepareTypes)
  //  2. Base class identifies types dependent on step 1.
  //  3. Base class populates new types for dependent types of step 2.
  //  4. Child class populates types of step 1. (populateTypes)
  //  5. Child class performs any module level transform to create new
  //  variables. (prepareModule)
  //  6. Base class creates new function prototypes for dependent functions.
  //  7. Base class creates new global variables for dependent variables.
  //  8. For each function:
  //    8a. Child class performs transformation (processFunction)
  //    8b. Base class clones or remaps types for function
  //    8c. Child class perform post-processing of transformed functions
  //    (postProcessFunction)
  bool run(Module &M);

protected:
  // Derived classes need to implement this method to construct 'llvm::Type'
  // objects for any structures they are directly converting. When new types are
  // created they must be added to the TypeRemapper. Generally, the derived
  // class will create an opaque type within this routine because the structure
  // being converted may contain pointers to other structures that need to be
  // remapped. The body elements of the type will be populated after all types
  // have been created.
  virtual bool prepareTypes(Module &M) = 0;

  // Derived classes need to implement this method to populate the body for any
  // types they are directly converting to contain the body elements of the new
  // type, based on the remapped types returned by calls to the TypeRemapper.
  virtual void populateTypes(Module &M) = 0;

  // Derived classes may implement this to perform module level work that needs
  // to be performed on global variables prior to beginning any function
  // transformation work. For example, creating any new global variables needed
  // for the optimization.
  virtual void prepareModule(Module &M){};

  // Derived classes may implement this method to create the replacement
  // variable for an existing global variable. If a replacement is made, then
  // the new variable must be returned, and the derived class will be
  // responsible for initializing the variable when a call to
  // initializeGlobalVarialbe is made. If the child class does not need to do
  // something specific for replacing the variable, it should return nullptr. An
  // example of the use would be if a global variable is instantiated for a type
  // that is having some field deleted, the base class would not know how to
  // initialize the value of a newly created variable, but the derived class
  // would. In effect, this method is to declaring that the replacement and
  // initialization of some global variable that needs transforming is going to
  // be delegated to the derived class.
  virtual GlobalVariable *createGlobalVariableReplacement(GlobalVariable *GV) {
    return nullptr;
  }

  // Derived class that implement createReplacementGlobalVariable must implement
  // this method to handle the initialization of any GlobalVariable objects the
  // derived class returned within that method
  virtual void initializeGlobalVariableReplacement(GlobalVariable *OrigGV,
                                                   GlobalVariable *NewGV) {
    llvm_unreachable("Global variable replacement must be done by derived "
                     "class implementing createGlobalVariableReplacement");
  }

  // Derived classes may implement this to perform the transformation on a
  // function.
  virtual void processFunction(Function &F) {}

  // Derived class may implement this to perform any work that is needed on
  // the function following all the types being remapped to new types.
  virtual void postprocessFunction(Function &OrigFunc, bool isCloned) {}

  // Sets the body for the all the types in the \p DependentTypeMapping based
  // on types computed by the TypeRemapper.
  void populateDependentTypes(Module &M, TypeToTypeMap &DependentTypeMapping);

private:
  // Identify and create new types for any types the child class is going
  // to replace.
  bool prepareTypesBaseImpl(Module &M);

  // Identify and create types that need to be remapped because due to an
  // existing type that contains a reference to a type being changed by
  // the transformation.
  using TypeDependencyMapping = DenseMap<llvm::Type *, SetVector<Type *>>;
  void buildTypeDependencyMapping(TypeDependencyMapping &TypeToDependentTypes);
  void collectDependenciesForType(Type *Ty, TypeDependencyMapping &Map);

  void collectDependenciesForTypeRecurse(Type *Depender, Type *Ty,
                                         TypeDependencyMapping &Map);

#if !defined(NDEBUG)
  // Debug method to print the type dependency mapping table.
  void dumpTypeDepenencyMapping(TypeDependencyMapping &TypeToDependentTypes);
#endif // !defined(NDEBUG)

  void prepareDependentTypes(Module &M,
                             TypeDependencyMapping &TypeToDependentTypes,
                             TypeToTypeMap &DependentTypeMapping);

  // Identify and clone any function prototypes for functions that will need
  // to be cloned.
  void createCloneFunctionDeclarations(Module &M);

  // Remap global variables that have type changes to their new types
  void convertGlobalVariables(Module &M, ValueMapper &Mapper);

  // Perform all the module and function processing to transform the IR.
  void transformIR(Module &M, ValueMapper &Mapper);

  // Remove functions and global variables that have been completely
  // replaced due to the remapping.
  void removeDeadValues();

  // Given a stack of value-operand pairs representing the use-def chain from
  // a place where a size-multiple value is used, back to the instruction that
  // defines a multiplication by a constant multiple of the size, replace
  // the size constant and clone any intermediate values as needed based on
  // other uses of values in the chain.
  void
  replaceSizeValue(Instruction *I,
                   SmallVectorImpl<std::pair<User *, unsigned>> &SizeUseStack,
                   uint64_t OrigSize, uint64_t ReplSize);

protected:
  // Derived classes may call this function to find and replace the
  // input value to the specified instruction which is a multiple of the
  // original operand size. This function uses the instruction type to
  // determine which operand is expected to be a size operand and then
  // searches the use-def chain of that operand (if necessary) to find
  // a constant value which is a multiple of the alloc size of the original
  // type and replaces it with the same constant multiple of the alloc size
  // of the replacement type. If multiple possible values are found (such
  // as in the case of a calloc instruction whose size and count arguments
  // are both multiples of the original size) only one value will be
  // replaced. If any value in the use-def chain between the instruction and
  // the constant value that is updated has multiple uses, all instructions
  // between the first instruction in the chain with multiple uses and the
  // value being replaced will be cloned.
  //
  // Note: This function assumes that the calls involved are all processing
  // the entire function. Optimizations which use this function should check
  // the MemFuncPartialWrite safety condition.
  void updateCallSizeOperand(Instruction *I, dtrans::CallInfo *CInfo,
                             llvm::Type *OrigTy, llvm::Type *ReplTy);

  // Given a pointer to a sub instruction that is known to subtract two
  // pointers, find all users of the instruction that divide the result by
  // a constant multiple of the original type and replace them with a divide
  // by the a constant that is the same multiple of the replacement type.
  // This function requires that all uses of this instruction be either
  // sdiv or udiv instructions.
  void updatePtrSubDivUserSizeOperand(llvm::BinaryOperator *Sub,
                                      llvm::Type *OrigTy, llvm::Type *ReplTy);

  // Derived classes may use this function to find a constant input value,
  // searching from the specified operand and following the use-def chain
  // as necessary, which is a multiple of the specified size. If such a value
  // is found, the function will return true and the \p UseStack vector will
  // contain the stack of User-Index pairs in the use-def chain which led to
  // the constant. Each entry in the stack represents an instruction and the
  // index of the operand that was followed.
  //
  // If such a value is not found, the function will return false and the
  // \p UseStack vector will not be changed.
  bool findValueMultipleOfSizeInst(
      User *U, unsigned Idx, uint64_t Size,
      SmallVectorImpl<std::pair<User *, unsigned>> &UseStack);

protected:
  DTransAnalysisInfo &DTInfo;
  LLVMContext &Context;
  const DataLayout &DL;

  // Optional string to precede names of dependent types that get renamed.
  std::string DepTypePrefix;

  DTransTypeRemapper *TypeRemapper;
  ValueMaterializer *Materializer;

  // Mapping of original Value* to the replacement Value*. This mapping serves
  // two purposes.
  // 1: It is used by the ValueMapper to lookup whether a replacement for
  //    a value has been defined. Therefore, transformations can set items into
  //    this map prior to running the remapping to get those replacements to
  //    occur. This will be done for things like changing a function call to
  //    instead go to a cloned function.
  //  2: This mapping also gets populated as the replacements are created during
  //     the remapping process. This allows finding what value was used as
  //     the replacement.
  // Initially it will be primed with the global variables and functions that
  // need cloning. As the ValueMapper replaces values those will get inserted.
  ValueToValueMapTy VMap;

  // A mapping from the original function to the clone function that will
  // replace the original function.
  DenseMap<Function *, Function *> OrigFuncToCloneFuncMap;

  // A mapping from the clone function to the original function to enable
  // lookups of the original function based on a clone function pointer.
  DenseMap<Function *, Function *> CloneFuncToOrigFuncMap;

  // List of global variables that are being replaced with variables of the new
  // types due to the type remapping. The variables in this list need to be
  // destroyed once the entire module has been remapped.
  SmallVector<GlobalVariable *, 16> GlobalsForRemoval;
};

} // namespace llvm

#endif // INTEL_OPTIONALCOMPONENTS_INTEL_DTRANS_TRANSFORMS_DTRANSOPTBASE_H
