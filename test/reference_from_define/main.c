#include "defs.h"

int helper(int v);
struct ops_ret { int v; };

DEFINE_LOCK(lock);
static DEFINE_LOCK(slock);
EXPORT(lock);
DECLARE_COUNTER(hits);
DEPRECATED int old_api(void);

struct hooks {
    HOOK(struct ops_ret, open);
};

struct other {
    HOOK(int, close);
};

PROFILE_FUNC(int, send, x) { return helper(x); }

FAMILY(pair)

int user(void) {
    CHECK(sizeof(struct ops_ret) > 2);
    return lock + slock + hits;
}
