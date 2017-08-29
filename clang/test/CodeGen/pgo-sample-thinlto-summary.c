<<<<<<< HEAD
// INTEL -- xmain inlining logic prefers cloning foo() to inlining, so this test
//          fails.  To work around that, disable xmain-specific inlining logic.
// RUN: %clang_cc1 -O2 -fprofile-sample-use=%S/Inputs/pgo-sample-thinlto-summary.prof %s -emit-llvm -mllvm -inline-for-xmain=0 -o - 2>&1 | FileCheck %s -check-prefix=O2
// RUN: %clang_cc1 -O2 -fprofile-sample-use=%S/Inputs/pgo-sample-thinlto-summary.prof %s -emit-llvm -mllvm -inline-for-xmain=0 -flto=thin -o - 2>&1 | FileCheck %s -check-prefix=THINLTO
=======
// RUN: %clang_cc1 -O2 -fprofile-sample-use=%S/Inputs/pgo-sample-thinlto-summary.prof %s -emit-llvm -o - 2>&1 | FileCheck %s -check-prefix=SAMPLEPGO
// RUN: %clang_cc1 -O2 -fprofile-sample-use=%S/Inputs/pgo-sample-thinlto-summary.prof %s -emit-llvm -flto=thin -o - 2>&1 | FileCheck %s -check-prefix=THINLTO
>>>>>>> dcdc5a7d3cbec7c4e0aaccf9e2adfa9c4ae66b4a
// Checks if hot call is inlined by normal compile, but not inlined by
// thinlto compile.

int baz(int);
int g;

void foo(int n) {
  for (int i = 0; i < n; i++)
    g += baz(i);
}

// SAMPLEPGO-LABEL: define void @bar
// THINLTO-LABEL: define void @bar
// SAMPLEPGO-NOT: call{{.*}}foo
// THINLTO: call{{.*}}foo
void bar(int n) {
  for (int i = 0; i < n; i++)
    foo(i);
}

// Checks if loop unroll is invoked by normal compile, but not thinlto compile.
// SAMPLEPGO-LABEL: define void @unroll
// THINLTO-LABEL: define void @unroll
// SAMPLEPGO: call{{.*}}baz
// SAMPLEPGO: call{{.*}}baz
// THINLTO: call{{.*}}baz
// THINLTO-NOT: call{{.*}}baz
void unroll() {
  for (int i = 0; i < 2; i++)
    baz(i);
}

// Checks that icp is not invoked for ThinLTO, but invoked for normal samplepgo.
// SAMPLEPGO-LABEL: define void @icp
// THINLTO-LABEL: define void @icp
// SAMPLEPGO: if.true.direct_targ
// ThinLTO-NOT: if.true.direct_targ
void icp(void (*p)()) {
  p();
}
