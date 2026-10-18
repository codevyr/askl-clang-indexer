#ifndef LOG_H
#define LOG_H

void printk(int level);

/* A wrapper macro: its body names `printk`, and its argument is named by
 * whoever calls it. */
#define DEV_PRINT(x) do { printk(x); } while (0)

/* An argument that is ITSELF a macro.  Its expansion is written inside the
 * text of the DEV_PRINT invocation, so range parentage makes it a child of
 * DEV_PRINT's expansion -- but the text belongs to the CALLER, so the edge
 * must be `handler -> LEVEL`, never `DEV_PRINT -> LEVEL`. */
#define LEVEL 3

#endif
