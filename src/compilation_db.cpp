#include "compilation_db.h"
#include <algorithm>
#include <array>
#include <clang-c/Index.h>
#include <cstdio>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

static const std::unordered_set<std::string> gcc_only_flags = {
    "-mno-sse-fp-math", "-fno-var-tracking-assignments",
    "-fconserve-stack", "-fno-allow-store-data-races",
    "-fno-code-hoisting", "-fno-delete-null-pointer-checks",
    "-fno-schedule-insns", "-fno-tree-loop-im",
    "-mindirect-branch=thunk-extern", "-mindirect-branch-register",
    "-mrecord-mcount", "-mfentry",
    "-fno-reorder-blocks-and-partition",
    "-fno-partial-inlining",
    "-fno-tree-loop-distribute-patterns",
};

static const std::vector<std::string> gcc_only_prefixes = {
    "-mabi=", "-mpreferred-stack-boundary=",
    "-mindirect-branch-cs-prefix",
};

CompilationDB::CompilationDB(const std::string& dir) {
    CXCompilationDatabase_Error error;
    db_ = clang_CompilationDatabase_fromDirectory(dir.c_str(), &error);
    if (error != CXCompilationDatabase_NoError || !db_) {
        fprintf(stderr, "Failed to load compile_commands.json from: %s\n", dir.c_str());
        return;
    }

    CXCompileCommands cmds = clang_CompilationDatabase_getAllCompileCommands(db_);
    unsigned count = clang_CompileCommands_getSize(cmds);

    for (unsigned i = 0; i < count; i++) {
        CXCompileCommand cmd = clang_CompileCommands_getCommand(cmds, i);

        CXString cx_filename = clang_CompileCommand_getFilename(cmd);
        CXString cx_directory = clang_CompileCommand_getDirectory(cmd);

        CompileCommand entry;
        entry.filename = clang_getCString(cx_filename);
        entry.directory = clang_getCString(cx_directory);

        clang_disposeString(cx_filename);
        clang_disposeString(cx_directory);

        std::string compiler;
        unsigned num_args = clang_CompileCommand_getNumArgs(cmd);
        for (unsigned j = 0; j < num_args; j++) {
            CXString cx_arg = clang_CompileCommand_getArg(cmd, j);
            std::string arg = clang_getCString(cx_arg);
            clang_disposeString(cx_arg);

            // Capture and skip arg 0 (compiler path)
            if (j == 0) { compiler = arg; continue; }
            // Skip -c (implied by clang_parseTranslationUnit)
            if (arg == "-c") continue;
            // Skip -o and its argument
            if (arg == "-o") {
                j++; // skip the next arg too
                // Need to dispose it
                if (j < num_args) {
                    CXString skip = clang_CompileCommand_getArg(cmd, j);
                    clang_disposeString(skip);
                }
                continue;
            }
            // Skip the source file itself (may be relative or absolute)
            if (arg == entry.filename) continue;
            // Check if relative path matches
            if (!arg.empty() && arg[0] != '-') {
                std::string abs_arg = arg;
                if (arg[0] != '/') {
                    abs_arg = entry.directory + "/" + arg;
                }
                // Normalize: check if it matches the filename
                if (abs_arg == entry.filename) continue;
                // Also check if the arg is just the basename matching the end of filename
                if (entry.filename.size() >= arg.size() &&
                    entry.filename.substr(entry.filename.size() - arg.size()) == arg &&
                    (entry.filename.size() == arg.size() ||
                     entry.filename[entry.filename.size() - arg.size() - 1] == '/')) {
                    continue;
                }
            }

            entry.args.push_back(std::move(arg));
        }

        filterGccFlags(entry.args);
        addSystemIncludes(compiler, entry.args);
        commands_.push_back(std::move(entry));
    }

    clang_CompileCommands_dispose(cmds);
}

CompilationDB::~CompilationDB() {
    if (db_) clang_CompilationDatabase_dispose(db_);
}

void CompilationDB::filterGccFlags(std::vector<std::string>& args) {
    args.erase(
        std::remove_if(args.begin(), args.end(), [](const std::string& arg) {
            if (gcc_only_flags.count(arg)) return true;
            for (auto& prefix : gcc_only_prefixes) {
                if (arg.substr(0, prefix.size()) == prefix) return true;
            }
            return false;
        }),
        args.end());
}

std::vector<std::string> CompilationDB::querySystemIncludes(const std::string& compiler) {
    std::vector<std::string> includes;

    // Use pipe+fork+exec instead of popen to avoid shell interpretation
    // of the compiler path (which may contain special characters).
    int pipefd[2];
    if (pipe(pipefd) == -1) return includes;

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        return includes;
    }

    if (pid == 0) {
        // Child: redirect stdout+stderr to pipe, exec compiler
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        dup2(pipefd[1], STDERR_FILENO);
        close(pipefd[1]);
        execlp(compiler.c_str(), compiler.c_str(),
               "-E", "-x", "c", "-v", "/dev/null", nullptr);
        _exit(127);
    }

    // Parent: read from pipe
    close(pipefd[1]);
    std::array<char, 512> buf;
    std::string output;
    ssize_t n;
    while ((n = read(pipefd[0], buf.data(), buf.size())) > 0) {
        output.append(buf.data(), n);
    }
    close(pipefd[0]);
    waitpid(pid, nullptr, 0);

    // Parse the output between "#include <...> search starts here:" and "End of search list."
    auto start = output.find("#include <...> search starts here:");
    auto end = output.find("End of search list.");
    if (start == std::string::npos || end == std::string::npos) return includes;

    std::istringstream stream(output.substr(start, end - start));
    std::string line;
    std::getline(stream, line); // skip the header line
    while (std::getline(stream, line)) {
        // Each line is " /path/to/include" (with leading space)
        size_t first = line.find_first_not_of(" \t");
        if (first == std::string::npos) continue;
        std::string path = line.substr(first);
        // Remove " (framework directory)" suffix if present
        auto paren = path.find(" (");
        if (paren != std::string::npos) path = path.substr(0, paren);
        if (!path.empty()) includes.push_back(path);
    }

    return includes;
}

void CompilationDB::addSystemIncludes(const std::string& compiler, std::vector<std::string>& args) {
    if (compiler.empty()) return;

    // Projects like the Linux kernel use -nostdinc to avoid system headers.
    // Respect that: don't inject system includes if the command already opts out.
    for (auto& arg : args) {
        if (arg == "-nostdinc") return;
    }

    auto it = system_includes_cache_.find(compiler);
    if (it == system_includes_cache_.end()) {
        auto includes = querySystemIncludes(compiler);
        it = system_includes_cache_.emplace(compiler, std::move(includes)).first;
    }

    for (auto& path : it->second) {
        args.push_back("-isystem");
        args.push_back(path);
    }
}
