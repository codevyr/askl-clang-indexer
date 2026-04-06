#ifndef ERRORS_H
#define ERRORS_H

struct config {
    int verbose;
    int debug;
};

/* This inline function has a semantic error (undeclared identifier).
 * Clang emits a diagnostic error but still builds the AST, so all
 * symbols in this header and any file that includes it must still
 * be indexed.  Mirrors real-world cases like __builtin_counted_by_ref
 * errors in linux/slab.h.
 */
static inline int broken_function(struct config *cfg) {
    return nonexistent_variable + cfg->verbose;
}

struct options {
    int count;
    char *name;
};

void setup(struct config *cfg);

#endif
