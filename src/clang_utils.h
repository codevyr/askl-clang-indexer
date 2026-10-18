#pragma once

#include "clang_raii.h"
#include "symbol_table.h"
#include "symbol_types.h"

#include <clang-c/Index.h>
#include <algorithm>
#include <cstring>
#include <filesystem>
#include <string>

// Check if a CXType is a pointer to a function (CXType_FunctionProto or CXType_FunctionNoProto).
// Resolves through typedefs so that e.g. `typedef void (*func_t)(void);` fields are recognized.
inline bool isFunctionPointerType(CXType t) {
    CXType canonical = clang_getCanonicalType(t);
    if (canonical.kind != CXType_Pointer) return false;
    CXType pointee = clang_getPointeeType(canonical);
    return pointee.kind == CXType_FunctionProto || pointee.kind == CXType_FunctionNoProto;
}

// Resolve a child cursor (FieldDecl or EnumConstantDecl) to a compound name
// "parent_name.child_name".  Returns empty string when the parent kind doesn't
// match, the parent is anonymous, or either name is empty.
inline std::string resolveCompoundName(CXCursor child, CXCursorKind expected_parent_kind) {
    CXCursor parent = clang_getCursorSemanticParent(child);
    if (clang_getCursorKind(parent) != expected_parent_kind) return "";
    if (clang_Cursor_isAnonymous(parent)) return "";
    ClangString pn(clang_getCursorSpelling(parent));
    std::string parent_name = pn.to_string();
    if (parent_name.empty()) return "";
    ClangString cn(clang_getCursorSpelling(child));
    std::string child_name = cn.to_string();
    if (child_name.empty()) return "";
    return parent_name + "." + child_name;
}


// Canonicalize a path from clang_getFileName (resolve relative paths to absolute,
// normalize away . and .. components in absolute paths)
inline std::string canonicalizePath(const std::string& path) {
    if (path.empty()) return path;
    if (path[0] != '/') {
        std::error_code ec;
        auto abs = std::filesystem::canonical(path, ec);
        if (!ec) return abs.string();
        // canonical() failed (file doesn't exist on disk) — fall back to
        // prepending cwd so the result is still an absolute path.
        return (std::filesystem::current_path() / path).lexically_normal().string();
    }
    return std::filesystem::path(path).lexically_normal().string();
}

// Get the canonical path for a CXFile.
// Prefer clang_File_tryGetRealPathName which returns the OS-resolved real path
// (follows symlinks, resolves the actual location on disk).  This gives a
// consistent canonical path regardless of which -I include path was used to
// find the file.  Fall back to clang_getFileName + canonicalizePath when the
// real-path API returns an empty string.
inline std::string getCanonicalPath(CXFile file) {
    ClangString real(clang_File_tryGetRealPathName(file));
    std::string real_path = real.to_string();
    if (!real_path.empty())
        return real_path;
    ClangString name(clang_getFileName(file));
    return canonicalizePath(name.to_string());
}

