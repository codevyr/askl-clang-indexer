#include "stage1.h"
#include "clang_raii.h"
#include "symbol_table.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>

static std::atomic<int64_t> global_object_id{1};

Stage1::Stage1(SymbolTable& symbols, const std::string& root_path,
               std::unordered_set<std::string>& processed_files, std::mutex& files_mutex)
    : symbols_(symbols), root_path_(root_path),
      processed_files_(processed_files), files_mutex_(files_mutex),
      next_object_id_(0) {}

std::string Stage1::computeModulePath(const std::string& abs_path) {
    if (abs_path.substr(0, root_path_.size()) == root_path_) {
        std::string rel = abs_path.substr(root_path_.size());
        if (!rel.empty() && rel[0] == '/') rel = rel.substr(1);
        return rel;
    }
    return abs_path;
}

std::string Stage1::computeFiletype(const std::string& path) {
    if (path.size() >= 2 && path.substr(path.size() - 2) == ".c") return "text/x-c";
    if (path.size() >= 2 && path.substr(path.size() - 2) == ".h") return "text/x-c-header";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".cpp") return "text/x-c";
    if (path.size() >= 4 && path.substr(path.size() - 4) == ".hpp") return "text/x-c-header";
    return "text/plain";
}

FileData& Stage1::getOrCreateFile(CXFile file) {
    ClangString filename(clang_getFileName(file));
    std::string path = filename.to_string();

    auto it = file_index_.find(path);
    if (it != file_index_.end()) {
        return result_.files[it->second];
    }

    // Check if already processed by another TU
    bool new_file;
    {
        std::lock_guard<std::mutex> lock(files_mutex_);
        new_file = processed_files_.insert(path).second;
    }

    FileData fd;
    fd.object_local_id = global_object_id.fetch_add(1);
    fd.filesystem_path = path;
    fd.module_path = computeModulePath(path);
    fd.filetype = computeFiletype(path);

    if (new_file) {
        // Read file content
        std::ifstream in(path, std::ios::binary);
        if (in) {
            fd.content = std::vector<uint8_t>(
                (std::istreambuf_iterator<char>(in)),
                std::istreambuf_iterator<char>());
        }

        // Create FILE symbol
        auto [file_sym_id, _] = symbols_.getOrCreate(
            fd.module_path, /*scope=*/2 /*GLOBAL*/, /*type=*/2 /*FILE*/);
        fd.instances.push_back({file_sym_id, 0, static_cast<int32_t>(fd.content.size())});
    }

    size_t idx = result_.files.size();
    result_.files.push_back(std::move(fd));
    file_index_[path] = idx;
    return result_.files[idx];
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
        int scope = (linkage == CXLinkage_Internal) ? 1 : 2;

        ClangString file_name(clang_getFileName(file));
        std::string file_path = file_name.to_string();

        auto [sym_id, was_new] = symbols_.getOrCreate(
            sname, scope, /*type=*/1 /*FUNCTION*/,
            (scope == 1) ? file_path : "");

        // Get extent for instance
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation start_loc = clang_getRangeStart(range);
        CXSourceLocation end_loc = clang_getRangeEnd(range);
        unsigned start_off, end_off;
        CXFile start_file;
        clang_getSpellingLocation(start_loc, &start_file, nullptr, nullptr, &start_off);
        clang_getSpellingLocation(end_loc, nullptr, nullptr, nullptr, &end_off);

        if (start_file) {
            FileData& fd = getOrCreateFile(start_file);
            fd.instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
    case CXCursor_EnumDecl: {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break; // skip anonymous

        auto [sym_id, was_new] = symbols_.getOrCreate(sname, /*scope=*/2, /*type=*/5 /*TYPE*/);

        CXSourceRange range = clang_getCursorExtent(cursor);
        unsigned start_off, end_off;
        CXFile start_file;
        clang_getSpellingLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &start_off);
        clang_getSpellingLocation(clang_getRangeEnd(range), nullptr, nullptr, nullptr, &end_off);

        if (start_file) {
            FileData& fd = getOrCreateFile(start_file);
            fd.instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_TypedefDecl: {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        auto [sym_id, was_new] = symbols_.getOrCreate(sname, /*scope=*/2, /*type=*/5 /*TYPE*/);

        CXSourceRange range = clang_getCursorExtent(cursor);
        unsigned start_off, end_off;
        CXFile start_file;
        clang_getSpellingLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &start_off);
        clang_getSpellingLocation(clang_getRangeEnd(range), nullptr, nullptr, nullptr, &end_off);

        if (start_file) {
            FileData& fd = getOrCreateFile(start_file);
            fd.instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
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
        int scope = (linkage == CXLinkage_Internal) ? 1 : 2;

        ClangString file_name(clang_getFileName(file));
        std::string file_path = file_name.to_string();

        auto [sym_id, was_new] = symbols_.getOrCreate(
            sname, scope, /*type=*/6 /*DATA*/,
            (scope == 1) ? file_path : "");

        CXSourceRange range = clang_getCursorExtent(cursor);
        unsigned start_off, end_off;
        CXFile start_file;
        clang_getSpellingLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &start_off);
        clang_getSpellingLocation(clang_getRangeEnd(range), nullptr, nullptr, nullptr, &end_off);

        if (start_file) {
            FileData& fd = getOrCreateFile(start_file);
            fd.instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
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
        int scope = (linkage == CXLinkage_Internal) ? 1 : 2;

        // Get file path of the referenced function for LOCAL dedup
        std::string ref_file_path;
        if (scope == 1) {
            CXSourceLocation ref_loc = clang_getCursorLocation(referenced);
            CXFile ref_file;
            clang_getSpellingLocation(ref_loc, &ref_file, nullptr, nullptr, nullptr);
            if (ref_file) {
                ClangString rf(clang_getFileName(ref_file));
                ref_file_path = rf.to_string();
            }
        }

        auto [sym_id, _] = symbols_.getOrCreate(
            sname, scope, /*type=*/1 /*FUNCTION*/,
            (scope == 1) ? ref_file_path : "");

        // Use expansion location for where the call occurs
        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation start_loc = clang_getRangeStart(range);
        CXSourceLocation end_loc = clang_getRangeEnd(range);
        CXFile call_file;
        unsigned start_off, end_off;
        clang_getExpansionLocation(start_loc, &call_file, nullptr, nullptr, &start_off);
        clang_getExpansionLocation(end_loc, nullptr, nullptr, nullptr, &end_off);

        if (call_file) {
            FileData& fd = getOrCreateFile(call_file);
            fd.refs.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
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
            // Function refs via DeclRefExpr that are NOT call expressions
            // (function pointers, etc.) — these are handled here
        } else if (ref_kind == CXCursor_EnumConstantDecl) {
            // Enum constant references — treat as DATA refs
        } else {
            break;
        }

        ClangString ref_name(clang_getCursorSpelling(referenced));
        std::string sname = ref_name.to_string();
        if (sname.empty()) break;

        CXLinkageKind linkage = clang_getCursorLinkage(referenced);
        int scope = (linkage == CXLinkage_Internal) ? 1 : 2;

        int sym_type;
        if (ref_kind == CXCursor_FunctionDecl) sym_type = 1; // FUNCTION
        else if (ref_kind == CXCursor_EnumConstantDecl) sym_type = 6; // DATA
        else sym_type = 6; // DATA (file-scope VarDecl)

        std::string ref_file_path;
        if (scope == 1) {
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
            (scope == 1) ? ref_file_path : "");

        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation start_loc = clang_getRangeStart(range);
        CXSourceLocation end_loc = clang_getRangeEnd(range);
        CXFile expr_file;
        unsigned start_off, end_off;
        clang_getExpansionLocation(start_loc, &expr_file, nullptr, nullptr, &start_off);
        clang_getExpansionLocation(end_loc, nullptr, nullptr, nullptr, &end_off);

        if (expr_file) {
            FileData& fd = getOrCreateFile(expr_file);
            fd.refs.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
        }
        break;
    }
    case CXCursor_TypeRef: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;

        ClangString ref_name(clang_getCursorSpelling(referenced));
        std::string sname = ref_name.to_string();
        if (sname.empty()) break;

        auto [sym_id, _] = symbols_.getOrCreate(sname, /*scope=*/2, /*type=*/5 /*TYPE*/);

        CXSourceRange range = clang_getCursorExtent(cursor);
        CXSourceLocation start_loc = clang_getRangeStart(range);
        CXSourceLocation end_loc = clang_getRangeEnd(range);
        CXFile ref_file;
        unsigned start_off, end_off;
        clang_getExpansionLocation(start_loc, &ref_file, nullptr, nullptr, &start_off);
        clang_getExpansionLocation(end_loc, nullptr, nullptr, nullptr, &end_off);

        if (ref_file) {
            FileData& fd = getOrCreateFile(ref_file);
            fd.refs.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off)});
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
