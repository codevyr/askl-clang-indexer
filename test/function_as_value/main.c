typedef int (*fn_t)(int);

int helper(int x);
int other(int x);
void register_cb(fn_t cb);

#define INIT(fn) fn_t get_init(void) { return fn; }

void pass(void)
{
    register_cb(helper);
}

fn_t ret(void)
{
    return helper;
}

fn_t addr(void)
{
    return &other;
}

int call(void)
{
    return helper(1);
}

void local(void)
{
    fn_t f = other;
    (void)f;
}

INIT(other)

int apply(void *f);

void twice(void)
{
    apply((void *)apply);
}
