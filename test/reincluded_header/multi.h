/* No include guard, on purpose: this header is included twice, once per
 * SUFFIX, and the single DECL(alpha) line below expands to a DIFFERENT
 * function each time.  One source line, two expansions, two results -- the
 * shape glibc's bits/socket.h uses via __SOCKADDR_ALLTYPES. */
DECL(alpha)
