struct mod { int (*pml_send)(int); int (*pml_recv)(int); int (*pml_probe)(int); };
extern struct mod mca_pml;
int kh_put_tbl(int k);
typedef int kh_tbl_t;

#define CALL(a) mca_pml.pml_ ## a
#define OUTER(a) CALL(a)
#define PROBE(x) CALL(probe(x))
#define kh_put(name, k) kh_put_##name(k)
#define khash_t(name) kh_##name##_t
#define FIXED() mca_pml.pml_ ## send

int op_add_avx(int x);
#define SUFFIX _avx
#define CONCAT_(a, b) a ## b
#define CONCAT(a, b) CONCAT_(a, b)
#define OP(name) CONCAT(op_ ## name, SUFFIX)

int log_at_info(const char *fmt, ...);
#define LOG(level, fmt, ...) log_at_ ## level(fmt, ## __VA_ARGS__)

int handle_alpha(int x);
int handle_beta(int x);
#define EACH(_m) _m(alpha) _m(beta)
#define ENTRY(_n) handle_ ## _n,
