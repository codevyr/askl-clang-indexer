#include "stage2.h"
#include "clang_raii.h"
#include "clang_utils.h"
#include "symbol_table.h"
#include "symbol_types.h"

Stage2::Stage2(SymbolTable& symbols) : symbols_(symbols) {}

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

void Stage2::addFuncPtrRef(CXCursor func_ref, CXFile source_file, unsigned start_off, unsigned end_off) {
    CXCursor referenced = clang_getCursorReferenced(func_ref);
    if (clang_Cursor_isNull(referenced)) return;
    if (clang_getCursorKind(referenced) != CXCursor_FunctionDecl) return;

    ClangString ref_name(clang_getCursorSpelling(referenced));
    std::string sname = ref_name.to_string();
    if (sname.empty()) return;

    int64_t sym_id = resolveSymbol(symbols_, referenced, sname, SYMTYPE_FUNCTION);

    Stage2Ref ref;
    ref.source_file = getCanonicalPath(source_file);
    ref.data = {sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)};
    result_.refs.push_back(std::move(ref));
}

void Stage2::addFieldImplRef(CXCursor member_ref, CXCursor func_ref) {
    CXCursor field_decl = clang_getCursorReferenced(member_ref);
    addFieldImplRefFromDecl(field_decl, func_ref);
}

void Stage2::addFieldImplRefFromDecl(CXCursor field_decl, CXCursor func_ref) {
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

    // Create synthetic ref: func implementation attributed to the field's declaration site
    Stage2Ref ref;
    ref.source_file = getCanonicalPath(field_file);
    ref.data = {func_sym_id, static_cast<int32_t>(field_start), static_cast<int32_t>(field_end)};
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

    CXFile range_file;
    unsigned start_off, end_off;
    if (getExpansionRange(child, range_file, start_off, end_off, true))
        addFuncPtrRef(data.func_ref, range_file, start_off, end_off);

    if (data.has_member) {
        addFieldImplRef(data.member_ref, data.func_ref);
    } else if (field_index < (int)fields.size()) {
        addFieldImplRefFromDecl(fields[field_index], data.func_ref);
    }
}

// Helper to find a DeclRefExpr to a FunctionDecl in the AST subtree
struct RhsFuncFinder {
    CXCursor func_ref;
    bool found = false;
};

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

    if (!rhs_data.found) return;

    CXFile range_file;
    unsigned start_off, end_off;
    if (getExpansionRange(cursor, range_file, start_off, end_off, true)) {
        addFuncPtrRef(rhs_data.func_ref, range_file, start_off, end_off);
    }
    // Create synthetic ref from field declaration to implementing function
    addFieldImplRef(lhs, rhs_data.func_ref);
}

void Stage2::process(CXTranslationUnit tu, const std::string& tu_filename) {
    tu_filename_ = tu_filename;
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, this);
}

Stage2Result Stage2::takeResults() {
    return std::move(result_);
}
