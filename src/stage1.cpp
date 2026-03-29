#include "stage1.h"
#include "clang_raii.h"
#include "symbol_table.h"
#include "symbol_types.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>

static std::atomic<int64_t> global_object_id{1};

Stage1::Stage1(SymbolTable& symbols, const std::string& root_path)
    : symbols_(symbols), root_path_(root_path) {}

std::string Stage1::computeModulePath(const std::string& abs_path) {
    if (abs_path.size() > root_path_.size() &&
        abs_path.substr(0, root_path_.size()) == root_path_ &&
        abs_path[root_path_.size()] == '/') {
        return abs_path.substr(root_path_.size() + 1);
    }
    if (abs_path == root_path_) return "";
    return abs_path;
}

std::string Stage1::computeFiletype(const std::string& path) {
    if (path.size() >= 2 && path.substr(path.size() - 2) == ".c") return "text/x-c";
    if (path.size() >= 2 && path.substr(path.size() - 2) == ".h") return "text/x-c-header";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".cpp") return "text/x-c";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".hpp") return "text/x-c-header";
    return "text/plain";
}

size_t Stage1::getOrCreateFile(CXFile file) {
    ClangString filename(clang_getFileName(file));
    std::string path = filename.to_string();

    // clang_getFileName may return relative paths for headers included
    // via relative -I/-iquote paths. Resolve to absolute.
    if (!path.empty() && path[0] != '/') {
        std::error_code ec;
        auto abs = std::filesystem::canonical(path, ec);
        if (!ec) path = abs.string();
    }

    auto it = file_index_.find(path);
    if (it != file_index_.end()) {
        return it->second;
    }

    FileData fd;
    fd.object_local_id = global_object_id.fetch_add(1);
    fd.filesystem_path = path;
    fd.module_path = computeModulePath(path);
    fd.filetype = computeFiletype(path);

    // Always read content — merging in indexer.cpp will pick whichever entry has it
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

bool Stage1::isLocalVariable(CXCursor cursor) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    if (kind == CXCursor_VarDecl) {
        CXCursor semantic_parent = clang_getCursorSemanticParent(cursor);
        CXCursorKind parent_kind = clang_getCursorKind(semantic_parent);
        // File-scope vars have TranslationUnit as parent
        return parent_kind != CXCursor_TranslationUnit;
    }
    if (kind == CXCursor_ParmDecl) return true;
    return false;
}

CXChildVisitResult Stage1::visitor(CXCursor cursor, CXCursor parent, CXClientData data) {
    auto* self = static_cast<Stage1*>(data);
    self->visitCursor(cursor, parent);
    return CXChildVisit_Recurse;
}

