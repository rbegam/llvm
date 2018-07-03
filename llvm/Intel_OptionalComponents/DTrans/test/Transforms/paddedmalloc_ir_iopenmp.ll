; Test that identifies if the DTrans padded malloc optimization built the
; global counter and the interface correctly, and modified the malloc
; function successfully when -fiopenmp is used.

; RUN: opt < %s -vpo-paropt -dtrans-paddedmalloc -S 2>&1 | FileCheck %s

%struct.testStruct = type { i8* }

@globalstruct = internal global %struct.testStruct zeroinitializer, align 8
@arr1 = internal global [10 x i32] zeroinitializer, align 16
@arr2 = internal global [10 x i32] zeroinitializer, align 16

declare noalias i8* @malloc(i64)

declare token @llvm.directive.region.entry()
declare void @llvm.directive.region.exit(token)

declare void @free(i8* nocapture)

; Malloc function
define internal noalias i8* @mallocFunc(i64) {
  %2 = tail call noalias i8* @malloc(i64 %0)
  ret i8* %2
}

; Search loop
define internal zeroext i1 @searchloop() #5 {
  br label %3

; <label>:1:                                      ; preds = %3
  %2 = icmp ult i64 %10, 10
  br i1 %2, label %3, label %11

; <label>:3:                                      ; preds = %1, %0
  %4 = phi i64 [ 0, %0 ], [ %10, %1 ]
  %5 = getelementptr inbounds [10 x i32], [10 x i32]* @arr1, i64 0, i64 %4
  %6 = load i32, i32* %5, align 4
  %7 = getelementptr inbounds [10 x i32], [10 x i32]* @arr2, i64 0, i64 %4
  %8 = load i32, i32* %7, align 4
  %9 = icmp eq i32 %6, %8
  %10 = add nuw nsw i64 %4, 1
  br i1 %9, label %11, label %1

; <label>:11:                                     ; preds = %3, %1
  %12 = phi i1 [ true, %3 ], [ false, %1 ]
  ret i1 %12
}

define i32 @main() {

  %1 = call token @llvm.directive.region.entry() [ "DIR.OMP.PARALLEL"() ]
  br label %DIR.OMP.PARALLEL.START

DIR.OMP.PARALLEL.START:
  %2 = tail call noalias i8* @mallocFunc(i64 100)
  store i8* %2, i8** getelementptr inbounds (%struct.testStruct,
    %struct.testStruct* @globalstruct, i64 0, i32 0), align 8
  tail call void @free(i8* %2)
  store i8* null, i8** getelementptr inbounds (%struct.testStruct,
    %struct.testStruct* @globalstruct, i64 0, i32 0), align 8
  call zeroext i1 @searchloop()
  br label %DIR.OMP.END.PARALLEL.EXIT

DIR.OMP.END.PARALLEL.EXIT:
  call void @llvm.directive.region.exit(token %1) [ "DIR.OMP.END.PARALLEL"() ]
  ret i32 0
}

; Verify that the counter was set correctly
; CHECK-LABEL: define internal noalias i8* @mallocFunc(i64) {
; CHECK:   %2 = load atomic i32, i32* @PaddedMallocCounter seq_cst, align 4
; CHECK:   %3 = icmp ult i32 %2, 250
; CHECK:   br i1 %3, label %BBif, label %BBelse
;
; CHECK-LABEL: BBif:                                             ; preds = %1
; CHECK:   %4 = add i64 %0, 32
; CHECK:   %5 = tail call noalias i8* @malloc(i64 %4)
; CHECK:   %6 = atomicrmw add i32* @PaddedMallocCounter, i32 1 seq_cst
; CHECK:   br label %8
;
; CHECK-LABEL: BBelse:                                           ; preds = %1
; CHECK:   %7 = tail call noalias i8* @malloc(i64 %0)
; CHECK:   br label %8
;
; CHECK-LABEL: ; <label>:8:                                      ; preds = %BBelse, %BBif
; CHECK:   %9 = phi i8* [ %5, %BBif ], [ %7, %BBelse ]
; CHECK:   ret i8* %9
; CHECK: }

