#ifndef TYPES_H
#define TYPES_H
struct node { int value; struct node *next; };
typedef struct node node_t;
typedef node_t *node_ptr;
struct list { node_ptr head; int length; };
#endif
