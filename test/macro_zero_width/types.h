#ifndef TYPES_H
#define TYPES_H

// Reproduces glibc __SOCKADDR_ALLTYPES pattern exactly:
// The struct forward-declarations are spelled inside ONE_STRUCT's body
// and expanded via ALL_STRUCTS inside a typedef union.
// clang_getExpansionLocation on both extent endpoints of each StructDecl
// resolves to the single token ALL_STRUCTS -> start == end (zero-width).
#define ALL_STRUCTS \
    ONE_STRUCT(alpha)

#define ONE_STRUCT(name) struct name *name##_ptr;
typedef union { ALL_STRUCTS } wrapper;
#undef ONE_STRUCT

// Source-level struct: start < end (proper range).
struct beta { int value; };

#endif
