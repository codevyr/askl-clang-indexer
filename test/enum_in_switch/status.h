#ifndef STATUS_H
#define STATUS_H
enum status { OK, ERR_IO, ERR_MEM };
int handle_status(enum status s);
#endif
