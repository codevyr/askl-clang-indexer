#define CHECK(cond) \
    do { extern void check_failed(void); if (!(cond)) check_failed(); } while (0)

#ifdef VARIANT
#define work work_variant
#endif

int work(int a)
{
    CHECK(a > 0);
    return a;
}
