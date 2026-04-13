#pragma once

#include <clang-c/CXCompilationDatabase.h>
#include <string>
#include <unordered_map>
#include <vector>

struct CompileCommand {
    std::string filename;
    std::string directory;
    std::vector<std::string> args;
    std::string modname;  // module name, empty if not part of a module
};

class CompilationDB {
public:
    explicit CompilationDB(const std::string& dir, const std::string& modules_method = "");
    ~CompilationDB();

    const std::vector<CompileCommand>& commands() const { return commands_; }

    CompilationDB(const CompilationDB&) = delete;
    CompilationDB& operator=(const CompilationDB&) = delete;

private:
    CXCompilationDatabase db_ = nullptr;
    std::vector<CompileCommand> commands_;
    std::unordered_map<std::string, std::vector<std::string>> system_includes_cache_;
    std::string modules_method_;

    void adaptForCuda(CompileCommand& entry, const std::string& compiler);
    std::vector<std::string> querySystemIncludes(const std::string& compiler);
    void addSystemIncludes(const std::string& compiler, std::vector<std::string>& args);
    void extractModuleName(CompileCommand& entry);
};
