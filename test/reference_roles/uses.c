#include "ops.h"

struct inner {
    int b;
    int (*cb)(int);
};

struct dev {
    int (*cb)(int);
    int count;
    int arr[4];
    struct inner a;
    struct inner *p;
    int (*tbl[2])(int);
    int *ptr;
    struct file_ops *f_op;
};

int n;
int garr[4];
struct dev gdev;
int (*gfn)(int);
int *gptr;
int (**gpp)(int);

int target(int x) { return x; }
void sink(int *p);

#define CALL_CB(s, x) ((s)->cb(x))
#define SET_COUNT(s, v) ((s)->count = (v))
#define WRITE_ONCE(x, val) do { *(volatile typeof(x) *)&(x) = (val); } while (0)
#define READ_ONCE(x) (*(const volatile typeof(x) *)&(x))

static struct dev table = {
    .cb = target,
    .count = 1,
    .a.b = 2,
    .arr[1] = 3,
};

unsigned long offsets[] = { __builtin_offsetof(struct dev, count) };

struct sized {
    typeof(gdev.count) copy;
};

int called(struct dev *s)
{
    s->cb(10);
    s->f_op->read(11);
    (s->cb)(12);
    (*s->cb)(13);
    ((int (*)(int))s->cb)(14);
    s->a.cb(15);
    s->tbl[1](16);
    gfn(17);
    (*gfn)(18);
    (*gpp)(20);
    return CALL_CB(s, 19);
}

int written(struct dev *s)
{
    s->cb = target;
    s->count = 21;
    (s->count) = 22;
    s->a.b = 23;
    s->p->b = 24;
    s->arr[2] = 25;
    s->ptr[1] = 26;
    n = 27;
    garr[3] = 28;
    gdev.count = 29;
    gfn = target;
    *&n = 30;
    SET_COUNT(s, 31);
    WRITE_ONCE(s->count, 32);
    *gptr = 33;
    *gdev.ptr = 34;
    gdev.ptr[1] = 35;
    2[garr] = 36;
    garr[garr[1]] = 37;
    return 0;
}

int both_ways(struct dev *s)
{
    s->count += 40;
    s->count++;
    --s->count;
    n++;
    n -= 41;
    n += gdev.count;
    gdev.a.b++;
    s->count = s->count + 42;
    return 0;
}

int taken(struct dev *s, int (*g)(int))
{
    int l = s->count + 49;
    g = s->cb;
    l = n + 50;
    sink(&s->count);
    sink(&n);
    l = s->arr[3] + 51;
    l = READ_ONCE(s->count) + 52;
    l = s->cb ? 53 : 54;
    l = garr[1] + gdev.count;
    return l + g(55);
}

int only_named(struct dev *s)
{
    typeof(s->count) t = 60;
    int l = sizeof(s->count) + sizeof n;
    l += _Alignof(s->arr);
    l += __builtin_types_compatible_p(typeof(s->ptr), int *);
    l += _Generic((s->f_op), default: 61);
    l += __builtin_constant_p(s->p);
    l += (typeof(gdev.count))62;
    l += __builtin_object_size(&gdev, 1);
    l += __builtin_dynamic_object_size(gptr, 0);
    return l + t;
}
