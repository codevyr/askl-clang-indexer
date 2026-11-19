#define struct_group(NAME, MEMBERS...) \
    union { struct { MEMBERS }; struct { MEMBERS } NAME; }

struct w { int q; };

struct outer {
    int before;
    struct_group(clear,
        int a;
        union {
            struct w wpq;
            struct { int meta; int state; };
        };
    );
};
