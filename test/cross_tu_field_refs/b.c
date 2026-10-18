#define WANT_EXTRA 1
#include "ops.h"

void worker_b(void) {}

static const struct ops ops_b = { worker_b };
