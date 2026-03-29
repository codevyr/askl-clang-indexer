#ifndef TYPES_H
#define TYPES_H

/*
 * Reproduces the glibc __SOCKADDR_ALLTYPES pattern:
 * struct names are spelled inside a macro definition,
 * but expanded later — causing clang_getSpellingLocation
 * to return inverted ranges (start > end).
 */

#define ALL_TYPES \
    ONE_TYPE(type_a) \
    ONE_TYPE(type_b) \
    ONE_TYPE(type_c)

#define ONE_TYPE(t) struct t *__##t##__;
typedef union { ALL_TYPES } any_type;
#undef ONE_TYPE

#endif
