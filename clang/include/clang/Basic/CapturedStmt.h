//===--- CapturedStmt.h - Types for CapturedStmts ---------------*- C++ -*-===//
//
//                     The LLVM Compiler Infrastructure
//
// This file is distributed under the University of Illinois Open Source
// License. See LICENSE.TXT for details.
//
//===----------------------------------------------------------------------===//


#ifndef LLVM_CLANG_BASIC_CAPTUREDSTMT_H
#define LLVM_CLANG_BASIC_CAPTUREDSTMT_H

namespace clang {

/// \brief The different kinds of captured statement.
enum CapturedRegionKind {
  CR_Default,
#ifdef INTEL_CUSTOMIZATION  
  CR_CilkSpawn,
  CR_CilkFor,
  CR_SIMDFor,
#endif  
  CR_OpenMP
};

} // end namespace clang

#endif // LLVM_CLANG_BASIC_CAPTUREDSTMT_H
