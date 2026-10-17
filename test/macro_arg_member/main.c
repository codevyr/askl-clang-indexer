#include "m.h"

struct cfg { int mode; };

unsigned long from_arg(void)  { return FIELD_OF(struct cfg, mode); }
unsigned long from_body(void) { return MODE_OFF; }
