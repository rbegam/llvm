// RUN: %clang_cc1 -x cl -triple spir-unknown-unknown-intelfpga -disable-llvm-passes -emit-llvm %s -o - | FileCheck %s
// RUN: %clang_cc1 -x cl -triple x86_64-unknown-unknown-intelfpga -disable-llvm-passes -emit-llvm %s -o - | FileCheck %s

//CHECK: [[ANN2:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{numbanks:4}{bank_bits:4,5}
//CHECK: [[ANN3:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{numreadports:2}{numwriteports:3}
//CHECK: [[ANN4:@.str[\.]*[0-9]*]] = {{.*}}{register:1}
//CHECK: [[ANN5:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}
//CHECK: [[ANN6:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{bankwidth:4}
//CHECK: [[ANN6A:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{max_concurrency:4}
//CHECK: [[ANN7:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{pump:1}
//CHECK: [[ANN8:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{pump:2}
//CHECK: [[ANN9:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{merge:foo:depth}
//CHECK: [[ANN10:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{merge:bar:width}
//CHECK: [[ANN11:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{internal_max_block_ram_depth:32}
//CHECK: [[ANN12:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{optimize_fmax:1}
//CHECK: [[ANN13:@.str[\.]*[0-9]*]] = {{.*}}{memory:DEFAULT}{optimize_ram_usage:1}

//__attribute__((ihc_component))
void foo_two() {
  //CHECK: %[[VAR_TWO:[0-9]+]] = bitcast{{.*}}var_two
  //CHECK: %[[VAR_TWO1:var_two[0-9]+]] = bitcast{{.*}}var_two
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_TWO1]],{{.*}}[[ANN2]]
  int __attribute__((bank_bits(4,5))) var_two;
  //CHECK: %[[VAR_THREE:[0-9]+]] = bitcast{{.*}}var_three
  //CHECK: %[[VAR_THREE1:var_three[0-9]+]] = bitcast{{.*}}var_three
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_THREE1]],{{.*}}[[ANN2]]
  int __attribute__((numbanks(4),bank_bits(4,5))) var_three;
  //CHECK: %[[VAR_FOUR:[0-9]+]] = bitcast{{.*}}var_four
  //CHECK: %[[VAR_FOUR1:var_four[0-9]+]] = bitcast{{.*}}var_four
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_FOUR1]],{{.*}}[[ANN2]]
  int __attribute__((bank_bits(4,5),numbanks(4))) var_four;
  //CHECK: %[[VAR_FIVE:[0-9]+]] = bitcast{{.*}}var_five
  //CHECK: %[[VAR_FIVE1:var_five[0-9]+]] = bitcast{{.*}}var_five
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_FIVE1]],{{.*}}[[ANN3]]
  int __attribute__((numports_readonly_writeonly(2,3))) var_five;
  //CHECK: %[[VAR_SIX:[0-9]+]] = bitcast{{.*}}var_six
  //CHECK: %[[VAR_SIX1:var_six[0-9]+]] = bitcast{{.*}}var_six
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_SIX1]],{{.*}}[[ANN3]]
  int __attribute__((numreadports(2),numwriteports(3))) var_six;
  //CHECK: %[[VAR_SEVEN:[0-9]+]] = bitcast{{.*}}var_seven
  //CHECK: %[[VAR_SEVEN1:var_seven[0-9]+]] = bitcast{{.*}}var_seven
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_SEVEN1]],{{.*}}[[ANN4]]
  int __attribute__((register)) var_seven;
  //CHECK: %[[VAR_EIGHT:[0-9]+]] = bitcast{{.*}}var_eight
  //CHECK: %[[VAR_EIGHT1:var_eight[0-9]+]] = bitcast{{.*}}var_eight
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_EIGHT1]],{{.*}}[[ANN5]]
  int __attribute__((__memory__)) var_eight;
  //CHECK: %[[VAR_NINE:[0-9]+]] = bitcast{{.*}}var_nine
  //CHECK: %[[VAR_NINE1:var_nine[0-9]+]] = bitcast{{.*}}var_nine
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_NINE1]],{{.*}}[[ANN6]]
  int __attribute__((__bankwidth__(4))) var_nine;
  //CHECK: %[[VAR_NINE_TWO:[0-9]+]] = bitcast{{.*}}var_nine_two
  //CHECK: %[[VAR_NINE_TWO1:var_nine_two[0-9]+]] = bitcast{{.*}}var_nine_two
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_NINE_TWO1]],{{.*}}[[ANN6A]]
  int __attribute__((__max_concurrency__(4))) var_nine_two;
  //CHECK: %[[VAR_TEN:[0-9]+]] = bitcast{{.*}}var_ten
  //CHECK: %[[VAR_TEN1:var_ten[0-9]+]] = bitcast{{.*}}var_ten
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_TEN1]],{{.*}}[[ANN7]]
  int __attribute__((singlepump)) var_ten;
  //CHECK: %[[VAR_ELEVEN:[0-9]+]] = bitcast{{.*}}var_eleven
  //CHECK: %[[VAR_ELEVEN1:var_eleven[0-9]+]] = bitcast{{.*}}var_eleven
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_ELEVEN1]],{{.*}}[[ANN8]]
  int __attribute__((doublepump)) var_eleven;
  //CHECK: %[[VAR_TWELVE:[0-9]+]] = bitcast{{.*}}var_twelve
  //CHECK: %[[VAR_TWELVE1:var_twelve[0-9]+]] = bitcast{{.*}}var_twelve
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_TWELVE1]],{{.*}}[[ANN9]]
  int __attribute__((merge("foo","depth"))) var_twelve;
  //CHECK: %[[VAR_THIRTEEN:[0-9]+]] = bitcast{{.*}}var_thirteen
  //CHECK: %[[VAR_THIRTEEN1:var_thirteen[0-9]+]] = bitcast{{.*}}var_thirteen
  //CHECK: llvm.var.annotation{{.*}}%[[VAR_THIRTEEN1]],{{.*}}[[ANN10]]
  int __attribute__((merge("bar","width"))) var_thirteen;
}

