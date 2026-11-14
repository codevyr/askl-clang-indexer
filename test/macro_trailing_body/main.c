#include "sys.h"

int helper(int x);

SYSCALL(read)
{
    return helper(a);
}
