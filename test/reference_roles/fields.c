#include "ops.h"

void copy(struct file_ops *to, struct file_ops *from)
{
    to->write = from->read;
}
