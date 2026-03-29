#include "ops.h"

extern void setup(struct file_ops *ops);

int main(void) {
    struct file_ops ops;
    setup(&ops);
    ops.read(0, 0, 0);
    return 0;
}
