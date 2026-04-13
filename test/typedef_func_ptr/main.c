#include "ops.h"

static int my_read(int fd, char *buf, int size) { return 0; }
static int my_write(int fd, const char *buf, int size) { return 0; }

static const struct iface_ops default_ops = {
    .read = my_read,
    .write = my_write,
    .flags = 0,
};

void use(struct iface_ops *ops) {
    ops->read(0, 0, 0);
    ops->write(0, 0, 0);
}
