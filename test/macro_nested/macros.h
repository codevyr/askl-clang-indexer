extern int output(int level, const char *msg);
#define MSG(level, msg) output(level, msg)
#define ERR(msg) MSG(1, msg)
#define FATAL(msg) ERR(msg)
