#include "errors.h"

struct config global_cfg;
struct options global_opts;

void setup(struct config *cfg) {
    cfg->verbose = 1;
    cfg->debug = 1;
}

int main(void) {
    setup(&global_cfg);
    global_opts.count = 42;
    return 0;
}
