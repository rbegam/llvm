// RUN: %clang_cc1 -x cl -cl-std=CL2.0 -triple spir-unknown-unknown -fsyntax-only -verify %s
// RUN: %clang_cc1 -x cl -triple spir-unknown-unknown -fsyntax-only -verify %s

#pragma OPENCL EXTENSION cl_intel_channels : enable

struct st {
  int i1;
  int i2;
};

channel int ich __attribute__((io("eth0"))) __attribute__((depth(-1))); // expected-warning{{'depth' attribute parameter 0 is negative and will be ignored}}
channel long lch __attribute__((depth(3)));
channel struct st sch __attribute__((depth(0)));

channel int arr[5] __attribute__((io("eth0")));
channel int multiarr[2][7] __attribute__((depth(0)));

__constant int a1 __attribute((depth(3))) = 2; // expected-warning{{'depth' attribute only applies to OpenCL channels}}
__constant long a2 __attribute((io("eth1"))) = 2; // expected-warning{{'io' attribute only applies to OpenCL channels}}

__kernel void k1() __attribute__((depth(100))) { // expected-warning{{'depth' attribute only applies to parameters and global variables}}
}

__kernel void k2() {
  int i1 = read_channel_intel(arr[1]);
  int i2 = read_channel_intel(multiarr[1][2]);
}

channel foo; // expected-error{{missing actual type specifier for channel}}

struct incomplete;

channel struct incomplete ch_arr[10]; // expected-error{{array has incomplete element type '__global channel struct incomplete'}}
channel struct incomplete ch; // expected-error{{tentative definition has type '__global channel struct incomplete' that is never completed}}
