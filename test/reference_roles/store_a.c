/* The first file by path that stores `shared_read` in the slot.  This comment
 * pushes the place further into the file than either place in store_b.c is,
 * so a witness chosen by offset before path would be the wrong one. */
#include "shared.h"

static struct file_ops a_ops = {
    .read = shared_read,
};