struct foo_three{
  int __attribute__((bank_bits(4,5))) f1;
  int __attribute__((numbanks(4),bank_bits(4,5))) f2;
  int __attribute__((numports_readonly_writeonly(2,3))) f3;
  int __attribute__((numreadports(2),numwriteports(3))) f4;
  int __attribute__((register)) f5;
  int __attribute__((__memory__)) f6;
  int __attribute__((__bankwidth__(4))) f7;
  int __attribute__((singlepump)) f8;
  int __attribute__((doublepump)) f9;
  int __attribute__((merge("foo","depth"))) f10;
  int __attribute__((internal_max_block_ram_depth(32))) f11;
  int __attribute__((optimize_fmax)) f12;
  int __attribute__((optimize_ram_usage)) f13;
};

void bar() {
  struct foo_three s1;
  //CHECK: %[[FIELD1:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD1]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN2]]
  s1.f1 = 0;
  //CHECK: %[[FIELD2:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD2]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN2]]
  s1.f2 = 0;
  //CHECK: %[[FIELD3:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD3]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN3]]
  s1.f3 = 0;
  //CHECK: %[[FIELD4:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD4]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN3]]
  s1.f4 = 0;
  //CHECK: %[[FIELD5:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD5]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN4]]
  s1.f5 = 0;
  //CHECK: %[[FIELD6:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD6]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN5]]
  s1.f6 = 0;
  //CHECK: %[[FIELD7:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD7]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN6]]
  s1.f7 = 0;
  //CHECK: %[[FIELD8:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD8]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN7]]
  s1.f8 = 0;
  //CHECK: %[[FIELD9:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD9]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN8]]
  s1.f9 = 0;
  //CHECK: %[[FIELD11:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD11]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN11]]
  s1.f11 = 0;
  //CHECK: %[[FIELD12:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD12]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN12]]
  s1.f12 = 0;
  //CHECK: %[[FIELD13:.*]] = getelementptr inbounds %struct.foo_three{{.*}}
  //CHECK: %[[CAST:.*]] = bitcast{{.*}}%[[FIELD13]]
  //CHECK: call i8* @llvm.ptr.annotation.p0i8{{.*}}%[[CAST]]{{.*}}[[ANN13]]
  s1.f13 = 0;
}

