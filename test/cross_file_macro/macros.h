#ifndef MACROS_H
#define MACROS_H

/* Macro that expands to a function signature — the body is in the .c file,
   so the cursor extent spans from this header to the .c file. */
#define DEFINE_HANDLER(name) int name(int code)

/* Macro that wraps a struct type name */
struct result { int code; };
#define RESULT struct result

/* Enum constant via macro */
enum status { S_OK, S_ERR };
#define DEFAULT_STATUS S_OK

#endif
