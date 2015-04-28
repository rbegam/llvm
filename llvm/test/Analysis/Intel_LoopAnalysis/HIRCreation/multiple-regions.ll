; RUN: opt < %s -analyze -hir-creation | FileCheck %s

; Check output of hir-creation
; CHECK: BEGIN REGION
; CHECK: for.body4:
; CHECK: goto for.body4;
; CHECK: END REGION
; CHECK: BEGIN REGION
; CHECK: for.body:
; CHECK: goto for.body;
; CHECK: END REGION

; ModuleID = 'loops.c'
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@B = common global [100 x i32] zeroinitializer, align 16
@A = common global [100 x i32] zeroinitializer, align 16

; Function Attrs: nounwind uwtable
define void @foo(i32 %m, i32 %n) #0 {
entry:
  %cmp16 = icmp sgt i32 %n, 0
  br i1 %cmp16, label %for.body.lr.ph, label %for.cond2.preheader

for.body.lr.ph:                                   ; preds = %entry
  %0 = add i32 %n, -1
  br label %for.body

for.cond2.preheader.loopexit:                     ; preds = %for.body
  br label %for.cond2.preheader

for.cond2.preheader:                              ; preds = %for.cond2.preheader.loopexit, %entry
  %cmp314 = icmp sgt i32 %m, 0
  br i1 %cmp314, label %for.body4.lr.ph, label %for.end9

for.body4.lr.ph:                                  ; preds = %for.cond2.preheader
  %1 = add i32 %m, -1
  br label %for.body4

for.body:                                         ; preds = %for.body, %for.body.lr.ph
  %indvars.iv18 = phi i64 [ 0, %for.body.lr.ph ], [ %indvars.iv.next19, %for.body ]
  %arrayidx = getelementptr inbounds [100 x i32]* @B, i64 0, i64 %indvars.iv18
  %2 = load i32* %arrayidx, align 4, !tbaa !1
  %inc = add nsw i32 %2, 1
  store i32 %inc, i32* %arrayidx, align 4, !tbaa !1
  %indvars.iv.next19 = add nuw nsw i64 %indvars.iv18, 1
  %lftr.wideiv20 = trunc i64 %indvars.iv18 to i32
  %exitcond21 = icmp eq i32 %lftr.wideiv20, %0
  br i1 %exitcond21, label %for.cond2.preheader.loopexit, label %for.body

for.body4:                                        ; preds = %for.body4, %for.body4.lr.ph
  %indvars.iv = phi i64 [ 0, %for.body4.lr.ph ], [ %indvars.iv.next, %for.body4 ]
  %arrayidx6 = getelementptr inbounds [100 x i32]* @A, i64 0, i64 %indvars.iv
  store i32 0, i32* %arrayidx6, align 4, !tbaa !1
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %lftr.wideiv = trunc i64 %indvars.iv to i32
  %exitcond = icmp eq i32 %lftr.wideiv, %1
  br i1 %exitcond, label %for.end9.loopexit, label %for.body4

for.end9.loopexit:                                ; preds = %for.body4
  br label %for.end9

for.end9:                                         ; preds = %for.end9.loopexit, %for.cond2.preheader
  ret void
}

attributes #0 = { nounwind uwtable "less-precise-fpmad"="false" "no-frame-pointer-elim"="false" "no-infs-fp-math"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "unsafe-fp-math"="false" "use-soft-float"="false" }

!llvm.ident = !{!0}

!0 = !{!"clang version 3.7.0 (trunk 315) (llvm/branches/loopopt 593)"}
!1 = !{!2, !2, i64 0}
!2 = !{!"int", !3, i64 0}
!3 = !{!"omnipotent char", !4, i64 0}
!4 = !{!"Simple C/C++ TBAA"}

