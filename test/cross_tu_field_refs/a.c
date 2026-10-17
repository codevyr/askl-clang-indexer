#include "ops.h"

void worker_a(void) {}

static const struct ops ops_a = { worker_a };
