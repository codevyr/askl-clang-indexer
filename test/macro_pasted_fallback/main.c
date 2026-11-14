struct ops { int (*open)(int); };
int fn_a(int x);
int fn_b(int x);

#define SEL(n) n ## _a
#define MK(n) { .open = SEL(n) }
struct ops first = MK(fn);
#undef SEL
#define SEL(n) n ## _b
struct ops second = MK(fn);
