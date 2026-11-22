#pragma once

#include <clang-c/Index.h>
#include <cstdint>

// WHAT AN EXPRESSION IS USED FOR IS DECIDED BY THE EXPRESSION AROUND IT.
//
// `s->cb` is the same two tokens in `s->cb(x)`, `s->cb = f` and `g = s->cb`,
// and the three do different things to the field: it is called through,
// assigned, read.  The cursor for `s->cb` does not say which; where it stands
// in its parent does -- the callee of a call, the left side of `=`, anything
// else.  So the walk carries one `Use` down the tree: each cursor is handed
// the use its parent makes of it (`childUse`), and a token that names a field
// or a variable states the roles of the use it was handed (`rolesOf`).
//
// The rules, all of them about the text and none about what runs:
//
//   * THE CALLEE OF A CALL IS CALLED, also through parentheses, a cast and
//     the `*` of a pointer to function: `(*s->cb)(x)`, `((fn_t)s->cb)(x)`.
//   * THE LEFT SIDE OF `=` IS WRITTEN; of `+=`, and the operand of `++` or
//     `--`, written and read.
//   * A USE REACHES A PART'S WHOLE AND STOPS AT A POINTER.  `s->a.b = 1`
//     writes `b` and `a`, and `arr[2] = 7` writes `arr`: the storage assigned
//     is inside them.  `s->p->b = 1` and `ptr[1] = 5` read `p` and `ptr`: what
//     is written is another object, which no token here names.  The same for
//     a call: `tbl[1](x)` calls through the table `tbl`.
//   * `*&x` IS `x`.  This is how `WRITE_ONCE(x, v)` writes: `*(T *)&(x) = v`.
//   * EVERYTHING ELSE TAKES THE VALUE OR THE ADDRESS: an argument, `&x`, an
//     operand, a condition, the struct or pointer on the way to a member.
//   * WHAT IS NOT EVALUATED STATES NOTHING: the operand of `sizeof`,
//     `_Alignof` and `typeof`, the arguments of `__builtin_types_compatible_p`
//     and `__builtin_constant_p`, everything under `_Generic`.  The field is
//     named there and nothing is done to it; there is no role for "named",
//     and nothing stated is never "no".
//
// Where the tree does not say what an expression is -- libclang hands several
// kinds over as one "unexposed" cursor -- the use is `Unevaluated` unless the
// cursor is recognisably a wrapper around its operand (an implicit cast: it
// covers the same text) or a designated initializer.  Stating nothing is
// always allowed; a wrong role is not.
struct Use {
    enum Kind : uint8_t {
        Value,        // its value or address is taken
        Callee,       // it is what a call calls
        Written,      // it is assigned
        ReadWritten,  // it is assigned from its own value: `+=`, `++`
        Unevaluated,  // it is named and nothing is done to it
    };
    Kind kind = Value;
    /// The use is of what this value POINTS TO (`*p = x`), so the token it
    /// reaches is only read -- unless an `&` is met first: `*&x = 1`.
    bool through_pointer = false;
    /// The use is of an ELEMENT of this value (`a[i] = x`): the use of `a`
    /// itself where `a` is an array, and a read of `a` where it is a pointer.
    bool of_element = false;
};

/// One cursor the walk is inside, and what it needs to know to hand a use to
/// each of its children.  The answers that cost a libclang call are asked
/// once, when the first child needs them.
struct UseFrame {
    CXCursor cursor;
    CXCursorKind kind;
    Use use;
    unsigned next_child = 0;

    int n_children = -1;       // a cast: counted on demand, to know its last
    int op = -1;               // the operator of a binary / unary operator
    int callee_first = -1;     // CallExpr: is its first child the callee?
    int args_unevaluated = -1; // CallExpr: a builtin that evaluates no argument
    int designator = -1;       // UnexposedExpr: is it `.a = x` / `[i] = x`?
    bool has_init = false;     // VarDecl: `init` was asked for
    CXCursor init;

    UseFrame(CXCursor c, CXCursorKind k, Use u)
        : cursor(c), kind(k), use(u), init(clang_getNullCursor()) {}
};

/// The use `parent` makes of its next child, `child`.  Advances the frame.
Use childUse(UseFrame& parent, CXCursor child);

/// What a token that names a field or a variable states, used this way.
/// `token` is the DeclRefExpr or MemberRefExpr itself.  Zero when nothing is
/// done to it.
uint32_t rolesOf(const Use& use, CXCursor token);

/// Is `frame` a designated initializer (`.a.b = x`, `[i] = x`) -- the cursor
/// whose `MemberRef` children name the fields being initialised?  (An
/// `__builtin_offsetof` has `MemberRef` children too, and initialises
/// nothing.)
bool isDesignator(UseFrame& frame);
