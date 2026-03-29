#pragma once

#include "compilation_db.h"
#include "symbol_table.h"
#include "stage1.h"

#include <atomic>
#include <mutex>
#include <string>
#include <vector>

class Indexer {
public:
    Indexer(const std::string& project_name, const std::string& compile_commands_dir,
            const std::string& root_path, int threads);

    void run();
    void write(const std::string& output_path);

    const SymbolTable& symbolTable() const { return symbol_table_; }
    const std::vector<FileData>& allFiles() const { return all_files_; }

private:
    std::string project_name_;
    std::string root_path_;
    int threads_;

    CompilationDB compile_db_;
    SymbolTable symbol_table_;

    std::atomic<int64_t> next_object_id_{1};
    std::mutex files_mutex_;
    std::vector<FileData> all_files_;

    void processTU(const CompileCommand& cmd);
    void createDirectorySymbols();
    std::string computeCommonAncestor(const std::vector<CompileCommand>& cmds);
};
