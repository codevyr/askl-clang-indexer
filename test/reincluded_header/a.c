int helper(int v);

/* The paste goes through a second macro so that SUFFIX is expanded BEFORE
 * ## sees it.  Pasting directly (`name##SUFFIX`) would suppress the
 * expansion and give the identifier `alpha_SUFFIX` both times -- genuinely
 * one instance, which is not the case under test. */
#define CAT(a, b) a##b
#define XCAT(a, b) CAT(a, b)
#define DECL(name) int XCAT(name, SUFFIX)(int v) { return helper(v); }

#define SUFFIX _f
#include "multi.h"
#undef SUFFIX

#define SUFFIX _d
#include "multi.h"
#undef SUFFIX
