// RUN: %clang_cc1 -x cl -cl-std=CL2.0 -triple spir-unknown-unknown-intelfpga %s -verify

__kernel void k1(__local  __attribute__((local_mem_size(1042))) int *a ) {
}

__kernel void k2(__global __attribute__((buffer_location("DDR"))) int * a ) {
}

__kernel void k3(__local __attribute__((buffer_location("DDR"))) int *a ) { // expected-warning{{'buffer_location' attribute can be aplied only for parameters that reside in global address space, attribute ignored}}
}

__kernel void k4(__global __attribute__((local_mem_size(1024))) int * a ) { // expected-warning{{'local_mem_size' attribute can be aplied only for parameters that reside in local address space, attribute ignored}}
}

__kernel void k5(write_only pipe int __attribute__((blocking)) a) {
}

__kernel void k6(__local  __attribute__((blocking)) int *a ) { // expected-warning{{'blocking' attribute only applies to OpenCL pipes}}

}

__kernel void k7(__global __attribute__((buffer_location(0))) int *a ) { // expected-error{{'buffer_location' attribute requires a string}}
}

__kernel void k8(__attribute__((buffer_location("DDR"))) int a ) { // expected-warning{{''buffer_location'' only applies to pointer types; type here is Builtin}}
}
