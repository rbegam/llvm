// INTEL_COLLAB
// RUN: %clang_cc1 -emit-llvm -o - -fopenmp -fopenmp-late-outline \
// RUN:  -triple x86_64-unknown-linux-gnu %s | FileCheck %s
// RUN: %clang_cc1 -emit-llvm -o - -fopenmp -fopenmp-late-outline -O2 \
// RUN:  -triple x86_64-unknown-linux-gnu %s \
// RUN: | FileCheck %s --check-prefix OPT

int foo();

// CHECK-LABEL: @_Z3barii
// CHECK: [[IF_VAL_ADDR:%.+]] = alloca i32,
// CHECK: [[NUM_THREADS_VAL_ADDR:%.+]] = alloca i32,
void bar(int if_val, int num_threads_val) {
  // CHECK: [[IF1_ADDR:%.+]] = alloca i32,
  int if1 = 1;
  // CHECK: [[IF2_ADDR:%.+]] = alloca i32,
  int if2 = 2;
  // CHECK: [[PB1_ADDR:%.+]] = alloca i32,
  int pb1 = 1;
  // CHECK: [[PB2_ADDR:%.+]] = alloca i32,
  int pb2 = 2;
  // CHECK: [[PB3_ADDR:%.+]] = alloca i32,
  int pb3 = 3;
  // CHECK: [[NT1_ADDR:%.+]] = alloca i32,
  int nt1 = 1;
  // CHECK: [[NT2_ADDR:%.+]] = alloca i32,
  int nt2 = 2;
  // CHECK: [[DF1_ADDR:%.+]] = alloca i32,
  int df1 = 1;
  // CHECK: [[DF2_ADDR:%.+]] = alloca i32,
  int df2 = 2;

  // if
  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.PRIVATE"(i32* [[IF1_ADDR]])
  // CHECK-SAME: "QUAL.OMP.IF"(i1 true)
  #pragma omp parallel private(if1) if(1)
  { foo(); }

  // CHECK: [[ILOAD1:%.+]] = load i32, i32* [[IF_VAL_ADDR]]
  // CHECK-NEXT: [[TOBOOL:%.+]] = icmp ne i32 [[ILOAD1]], 0
  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.PRIVATE"(i32* [[IF2_ADDR]])
  // CHECK-SAME: "QUAL.OMP.IF"(i1 [[TOBOOL]])
  // CHECK: region.exit{{.*}}"DIR.OMP.END.PARALLEL"
  #pragma omp parallel private(if2) if(if_val)
  { foo(); }

  // CHECK: [[L1:%.+]] = load i32, i32* [[IF1_ADDR]]
  // CHECK-NEXT: [[TB2:%.+]] = icmp ne i32 [[L1]], 0
  // CHECK-NEXT: br i1 [[TB2]]
  // CHECK: [[L2:%.+]] = load i32, i32* [[IF2_ADDR]]
  // CHECK-NEXT: [[TB3:%.+]] = icmp ne i32 [[L2]], 0
  // CHECK: [[P5:%.+]] = phi i1
  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.IF"(i1 [[P5]])
  // CHECK: region.exit{{.*}}"DIR.OMP.END.PARALLEL"
  #pragma omp parallel if(if1 && if2)
  { foo(); }

  // proc_bind
  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.PRIVATE"(i32* [[PB1_ADDR]])
  // CHECK-SAME: "QUAL.OMP.PROCBIND.MASTER"
  #pragma omp parallel private(pb1) proc_bind(master)
  { foo(); }

  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.PRIVATE"(i32* [[PB2_ADDR]])
  // CHECK-SAME: "QUAL.OMP.PROCBIND.CLOSE"
  #pragma omp parallel private(pb2) proc_bind(close)
  { foo(); }

  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.PRIVATE"(i32* [[PB3_ADDR]])
  // CHECK-SAME: "QUAL.OMP.PROCBIND.SPREAD"
  #pragma omp parallel private(pb3) proc_bind(spread)
  { foo(); }

  // num_threads
  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.PRIVATE"(i32* [[NT1_ADDR]])
  // CHECK-SAME: "QUAL.OMP.NUM_THREADS"(i32 8)
  #pragma omp parallel private(nt1) num_threads(8)
  { foo(); }

  // CHECK: [[ILOAD2:%.*]] = load i32, i32* [[NUM_THREADS_VAL_ADDR]]
  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.PRIVATE"(i32* [[NT2_ADDR]])
  // CHECK-SAME: "QUAL.OMP.NUM_THREADS"(i32 [[ILOAD2]])
  #pragma omp parallel private(nt2) num_threads(num_threads_val)
  { foo(); }

  // default
  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.PRIVATE"(i32* [[DF1_ADDR]])
  // CHECK-SAME: "QUAL.OMP.DEFAULT.NONE"
  #pragma omp parallel private(df1) default(none)
  { foo(); }

  // CHECK: region.entry() [ "DIR.OMP.PARALLEL"()
  // CHECK-SAME: "QUAL.OMP.PRIVATE"(i32* [[DF2_ADDR]])
  // CHECK-SAME: "QUAL.OMP.DEFAULT.SHARED"
  #pragma omp parallel private(df2) default(shared)
  { foo(); }
}

//OPT-LABEL: @_Z4bar2
void bar2()
{
  //OPT: [[CDS:%cleanup.dest.slot.*]] = alloca i32,
  //OPT: region.entry() [ "DIR.OMP.PARALLEL"()
  //OPT-SAME: "QUAL.OMP.PRIVATE"(i32* [[CDS]])
  //OPT: "DIR.OMP.END.PARALLEL"
  #pragma omp parallel
  {
    for ( int y = 0 ; y < 100 ; y++ )
    {
      for (int x = 0 ; x < 100 ; x++ )
      {
      }
    }
  }

  //OPT: region.entry() [ "DIR.OMP.PARALLEL"()
  //OPT-SAME: "QUAL.OMP.PRIVATE"(i32* [[CDS]])
  //OPT: "DIR.OMP.END.PARALLEL"
  #pragma omp parallel
  {
    for ( int i = 0 ; i < 8 ; i++ )
    {
    }
  }
}
// end INTEL_COLLAB
