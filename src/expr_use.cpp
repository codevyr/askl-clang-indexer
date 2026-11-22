#include "expr_use.h"

#include "clang_raii.h"
#include "symbol_types.h"

#include <climits>
#include <string>

namespace {

CXTypeKind canonicalKind(CXCursor cursor) {
    return clang_getCanonicalType(clang_getCursorType(cursor)).kind;
}

bool isArray(CXCursor cursor) {
    switch (canonicalKind(cursor)) {
    case CXType_ConstantArray:
    case CXType_IncompleteArray:
    case CXType_VariableArray:
    case CXType_DependentSizedArray:
        return true;
    default:
        return false;
    }
}

bool isPointer(CXCursor cursor) { return canonicalKind(cursor) == CXType_Pointer; }

/// Is `cursor` a pointer to a function?  `*` on one of those is no hop to
/// another object: `(*s->cb)(x)` is the call `s->cb(x)`.
bool pointsToFunction(CXCursor cursor) {
    CXType type = clang_getCanonicalType(clang_getCursorType(cursor));
    if (type.kind != CXType_Pointer) return false;
    CXTypeKind pointee = clang_getCanonicalType(clang_getPointeeType(type)).kind;
    return pointee == CXType_FunctionProto || pointee == CXType_FunctionNoProto;
}

/// How many children `cursor` has, counting no further than `limit`.
int childCount(CXCursor cursor, int limit) {
    struct Count {
        int n;
        int limit;
    } count{0, limit};
    clang_visitChildren(
        cursor,
        [](CXCursor, CXCursor, CXClientData data) {
            auto* c = static_cast<Count*>(data);
            return ++c->n >= c->limit ? CXChildVisit_Break : CXChildVisit_Continue;
        },
        &count);
    return count.n;
}

/// The builtins that evaluate none of their arguments.  They are calls in the
/// tree and questions about an expression in fact.
bool evaluatesNoArgument(CXCursor call) {
    CXCursor callee = clang_getCursorReferenced(call);
    if (clang_Cursor_isNull(callee) || clang_getCursorKind(callee) != CXCursor_FunctionDecl)
        return false;
    ClangString spelling(clang_getCursorSpelling(callee));
    const std::string name = spelling.to_string();
    return name == "__builtin_constant_p" || name == "__builtin_object_size" ||
           name == "__builtin_dynamic_object_size";
}

Use of(Use::Kind kind) {
    Use use;
    use.kind = kind;
    return use;
}

} // namespace

bool isDesignator(UseFrame& frame) {
    if (frame.designator < 0) {
        // A designated initializer is an "unexposed" cursor whose type is
        // `void`: it is no value, it says where one goes.  (`__builtin_offsetof`
        // is a `size_t`.)
        frame.designator = frame.kind == CXCursor_UnexposedExpr &&
                           clang_getCursorType(frame.cursor).kind == CXType_Void;
    }
    return frame.designator != 0;
}

