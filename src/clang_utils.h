#pragma once

#include "clang_raii.h"
#include "symbol_table.h"
#include "symbol_types.h"

#include <clang-c/Index.h>
#include <filesystem>
#include <string>

// Check if a CXType is a pointer to a function (CXType_FunctionProto or CXType_FunctionNoProto)
inline bool isFunctionPointerType(CXType t) {
    if (t.kind != CXType_Pointer) return false;
    CXType pointee = clang_getPointeeType(t);
    return pointee.kind == CXType_FunctionProto || pointee.kind == CXType_FunctionNoProto;
}

// Resolve a FieldDecl cursor to its compound name "struct_name.field_name".
// Returns empty string on failure (null cursor, non-struct parent, or empty names).
inline std::string resolveFieldCompoundName(CXCursor field_decl) {
    CXCursor parent = clang_getCursorSemanticParent(field_decl);
    if (clang_getCursorKind(parent) != CXCursor_StructDecl) return "";
    ClangString sn(clang_getCursorSpelling(parent));
    std::string struct_name = sn.to_string();
    if (struct_name.empty()) return "";
    ClangString fn(clang_getCursorSpelling(field_decl));
    std::string field_name = fn.to_string();
    if (field_name.empty()) return "";
    return struct_name + "." + field_name;
}

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

// Get expansion range from a CXSourceRange. Returns false if the range spans
// multiple files or is invalid. When clamp_zero_width is true, zero-width ranges
// are clamped to [start, start+1); when false, zero-width ranges return false.
inline bool getExpansionRange(CXSourceRange range, CXFile& out_file,
                              unsigned& out_start, unsigned& out_end,
                              bool clamp_zero_width = false) {
    CXFile start_file = nullptr, end_file = nullptr;
    clang_getExpansionLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &out_start);
    clang_getExpansionLocation(clang_getRangeEnd(range), &end_file, nullptr, nullptr, &out_end);
    if (!start_file || !end_file || !clang_File_isEqual(start_file, end_file)) return false;
    if (out_start == out_end) {
        if (!clamp_zero_width) return false;
        out_end = out_start + 1;
    }
    out_file = start_file;
    return true;
}

// Get expansion range for a cursor's extent.
inline bool getExpansionRange(CXCursor cursor, CXFile& out_file,
                              unsigned& out_start, unsigned& out_end,
                              bool clamp_zero_width = false) {
    return getExpansionRange(clang_getCursorExtent(cursor), out_file,
                             out_start, out_end, clamp_zero_width);
}

// Get spelling range for a cursor. Returns false if the file is null.
// Always clamps zero-width ranges to [start, start+1) to preserve refs inside macro bodies.
// When the extent spans multiple files (e.g., CallExpr with macro-parameter arguments),
// falls back to cursor location spelling + name_len for the range.
inline bool getSpellingRange(CXCursor cursor, CXFile& out_file,
                             unsigned& out_start, unsigned& out_end,
                             unsigned name_len = 0) {
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXFile start_file = nullptr, end_file = nullptr;
    clang_getSpellingLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr, &out_start);
    clang_getSpellingLocation(clang_getRangeEnd(range), &end_file, nullptr, nullptr, &out_end);
    if (!start_file) return false;
    if (!end_file || !clang_File_isEqual(start_file, end_file) || out_start >= out_end) {
        // Extent spans multiple files or is zero-width; use start + name_len
        out_end = out_start + (name_len > 0 ? name_len : 1);
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
