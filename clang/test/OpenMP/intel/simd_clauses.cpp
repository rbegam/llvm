// RUN: %clang_cc1 -emit-llvm -o - %s -fopenmp -fintel-compatibility -fintel-openmp -triple x86_64-unknown-linux-gnu | FileCheck %s

struct A {
  A();
  A(const A&);
  A& operator=(const A&);
  ~A();
  int *ip;
} obj, objarr[4];

// CHECK-LABEL: @_Z3foov
void foo()
{
  // CHECK: [[I_ADDR:%.+]] = alloca i32,
  // CHECK: [[J_ADDR:%.+]] = alloca i32,
  // CHECK: [[Y_ADDR:%.+]] = alloca i32*,
  // CHECK: [[Z_ADDR:%.+]] = alloca i32*,
  // CHECK: [[X_ADDR:%.+]] = alloca i32*,
  // CHECK: [[Q_ADDR:%.+]] = alloca i32*,
  // CHECK: [[SIMPLE_ADDR:%.+]] = alloca double,
  int i,j;
  int *y,*z,*x, *q;
  double simple;

  // CHECK: directive(metadata !"DIR.OMP.SIMD")
  // CHECK: qual.opnd.i32(metadata !"QUAL.OMP.SAFELEN", i32 4)
  // CHECK: qual.opnd.i32(metadata !"QUAL.OMP.SIMDLEN", i32 4)
  // CHECK: qual.opnd.i32(metadata !"QUAL.OMP.COLLAPSE", i32 2)
  // CHECK: qual.opndlist(metadata !"QUAL.OMP.ALIGNED", i32** [[Y_ADDR]], i32** [[Z_ADDR]], i32 8)
  // CHECK: opndlist(metadata !"QUAL.OMP.ALIGNED", i32** [[X_ADDR]], i32 4)
  // CHECK: opndlist(metadata !"QUAL.OMP.ALIGNED", i32** [[Q_ADDR]], i32 0)
  // CHECK: opndlist(metadata !"QUAL.OMP.LASTPRIVATE", double* [[SIMPLE_ADDR]])
  // CHECK: opndlist(metadata !"QUAL.OMP.LASTPRIVATE:NONPOD", %struct.A* @obj, %struct.A* (%struct.A*)* @_ZTS1A.omp.def_constr, void (%struct.A*, %struct.A*)* @_ZTS1A.omp.copy_assign, void (%struct.A*)* @_ZTS1A.omp.destr)
  // CHECK: opndlist(metadata !"QUAL.OMP.LASTPRIVATE:NONPOD", [4 x %struct.A]* @objarr, [4 x %struct.A]* ([4 x %struct.A]*)* @_ZTSA4_1A.omp.def_constr, void ([4 x %struct.A]*, [4 x %struct.A]*)* @_ZTSA4_1A.omp.copy_assign, void ([4 x %struct.A]*)* @_ZTSA4_1A.omp.destr)
  // CHECK: directive(metadata !"DIR.QUAL.LIST.END")
  #pragma omp simd safelen(4) simdlen(4) collapse(2) \
            aligned(y,z:8) aligned(x:4) aligned(q) \
            lastprivate(simple) lastprivate(obj) lastprivate(objarr)
  for (i=0;i<10;++i)
  for (j=0;j<10;++j) {}
}

// CHECK: define internal void @_ZTS1A.omp.copy_assign
// CHECK: define internal void @_ZTSA4_1A.omp.copy_assign
