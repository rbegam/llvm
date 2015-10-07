; Test for General Unrolling with call statement.
; There should not be any unrolling with call stmts in body.

; RUN: opt -loop-simplify -hir-ssa-deconstruction -hir-general-unroll -print-after=hir-general-unroll -HIRCG -S < %s 2>&1 | FileCheck %s 
; HIR Test.
; Region should not be modified.
; CHECK: REGION { }

; Test codegen.
; CHECK: entry
; CHECK-NOT: region

target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@A = common global [1000 x [1000 x i32]] zeroinitializer, align 16

; Function Attrs: nounwind uwtable
define void @foo() {
entry:
  br label %for.cond1.preheader

for.cond1.preheader:                              ; preds = %for.inc6, %entry
  %indvars.iv20 = phi i64 [ 0, %entry ], [ %indvars.iv.next21, %for.inc6 ]
  %0 = trunc i64 %indvars.iv20 to i32
  br label %for.body3

for.body3:                                        ; preds = %for.body3, %for.cond1.preheader
  %indvars.iv = phi i64 [ 0, %for.cond1.preheader ], [ %indvars.iv.next, %for.body3 ]
  %arrayidx5 = getelementptr inbounds [1000 x [1000 x i32]], [1000 x [1000 x i32]]* @A, i64 0, i64 %indvars.iv, i64 %indvars.iv20
  %1 = load i32, i32* %arrayidx5, align 4
  %add = add nsw i32 %1, %0
  %call = tail call i32 @foo1(i32 %add) 
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond = icmp eq i64 %indvars.iv.next, 500
  br i1 %exitcond, label %for.inc6, label %for.body3

for.inc6:                                         ; preds = %for.body3
  %indvars.iv.next21 = add nuw nsw i64 %indvars.iv20, 1
  %exitcond22 = icmp eq i64 %indvars.iv.next21, 500
  br i1 %exitcond22, label %for.end8, label %for.cond1.preheader

for.end8:                                         ; preds = %for.inc6
  %add9 = add nsw i32 %add, 5
  store i32 %add9, i32* getelementptr inbounds ([1000 x [1000 x i32]], [1000 x [1000 x i32]]* @A, i64 0, i64 5, i64 5), align 4
  ret void
}

; Function Attrs: nounwind
declare void @llvm.lifetime.start(i64, i8* nocapture) 

declare i32 @foo1(i32) 

; Function Attrs: nounwind
declare void @llvm.lifetime.end(i64, i8* nocapture) 

