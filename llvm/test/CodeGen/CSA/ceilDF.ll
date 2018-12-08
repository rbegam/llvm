; RUN: llc -mtriple=csa < %s | FileCheck %s --check-prefix=CSA_CHECK
; RUN: llc -mtriple=csa -csa-force-math0-instructions < %s | FileCheck %s --check-prefix=CSA_CHECK_NOMATHLIB
; Math library calls in lit tests are temporarily broken because they can't be
; lowered to calls early. Re-enable these tests when this issue is fixed.
; XFAIL: *

; ModuleID = 'MathOps.c'
target datalayout = "e-m:e-i64:64-n32:64"
target triple = "csa"

; Function Attrs: nounwind readnone
define double @ceilDF(double %x) local_unnamed_addr #0 {
; CSA_CHECK-label: ceilDF
; CSA_CHECK: .call ceil
; CSA_CHECK_NOMATHLIB: ceilf64

entry:
  %call = tail call double @ceil(double %x) #2
  ret double %call
}

; Function Attrs: nounwind readnone
declare double @ceil(double) local_unnamed_addr #1

attributes #0 = { nounwind readnone "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="true" "no-frame-pointer-elim-non-leaf" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { nounwind readnone "correctly-rounded-divide-sqrt-fp-math"="false" "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="true" "no-frame-pointer-elim-non-leaf" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "no-signed-zeros-fp-math"="false" "no-trapping-math"="false" "stack-protector-buffer-size"="8" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #2 = { nounwind readnone }

!llvm.ident = !{!0}

!0 = !{!"clang version 4.0.0 "}
