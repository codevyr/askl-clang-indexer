#include "status.h"
int handle_status(enum status s) {
    switch (s) {
    case OK:      return 0;
    case ERR_IO:  return 1;
    case ERR_MEM: return 2;
    default:      return -1;
    }
}
enum status last_error = ERR_IO;
