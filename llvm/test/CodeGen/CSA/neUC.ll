; RUN: llc -mtriple=csa < %s | FileCheck %s --check-prefix=CSA_CHECK 

; ModuleID = 'tools/src/llvm/test/CodeGen/CSA/ALUOps.c'
target datalayout = "e-m:e-i64:64-n32:64"
target triple = "csa"

; Function Attrs: nounwind
define i32 @neUC(i8 zeroext %a, i32 %b) #0 {
; CSA_CHECK-LABEL: neUC
; CSA_CHECK: cmpne32

entry:
  %a.addr = alloca i8, align 1
  %b.addr = alloca i32, align 4
  store i8 %a, i8* %a.addr, align 1
  store i32 %b, i32* %b.addr, align 4
  %0 = load i8, i8* %a.addr, align 1
  %conv = zext i8 %0 to i32
  %1 = load i32, i32* %b.addr, align 4
  %cmp = icmp ne i32 %conv, %1
  %conv1 = zext i1 %cmp to i32
  ret i32 %conv1
}

attributes #0 = { nounwind "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-realign-stack" "stack-protector-buffer-size"="8" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.ident = !{!0}

!0 = !{!"clang version 3.6.0 (tags/RELEASE_360/final)"}