int helper(int v);

#define PROFILE_FUNC(type, name, param) int blah; type blah_##name(int param)

PROFILE_FUNC(int, send, x) { return helper(x); }
