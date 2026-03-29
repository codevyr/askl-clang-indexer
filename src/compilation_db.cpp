#include "compilation_db.h"
#include <algorithm>
#include <clang-c/Index.h>
#include <cstdio>
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

        unsigned num_args = clang_CompileCommand_getNumArgs(cmd);
        for (unsigned j = 0; j < num_args; j++) {
            CXString cx_arg = clang_CompileCommand_getArg(cmd, j);
            std::string arg = clang_getCString(cx_arg);
            clang_disposeString(cx_arg);

            // Skip arg 0 (compiler path)
            if (j == 0) continue;
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
