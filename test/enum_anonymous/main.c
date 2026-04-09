/* Anonymous enum - no parent name, falls back to DATA */
enum { ANON_A, ANON_B };

/* Typedef enum with unnamed inner enum - falls back to DATA */
typedef enum { TD_X, TD_Y } td_enum;

/* Named enum - indexed as FIELD with compound name */
enum named { N_ONE, N_TWO };

int use_enums(void) {
    int a = ANON_A;
    td_enum t = TD_X;
    enum named n = N_ONE;
    return a + t + n;
}
