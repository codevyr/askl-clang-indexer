#include "prof.h"

int helper(int x);
int shown(int x);

int plain(int a)
{
    return helper(a);
}

PROFILE_FUNC(int, wrapped, (a), int a)
{
    return helper(a);
}

#define alias renamed
int alias(int a)
{
    return helper(a);
}

DEFINE_SHOW(event)
