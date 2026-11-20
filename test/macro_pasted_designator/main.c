enum req_op { REQ_OP_READ, REQ_OP_WRITE, REQ_OP_FLUSH };

#define READ 0
#define WRITE 1

#define REQ_OP_NAME(name) [REQ_OP_##name] = #name
static const char *const blk_op_name[] = {
	REQ_OP_NAME(READ),
	REQ_OP_NAME(WRITE),
	REQ_OP_NAME(FLUSH),
};
#undef REQ_OP_NAME

const char *blk_op_str(enum req_op op) { return blk_op_name[op]; }
