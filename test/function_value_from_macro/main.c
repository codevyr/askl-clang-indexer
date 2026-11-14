#include "ops.h"

int ev_open(int x);
int fixed_open(int x);
int arg_open(int x);

DEFINE_OPS(ev);
STATIC_OPS;
OPS_WITH(arg_open);
