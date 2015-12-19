; RUN: opt < %s -loop-simplify -hir-ssa-deconstruction | opt -analyze -hir-parser | FileCheck %s

; This command checks that -hir-ssa-deconstruction invalidates SCEV so that the parser doesn't pick up the cached version. HIR output should be the same as for the above command.
; RUN: opt < %s -loop-simplify -hir-ssa-deconstruction -hir-complete-unroll -print-before=hir-complete-unroll 2>&1 | FileCheck %s

; Check parsing output for the loop
; CHECK: DO i1 = 0, zext.i32.i64((-1 + %n))
; CHECK-SAME: DO_LOOP
; CHECK-NEXT: %a.addr.014.out = %a.addr.014
; CHECK-NEXT: %output.1 = %b
; CHECK-NEXT: i1 > 77
; CHECK: (%A)[i1] = %a.addr.014
; CHECK-NEXT: %output.1 = %a.addr.014.out
; CHECK: (%B)[i1] = %output.1
; CHECK-NEXT: END LOOP


; ModuleID = 'de_ssa1.c'
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Function Attrs: nounwind uwtable
define i32 @foo(i32* nocapture %A, i32* nocapture %B, i32 %a, i32 %b, i32 %n) {
entry:
  %cmp13 = icmp sgt i32 %n, 0
  br i1 %cmp13, label %for.body, label %for.end

for.body:                                         ; preds = %entry, %if.end
  %indvars.iv = phi i64 [ %indvars.iv.next, %if.end ], [ 0, %entry ]
  %a.addr.014 = phi i32 [ %a.addr.1, %if.end ], [ %a, %entry ]
  %cmp1 = icmp sgt i64 %indvars.iv, 77
  br i1 %cmp1, label %if.then, label %if.end

if.then:                                          ; preds = %for.body
  %inc = add nsw i32 %a.addr.014, 1
  %arrayidx = getelementptr inbounds i32, i32* %A, i64 %indvars.iv
  store i32 %inc, i32* %arrayidx, align 4 
  br label %if.end

if.end:                                           ; preds = %for.body, %if.then
  %a.addr.1 = phi i32 [ %inc, %if.then ], [ %a.addr.014, %for.body ]
  %output.1 = phi i32 [ %a.addr.014, %if.then ], [ %b, %for.body ]
  %arrayidx3 = getelementptr inbounds i32, i32* %B, i64 %indvars.iv
  store i32 %output.1, i32* %arrayidx3, align 4 
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %lftr.wideiv = trunc i64 %indvars.iv.next to i32
  %exitcond = icmp eq i32 %lftr.wideiv, %n
  br i1 %exitcond, label %for.end, label %for.body

for.end:                                          ; preds = %if.end, %entry
  %output.0.lcssa = phi i32 [ -1, %entry ], [ %output.1, %if.end ]
  ret i32 %output.0.lcssa
}

