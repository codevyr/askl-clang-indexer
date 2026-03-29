#include "types.h"

result_t compute_a(int x) {
    result_t r;
    r.quot = x / 2;
    r.rem = x % 2;
    return r;
}
