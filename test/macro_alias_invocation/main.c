#define REAL_CHECK(cond) \
    do { extern void check_failed(void); if (!(cond)) check_failed(); } while (0)
#define CHECK REAL_CHECK
#define PREFIX static

int f(int a)
{
    CHECK(a > ")("[0]);
    return a;
}

PREFIX int own(int a) { return a; }