/// Rewrite the source paths clang embeds in a symbol's NAME so that one record
/// has one name.
///
/// clang identifies a record with no tag by where it is written --
/// `struct (anonymous at compel/include/uapi/asm/fpu.h:122:2)` -- and the path
/// it prints is the PRESUMED one, i.e. whatever the `#include` resolved to.  A
/// project reached through a symlink or compiled with both `-I./x` and `-Ix`
/// therefore gets several names for one declaration.  criu has
/// `compel/include/uapi/compel -> .`, so one union appeared under four names
/// (`uapi/asm/sigframe.h` and `uapi/compel/asm/sigframe.h`, each with and
/// without a `./`) -- 23 redundant symbols across 18 records.
///
/// File IDENTITY already avoids this: `getOrCreateFile` keys objects on
/// `getCanonicalPath`, which is why no file is ever duplicated.  Only the names
/// bypassed it.
///
/// Lexical normalisation is not enough -- measured on criu it merges 19 of the
/// 23, missing exactly the symlinked spellings -- so the path is taken from the
/// cursor's own `CXFile`, which costs no filesystem lookup and cannot depend on
/// the process's working directory the way resolving a relative path would.
///
/// Rewrites only when the basename agrees, so a name whose presumed path names
/// a different file than the cursor sits in is left exactly as clang gave it.
/// Both spellings appear -- "anonymous" for a member with no name of its own,
/// "unnamed" for a record named indirectly -- and the distinction is clang's to
/// make, so only the path is touched.
inline std::string canonicaliseEmbeddedPaths(const std::string& name, CXCursor cursor) {
    static const char* kMarkers[] = {"(anonymous at ", "(unnamed at "};

    size_t probe = std::string::npos;
    for (const char* m : kMarkers) probe = std::min(probe, name.find(m));
    if (probe == std::string::npos) return name;

    CXFile cursor_file = nullptr;
    clang_getExpansionLocation(clang_getCursorLocation(cursor), &cursor_file,
                               nullptr, nullptr, nullptr);
    if (!cursor_file) return name;
    const std::string canon = getCanonicalPath(cursor_file);
    if (canon.empty()) return name;
    auto basename = [](const std::string& p) {
        size_t slash = p.rfind('/');
        return slash == std::string::npos ? p : p.substr(slash + 1);
    };
    const std::string canon_base = basename(canon);

    std::string out;
    size_t pos = 0;
    while (pos < name.size()) {
        size_t marker = std::string::npos, marker_len = 0;
        for (const char* m : kMarkers) {
            size_t at = name.find(m, pos);
            if (at < marker) { marker = at; marker_len = std::strlen(m); }
        }
        if (marker == std::string::npos) { out += name.substr(pos); break; }

        const size_t path_start = marker + marker_len;
        const size_t close = name.find(')', path_start);
        if (close == std::string::npos) { out += name.substr(pos); break; }

        // `<path>:<line>:<column>` -- peel the two numbers off the end rather
        // than scanning forward, so a path containing a colon stays intact.
        const std::string seg = name.substr(path_start, close - path_start);
        size_t c2 = seg.rfind(':');
        size_t c1 = (c2 == std::string::npos || c2 == 0) ? std::string::npos
                                                         : seg.rfind(':', c2 - 1);
        auto all_digits = [](const std::string& t) {
            if (t.empty()) return false;
            for (unsigned char ch : t)
                if (ch < '0' || ch > '9') return false;
            return true;
        };
        const bool parsed = c1 != std::string::npos && c2 != std::string::npos &&
                            all_digits(seg.substr(c1 + 1, c2 - c1 - 1)) &&
                            all_digits(seg.substr(c2 + 1));
        const std::string embedded = parsed ? seg.substr(0, c1) : std::string();

        out += name.substr(pos, path_start - pos);
        if (parsed && basename(embedded) == canon_base)
            out += canon + seg.substr(c1);
        else
            out += seg;
        out += ')';
        pos = close + 1;
    }
    return out;
}

// Overload for FieldDecl cursors: auto-detects struct or union parent.
// Returns empty string when the parent is neither, is anonymous, or names are empty.
inline std::string resolveFieldCompoundName(CXCursor field) {
    CXCursor parent = clang_getCursorSemanticParent(field);
    CXCursorKind pk = clang_getCursorKind(parent);
    if (pk != CXCursor_StructDecl && pk != CXCursor_UnionDecl) return "";
    if (clang_Cursor_isAnonymous(parent)) return "";
    ClangString pn(clang_getCursorSpelling(parent));
    std::string parent_name = pn.to_string();
    if (parent_name.empty()) return "";
    ClangString cn(clang_getCursorSpelling(field));
    std::string child_name = cn.to_string();
    if (child_name.empty()) return "";
    // An anonymous member's own spelling is its type's qualified name, so the
    // compound name inherits a presumed path and needs the same canonicalising
    // a bare record name does.
    return canonicaliseEmbeddedPaths(parent_name, parent) + "." +
           canonicaliseEmbeddedPaths(child_name, field);
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
