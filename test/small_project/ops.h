#ifndef OPS_H
#define OPS_H

struct file_ops {
    int (*read)(int fd, char *buf, int size);
    int (*write)(int fd, const char *buf, int size);
};

struct container {
    struct file_ops ops;
    int data;
};

#endif