Use childUse(UseFrame& p, CXCursor child) {
    const unsigned i = p.next_child++;
    const Use value = of(Use::Value);
    const Use unevaluated = of(Use::Unevaluated);

    // Nothing under an operand that is not evaluated is evaluated.
    if (p.use.kind == Use::Unevaluated) return unevaluated;

    switch (p.kind) {
    case CXCursor_CallExpr: {
        if (i == 0) {
            if (p.callee_first < 0) {
                // A call's first child is its callee, except where the call
                // is an overloaded operator written between its operands
                // (`a != b`): there the call names a function its first
                // child does not.
                CXCursor called = clang_getCursorReferenced(p.cursor);
                p.callee_first = clang_Cursor_isNull(called) ||
                                 clang_equalCursors(called, clang_getCursorReferenced(child));
            }
            if (p.callee_first) return of(Use::Callee);
        }
        if (p.args_unevaluated < 0) p.args_unevaluated = evaluatesNoArgument(p.cursor);
        return p.args_unevaluated ? unevaluated : value;
    }

    case CXCursor_BinaryOperator:
        if (i != 0) return value;
        if (p.op < 0) p.op = clang_getCursorBinaryOperatorKind(p.cursor);
        return p.op == CXBinaryOperator_Assign ? of(Use::Written) : value;

    case CXCursor_CompoundAssignOperator:
        return i == 0 ? of(Use::ReadWritten) : value;

    case CXCursor_UnaryOperator: {
        if (p.op < 0) p.op = clang_getCursorUnaryOperatorKind(p.cursor);
        switch (p.op) {
        case CXUnaryOperator_PostInc:
        case CXUnaryOperator_PostDec:
        case CXUnaryOperator_PreInc:
        case CXUnaryOperator_PreDec:
            return of(Use::ReadWritten);
        case CXUnaryOperator_AddrOf:
            // `*&x` is `x`.  Any other `&x` takes the address.
            return p.use.through_pointer ? of(p.use.kind) : value;
        case CXUnaryOperator_Deref: {
            // `(*s->cb)(x)` is the call `s->cb(x)`; any other `*p` used this
            // way uses what `p` points to.
            if (p.use.kind == Use::Callee) return pointsToFunction(child) ? p.use : value;
            if (p.use.kind == Use::Written || p.use.kind == Use::ReadWritten) {
                Use pointee = p.use;
                pointee.through_pointer = true;
                return pointee;
            }
            return value;
        }
        case CXUnaryOperator_Extension:
            return p.use;
        default:
            return value;
        }
    }

    case CXCursor_ParenExpr:
        return p.use;

    case CXCursor_UnexposedExpr:
        // "Unexposed" is several kinds of expression libclang does not tell
        // apart.  One that covers exactly the text of its child is a wrapper
        // the source does not spell (an implicit cast), and the use goes
        // through it.  A designated initializer evaluates its index and its
        // value.  Anything else (`__builtin_types_compatible_p`,
        // `__builtin_choose_expr`, `va_arg`) is not known to evaluate what is
        // below it.
        if (clang_equalRanges(clang_getCursorExtent(p.cursor), clang_getCursorExtent(child)))
            return p.use;
        return isDesignator(p) ? value : unevaluated;

    case CXCursor_CStyleCastExpr:
    case CXCursor_CXXStaticCastExpr:
    case CXCursor_CXXReinterpretCastExpr:
    case CXCursor_CXXConstCastExpr:
    case CXCursor_CXXFunctionalCastExpr:
        // The operand is the last child.  What stands before it is the type,
        // and an expression in a type is the operand of a `typeof`.
        if (p.n_children < 0) p.n_children = childCount(p.cursor, INT_MAX);
        return static_cast<int>(i) == p.n_children - 1 ? p.use : unevaluated;

    case CXCursor_MemberRefExpr: {
        // The child is what the member is a member OF.  Reached through a
        // pointer (`->`) it is read.  Reached directly (`.`) it is the whole
        // the member is a part of: assigned when the member is.
        if (p.use.through_pointer) return value;
        if (p.use.of_element && !isArray(p.cursor)) return value;
        if (isPointer(child)) return value;
        if (p.use.kind == Use::Written || p.use.kind == Use::ReadWritten) return of(p.use.kind);
        return value;
    }

    case CXCursor_ArraySubscriptExpr: {
        // `a[i]`: the use is of an element of `a`, and `i` is read.  `a` is
        // the operand that is a pointer -- an array is one by the time it is
        // subscripted -- wherever it stands (`i[a]` is `a[i]`).  Whether a
        // use of its element is a use of `a` itself is for the token to say,
        // which knows if it is an array.
        if (!isPointer(child)) return value;
        Use element = p.use;
        element.of_element = true;
        return element;
    }

    case CXCursor_UnaryExpr:            // sizeof, _Alignof
    case CXCursor_GenericSelectionExpr: // _Generic: one branch is chosen, by type
        return unevaluated;

    case CXCursor_VarDecl:
        // An expression in a declaration is its initializer, or it is part of
        // the type: the operand of a `typeof`, the size of an array.
        if (!clang_isExpression(clang_getCursorKind(child))) return value;
        if (!p.has_init) {
            p.init = clang_Cursor_getVarDeclInitializer(p.cursor);
            p.has_init = true;
        }
        return !clang_Cursor_isNull(p.init) && clang_equalCursors(p.init, child) ? value
                                                                                 : unevaluated;

    case CXCursor_ParmDecl:
    case CXCursor_FieldDecl:
    case CXCursor_TypedefDecl:
    case CXCursor_TypeAliasDecl:
        return clang_isExpression(clang_getCursorKind(child)) ? unevaluated : value;

    default:
        return value;
    }
}

uint32_t rolesOf(const Use& use, CXCursor token) {
    if (use.kind == Use::Unevaluated) return 0;
    // The pointer a use goes through is read; what is used is its target.
    if (use.through_pointer) return REFROLE_TAKE;
    if (use.of_element && !isArray(token)) return REFROLE_TAKE;
    switch (use.kind) {
    case Use::Callee:
        return REFROLE_CALL;
    case Use::Written:
        return REFROLE_WRITE;
    case Use::ReadWritten:
        return REFROLE_WRITE | REFROLE_TAKE;
    default:
        return REFROLE_TAKE;
    }
}
