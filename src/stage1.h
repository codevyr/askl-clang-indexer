#pragma once

#include <atomic>
#include <clang-c/Index.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

class SymbolTable;

struct InstanceData {
    int64_t symbol_local_id;
    int32_t start_offset;
    int32_t end_offset;
};

struct RefData {
    int64_t to_symbol_local_id;
    int32_t from_offset_start;
    int32_t from_offset_end;
};

struct FileData {
    int64_t object_local_id;
    std::string module_path;
    std::string filesystem_path;
    std::string filetype;
    std::vector<uint8_t> content;
    std::vector<InstanceData> instances;
    std::vector<RefData> refs;
};

struct Stage1Result {
    std::vector<FileData> files;
    std::unordered_map<std::string, size_t> file_index; // filesystem_path -> index in files
};

class Stage1 {
public:
    Stage1(SymbolTable& symbols, std::atomic<int64_t>& next_object_id);

    void process(CXTranslationUnit tu, const std::string& tu_filename);
    Stage1Result takeResults();

private:
    SymbolTable& symbols_;
    std::atomic<int64_t>& next_object_id_;
    Stage1Result result_;

    // Per-TU state
    std::unordered_map<std::string, size_t> file_index_; // filepath -> index in result_.files

    size_t getOrCreateFile(CXFile file);

    void collectMacros(CXTranslationUnit tu);
    static CXChildVisitResult macroVisitor(CXCursor cursor, CXCursor parent, CXClientData data);

    static CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData data);
    static void inclusionCallback(CXFile included_file, CXSourceLocation* stack,
                                  unsigned len, CXClientData data);
    void visitCursor(CXCursor cursor, CXCursor parent);
};
