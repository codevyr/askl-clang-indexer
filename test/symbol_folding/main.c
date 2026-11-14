#include "shared.h"

int plain(int a) { return a; }
DEFINE_COUNTER(hits)
DEFINE_COUNTER(idle)
DEFINE_SHOW(unused)
DEFINE_SHOW(event)
DEFINE_SHOW(far)
DEFINE_HIDDEN()
DEFINE_PAIR(pair)
EACH(DEFINE_HANDLER)
int (*table[])(int) = { EACH(ENTRY) };

struct x { int v; };

int user(void) {
    CHECK(sizeof(struct x) > 4);
    return SHOW(event)(1) + plain(hits);
}
