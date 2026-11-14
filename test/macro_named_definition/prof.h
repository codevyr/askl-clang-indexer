#define PROFILE_FUNC(_ret, _name, _arglist, ...) _ret _name(__VA_ARGS__)
#define DEFINE_SHOW(_n) int _n##_show(int a) { return shown(a); }
