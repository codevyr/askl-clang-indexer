/* No include guard, on purpose: this header is included twice, once per
 * SUFFIX, and the single DECL(alpha) line below expands to a DIFFERENT
 * function each time.  One source line, two expansions, two results -- the
 * shape glibc's bits/socket.h uses via __SOCKADDR_ALLTYPES. */
DECL(alpha)

/* The discriminator for instance identity, right next to it: this line does
 * not depend on SUFFIX, so both inclusions parse the SAME declaration of the
 * SAME symbol at the SAME extent.  That is one declaration in the text and
 * must be ONE instance, however many times the text is parsed -- while
 * alpha_f and alpha_d above are two different symbols and stay two.
 * glibc's libc-header-start.h is this case at scale. */
int plain(int v);
