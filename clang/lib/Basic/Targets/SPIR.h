//===--- SPIR.h - Declare SPIR target feature support -----------*- C++ -*-===//
//
// Part of the LLVM Project, under the Apache License v2.0 with LLVM Exceptions.
// See https://llvm.org/LICENSE.txt for license information.
// SPDX-License-Identifier: Apache-2.0 WITH LLVM-exception
//
//===----------------------------------------------------------------------===//
//
// This file declares SPIR TargetInfo objects.
//
//===----------------------------------------------------------------------===//

#ifndef LLVM_CLANG_LIB_BASIC_TARGETS_SPIR_H
#define LLVM_CLANG_LIB_BASIC_TARGETS_SPIR_H

#include "clang/Basic/TargetInfo.h"
#include "clang/Basic/TargetOptions.h"
#include "llvm/ADT/Triple.h"
#include "llvm/Support/Compiler.h"

namespace clang {
namespace targets {

static const unsigned SPIRAddrSpaceMap[] = {
    0, // Default
    1, // opencl_global
    3, // opencl_local
    2, // opencl_constant
    0, // opencl_private
    4, // opencl_generic
    0, // cuda_device
    0, // cuda_constant
    0  // cuda_shared
};

#if INTEL_COLLAB
static const unsigned SPIRAddrSpaceDefIsGenMap[] = {
    4, // Default
    1, // opencl_global
    3, // opencl_local
    2, // opencl_constant
    0, // opencl_private
    4, // opencl_generic
    0, // cuda_device
    0, // cuda_constant
    0  // cuda_shared
};
#endif // INTEL_COLLAB

class LLVM_LIBRARY_VISIBILITY SPIRTargetInfo : public TargetInfo {
#if INTEL_COLLAB
  bool UseAutoOpenCLAddrSpaceForOpenMP = false;
#endif  // INTEL_COLLAB
public:
  SPIRTargetInfo(const llvm::Triple &Triple, const TargetOptions &)
      : TargetInfo(Triple) {
    assert(getTriple().getOS() == llvm::Triple::UnknownOS &&
           "SPIR target must use unknown OS");
#if INTEL_CUSTOMIZATION
    assert((getTriple().getEnvironment() == llvm::Triple::UnknownEnvironment ||
            getTriple().getEnvironment() == llvm::Triple::IntelFPGA ||
            getTriple().getEnvironment() == llvm::Triple::IntelEyeQ) &&
           "SPIR target must use unknown environment type");
#endif // INTEL_CUSTOMIZATION
    TLSSupported = false;
    VLASupported = false;
    LongWidth = LongAlign = 64;
    AddrSpaceMap = &SPIRAddrSpaceMap;
    UseAddrSpaceMapMangling = true;
    HasLegalHalfType = true;
    HasFloat16 = true;
    // Define available target features
    // These must be defined in sorted order!
    NoAsmVariants = true;
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;

#if INTEL_COLLAB
  void adjust(LangOptions &Opts) override {
    TargetInfo::adjust(Opts);
    if (Opts.OpenMPLateOutline &&
        // FIXME: Temporarily quaery for ENABLE_INFER_AS environment variable.
        //        In the long term we should probably rely on
        //        UseAutoOpenCLAddrSpaceForOpenMP language option.
        //        The check for OpenMPLateOutline is also unnecessary.
        (Opts.UseAutoOpenCLAddrSpaceForOpenMP || getenv("ENABLE_INFER_AS"))) {
      // Use generic address space for all pointers except
      // globals and stack locals.
      Opts.UseAutoOpenCLAddrSpaceForOpenMP = true; // FIXME: remove this
      UseAutoOpenCLAddrSpaceForOpenMP = true;
      AddrSpaceMap = &SPIRAddrSpaceDefIsGenMap;
    }
  }

  llvm::Optional<LangAS> getConstantAddressSpace() const override {
    if (UseAutoOpenCLAddrSpaceForOpenMP)
      // Place constants into a global address space.
      return getLangASFromTargetAS(1);
    return LangAS::Default;
  }
#endif  // INTEL_COLLAB

  bool hasFeature(StringRef Feature) const override {
    return Feature == "spir";
  }

  // SPIR supports the half type and the only llvm intrinsic allowed in SPIR is
  // memcpy as per section 3 of the SPIR spec.
  bool useFP16ConversionIntrinsics() const override { return false; }

  ArrayRef<Builtin::Info> getTargetBuiltins() const override { return None; }

  const char *getClobbers() const override { return ""; }

  ArrayRef<const char *> getGCCRegNames() const override { return None; }

  bool validateAsmConstraint(const char *&Name,
                             TargetInfo::ConstraintInfo &info) const override {
    return true;
  }

  ArrayRef<TargetInfo::GCCRegAlias> getGCCRegAliases() const override {
    return None;
  }

  BuiltinVaListKind getBuiltinVaListKind() const override {
    return TargetInfo::VoidPtrBuiltinVaList;
  }

  CallingConvCheckResult checkCallingConvention(CallingConv CC) const override {
    return (CC == CC_SpirFunction || CC == CC_OpenCLKernel) ? CCCR_OK
                                                            : CCCR_Warning;
  }

  CallingConv getDefaultCallingConv(CallingConvMethodType MT) const override {
    return CC_SpirFunction;
  }

  void setSupportedOpenCLOpts() override {
    // Assume all OpenCL extensions and optional core features are supported
    // for SPIR since it is a generic target.
    getSupportedOpenCLOpts().supportAll();
  }
};
class LLVM_LIBRARY_VISIBILITY SPIR32TargetInfo : public SPIRTargetInfo {
public:
  SPIR32TargetInfo(const llvm::Triple &Triple, const TargetOptions &Opts)
      : SPIRTargetInfo(Triple, Opts) {
    PointerWidth = PointerAlign = 32;
    SizeType = TargetInfo::UnsignedInt;
    PtrDiffType = IntPtrType = TargetInfo::SignedInt;
    resetDataLayout("e-p:32:32-i64:64-v16:16-v24:32-v32:32-v48:64-"
                    "v96:128-v192:256-v256:256-v512:512-v1024:1024");
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;
};

class LLVM_LIBRARY_VISIBILITY SPIR64TargetInfo : public SPIRTargetInfo {
public:
  SPIR64TargetInfo(const llvm::Triple &Triple, const TargetOptions &Opts)
      : SPIRTargetInfo(Triple, Opts) {
    PointerWidth = PointerAlign = 64;
    SizeType = TargetInfo::UnsignedLong;
    PtrDiffType = IntPtrType = TargetInfo::SignedLong;
    resetDataLayout("e-i64:64-v16:16-v24:32-v32:32-v48:64-"
                    "v96:128-v192:256-v256:256-v512:512-v1024:1024");
  }

  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;
};

#if INTEL_CUSTOMIZATION
class LLVM_LIBRARY_VISIBILITY SPIR32INTELFpgaTargetInfo
    : public SPIR32TargetInfo {
  static const Builtin::Info BuiltinInfo[];
public:
  SPIR32INTELFpgaTargetInfo(const llvm::Triple &Triple,
                            const TargetOptions &Opts)
      : SPIR32TargetInfo(Triple, Opts) {}
  ArrayRef<Builtin::Info> getTargetBuiltins() const override;
  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;
};

class LLVM_LIBRARY_VISIBILITY SPIR64INTELFpgaTargetInfo
    : public SPIR64TargetInfo {
  static const Builtin::Info BuiltinInfo[];
public:
  SPIR64INTELFpgaTargetInfo(const llvm::Triple &Triple,
                            const TargetOptions &Opts)
      : SPIR64TargetInfo(Triple, Opts) {}
  ArrayRef<Builtin::Info> getTargetBuiltins() const override;
  void getTargetDefines(const LangOptions &Opts,
                        MacroBuilder &Builder) const override;
};
#endif // INTEL_CUSTOMIZATION
} // namespace targets
} // namespace clang
#endif // LLVM_CLANG_LIB_BASIC_TARGETS_SPIR_H
