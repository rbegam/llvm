; RUN: opt < %s -loop-simplify -hir-de-ssa | opt -analyze -hir-parser | FileCheck %s

; Check parsing output for the loop verifying that the select instruction is parsed correctly.
; CHECK: DO i1 = 0, %n + -2
; CHECK-NEXT: %maxval.011.out = %maxval.011
; CHECK-NEXT: %1 = (@x)[0][i1 + 1]
; CHECK-NEXT: %maxval.011 = (%maxval.011.out < %1) ? %1 : %maxval.011
; CHECK-NEXT: END LOOP


; ModuleID = 'kernel24.c'
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@x = common global [1000 x double] zeroinitializer, align 16
@mm = common global i64 0, align 8

; Function Attrs: nounwind uwtable
define void @kernel24(i64 %n) {
entry:
  %0 = load double, double* getelementptr inbounds ([1000 x double], [1000 x double]* @x, i64 0, i64 0), align 16
  %cmp.9 = icmp sgt i64 %n, 1
  br i1 %cmp.9, label %for.body, label %for.end

for.body:                                         ; preds = %entry, %for.body
  %maxval.011 = phi double [ %2, %for.body ], [ %0, %entry ]
  %k.010 = phi i64 [ %inc, %for.body ], [ 1, %entry ]
  %arrayidx = getelementptr inbounds [1000 x double], [1000 x double]* @x, i64 0, i64 %k.010
  %1 = load double, double* %arrayidx, align 8
  %cmp1 = fcmp olt double %maxval.011, %1
  %2 = select i1 %cmp1, double %1, double %maxval.011
  %inc = add nuw nsw i64 %k.010, 1
  %exitcond = icmp eq i64 %inc, %n
  br i1 %exitcond, label %for.end, label %for.body

for.end:                                          ; preds = %for.body, %entry
  %maxval.0.lcssa = phi double [ %0, %entry ], [ %2, %for.body ]
  store double %maxval.0.lcssa, double* getelementptr inbounds ([1000 x double], [1000 x double]* @x, i64 0, i64 0), align 16
  ret void
}

