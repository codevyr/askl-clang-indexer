static int count = 10;
static int helper(void) { return count; }
int get_a(void) { return helper(); }
