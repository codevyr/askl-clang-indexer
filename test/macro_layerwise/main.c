#include "log.h"

int threshold;

void handler(void) {
    DEV_PRINT(threshold);
    DEV_PRINT(LEVEL);
}
