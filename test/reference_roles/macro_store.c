#include "ops.h"

int generated_read(int x);
int wrapped_read(int x);

/* The whole initialiser comes out of a macro: nobody typed `.read = ...`
 * anywhere but in the `#define`, and the invocation is where it happened. */
#define DEFINE_OPS(name, fn) static struct file_ops name = { .read = fn }
DEFINE_OPS(gen_ops, generated_read);

/* Only what is stored comes out of a macro. */
#define WRAP(fn) fn
static struct file_ops wrapped_ops = {
    .read = WRAP(wrapped_read),
};

/* A positional initialiser has no `.read =` to show. */
int positional_read(int x);
static struct file_ops positional_ops = { positional_read };
