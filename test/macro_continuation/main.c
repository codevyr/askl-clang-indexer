#include "m.h"

int use(int v)
{
	int a = 0, b = 0;
	OUTER(a, v);
	MIDLINE(b, v);
	return a + b;
}
