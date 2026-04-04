/**
 * Adds two integers.
 */
int add(int a, int b) {
    return a + b;
}

/** Global counter. */
int counter = 0;

/**
 * A point in 2D space.
 */
struct point {
    int x;
    int y;
};

/* This is NOT a doc comment (single star). */
int mul(int a, int b) {
    return a * b;
}

void no_doc(void) {
    counter = add(1, 2);
}
