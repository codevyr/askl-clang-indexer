#pragma once

#include <clang-c/CXCompilationDatabase.h>
#include <string>
#include <unordered_map>
#include <vector>

struct CompileCommand {
    std::string filename;
    std::string directory;
    std::vector<std::string> args;
};

class CompilationDB {
public:
    explicit CompilationDB(const std::string& dir);
    ~CompilationDB();

    const std::vector<CompileCommand>& commands() const { return commands_; }

    CompilationDB(const CompilationDB&) = delete;
    CompilationDB& operator=(const CompilationDB&) = delete;

private:
    CXCompilationDatabase db_ = nullptr;
    std::vector<CompileCommand> commands_;
    std::unordered_map<std::string, std::vector<std::string>> system_includes_cache_;

    void filterGccFlags(std::vector<std::string>& args);
    std::vector<std::string> querySystemIncludes(const std::string& compiler);
    void addSystemIncludes(const std::string& compiler, std::vector<std::string>& args);
};
