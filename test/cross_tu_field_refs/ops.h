#ifndef OPS_H
#define OPS_H

/* Seen only by b.c, which defines WANT_EXTRA before including this.  It sits
   BEFORE struct ops deliberately: it shifts every later instance's DFS numbers
   in b.c's walk, so b.c's numbering of this header differs from a.c's.  That is
   what makes the cross-TU merge observable -- `processTU` keeps a.c's instances
   and merges b.c's refs, whose numbers were computed against instances that are
   then discarded.
   It also illustrates a separate, unfixed gap -- codevyr/askl-clang-indexer#7.
   The symbol table is project-global, so `extra_decl` always reaches the index;
   the INSTANCES come from whichever TU compiled the header first.  Measured: in
   TU order (a.c, b.c) the symbol has no instance in any object, and in order
   (b.c, a.c) it has one.  An orphan symbol the engine can name and never
   locate. */
#ifdef WANT_EXTRA
struct extra_decl {
    int pad;
};
#endif

struct ops {
    void (*run)(void);
};

void worker_a(void);
void worker_b(void);

#endif
