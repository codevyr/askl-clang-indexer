#include "stage2.h"
#include "clang_raii.h"
#include "clang_utils.h"
#include "symbol_table.h"
#include "symbol_types.h"

#include <utility>

Stage2::Stage2(SymbolTable& symbols, PlacedByStage1 placed_by_stage1,
               InvocationEndAt invocation_end_at)
    : symbols_(symbols),
      placed_by_stage1_(std::move(placed_by_stage1)),
      invocation_end_at_(std::move(invocation_end_at)) {}

// A PLACE A MACRO WROTE IS THE INVOCATION.
//
// Text that came out of a macro has no place of its own in the file: clang
// reports every token of an expansion at the point the invocation STARTS.  So
//
//     DEFINE_OPS(gen_ops, my_read);        // expands to `... = { .read = my_read }`
//     .read = WRAP(my_read),
//
// gave the first a place zero bytes wide and ended the second at `WRAP`.  A
// range that ends where an invocation starts ends where the invocation ends
// -- the same correction `Stage1::declExtent` makes for a declaration -- and
// the place is what the author typed there: the whole invocation, or the
// store up to and including it.
Stage2Place Stage2::placeOf(CXCursor cursor) const {
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXFile start_file = nullptr, end_file = nullptr;
    unsigned start = 0, end = 0;
    clang_getExpansionLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &start);
    clang_getExpansionLocation(clang_getRangeEnd(range), &end_file, nullptr, nullptr, &end);
    if (!start_file || !end_file || !clang_File_isEqual(start_file, end_file)) return {};

    Stage2Place place{true, getCanonicalPath(start_file), static_cast<int32_t>(start),
                      static_cast<int32_t>(end)};
    if (invocation_end_at_) {
        const int32_t invocation_end = invocation_end_at_(place.file, place.end);
        if (invocation_end > place.end) place.end = invocation_end;
    }
    // Nothing known to widen it with: one byte, so it is still a place.
    if (place.end <= place.start) place.end = place.start + 1;
    return place;
}

CXChildVisitResult Stage2::visitor(CXCursor cursor, CXCursor parent, CXClientData data) {
    auto* self = static_cast<Stage2*>(data);
    self->visitCursor(cursor, parent);
    return CXChildVisit_Recurse;
}

void Stage2::visitCursor(CXCursor cursor, CXCursor parent) {
    CXCursorKind kind = clang_getCursorKind(cursor);

    switch (kind) {
    case CXCursor_InitListExpr:
        handleInitList(cursor);
        break;

    case CXCursor_BinaryOperator:
        handleBinaryAssignment(cursor);
        break;

    default:
        break;
    }
}

// THE USE ITSELF, WHEN NOTHING BETTER CAN SAY IT.
//
// For a function stored in a field this pass records two things: which FIELD
// the function implements (below), and the use -- over the whole
// `.read = my_read` or `ops->read = my_read`.
//
// The second was the only record of the use while Stage1 took a reference to
// a function from calls alone.  It no longer is.  Stage1 records a function
// named as a value at the token that names it, wherever that token was
// written, and a name a macro pasted together at the part its caller typed.
// Wherever it does, a reference over the wider construct says the same use a
// second time, with a different range, and nothing downstream can tell the
// two are one.
//
// So this is the FALLBACK and nothing else: emitted only when Stage1 has no
// token to put the reference on, which leaves a pasted name the replay could
// not follow.  Asked of Stage1 itself rather than re-derived here, so the two
// passes cannot come to disagree about what counts as placed.
void Stage2::addFuncPtrRef(CXCursor func_ref, CXFile source_file, unsigned start_off,
                           unsigned end_off) {
    CXCursor referenced = clang_getCursorReferenced(func_ref);
    if (clang_Cursor_isNull(referenced)) return;
    if (clang_getCursorKind(referenced) != CXCursor_FunctionDecl) return;

    ClangString ref_name(clang_getCursorSpelling(referenced));
    std::string sname = ref_name.to_string();
    if (sname.empty()) return;

    if (placed_by_stage1_ && placed_by_stage1_(func_ref, sname)) return;

    int64_t sym_id = resolveSymbol(symbols_, referenced, sname, SYMTYPE_FUNCTION);

    Stage2Ref ref;
    ref.source_file = getCanonicalPath(source_file);
    ref.data = {sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)};
    // The same use Stage1 would have put on the token: the function is
    // stored, not called.
    ref.data.roles = REFROLE_TAKE;
    result_.refs.push_back(std::move(ref));
}

