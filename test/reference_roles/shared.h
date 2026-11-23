#ifndef SHARED_H
#define SHARED_H

#include "ops.h"

int shared_read(int x);
int header_read(int x);

/* Read by every unit that includes this header: one place, however many
 * units read it. */
static struct file_ops header_ops = {
    .read = header_read,
};

DEFINE_COUNTER(header_hits);

#endif
