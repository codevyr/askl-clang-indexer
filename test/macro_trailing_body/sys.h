#define SYSCALL(_n)                              \
    static int do_##_n(int a);                   \
    int entry_##_n(int a) { return do_##_n(a); } \
    static int do_##_n(int a)
