#include "macros.h"

/* Function defined via macro from header — extent spans macros.h to main.c */
DEFINE_HANDLER(handle_open) {
    return code + 1;
}

DEFINE_HANDLER(handle_close) {
    return code - 1;
}

/* Type ref via macro — TypeRef spelling is in macros.h */
RESULT get_result(void) {
    RESULT r;
    r.code = DEFAULT_STATUS;
    return r;
}
