#ifndef DEFS_H
#define DEFS_H

#define DEFINE_LOCK(name) int name = 0
#define HOOK(ret_t, name) ret_t (*name)(void)
#define PROFILE_FUNC(type, name, param) int blah; type blah_##name(int param)
#define FAMILY(name)                                             \
    struct name##_s { int n; };                                  \
    static int name##_get(struct name##_s *p) { return p != 0; }
#define CHECK(cond) do { extern void check_failed(void); (void)sizeof(cond); } while (0)
#define EXPORT(sym) extern __typeof__(sym) sym
#define DECLARE_COUNTER(name) extern int name
#define DEPRECATED __attribute__((deprecated))

#endif
