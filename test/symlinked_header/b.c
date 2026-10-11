/* Reaches the SAME header through the symlink, so clang prints a different
 * path for the same anonymous union. */
#include "self/types.h"
int read_b(struct holder *h) { return h->as_int; }
