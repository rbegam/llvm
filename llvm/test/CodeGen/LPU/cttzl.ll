; RUN: llc -mtriple=lpu < %s | FileCheck %s --check-prefix=LPU_CHECK
; ModuleID = '<stdin>'
target datalayout = "e-m:e-i64:64-n32:64"
target triple = "lpu"

; Function Attrs: nounwind readnone
define i64 @cttzl(i64 %l) #0 {
; LPU_CHECK-LABEL: cttzl
; LPU_CHECK: cttz64
entry:
  %0 = tail call i64 @llvm.cttz.i64(i64 %l, i1 true)
  ret i64 %0
}

; Function Attrs: nounwind readnone
declare i64 @llvm.cttz.i64(i64, i1) #1

attributes #0 = { nounwind readnone "less-precise-fpmad"="false" "no-frame-pointer-elim"="true" "no-frame-pointer-elim-non-leaf" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind readnone }

!llvm.ident = !{!0}

!0 = !{!"clang version 3.6.0 (tags/RELEASE_360/final)"}
