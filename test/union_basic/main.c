union value { int i; float f; };
void set_int(union value *v, int x) { v->i = x; }
