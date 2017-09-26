<<<<<<< HEAD
#if INTEL_CUSTOMIZATION
// This entire file is cherry picked from LLVM r31366, as amended by r313684
// and r313784.
// When r313666 is merged with xmain, the INTEL_CUSTOMIZATION of this file 
// can be deleted and the LLVM trunk change followed without modification.
// The test will fail if tested with a 32-bit target triple, but the failure
// will be fixed by r313684.
#endif // INTEL_CUSTOMIZATION
// RUN: %clang_cc1 -S %s -emit-llvm -o - | FileCheck %s
// RUN: %clang_cc1 -S %s -emit-llvm -triple i686-unknown-unknown -o - | FileCheck %s
// RUN: %clang_cc1 -S %s -emit-llvm -triple x86_64-unknown-unknown -o - | FileCheck %s
=======
// RUN: %clang_cc1 -S %s -emit-llvm -o - | FileCheck %s
>>>>>>> cac5bac11b51c77c46ed014d8ceab2473aa6a820

#include <stdint.h>

// This test is meant to verify code that handles the 'p = nullptr + n' idiom
// used by some versions of glibc and gcc.  This is undefined behavior but
// it is intended there to act like a conversion from a pointer-sized integer
// to a pointer, and we would like to tolerate that.

#define NULLPTRI8 ((int8_t*)0)

// This should get the inttoptr instruction.
int8_t *test1(intptr_t n) {
  return NULLPTRI8 + n;
}
// CHECK-LABEL: test1
// CHECK: inttoptr
// CHECK-NOT: getelementptr

<<<<<<< HEAD
// This doesn't meet the idiom because the element type is larger than a byte.
int16_t *test2(intptr_t n) {
  return (int16_t*)0 + n;
=======
// This doesn't meet the idiom because the offset type isn't pointer-sized.
int8_t *test2(int8_t n) {
  return NULLPTRI8 + n;
>>>>>>> cac5bac11b51c77c46ed014d8ceab2473aa6a820
}
// CHECK-LABEL: test2
// CHECK: getelementptr
// CHECK-NOT: inttoptr

<<<<<<< HEAD
// This doesn't meet the idiom because the offset is subtracted.
int8_t* test3(intptr_t n) {
  return NULLPTRI8 - n;
=======
// This doesn't meet the idiom because the element type is larger than a byte.
int16_t *test3(intptr_t n) {
  return (int16_t*)0 + n;
>>>>>>> cac5bac11b51c77c46ed014d8ceab2473aa6a820
}
// CHECK-LABEL: test3
// CHECK: getelementptr
// CHECK-NOT: inttoptr

<<<<<<< HEAD
// This checks the case where the offset isn't pointer-sized.
// The front end will implicitly cast the offset to an integer, so we need to
// make sure that doesn't cause problems on targets where integers and pointers
// are not the same size.
int8_t *test4(int8_t b) {
  return NULLPTRI8 + b;
}
// CHECK-LABEL: test4
// CHECK: inttoptr
// CHECK-NOT: getelementptr
=======
// This doesn't meet the idiom because the offset is subtracted.
int8_t* test4(intptr_t n) {
  return NULLPTRI8 - n;
}
// CHECK-LABEL: test4
// CHECK: getelementptr
// CHECK-NOT: inttoptr
>>>>>>> cac5bac11b51c77c46ed014d8ceab2473aa6a820
