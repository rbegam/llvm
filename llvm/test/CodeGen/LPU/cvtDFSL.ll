; RUN: llc -mtriple=lpu < %s | FileCheck %s --check-prefix=LPU_CHECK 

; ModuleID = 'tools/src/llvm/test/CodeGen/LPU/ALUOps.c'
target datalayout = "e-m:e-i64:64-n32:64"
target triple = "lpu"

; Function Attrs: nounwind
define double @cvtDFSL(i64 %l) #0 {
; LPU_CHECK-LABEL: cvtDFSL
; LPU_CHECK: cvtf64s64

entry:
  %l.addr = alloca i64, align 8
  store i64 %l, i64* %l.addr, align 8
  %0 = load i64* %l.addr, align 8
  %conv = sitofp i64 %0 to double
  ret double %conv
}

attributes #0 = { nounwind "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="8" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.ident = !{!0}

!0 = !{!"clang version 3.6.0 (tags/RELEASE_360/final)"}