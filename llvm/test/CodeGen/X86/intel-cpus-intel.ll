; INTEL_FEATURE_CPU_GLC
; REQUIRES: intel_feature_cpu_glc
; Test that the CPU names work.
; CHECK-NO-ERROR-NOT: not a recognized processor for this target

; RUN: llc < %s -o /dev/null -mtriple=x86_64-unknown-unknown -mcpu=goldencove 2>&1 | FileCheck %s --check-prefix=CHECK-NO-ERROR --allow-empty

; end INTEL_FEATURE_CPU_GLC
