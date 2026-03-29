#pragma once

#include <clang-c/CXCompilationDatabase.h>
#include <string>
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

    void filterGccFlags(std::vector<std::string>& args);
};
