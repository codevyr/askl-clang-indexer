#pragma once

#include "compilation_db.h"
#include "symbol_table.h"
#include "stage1.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

class Indexer {
public:
    Indexer(const std::string& project_name, const std::string& compile_commands_dir,
            const std::string& root_path, int threads, bool include_git_files = false);

    void run();
    void write(const std::string& output_path);

    const SymbolTable& symbolTable() const { return symbol_table_; }
    const std::vector<FileData>& allFiles() const { return all_files_; }

private:
    std::string project_name_;
    std::string root_path_;
    int threads_;
    bool include_git_files_;

    CompilationDB compile_db_;
    SymbolTable symbol_table_;

    std::atomic<int64_t> next_object_id_{1};
    std::mutex files_mutex_;
    std::vector<FileData> all_files_;
    std::unordered_map<std::string, size_t> file_index_; // guarded by files_mutex_

    void processTU(const CompileCommand& cmd);
    void addGitTrackedFiles();
    void addFile(const std::string& abs_path);
    void createDirectorySymbols();
    std::string computeCommonAncestor(const std::vector<CompileCommand>& cmds);
};