void Stage2::addFieldImplRef(CXCursor member_ref, CXCursor func_ref,
                             const Stage2Place& made_at) {
    CXCursor field_decl = clang_getCursorReferenced(member_ref);
    addFieldImplRefFromDecl(field_decl, func_ref, made_at);
}

void Stage2::addFieldImplRefFromDecl(CXCursor field_decl, CXCursor func_ref,
                                     const Stage2Place& made_at) {
    // Validate the field declaration first (cheap checks)
    if (clang_Cursor_isNull(field_decl)) return;
    if (clang_getCursorKind(field_decl) != CXCursor_FieldDecl) return;
    if (resolveFieldCompoundName(field_decl).empty()) return;

    // Get the field declaration's location (in the header)
    CXFile field_file;
    unsigned field_start, field_end;
    if (!getExpansionRange(field_decl, field_file, field_start, field_end)) return;

    // Resolve the function being assigned
    CXCursor func_referenced = clang_getCursorReferenced(func_ref);
    if (clang_Cursor_isNull(func_referenced)) return;
    if (clang_getCursorKind(func_referenced) != CXCursor_FunctionDecl) return;

    ClangString func_name(clang_getCursorSpelling(func_referenced));
    std::string fname = func_name.to_string();
    if (fname.empty()) return;

    int64_t func_sym_id = resolveSymbol(symbols_, func_referenced, fname, SYMTYPE_FUNCTION);

    // THE SLOT HOLDS THE FUNCTION.  No token of the field's declaration says
    // so -- the assignment is somewhere else entirely -- so this is not a
    // reference but a relation, and it sits over the field's declaration,
    // directly in the field: the field is its source, and the struct around
    // it is not.
    Stage2Ref ref;
    ref.source_file = getCanonicalPath(field_file);
    ref.data = {func_sym_id, static_cast<int32_t>(field_start), static_cast<int32_t>(field_end)};
    ref.data.relation = REFRELATION_HOLDS;
    ref.made_at = made_at;
    result_.refs.push_back(std::move(ref));
}

void Stage2::addFieldToFieldRef(CXCursor lhs_member_ref, CXCursor rhs_member_ref,
                                const Stage2Place& made_at) {
    // Get the LHS field declaration and its source location
    CXCursor lhs_field_decl = clang_getCursorReferenced(lhs_member_ref);
    if (clang_Cursor_isNull(lhs_field_decl)) return;
    if (clang_getCursorKind(lhs_field_decl) != CXCursor_FieldDecl) return;
    if (resolveFieldCompoundName(lhs_field_decl).empty()) return;

    CXFile field_file;
    unsigned field_start, field_end;
    if (!getExpansionRange(lhs_field_decl, field_file, field_start, field_end)) return;

    // Get the RHS field symbol
    CXCursor rhs_field_decl = clang_getCursorReferenced(rhs_member_ref);
    if (clang_Cursor_isNull(rhs_field_decl)) return;
    if (clang_getCursorKind(rhs_field_decl) != CXCursor_FieldDecl) return;

    std::string rhs_compound = resolveFieldCompoundName(rhs_field_decl);
    if (rhs_compound.empty()) return;

    auto [rhs_sym_id, _] = symbols_.getOrCreate(rhs_compound, SCOPE_GLOBAL, SYMTYPE_FIELD);

    // The slot holds what the other slot holds: `ptr->a = other->b`.  The
    // same relation, over the LHS field's declaration.
    Stage2Ref ref;
    ref.source_file = getCanonicalPath(field_file);
    ref.data = {rhs_sym_id, static_cast<int32_t>(field_start), static_cast<int32_t>(field_end)};
    ref.data.relation = REFRELATION_HOLDS;
    ref.made_at = made_at;
    result_.refs.push_back(std::move(ref));
}

