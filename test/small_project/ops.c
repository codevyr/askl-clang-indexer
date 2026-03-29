#include "ops.h"

static int my_read(int fd, char *buf, int size) { return 0; }
static int my_write(int fd, const char *buf, int size) { return 0; }

// Pattern 1: designated initializer
static const struct file_ops default_ops = {
    .read = my_read,
    .write = my_write,
};

// Pattern 2: direct assignment
void setup(struct file_ops *ops) {
    ops->read = my_read;
    ops->write = my_write;
}

// Pattern 3: struct composition
struct container my_container = {
    .ops = { .read = my_read },
    .data = 42,
};
