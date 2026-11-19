#ifndef DEFS_H
#define DEFS_H

#define DECLARE_V(name) extern int name
#define DEFINE_V(name) int name
#define DEFINE_VI(name) int name = 1

DECLARE_V(tentative);
DECLARE_V(initialised);
extern int plain;
extern int only_declared;

#endif
