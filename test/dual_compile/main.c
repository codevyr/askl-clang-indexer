int helper(int x);
#define TWICE(v) ((v) + (v))

#ifdef VARIANT
#define work work_variant
#endif

int work(int a)
{
    return helper(TWICE(a));
}
