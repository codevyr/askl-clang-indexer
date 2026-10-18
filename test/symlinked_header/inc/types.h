/* An anonymous union: clang names it after WHERE IT IS WRITTEN, and the path it
 * prints is the one the #include resolved to.  This header is reachable two
 * ways -- `inc/types.h` and `inc/self/types.h`, via the `self -> .` symlink
 * beside it -- which is the shape criu has as
 * `compel/include/uapi/compel -> .`.  Both spellings must give ONE symbol. */
struct holder {
    union {
        int as_int;
        char as_bytes[4];
    };
    int tag;
};
