; RUN: llc -mtriple=csa < %s | FileCheck %s --check-prefix=CSA_CHECK 

; ModuleID = 'tools/src/llvm/test/CodeGen/CSA/ALUOps.c'
target datalayout = "e-m:e-i64:64-n32:64"
target triple = "csa"

; Function Attrs: nounwind
define i32 @eqSC(i8 signext %a, i8 signext %b) #0 {
; CSA_CHECK-LABEL: eqSC
; CSA_CHECK: cmpeq32

entry:
  %a.addr = alloca i8, align 1
  %b.addr = alloca i8, align 1
  store i8 %a, i8* %a.addr, align 1
  store i8 %b, i8* %b.addr, align 1
  %0 = load i8, i8* %a.addr, align 1
  %conv = sext i8 %0 to i32
  %1 = load i8, i8* %b.addr, align 1
  %conv1 = sext i8 %1 to i32
  %cmp = icmp eq i32 %conv, %conv1
  %conv2 = zext i1 %cmp to i32
  ret i32 %conv2
}

attributes #0 = { nounwind "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="8" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.ident = !{!0}

!0 = !{!"clang version 3.6.0 (tags/RELEASE_360/final)"}