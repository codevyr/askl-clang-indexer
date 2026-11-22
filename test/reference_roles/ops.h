#ifndef OPS_H
#define OPS_H

struct file_ops {
    int (*read)(int);
    int (*write)(int);
};

int keep(int (*cb)(int));

#define DEFINE_COUNTER(name) int name = 0
#define CALL_AND_KEEP(fn) (fn(1) + keep(fn))
#define BODY_CALL() helper(3)

#ifdef AS_CALL
#define USE(fn) fn(2)
#else
#define USE(fn) keep(fn)
#endif

#endif
