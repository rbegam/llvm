//===-- CSAMCAsmInfo.cpp - CSA asm properties -----------------------===//
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
// This file contains the declarations of the CSAMCAsmInfo properties.
//
//===----------------------------------------------------------------------===//

#include "CSAMCAsmInfo.h"
#include "llvm/ADT/StringRef.h"
#include "../InstPrinter/CSAInstPrinter.h"
using namespace llvm;

void CSAMCAsmInfo::anchor() { }

CSAMCAsmInfo::CSAMCAsmInfo(const Triple &T) {
  CodePointerSize = CalleeSaveStackSlotSize = 8;
  // For now
  HasDotTypeDotSizeDirective = false;
  HasSingleParameterDotFile = false;
  MaxInstLength = 8;
  MinInstAlignment = 8;
  DollarIsPC = true;

  UsesELFSectionDirectiveForBSS = true;

  // For this to work, CSARegisterInfo.td needs dwarf register numbers for
  // registers.  This enables .loc, but it also enables a lot of other things
  // that we have no plans to deal with...
  // SupportsDebugInformation = true;

  UseIntegratedAssembler = true;
}