; Verify that the __kmpc functions were created
; CHECK-LABEL: define i32 @main() {
; CHECK:   %1 = alloca i32
; CHECK:   store i32 0, i32* %1
; CHECK:   %tid.val = tail call i32 @__kmpc_global_thread_num({ i32, i32, i32, i32, i8* }* @.kmpc_loc.0.0.4)
; CHECK:   %2 = alloca i32
; CHECK:   store i32 %tid.val, i32* %2
; CHECK:   br label %codeRepl
;
; CHECK-LABEL: codeRepl:                                         ; preds = %0
; CHECK:   %fork.test = tail call i32 @__kmpc_ok_to_fork({ i32, i32, i32, i32, i8* }* @.kmpc_loc.0.0.2)
; CHECK:   %fork.test2 = icmp ne i32 %fork.test, 0
; CHECK:   br i1 %fork.test2, label %if.then.fork.1, label %if.else.call.1
;
; CHECK-LABEL: if.then.fork.1:                                   ; preds = %codeRepl
; CHECK:   call void ({ i32, i32, i32, i32, i8* }*, i32, void (i32*, i32*, ...)*, ...) @__kmpc_fork_call({ i32, i32, i32, i32, i8* }* @.kmpc_loc.0.0, i32 0, void (i32*, i32*, ...)* bitcast (void (i32*, i32*)* @main_.split to void (i32*, i32*, ...)*))
; CHECK:   br label %codeRepl.split
;
; CHECK-LABEL: if.else.call.1:                                   ; preds = %codeRepl
; CHECK:   call void @main_.split(i32* %2, i32* %1)
; CHECK:   br label %codeRepl.split
;
; CHECK-LABEL: codeRepl.split:                                   ; preds = %if.then.fork.1, %if.else.call.1
; CHECK:   br label %DIR.OMP.END.PARALLEL.EXIT.ret
;
; CHECK-LABEL: DIR.OMP.END.PARALLEL.EXIT.ret:                    ; preds = %codeRepl.split
; CHECK:   ret i32 0
; CHECK: }

; Verify that the outline function was created
; CHECK-LABEL: define internal void @main_.split(i32* %tid, i32* %bid) #1 {
; CHECK-LABEL: newFuncRoot:
; CHECK:   br label %.split
;
; CHECK-LABEL: DIR.OMP.END.PARALLEL.EXIT.ret.exitStub:           ; preds = %DIR.OMP.END.PARALLEL.EXIT
; CHECK:   ret void
;
; CHECK-LABEL: DIR.OMP.PARALLEL.START:                           ; preds = %.split
; CHECK:   %0 = tail call noalias i8* @mallocFunc(i64 100)
; CHECK:   store i8* %0, i8** getelementptr inbounds (%struct.testStruct, %struct.testStruct* @globalstruct, i64 0, i32 0), align 8
; CHECK:   tail call void @free(i8* %0)
; CHECK:   store i8* null, i8** getelementptr inbounds (%struct.testStruct, %struct.testStruct* @globalstruct, i64 0, i32 0), align 8
; CHECK:   %1 = call zeroext i1 @searchloop()
; CHECK:   br label %DIR.OMP.END.PARALLEL.EXIT
;
; CHECK-LABEL: DIR.OMP.END.PARALLEL.EXIT:                        ; preds = %DIR.OMP.PARALLEL.START
; CHECK:   br label %DIR.OMP.END.PARALLEL.EXIT.ret.exitStub
;
; CHECK-LABEL: .split:                                           ; preds = %newFuncRoot
; CHECK:   br label %DIR.OMP.PARALLEL.START
; CHECK: }

; Verify that the interface was created
; CHECK-LABEL: define i1 @PaddedMallocInterface() {
; CHECK-LABEL: entry:
; CHECK:   %0 = load i32, i32* @PaddedMallocCounter
; CHECK:   %1 = icmp ult i32 %0, 250
; CHECK:   ret i1 %1
; CHECK: }

