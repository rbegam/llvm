// CQ#369185
// RUN: %clang_cc1 -std=c++11 -fintel-compatibility -ast-dump %s | FileCheck %s

// Simple typelist. Compile-time list of types.
template <typename... _Elements>
struct __reflection_typelist;

// Specialization for an empty typelist.
template <>
struct __reflection_typelist<> {
  typedef void empty;
};

// Partial specialization.
template <typename _First, typename... _Rest>
struct __reflection_typelist<_First, _Rest...> {
  typedef void empty;

  struct first {
    typedef _First type;
  };

  struct rest {
    typedef __reflection_typelist<_Rest...> type;
  };
};

//CHECK: ClassTemplateSpecializationDecl {{.+}} struct __reflection_typelist
//CHECK-NEXT: TemplateArgument pack
//CHECK-NEXT: TemplateArgument type 'A'
//CHECK-NEXT: ClassTemplateSpecializationDecl {{.+}} struct __reflection_typelist
//CHECK-NEXT: TemplateArgument pack
//CHECK-NEXT: TemplateArgument type 'A'
//CHECK-NEXT: TemplateArgument type 'B'
//CHECK-NEXT: ClassTemplateSpecializationDecl {{.+}} struct __reflection_typelist
//CHECK-NEXT: TemplateArgument pack
//CHECK-NEXT: TemplateArgument type 'A'
//CHECK-NEXT: TemplateArgument type 'C'
//CHECK-NEXT: TemplateArgument type 'B'
//CHECK-NEXT: TemplateArgument type 'D'
//CHECK-NEXT: ClassTemplateSpecializationDecl {{.+}} struct __reflection_typelist
//CHECK-NEXT: TemplateArgument pack
//CHECK-NEXT: TemplateArgument type 'A'
//CHECK-NEXT: TemplateArgument type 'B'
//CHECK-NEXT: TemplateArgument type 'D'
//CHECK-NEXT: TemplateArgument type 'A'
//CHECK-NEXT: TemplateArgument type 'C'
//CHECK-NEXT: ClassTemplateSpecializationDecl {{.+}} struct __reflection_typelist
//CHECK-NEXT: TemplateArgument pack
//CHECK-NEXT: TemplateArgument type 'C'
//CHECK-NEXT: TemplateArgument type 'D'
//CHECK-NEXT: ClassTemplateSpecializationDecl {{.+}} struct __reflection_typelist
//CHECK-NEXT: TemplateArgument pack
//CHECK-NEXT: TemplateArgument type 'D'
//CHECK-NEXT: TemplateArgument type 'C'

template <typename _Tp>
struct bases {
  typedef __reflection_typelist<__bases(_Tp)...> type;
};

//CHECK: ClassTemplateSpecializationDecl {{.+}} struct bases definition
//CHECK: TemplateArgument type 'A'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<>':'__reflection_typelist<>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct bases definition
//CHECK: TemplateArgument type 'B'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<A>':'__reflection_typelist<A>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct bases definition
//CHECK: TemplateArgument type 'C'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<A>':'__reflection_typelist<A>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct bases definition
//CHECK: TemplateArgument type 'D'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<A, B>':'__reflection_typelist<A, B>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct bases definition
//CHECK: TemplateArgument type 'E'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<A, C, B, D>':'__reflection_typelist<A, C, B, D>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct bases definition
//CHECK: TemplateArgument type 'E2'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<A, C, B, D>':'__reflection_typelist<A, C, B, D>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct bases definition
//CHECK: TemplateArgument type 'E3'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<A, B, D, A, C>':'__reflection_typelist<A, B, D, A, C>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct bases definition
//CHECK: TemplateArgument type 'F'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<>':'__reflection_typelist<>'

template <typename _Tp>
struct direct_bases {
  typedef __reflection_typelist<__direct_bases(_Tp)...> type;
};

//CHECK: ClassTemplateSpecializationDecl {{.+}} struct direct_bases definition
//CHECK: TemplateArgument type 'A'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct direct_bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<>':'__reflection_typelist<>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct direct_bases definition
//CHECK: TemplateArgument type 'B'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct direct_bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<A>':'__reflection_typelist<A>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct direct_bases definition
//CHECK: TemplateArgument type 'C'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct direct_bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<A>':'__reflection_typelist<A>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct direct_bases definition
//CHECK: TemplateArgument type 'D'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct direct_bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<A, B>':'__reflection_typelist<A, B>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct direct_bases definition
//CHECK: TemplateArgument type 'E'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct direct_bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<C, D>':'__reflection_typelist<C, D>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct direct_bases definition
//CHECK: TemplateArgument type 'E2'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct direct_bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<C, D>':'__reflection_typelist<C, D>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct direct_bases definition
//CHECK: TemplateArgument type 'E3'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct direct_bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<D, C>':'__reflection_typelist<D, C>'
//CHECK: ClassTemplateSpecializationDecl {{.+}} struct direct_bases definition
//CHECK: TemplateArgument type 'F'
//CHECK-NEXT: CXXRecordDecl {{.+}} implicit struct direct_bases
//CHECK-NEXT: TypedefDecl {{.+}} type '__reflection_typelist<>':'__reflection_typelist<>'

struct A {};
struct B : virtual A {};
struct C : A {};
struct D : virtual A, B {};
struct E : C, virtual D {};
typedef C G;
typedef D I;
struct E2 : public G, private D {};
struct E3 : I, G {};
struct F {};

int main() {
  bases<A> b1;
  bases<B> b2;
  bases<C> b3;
  bases<D> b4;
  bases<E> b5;
  bases<E2> b6;
  bases<E3> b7;
  bases<F> b8;
  direct_bases<A> db1;
  direct_bases<B> db2;
  direct_bases<C> db3;
  direct_bases<D> db4;
  direct_bases<E> db5;
  direct_bases<E2> db6;
  direct_bases<E3> db7;
  direct_bases<F> db8;
  return 0;
}
