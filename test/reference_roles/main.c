#include "ops.h"

enum mode { MODE_A, MODE_B };

int helper(int x) { return x; }
int my_read(int x) { return x; }
int other(int x) { return x; }
int both(int x) { return x; }
int either(int x) { return x; }

int keep(int (*cb)(int)) { return cb(0); }

DEFINE_COUNTER(hits);

static struct file_ops ops = {
    .read = my_read,
};

typedef int (*fn_t)(int);

fn_t pick(void) { return other; }

int run(enum mode m)
{
    switch (m) {
    case MODE_A:
        return helper(1);
    default:
        break;
    }
    keep(helper);
    return CALL_AND_KEEP(both) + BODY_CALL();
}

int unlabelled(void)
{
    hits++;
    return ops.read(2);
}

int two_units(void) { return USE(either); }
