static int count = 20;
static int helper(void) { return count; }
int get_b(void) { return helper(); }
