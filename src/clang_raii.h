#pragma once

#include <clang-c/Index.h>
#include <string>

struct ClangIndex {
    CXIndex idx;
    ClangIndex(int excludePCH = 0, int displayDiag = 0)
        : idx(clang_createIndex(excludePCH, displayDiag)) {}
    ~ClangIndex() { clang_disposeIndex(idx); }
    operator CXIndex() { return idx; }
    ClangIndex(const ClangIndex&) = delete;
    ClangIndex& operator=(const ClangIndex&) = delete;
};

struct ClangTU {
    CXTranslationUnit tu = nullptr;
    ~ClangTU() { if (tu) clang_disposeTranslationUnit(tu); }
    operator CXTranslationUnit() { return tu; }
    explicit operator bool() { return tu != nullptr; }
    ClangTU() = default;
    ClangTU(const ClangTU&) = delete;
    ClangTU& operator=(const ClangTU&) = delete;
};

struct ClangString {
    CXString str;
    explicit ClangString(CXString s) : str(s) {}
    ~ClangString() { clang_disposeString(str); }
    const char* c_str() { return clang_getCString(str); }
    std::string to_string() {
        const char* s = clang_getCString(str);
        return s ? std::string(s) : std::string();
    }
    ClangString(const ClangString&) = delete;
    ClangString& operator=(const ClangString&) = delete;
};
