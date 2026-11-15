#define PAIR(type_t, a, b) type_t a; type_t b;
#define TWO(a, b, base) a = base, b = base + 1

struct cfg { int v; };
struct holder {
    PAIR(struct cfg, first, second)
};

enum { START = 5 };
enum pair {
    TWO(LEFT, RIGHT, START)
};
