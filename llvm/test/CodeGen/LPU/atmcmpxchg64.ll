; RUN: llc -mtriple=lpu < %s | FileCheck %s --check-prefix=LPU_CHECK
target datalayout = "e-m:e-i64:64-n32:64"
target triple = "lpu"

; Function Attrs: nounwind
define i64 @f_xchg64(i64* %m, i64 %v, i64 %e) #0 {
; LPU_CHECK-LABEL: f_xchg64
; LPU_CHECK: atmcmpxchg64
entry:
  %0 = cmpxchg i64* %m, i64 %e, i64 %v seq_cst seq_cst
  %1 = extractvalue { i64, i1 } %0, 1
  %conv = zext i1 %1 to i64
  ret i64 %conv
}

