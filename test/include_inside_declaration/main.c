#define __SYSCALL(nr, sym) extern int sym(int);
#include "syscalls.h"
#undef __SYSCALL

#define __SYSCALL(nr, sym) sym,
int (*const table[])(int) = {
#include "syscalls.h"
};
#undef __SYSCALL

#define __SYSCALL(nr, sym) case nr: return sym(x);
int dispatch(int nr, int x) {
	switch (nr) {
#include "syscalls.h"
	default: return -1;
	}
}
