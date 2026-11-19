#define DEFINE_FAMILY(name, key_t, hash_func)                       \
    static int name##_get(key_t k)    { return hash_func(k); }      \
    static int name##_resize(key_t k) { return hash_func(k); }      \
    static int name##_put(key_t k)    { name##_resize(k);           \
                                        return hash_func(k); }

typedef int my_key_t;
static int my_hash(my_key_t k) { return k; }
DEFINE_FAMILY(tab, my_key_t, my_hash)

int use(void) { return tab_get(1) + tab_put(2); }
