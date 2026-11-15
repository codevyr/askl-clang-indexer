#ifdef SWAPPED
#define OUTER second
#define INNER first
#else
#define OUTER first
#define INNER second
#endif

#define NEST() struct OUTER { struct INNER { int x; } f; }

NEST();