void Stage1::visitCursor(CXCursor cursor, CXCursor parent) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    CXSourceLocation loc = clang_getCursorLocation(cursor);

    // Skip cursors in system headers? No — plan says include ALL headers.
    // But skip invalid locations
    if (clang_equalLocations(loc, clang_getNullLocation())) return;

    CXFile file;
    unsigned line, col, offset;
    clang_getSpellingLocation(loc, &file, &line, &col, &offset);
    if (!file) return;

    switch (kind) {
    case CXCursor_FunctionDecl: {
        // Only process definitions or first declarations
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        CXLinkageKind linkage = clang_getCursorLinkage(cursor);
        int scope = (linkage == CXLinkage_Internal) ? SCOPE_LOCAL : SCOPE_GLOBAL;

        ClangString file_name(clang_getFileName(file));
        std::string file_path = file_name.to_string();

        auto [sym_id, was_new] = symbols_.getOrCreate(
            sname, scope, SYMTYPE_FUNCTION,
            (scope == SCOPE_LOCAL) ? file_path : "");

        // Get extent for instance
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation start_loc = clang_getRangeStart(range);
        CXSourceLocation end_loc = clang_getRangeEnd(range);
        unsigned start_off, end_off;
        CXFile start_file, end_file;
        clang_getSpellingLocation(start_loc, &start_file, nullptr, nullptr, &start_off);
        clang_getSpellingLocation(end_loc, &end_file, nullptr, nullptr, &end_off);

        if (start_file && clang_File_isEqual(start_file, end_file)) {
            size_t fi = getOrCreateFile(start_file);
            result_.files[fi].instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
    case CXCursor_EnumDecl: {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break; // skip anonymous

        auto [sym_id, was_new] = symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_TYPE);

        CXSourceRange range = clang_getCursorExtent(cursor);
        unsigned start_off, end_off;
        CXFile start_file, end_file;
        clang_getSpellingLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &start_off);
        clang_getSpellingLocation(clang_getRangeEnd(range), &end_file, nullptr, nullptr, &end_off);

        if (start_file && clang_File_isEqual(start_file, end_file)) {
            size_t fi = getOrCreateFile(start_file);
            result_.files[fi].instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_TypedefDecl: {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        auto [sym_id, was_new] = symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_TYPE);

        CXSourceRange range = clang_getCursorExtent(cursor);
        unsigned start_off, end_off;
        CXFile start_file, end_file;
        clang_getSpellingLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &start_off);
        clang_getSpellingLocation(clang_getRangeEnd(range), &end_file, nullptr, nullptr, &end_off);

        if (start_file && clang_File_isEqual(start_file, end_file)) {
            size_t fi = getOrCreateFile(start_file);
            result_.files[fi].instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_VarDecl: {
        // Only file-scope variables
        CXCursor semantic_parent = clang_getCursorSemanticParent(cursor);
        if (clang_getCursorKind(semantic_parent) != CXCursor_TranslationUnit) break;

        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        CXLinkageKind linkage = clang_getCursorLinkage(cursor);
        int scope = (linkage == CXLinkage_Internal) ? SCOPE_LOCAL : SCOPE_GLOBAL;

        ClangString file_name(clang_getFileName(file));
        std::string file_path = file_name.to_string();

        auto [sym_id, was_new] = symbols_.getOrCreate(
            sname, scope, SYMTYPE_DATA,
            (scope == SCOPE_LOCAL) ? file_path : "");

        CXSourceRange range = clang_getCursorExtent(cursor);
        unsigned start_off, end_off;
        CXFile start_file, end_file;
        clang_getSpellingLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &start_off);
        clang_getSpellingLocation(clang_getRangeEnd(range), &end_file, nullptr, nullptr, &end_off);

        if (start_file && clang_File_isEqual(start_file, end_file)) {
            size_t fi = getOrCreateFile(start_file);
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

        CXLinkageKind linkage = clang_getCursorLinkage(referenced);
        int scope = (linkage == CXLinkage_Internal) ? SCOPE_LOCAL : SCOPE_GLOBAL;

        // Get file path of the referenced function for LOCAL dedup
        std::string ref_file_path;
        if (scope == SCOPE_LOCAL) {
            CXSourceLocation ref_loc = clang_getCursorLocation(referenced);
            CXFile ref_file;
            clang_getSpellingLocation(ref_loc, &ref_file, nullptr, nullptr, nullptr);
            if (ref_file) {
                ClangString rf(clang_getFileName(ref_file));
                ref_file_path = rf.to_string();
            }
        }

        auto [sym_id, _] = symbols_.getOrCreate(
            sname, scope, SYMTYPE_FUNCTION,
            (scope == SCOPE_LOCAL) ? ref_file_path : "");

        // Use expansion location for where the call occurs
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation start_loc = clang_getRangeStart(range);
        CXSourceLocation end_loc = clang_getRangeEnd(range);
        CXFile call_file, call_end_file;
        unsigned start_off, end_off;
        clang_getExpansionLocation(start_loc, &call_file, nullptr, nullptr, &start_off);
        clang_getExpansionLocation(end_loc, &call_end_file, nullptr, nullptr, &end_off);

        if (call_file && clang_File_isEqual(call_file, call_end_file)) {
            size_t fi = getOrCreateFile(call_file);
            result_.files[fi].refs.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_DeclRefExpr: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;

        CXCursorKind ref_kind = clang_getCursorKind(referenced);

        // Skip local variables
        if (ref_kind == CXCursor_VarDecl) {
            CXCursor ref_parent = clang_getCursorSemanticParent(referenced);
            if (clang_getCursorKind(ref_parent) != CXCursor_TranslationUnit) break;
        } else if (ref_kind == CXCursor_ParmDecl) {
            break;
        } else if (ref_kind == CXCursor_FunctionDecl) {
            // Skip — direct calls are handled by CallExpr, function pointer
            // assignments are handled by Stage 2.
            break;
        } else if (ref_kind == CXCursor_EnumConstantDecl) {
            // Enum constant references — treat as DATA refs
        } else {
            break;
        }

        ClangString ref_name(clang_getCursorSpelling(referenced));
        std::string sname = ref_name.to_string();
        if (sname.empty()) break;

        CXLinkageKind linkage = clang_getCursorLinkage(referenced);
        int scope = (linkage == CXLinkage_Internal) ? SCOPE_LOCAL : SCOPE_GLOBAL;

        int sym_type;
        if (ref_kind == CXCursor_FunctionDecl) sym_type = SYMTYPE_FUNCTION;
        else if (ref_kind == CXCursor_EnumConstantDecl) sym_type = SYMTYPE_DATA;
        else sym_type = SYMTYPE_DATA; // file-scope VarDecl

        std::string ref_file_path;
        if (scope == SCOPE_LOCAL) {
            CXSourceLocation ref_loc = clang_getCursorLocation(referenced);
            CXFile ref_file;
            clang_getSpellingLocation(ref_loc, &ref_file, nullptr, nullptr, nullptr);
            if (ref_file) {
                ClangString rf(clang_getFileName(ref_file));
                ref_file_path = rf.to_string();
            }
        }

        auto [sym_id, _] = symbols_.getOrCreate(
            sname, scope, sym_type,
            (scope == SCOPE_LOCAL) ? ref_file_path : "");

        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation start_loc = clang_getRangeStart(range);
        CXSourceLocation end_loc = clang_getRangeEnd(range);
        CXFile expr_file, expr_end_file;
        unsigned start_off, end_off;
        clang_getExpansionLocation(start_loc, &expr_file, nullptr, nullptr, &start_off);
        clang_getExpansionLocation(end_loc, &expr_end_file, nullptr, nullptr, &end_off);

        if (expr_file && clang_File_isEqual(expr_file, expr_end_file)) {
            size_t fi = getOrCreateFile(expr_file);
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

        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation start_loc = clang_getRangeStart(range);
        CXSourceLocation end_loc = clang_getRangeEnd(range);
        CXFile ref_file, ref_end_file;
        unsigned start_off, end_off;
        clang_getExpansionLocation(start_loc, &ref_file, nullptr, nullptr, &start_off);
        clang_getExpansionLocation(end_loc, &ref_end_file, nullptr, nullptr, &end_off);

        if (ref_file && clang_File_isEqual(ref_file, ref_end_file)) {
            size_t fi = getOrCreateFile(ref_file);
            result_.files[fi].refs.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    default:
        break;
    }
}

void Stage1::process(CXTranslationUnit tu, const std::string& tu_filename) {
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, this);
}

Stage1Result Stage1::takeResults() {
    return std::move(result_);
}
