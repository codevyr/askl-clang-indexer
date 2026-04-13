#ifndef OPS_H
#define OPS_H

typedef int (*read_func_t)(int fd, char *buf, int size);
typedef int (*write_func_t)(int fd, const char *buf, int size);

struct iface_ops {
    read_func_t  read;
    write_func_t write;
    int          flags;
};

#endif
