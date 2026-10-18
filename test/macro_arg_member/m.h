#ifndef M_H
#define M_H

/* (a) The field name comes from the CALLER as an argument.  The body names
 *     only its parameters, so FIELD_OF must gain no edge to any field. */
#define FIELD_OF(_t, _m)  ((unsigned long)&(((_t*)0)->_m))

/* (c) The field name is the MACRO's own text.  This one must keep its edge --
 *     it is the control that catches a fix which relocates every reference to
 *     the call site. */
#define MODE_OFF          ((unsigned long)&(((struct cfg*)0)->mode))

#endif
