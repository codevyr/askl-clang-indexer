#include "stage1.h"
#include "clang_raii.h"
#include "clang_utils.h"
#include "file_utils.h"
#include "symbol_table.h"
#include "symbol_types.h"

#include <cstdio>
#include <fstream>

Stage1::Stage1(SymbolTable& symbols, std::atomic<int64_t>& next_object_id)
    : symbols_(symbols), next_object_id_(next_object_id) {}

size_t Stage1::getOrCreateFile(CXFile file) {
    std::string path = getCanonicalPath(file);

    auto it = file_index_.find(path);
    if (it != file_index_.end()) {
        return it->second;
    }

    FileData fd;
    fd.object_local_id = next_object_id_.fetch_add(1);
    fd.filesystem_path = path;
    fd.module_path = path;
    fd.filetype = computeFiletype(path);

    std::ifstream in(path, std::ios::binary);
    if (in) {
        fd.content = std::vector<uint8_t>(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
    }

    // Create FILE symbol
    auto [file_sym_id, _] = symbols_.getOrCreate(
        fd.module_path, SCOPE_GLOBAL, SYMTYPE_FILE);
    fd.instances.push_back({file_sym_id, 0, static_cast<int32_t>(fd.content.size())});

    size_t idx = result_.files.size();
    result_.files.push_back(std::move(fd));
    file_index_[path] = idx;
    return idx;
}

CXChildVisitResult Stage1::macroVisitor(CXCursor cursor, CXCursor parent, CXClientData data) {
    auto* self = static_cast<Stage1*>(data);
    CXCursorKind kind = clang_getCursorKind(cursor);

    if (kind == CXCursor_MacroDefinition) {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) return CXChildVisit_Continue;

        // Get the file where the macro is defined
        CXSourceLocation loc = clang_getCursorLocation(cursor);
        CXFile file = nullptr;
        clang_getSpellingLocation(loc, &file, nullptr, nullptr, nullptr);
        if (!file) return CXChildVisit_Continue;

        // Create MACRO symbol
        auto [sym_id, _] = self->symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_MACRO);

        // Create instance at the #define body range
        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off)) {
            size_t fi = self->getOrCreateFile(range_file);
            self->result_.files[fi].instances.push_back(
                {sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
    } else if (kind == CXCursor_MacroExpansion) {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) return CXChildVisit_Continue;

        // Look up the MACRO symbol (should have been created by MacroDefinition)
        auto [sym_id, _] = self->symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_MACRO);

        // Create ref at spelling location
        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off, sname.size())) {
            size_t fi = self->getOrCreateFile(range_file);
            self->result_.files[fi].refs.push_back(
                {sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
    }

    return CXChildVisit_Continue;
}

void Stage1::collectMacros(CXTranslationUnit tu) {
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, macroVisitor, this);
}

CXChildVisitResult Stage1::visitor(CXCursor cursor, CXCursor parent, CXClientData data) {
    auto* self = static_cast<Stage1*>(data);
    self->visitCursor(cursor, parent);
    return CXChildVisit_Recurse;
}

void Stage1::visitCursor(CXCursor cursor, CXCursor parent) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    if (clang_equalLocations(loc, clang_getNullLocation())) return;

    CXFile file = nullptr;
    clang_getExpansionLocation(loc, &file, nullptr, nullptr, nullptr);
    if (!file) return;

    switch (kind) {
    case CXCursor_FunctionDecl: {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        int64_t sym_id = resolveSymbol(symbols_, cursor, sname, SYMTYPE_FUNCTION);

        CXFile range_file;
        unsigned start_off, end_off;
        if (getExpansionRange(cursor, range_file, start_off, end_off)) {
            size_t fi = getOrCreateFile(range_file);
            result_.files[fi].instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
    case CXCursor_EnumDecl:
    case CXCursor_TypedefDecl: {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        auto [sym_id, _] = symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_TYPE);

        CXFile range_file;
        unsigned start_off, end_off;
        if (getExpansionRange(cursor, range_file, start_off, end_off)) {
            size_t fi = getOrCreateFile(range_file);
            result_.files[fi].instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_VarDecl: {
        CXCursor semantic_parent = clang_getCursorSemanticParent(cursor);
        if (clang_getCursorKind(semantic_parent) != CXCursor_TranslationUnit) break;

        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        int64_t sym_id = resolveSymbol(symbols_, cursor, sname, SYMTYPE_DATA);

        CXFile range_file;
        unsigned start_off, end_off;
        if (getExpansionRange(cursor, range_file, start_off, end_off)) {
            size_t fi = getOrCreateFile(range_file);
            result_.files[fi].instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_CallExpr: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;
        if (clang_getCursorKind(referenced) != CXCursor_FunctionDecl) break;

        ClangString ref_name(clang_getCursorSpelling(referenced));
        std::string sname = ref_name.to_string();
        if (sname.empty()) break;

        int64_t sym_id = resolveSymbol(symbols_, referenced, sname, SYMTYPE_FUNCTION);

        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off, sname.size())) {
            size_t fi = getOrCreateFile(range_file);
            result_.files[fi].refs.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_DeclRefExpr: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;

        CXCursorKind ref_kind = clang_getCursorKind(referenced);

        // Only handle file-scope VarDecl and EnumConstantDecl.
        // ParmDecl, FunctionDecl (handled by CallExpr/Stage2), and others are skipped.
        if (ref_kind == CXCursor_VarDecl) {
            CXCursor ref_parent = clang_getCursorSemanticParent(referenced);
            if (clang_getCursorKind(ref_parent) != CXCursor_TranslationUnit) break;
        } else if (ref_kind != CXCursor_EnumConstantDecl) {
            break;
        }

        ClangString ref_name(clang_getCursorSpelling(referenced));
        std::string sname = ref_name.to_string();
        if (sname.empty()) break;

        int64_t sym_id = resolveSymbol(symbols_, referenced, sname, SYMTYPE_DATA);

        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off, sname.size())) {
            size_t fi = getOrCreateFile(range_file);
            result_.files[fi].refs.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_TypeRef: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;

        ClangString ref_name(clang_getCursorSpelling(referenced));
        std::string sname = ref_name.to_string();
        if (sname.empty()) break;

        auto [sym_id, _] = symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_TYPE);

        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off, sname.size())) {
            size_t fi = getOrCreateFile(range_file);
            // Note: clang visits TypeRef children of anonymous types through both
            // the TypedefDecl and the underlying type (UnionDecl/StructDecl),
            // producing duplicate refs. ProtoBuilder deduplicates via hash-set.
            result_.files[fi].refs.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    default:
        break;
    }
}

void Stage1::inclusionCallback(CXFile included_file, CXSourceLocation*,
                               unsigned, CXClientData data) {
    auto* self = static_cast<Stage1*>(data);
    self->getOrCreateFile(included_file);
}

void Stage1::process(CXTranslationUnit tu, const std::string& tu_filename) {
    // First, ensure all included files get FileData entries (with content + FILE symbol),
    // even if they contain no handled cursor kinds (e.g., macro-only headers).
    clang_getInclusions(tu, inclusionCallback, this);

    // Then also ensure the TU's own source file is indexed.
    CXFile tu_file = clang_getFile(tu, tu_filename.c_str());
    if (tu_file) getOrCreateFile(tu_file);

    // Collect macro definitions and expansions (requires DetailedPreprocessingRecord)
    collectMacros(tu);

    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, this);
}

Stage1Result Stage1::takeResults() {
    result_.file_index = std::move(file_index_);
    return std::move(result_);
}
