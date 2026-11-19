#include "defs.h"

DEFINE_V(tentative);
DEFINE_VI(initialised);
int plain;
static int counter;

int use(void) { return tentative + initialised + plain + counter + only_declared; }
