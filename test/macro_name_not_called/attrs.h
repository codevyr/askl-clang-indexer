int printk(const char *fmt, ...);
#define printf(fmt, args...) printk("x: " fmt, ## args)

/* `printf` is the name of a format here, not a use of the macro above. */
#define __printf(a, b) __attribute__((__format__(printf, a, b)))

#define CALLS printf("x")
#define alias printf
#define EACH(_m) _m(1) _m(2)
#define ENTRY(n) n,
#define ALL EACH(ENTRY)
#define APPLY(f) f(ENTRY)
#define PAREN (printf)("x")

/* A parameter is not the macro that happens to share its spelling. */
#define width 8
#define PAD(width) ((width) + 1)

/* The argument supplies the list: `ZIP((1, 2))` calls `PAIR(1, 2)`. */
#define PAIR(a, b) ((a) + (b))
#define ZIP(_tuple) PAIR _tuple
