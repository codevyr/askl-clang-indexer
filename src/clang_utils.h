#pragma once

#include "clang_raii.h"
#include "symbol_table.h"
#include "symbol_types.h"

#include <clang-c/Index.h>
#include <filesystem>
#include <string>

// Canonicalize a path from clang_getFileName (resolve relative paths to absolute)
inline std::string canonicalizePath(const std::string& path) {
    if (!path.empty() && path[0] != '/') {
        std::error_code ec;
        auto abs = std::filesystem::canonical(path, ec);
        if (!ec) return abs.string();
    }
    return path;
}

// Get the canonical path for a CXFile
inline std::string getCanonicalPath(CXFile file) {
    ClangString name(clang_getFileName(file));
    return canonicalizePath(name.to_string());
}

// Get expansion range for a cursor. Returns false if the range spans multiple files,
// or if the range is zero-width and clamp_zero_width is false (declaration mode).
// When clamp_zero_width is true (ref mode), zero-width ranges are clamped to
// [start, start+1) to preserve the reference to the macro expansion site.
inline bool getExpansionRange(CXCursor cursor, CXFile& out_file,
                              unsigned& out_start, unsigned& out_end,
                              bool clamp_zero_width = false) {
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXFile start_file = nullptr, end_file = nullptr;
    clang_getExpansionLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &out_start);
    clang_getExpansionLocation(clang_getRangeEnd(range), &end_file, nullptr, nullptr, &out_end);
    if (!start_file || !clang_File_isEqual(start_file, end_file)) return false;
    if (out_start == out_end) {
        if (!clamp_zero_width) return false;
        out_end = out_start + 1;
    }
    out_file = start_file;
    return true;
}

// Resolve a cursor's linkage to a scope-aware symbol ID.
// Uses expansion location to get the canonical file path for LOCAL scope dedup.
inline int64_t resolveSymbol(SymbolTable& symbols, CXCursor cursor,
                             const std::string& name, int sym_type) {
    CXLinkageKind linkage = clang_getCursorLinkage(cursor);
    int scope = (linkage == CXLinkage_Internal) ? SCOPE_LOCAL : SCOPE_GLOBAL;

    std::string file_path;
    if (scope == SCOPE_LOCAL) {
        CXSourceLocation loc = clang_getCursorLocation(cursor);
        CXFile file = nullptr;
        clang_getExpansionLocation(loc, &file, nullptr, nullptr, nullptr);
        if (file) {
            file_path = getCanonicalPath(file);
        }
    }

    auto [sym_id, _] = symbols.getOrCreate(name, scope, sym_type, file_path);
    return sym_id;
}
