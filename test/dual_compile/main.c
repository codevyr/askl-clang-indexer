int helper(int x);

#ifdef VARIANT
#define work work_variant
#endif

int work(int a)
{
    return helper(a);
}
