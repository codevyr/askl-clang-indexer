#pragma once

#include <clang-c/Index.h>
#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
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
};

class Stage1 {
public:
    Stage1(SymbolTable& symbols, const std::string& root_path,
           std::unordered_set<std::string>& processed_files, std::mutex& files_mutex);

    void process(CXTranslationUnit tu, const std::string& tu_filename);
    Stage1Result takeResults();

private:
    SymbolTable& symbols_;
    std::string root_path_;
    std::unordered_set<std::string>& processed_files_;
    std::mutex& files_mutex_;
    Stage1Result result_;
    int64_t next_object_id_;

    // Per-TU state
    std::unordered_map<std::string, size_t> file_index_; // filepath -> index in result_.files

    FileData& getOrCreateFile(CXFile file);
    std::string computeModulePath(const std::string& abs_path);
    std::string computeFiletype(const std::string& path);
    bool isLocalVariable(CXCursor cursor);

    static CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData data);
    void visitCursor(CXCursor cursor, CXCursor parent);
};
