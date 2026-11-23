#include "shared.h"

void fill(struct file_ops *o)
{
    o->read = shared_read;
}

void refill(struct file_ops *o)
{
    o->read = shared_read;
}