struct DesignatedInitData {
    CXCursor member_ref;
    CXCursor func_ref;
    bool has_member;
    bool has_func;
};

static CXChildVisitResult designatedInitVisitor(CXCursor child, CXCursor, CXClientData data) {
    auto* d = static_cast<DesignatedInitData*>(data);
    CXCursorKind kind = clang_getCursorKind(child);

    if (kind == CXCursor_MemberRef) {
        d->member_ref = child;
        d->has_member = true;
    } else if (kind == CXCursor_DeclRefExpr) {
        CXCursor referenced = clang_getCursorReferenced(child);
        if (!clang_Cursor_isNull(referenced) &&
            clang_getCursorKind(referenced) == CXCursor_FunctionDecl) {
            d->func_ref = child;
            d->has_func = true;
        }
    } else if (kind == CXCursor_UnexposedExpr) {
        clang_visitChildren(child, designatedInitVisitor, data);
    }
    return CXChildVisit_Continue;
}

void Stage2::handleInitList(CXCursor cursor) {
    // Get the struct type and enumerate its fields in declaration order.
    // Use getCanonicalType to resolve through typedefs.
    CXType init_type = clang_getCanonicalType(clang_getCursorType(cursor));
    CXCursor type_decl = clang_getTypeDeclaration(init_type);

    std::vector<CXCursor> fields;
    if (!clang_Cursor_isNull(type_decl)) {
        clang_visitChildren(type_decl, [](CXCursor child, CXCursor, CXClientData d) {
            if (clang_getCursorKind(child) == CXCursor_FieldDecl)
                static_cast<std::vector<CXCursor>*>(d)->push_back(child);
            return CXChildVisit_Continue;
        }, &fields);
    }

    struct Data { Stage2* self; std::vector<CXCursor>* fields; int index; }
        data{this, &fields, 0};

    clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData d) {
        auto* data = static_cast<Data*>(d);
        data->self->handleInitEntry(child, data->index, *data->fields);
        data->index++;
        return CXChildVisit_Continue;
    }, &data);
}

void Stage2::handleInitEntry(CXCursor child, int field_index,
                             const std::vector<CXCursor>& fields) {
    DesignatedInitData data{};
    CXCursorKind kind = clang_getCursorKind(child);

    if (kind == CXCursor_UnexposedExpr) {
        clang_visitChildren(child, designatedInitVisitor, &data);
    } else if (kind == CXCursor_DeclRefExpr) {
        CXCursor ref = clang_getCursorReferenced(child);
        if (!clang_Cursor_isNull(ref) &&
            clang_getCursorKind(ref) == CXCursor_FunctionDecl) {
            data.func_ref = child;
            data.has_func = true;
        }
    }

    if (!data.has_func) return;

    // The whole entry: `.read = my_read`, or the bare `my_read` of a
    // positional initialiser.  It is where the slot came to hold the function.
    const Stage2Place made_at = placeOf(child);
    CXFile range_file;
    unsigned start_off, end_off;
    if (getExpansionRange(child, range_file, start_off, end_off, true))
        addFuncPtrRef(data.func_ref, range_file, start_off, end_off);

    if (data.has_member) {
        addFieldImplRef(data.member_ref, data.func_ref, made_at);
    } else if (field_index < (int)fields.size()) {
        addFieldImplRefFromDecl(fields[field_index], data.func_ref, made_at);
    }
}

// Helper to find a DeclRefExpr to a FunctionDecl in the AST subtree
struct RhsFuncFinder {
    CXCursor func_ref;
    bool found = false;
};

// Helper to find a MemberRefExpr to a function-pointer field in the AST subtree
struct RhsFieldFinder {
    CXCursor member_ref;
    bool found = false;
};

