extern void do_something(int x);
extern int GLOBAL_VARIABLE;

#define some_macro(arg) do_something(arg + GLOBAL_VARIABLE)
