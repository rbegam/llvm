; Check HIR parsing of cases with undefined values in CanonExpr (undef * undef mul)
; |   (%A)[0][i1] = 5 * i1 + undef * undef + 1;
; |   <REG> (LINEAR [5 x i32]* %A)[0][LINEAR i64 i1] {sb:0}
; |   <BLOB> LINEAR [5 x i32]* %A {sb:12}
; |   <REG> NON-LINEAR i32 5 * i1 + undef * undef + 1 {undefined} {sb:9}
; |   <BLOB> LINEAR i32 undef {undefined} {sb:13}
; |   <BLOB> NON-LINEAR i32 %0 {sb:5}

; RUN: opt < %s -loop-rotate | opt -analyze -hir-parser -hir-details | FileCheck %s

; CHECK: ={{.*}}undef * undef{{.*}};
; CHECK: <REG>{{.*}}undef * undef{{.*}} {undefined}
; CHECK: <BLOB> LINEAR {{.*}} undef {undefined}

; ModuleID = '2.ll'
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@x = common global i32 0, align 4

; Function Attrs: nounwind uwtable
define i32 @main() #0 {
entry:
  %A = alloca [5 x i32], align 16
  br label %for.cond

for.cond:                                         ; preds = %for.inc, %entry
  %i.0 = phi i32 [ 0, %entry ], [ %inc, %for.inc ]
  %cmp = icmp slt i32 %i.0, 5
  br i1 %cmp, label %for.body, label %for.end

for.body:                                         ; preds = %for.cond
  %mul = mul nsw i32 5, %i.0
  %0 = load i32, i32* @x, align 4
  %mul1 = mul nsw i32 undef, undef
  %add = add nsw i32 %mul, %mul1
  %add2 = add nsw i32 %add, 1 
  %idxprom = sext i32 %i.0 to i64
  %arrayidx = getelementptr inbounds [5 x i32], [5 x i32]* %A, i32 0, i64 %idxprom
  store i32 %add2, i32* %arrayidx, align 4
  br label %for.inc

for.inc:                                          ; preds = %for.body
  %inc = add nsw i32 %i.0, 1
  br label %for.cond

for.end:                                          ; preds = %for.cond
  %arrayidx3 = getelementptr inbounds [5 x i32], [5 x i32]* %A, i32 0, i64 0
  %1 = load i32, i32* %arrayidx3, align 4
  ret i32 %1
}