static CXChildVisitResult findFieldInRhs(CXCursor child, CXCursor, CXClientData data) {
    auto* rd = static_cast<RhsFieldFinder*>(data);
    if (rd->found) return CXChildVisit_Break;

    CXCursorKind k = clang_getCursorKind(child);
    if (k == CXCursor_MemberRefExpr) {
        if (isFunctionPointerType(clang_getCursorType(child))) {
            rd->member_ref = child;
            rd->found = true;
            return CXChildVisit_Break;
        }
    } else if (k == CXCursor_UnexposedExpr) {
        clang_visitChildren(child, findFieldInRhs, data);
        if (rd->found) return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

static CXChildVisitResult findFuncInRhs(CXCursor child, CXCursor, CXClientData data) {
    auto* rd = static_cast<RhsFuncFinder*>(data);
    if (rd->found) return CXChildVisit_Break;

    CXCursorKind k = clang_getCursorKind(child);
    if (k == CXCursor_DeclRefExpr) {
        CXCursor ref = clang_getCursorReferenced(child);
        if (!clang_Cursor_isNull(ref) &&
            clang_getCursorKind(ref) == CXCursor_FunctionDecl) {
            rd->func_ref = child;
            rd->found = true;
            return CXChildVisit_Break;
        }
    } else if (k == CXCursor_UnexposedExpr) {
        clang_visitChildren(child, findFuncInRhs, data);
        if (rd->found) return CXChildVisit_Break;
    }
    return CXChildVisit_Continue;
}

void Stage2::handleBinaryAssignment(CXCursor cursor) {
    struct BinOpData {
        CXCursor children[2];
        int idx = 0;
    } binop;

    clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
        auto* d = static_cast<BinOpData*>(data);
        if (d->idx < 2) {
            d->children[d->idx++] = child;
        }
        return CXChildVisit_Continue;
    }, &binop);

    if (binop.idx != 2) return;

    CXCursor lhs = binop.children[0];
    CXCursor rhs = binop.children[1];

    if (clang_getCursorKind(lhs) != CXCursor_MemberRefExpr) return;

    if (!isFunctionPointerType(clang_getCursorType(lhs))) return;

    // The whole assignment, `ops->read = my_read`: where the slot came to
    // hold what is on the right.
    const Stage2Place made_at = placeOf(cursor);

    RhsFuncFinder rhs_data;
    // Check if rhs itself is a DeclRefExpr
    if (clang_getCursorKind(rhs) == CXCursor_DeclRefExpr) {
        CXCursor ref = clang_getCursorReferenced(rhs);
        if (!clang_Cursor_isNull(ref) &&
            clang_getCursorKind(ref) == CXCursor_FunctionDecl) {
            rhs_data.func_ref = rhs;
            rhs_data.found = true;
        }
    }
    if (!rhs_data.found) {
        clang_visitChildren(rhs, findFuncInRhs, &rhs_data);
    }

    if (!rhs_data.found) {
        // field-to-field: ptr->a = other->b
        RhsFieldFinder field_data;
        if (clang_getCursorKind(rhs) == CXCursor_MemberRefExpr &&
            isFunctionPointerType(clang_getCursorType(rhs))) {
            field_data.member_ref = rhs;
            field_data.found = true;
        }
        if (!field_data.found)
            clang_visitChildren(rhs, findFieldInRhs, &field_data);
        if (field_data.found)
            addFieldToFieldRef(lhs, field_data.member_ref, made_at);
        return;
    }

    CXFile range_file;
    unsigned start_off, end_off;
    if (getExpansionRange(cursor, range_file, start_off, end_off, true)) {
        addFuncPtrRef(rhs_data.func_ref, range_file, start_off, end_off);
    }
    // Create synthetic ref from field declaration to implementing function
    addFieldImplRef(lhs, rhs_data.func_ref, made_at);
}

void Stage2::process(CXTranslationUnit tu, const std::string& tu_filename) {
    tu_filename_ = tu_filename;
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, this);
}

Stage2Result Stage2::takeResults() {
    return std::move(result_);
}
