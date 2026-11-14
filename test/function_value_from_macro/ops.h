struct ops { int (*open)(int); };

#define DEFINE_OPS(_n) struct ops _n##_ops = { .open = _n##_open }
#define STATIC_OPS struct ops static_ops = { .open = fixed_open }
#define OPS_WITH(_fn) struct ops with_ops = { .open = _fn }
