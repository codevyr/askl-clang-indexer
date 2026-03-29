#include "types.h"

// Single-character global variable: proves that clang returns
// half-open ranges [start, start+len), so a 1-char token gives
// [x, x+1) with start != end — NOT filtered by zero-width check.
int x = 0;

void use_types(struct alpha *a, struct beta *b) {
    int y = x;
    (void)a;
    (void)b;
    (void)y;
}
