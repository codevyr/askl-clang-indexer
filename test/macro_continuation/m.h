#define INNER(...) do { __VA_ARGS__ } while (0)

/* The kernel shape, byte for byte: the body opens on the continued line with
 * the identifier at COLUMN 1, and every line is padded with tabs before the
 * trailing backslash.  rcu_assign_pointer in include/linux/rcupdate.h is
 * written exactly like this. */
#define OUTER(p, v)					      \
INNER(							      \
	int _r_a_p__v = (v);					      \
	(p) = _r_a_p__v;					      \
)

/* Control: INNER mid-line, the shape ucx uses, which never slid. */
#define MIDLINE(p, v) INNER(int _m = (v); (p) = _m;)
