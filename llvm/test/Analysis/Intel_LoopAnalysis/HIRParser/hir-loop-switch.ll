; RUN: opt < %s -loop-simplify -hir-ssa-deconstruction | opt -analyze -hir-parser | FileCheck %s

; Check parsing output for the loop verifying that the switch is parsed correctly.
; CHECK: DO i1 = 0, %n + -1
; CHECK-NEXT: switch(%c) 
; CHECK-NEXT: { 
; CHECK-NEXT: case 0:
; CHECK-NEXT: %call = @printf(&((@.str)[0][0]))
; CHECK-NEXT: goto sw.bb1
; CHECK-NEXT: case 1:
; CHECK-NEXT: sw.bb1:
; CHECK-NEXT: %call2 = @printf(&((@.str1)[0][0]))
; CHECK-NEXT: break
; CHECK-NEXT: default:
; CHECK-NEXT: %call3 = @printf(&((@.str2)[0][0]))
; CHECK-NEXT: break
; CHECK-NEXT: }
; CHECK-NEXT: END LOOP

; ModuleID = 'loop-switch.c'
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

@.str = private unnamed_addr constant [5 x i8] c"zero\00", align 1
@.str1 = private unnamed_addr constant [4 x i8] c"one\00", align 1
@.str2 = private unnamed_addr constant [8 x i8] c"default\00", align 1

; Function Attrs: nounwind uwtable
define void @foo(i32 %c, i32 %n) {
entry:
  %cmp5 = icmp sgt i32 %n, 0
  br i1 %cmp5, label %for.body, label %for.end

for.body:                                         ; preds = %entry, %for.inc
  %i.06 = phi i32 [ %inc, %for.inc ], [ 0, %entry ]
  switch i32 %c, label %sw.default [
    i32 0, label %sw.bb
    i32 1, label %sw.bb1
  ]

sw.bb:                                            ; preds = %for.body
  %call = tail call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([5 x i8], [5 x i8]* @.str, i64 0, i64 0)) #2
  br label %sw.bb1

sw.bb1:                                           ; preds = %for.body, %sw.bb
  %call2 = tail call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([4 x i8], [4 x i8]* @.str1, i64 0, i64 0)) #2
  br label %for.inc

sw.default:                                       ; preds = %for.body
  %call3 = tail call i32 (i8*, ...) @printf(i8* getelementptr inbounds ([8 x i8], [8 x i8]* @.str2, i64 0, i64 0)) #2
  br label %for.inc

for.inc:                                          ; preds = %sw.bb1, %sw.default
  %inc = add nuw nsw i32 %i.06, 1
  %exitcond = icmp eq i32 %inc, %n
  br i1 %exitcond, label %for.end, label %for.body

for.end:                                          ; preds = %for.inc, %entry
  ret void
}

declare i32 @printf(i8* nocapture readonly, ...)


