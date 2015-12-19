; RUN: opt < %s -loop-simplify -hir-ssa-deconstruction | opt -analyze -hir-parser -hir-details | FileCheck %s

; Check that the upper for i2 loop is parsed as linear defined at level 1.
; CHECK: DO i64 i2 = 0, zext.i32.i64((-1 + %0))
; CHECK-NEXT: <RVAL-REG> LINEAR i64 zext.i32.i64((-1 + %0)){def@1}
; CHECK-NEXT: <BLOB> LINEAR i32 %0{def@1}


; ModuleID = 'upper1.c'
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Function Attrs: nounwind uwtable
define void @foo(i32* nocapture readonly %A, i32* nocapture %B, i32 %n) {
entry:
  %cmp.19 = icmp sgt i32 %n, 0
  br i1 %cmp.19, label %for.body, label %for.end.9

for.body:                                         ; preds = %entry, %for.end
  %indvars.iv21 = phi i64 [ %indvars.iv.next22, %for.end ], [ 0, %entry ]
  %arrayidx = getelementptr inbounds i32, i32* %A, i64 %indvars.iv21
  %0 = load i32, i32* %arrayidx, align 4
  %cmp2.17 = icmp sgt i32 %0, 0
  br i1 %cmp2.17, label %for.body.3, label %for.end

for.body.3:                                       ; preds = %for.body, %for.body.3
  %indvars.iv = phi i64 [ %indvars.iv.next, %for.body.3 ], [ 0, %for.body ]
  %arrayidx5 = getelementptr inbounds i32, i32* %B, i64 %indvars.iv
  %1 = load i32, i32* %arrayidx5, align 4
  %inc = add nsw i32 %1, 1
  store i32 %inc, i32* %arrayidx5, align 4
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %lftr.wideiv = trunc i64 %indvars.iv.next to i32
  %exitcond = icmp eq i32 %lftr.wideiv, %0
  br i1 %exitcond, label %for.end, label %for.body.3

for.end:                                          ; preds = %for.body.3, %for.body
  %indvars.iv.next22 = add nuw nsw i64 %indvars.iv21, 1
  %lftr.wideiv23 = trunc i64 %indvars.iv.next22 to i32
  %exitcond24 = icmp eq i32 %lftr.wideiv23, %n
  br i1 %exitcond24, label %for.end.9, label %for.body

for.end.9:                                        ; preds = %for.end, %entry
  ret void
}

