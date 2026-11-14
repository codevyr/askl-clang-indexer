#include "mod.h"

int direct(int x) { return CALL(send(x)); }
int layered(int x) { return OUTER(recv(x)); }
int via_body(int x) { return PROBE(x); }
int func(int x) { return kh_put(tbl, x); }
khash_t(tbl) typed;
int fixed(void) { return FIXED()(1); }
int indirect(int x) { return OP(add)(x); }
int variadic(int x) { return LOG(info, "%d", x) + LOG(info, "none"); }
int (*table[])(int) = { EACH(ENTRY) };
