; RUN: opt < %s -hir-cost-model-throttling=0 -hir-avr-generate -avr-predicate -debug -debug-only=avr-predicate -S -o /dev/null 2>&1| FileCheck %s
; REQUIRES: asserts

; Verify the AVR predicator: if with short-circuit condition (goto)

; CHECK: Predicator finished:
; CHECK: LOOP
; CHECK: (48) PREDICATE {P48 := }
; CHECK: (4) if /P48/  ((5)EXPR{i1 (6)VALUE{i32 %0} icmp/slt (7)VALUE{i32 101}})   {
; CHECK: (49) PREDICATE {P49 := (48) && (56)EXPR{i1 (54)VALUE{i1 &(5)} icmp/eq (55)VALUE{i1 true}}}
; CHECK: (8) if /P49/  ((9)EXPR{i1 (10)VALUE{i32 %1} icmp/eq (11)VALUE{i32 0}})   {
; CHECK: (50) PREDICATE {P50 := (49) && (59)EXPR{i1 (57)VALUE{i1 &(9)} icmp/eq (58)VALUE{i1 true}}}
; CHECK: (53) PREDICATE {P53 := (49) && (65)EXPR{i1 (63)VALUE{i1 &(9)} icmp/eq (64)VALUE{i1 false}}}
; CHECK: (13) ASSIGN{/P53/ (26)EXPR{i32 (27)VALUE{i32* (%b)[i1]}} = (28)EXPR{i32 store (29)VALUE{i32 5 * %0}}}
; CHECK: (51) PREDICATE {P51 := (48) && (62)EXPR{i1 (60)VALUE{i1 &(5)} icmp/eq (61)VALUE{i1 false}} || (50)}
; CHECK: (15) ASSIGN{/P51/ (30)EXPR{i32 (31)VALUE{i32* (%a)[i1]}} = (32)EXPR{i32 store (33)VALUE{i32 %2 + 5}}}
; CHECK: (52) PREDICATE {P52 := (51) || (53)}
; CHECK: (16) ASSIGN{(34)EXPR{i32 (35)VALUE{i32 %3}} = (36)EXPR{i32 load (37)VALUE{i32* (%c)[i1]}}}
; CHECK: (17) ASSIGN{(38)EXPR{i32 (39)VALUE{i32* (%c)[i1]}} = (40)EXPR{i32 store (41)VALUE{i32 (%N * %3)}}}

; 1. icx test.c -o test_noopt.ll -fopenmp -Qoption,c,-fintel-openmp -O0 -restrict -S -emit-llvm
; 2. opt test_noopt.ll -O2 -debug-pass=Arguments
; 3. opt test_noopt.ll -S -o pred_if_else.ll -loopopt=false (+ all the flags from -O2 from #2, but -VPODirectiveCleanup and -loop-unroll)

; void foo(int * restrict a, int * restrict b, int * restrict c, int N)
; {
;   int i;
; #pragma omp simd
;   for (i = 0; i < 300; i++) {
;     if (b[i] <= 100 && a[i] != 0) 
;       b[i] = b[i] * 5;
;     else
;       a[i] = a[i] + 5;
; 
;     c[i] = c[i] * N;
;   }
; }


; ModuleID = 'pred_if_else_sc_goto_noopt.ll'
source_filename = "pred_if_else_sc_goto.c"
target datalayout = "e-m:e-i64:64-f80:128-n8:16:32:64-S128"
target triple = "x86_64-unknown-linux-gnu"

; Function Attrs: noinline nounwind uwtable
define void @foo(i32* noalias nocapture %a, i32* noalias nocapture %b, i32* noalias nocapture %c, i32 %N) local_unnamed_addr #0 {
entry:
  tail call void @llvm.intel.directive(metadata !"DIR.OMP.SIMD")
  tail call void @llvm.intel.directive(metadata !"DIR.QUAL.LIST.END")
  br label %for.body

for.body:                                         ; preds = %for.inc, %entry
  %indvars.iv = phi i64 [ 0, %entry ], [ %indvars.iv.next, %for.inc ]
  %arrayidx = getelementptr inbounds i32, i32* %b, i64 %indvars.iv
  %0 = load i32, i32* %arrayidx, align 4
  %cmp1 = icmp slt i32 %0, 101
  %arrayidx3 = getelementptr inbounds i32, i32* %a, i64 %indvars.iv
  %1 = load i32, i32* %arrayidx3, align 4
  br i1 %cmp1, label %land.lhs.true, label %if.else

land.lhs.true:                                    ; preds = %for.body
  %cmp4 = icmp eq i32 %1, 0
  br i1 %cmp4, label %if.else, label %if.then

if.then:                                          ; preds = %land.lhs.true
  %mul = mul nsw i32 %0, 5
  store i32 %mul, i32* %arrayidx, align 4
  br label %for.inc

if.else:                                          ; preds = %for.body, %land.lhs.true
  %2 = phi i32 [ 0, %land.lhs.true ], [ %1, %for.body ]
  %add = add nsw i32 %2, 5
  store i32 %add, i32* %arrayidx3, align 4
  br label %for.inc

for.inc:                                          ; preds = %if.then, %if.else
  %arrayidx14 = getelementptr inbounds i32, i32* %c, i64 %indvars.iv
  %3 = load i32, i32* %arrayidx14, align 4
  %mul15 = mul nsw i32 %3, %N
  store i32 %mul15, i32* %arrayidx14, align 4
  %indvars.iv.next = add nuw nsw i64 %indvars.iv, 1
  %exitcond = icmp eq i64 %indvars.iv.next, 300
  br i1 %exitcond, label %for.end, label %for.body

for.end:                                          ; preds = %for.inc
  tail call void @llvm.intel.directive(metadata !"DIR.OMP.END.SIMD")
  tail call void @llvm.intel.directive(metadata !"DIR.QUAL.LIST.END")
  ret void
}

; Function Attrs: argmemonly nounwind
declare void @llvm.intel.directive(metadata) #1

attributes #0 = { noinline nounwind uwtable "disable-tail-calls"="false" "less-precise-fpmad"="false" "no-frame-pointer-elim"="true" "no-frame-pointer-elim-non-leaf" "no-infs-fp-math"="false" "no-jump-tables"="false" "no-nans-fp-math"="false" "stack-protector-buffer-size"="8" "target-cpu"="x86-64" "target-features"="+fxsr,+mmx,+sse,+sse2,+x87" "unsafe-fp-math"="false" "use-soft-float"="false" }
attributes #1 = { argmemonly nounwind }

!llvm.ident = !{!0}

!0 = !{!"clang version 3.9.0 (branches/vpo 20354) (llvm/branches/vpo 20406)"}
