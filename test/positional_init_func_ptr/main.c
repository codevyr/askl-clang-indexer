#include "ops.h"

static int my_read(int fd, char *buf, int size) { return 0; }
static int my_write(int fd, const char *buf, int size) { return 0; }

static const struct iface_ops default_ops = {
    my_read,
    my_write,
};
