#define DEFINE_COUNTER(name) int name;
#define DECLARE_SHOW(name) int name ## _show(int a);
#define DEFINE_SHOW(name) int name ## _show(int a) { return a; }
#define SHOW(name) name ## _show
#define DEFINE_HIDDEN() int hidden_impl(void) { return 0; }
#define DEFINE_PAIR(name)                                  \
    int name ## _first(void) { return 0; }                 \
    int name ## _second(void) { return name ## _first(); }
#define CHECK(cond) \
    do { extern void check_failed(void); if (!(cond)) check_failed(); } while (0)
#define EACH(_m) _m(alpha) _m(beta)
#define DEFINE_HANDLER(_n) int handle_ ## _n(int a) { return a; }
#define ENTRY(_n) handle_ ## _n,
