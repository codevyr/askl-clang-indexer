#pragma once

#include "compilation_db.h"
#include "symbol_table.h"
#include "stage1.h"

#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

class Indexer {
public:
    Indexer(const std::string& project_name, const std::string& compile_commands_dir,
            const std::string& root_path, int threads);

    void run();
    void write(const std::string& output_path);

private:
    std::string project_name_;
    std::string root_path_;
    int threads_;

    CompilationDB compile_db_;
    SymbolTable symbol_table_;

    std::mutex files_mutex_;
    std::vector<FileData> all_files_;
    std::unordered_set<std::string> processed_files_;

    void processTU(const CompileCommand& cmd);
    void createDirectorySymbols();
    std::string computeCommonAncestor(const std::vector<CompileCommand>& cmds);
};
