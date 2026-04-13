#include "compilation_db.h"
#include <algorithm>
#include <array>
#include <clang-c/Index.h>
#include <cstdio>
#include <filesystem>
#include <sstream>
#include <sys/wait.h>
#include <unistd.h>
#include <unordered_set>

// ========== Flag filtering infrastructure ==========

struct FlagFilter {
    std::unordered_set<std::string> exact;      // Exact-match flags to drop
    std::vector<std::string> prefixes;           // Prefix-match flags to drop
    std::unordered_set<std::string> pairs;       // Flags that consume the next argument
};

// Filter args in-place, removing matching flags. Warning flags (-W...) are
// always stripped since they are irrelevant for indexing.
static void filterFlags(std::vector<std::string>& args, const FlagFilter& filter) {
    std::vector<std::string> result;
    for (size_t i = 0; i < args.size(); i++) {
        const auto& arg = args[i];
        if (arg.size() >= 2 && arg[0] == '-' && arg[1] == 'W') continue;
        if (filter.exact.count(arg)) continue;
        bool prefix_match = false;
        for (auto& prefix : filter.prefixes) {
            if (arg.compare(0, prefix.size(), prefix) == 0) { prefix_match = true; break; }
        }
        if (prefix_match) continue;
        if (filter.pairs.count(arg)) {
            i++; // skip the next argument
            continue;
        }
        result.push_back(arg);
    }
    args = std::move(result);
}

// GCC-only flags that clang doesn't understand
static const FlagFilter gcc_filter = {
    {
        "-mno-sse-fp-math", "-fno-var-tracking-assignments",
        "-fconserve-stack", "-fno-allow-store-data-races",
        "-fno-code-hoisting", "-fno-delete-null-pointer-checks",
        "-fno-schedule-insns", "-fno-tree-loop-im",
        "-mindirect-branch-register",
        "-mrecord-mcount", "-mfentry",
        "-fno-reorder-blocks-and-partition",
        "-fno-partial-inlining",
        "-fno-tree-loop-distribute-patterns",
        "-fno-ipa-cp-clone", "-fsched-pressure", "-mhard-float",
        "-fno-conserve-stack",
        "-fsanitize=bounds-strict",
        // GCC plugin defines — stripping these lets kernel headers use their
        // fallback (empty) definitions instead of referencing plugin-only attributes.
        "-DLATENT_ENTROPY_PLUGIN",
        "-DRANDSTRUCT_PLUGIN",
        "-DRANDSTRUCT",
        "-DSTRUCTLEAK_PLUGIN",
    },
    {
        "-mabi=", "-mpreferred-stack-boundary=",
        "-mindirect-branch-cs-prefix", "-mindirect-branch=",
        "-fzero-init-padding-bits=", "-fmin-function-alignment=",
        "-fasan-shadow-offset=",
        // GCC plugins can't be loaded by libclang
        "-fplugin=", "-fplugin-arg-",
    },
    {},
};

// NVCC-specific flags that clang doesn't understand
static const FlagFilter nvcc_filter = {
    {
        "--expt-extended-lambda", "--expt-relaxed-constexpr",
        "-compress-all", "-dc", "-dw",
    },
    {
        "-gencode=", "-maxrregcount=",
    },
    {
        "--compiler-options", "-Xcompiler",
        "-Xptxas", "-Xfatbin", "-Xlinker",
        "-ccbin", "-gencode",
    },
};

// ========== Utility helpers ==========

static bool hasExtension(const std::string& path, const std::string& ext) {
    return path.size() >= ext.size() &&
           path.compare(path.size() - ext.size(), ext.size(), ext) == 0;
}

static bool isAssemblyFile(const std::string& path) {
    return hasExtension(path, ".S") || hasExtension(path, ".s");
}

// ========== CUDA/NVCC helpers ==========

static bool isNvccCompiler(const std::string& compiler) {
    auto slash = compiler.rfind('/');
    std::string basename = (slash != std::string::npos) ? compiler.substr(slash + 1) : compiler;
    return basename == "nvcc";
}

// Derive --cuda-path from the nvcc path (e.g. /usr/local/cuda/bin/nvcc -> /usr/local/cuda).
static std::string deriveCudaPath(const std::string& compiler) {
    auto pos = compiler.rfind("/bin/nvcc");
    if (pos != std::string::npos) return compiler.substr(0, pos);
    return "";
}

// Extract GPU arch from -gencode=arch=compute_XX,code=sm_XX.
static std::string extractGpuArch(const std::vector<std::string>& args) {
    for (auto& arg : args) {
        static const std::string prefix = "-gencode=";
        if (arg.compare(0, prefix.size(), prefix) == 0) {
            auto code_pos = arg.find("code=sm_");
            if (code_pos != std::string::npos) {
                auto start = code_pos + 5; // after "code="
                auto end = arg.find(',', start);
                return arg.substr(start, end - start);
            }
        }
    }
    return "sm_70";
}

// Extract the host compiler from -ccbin.  Returns "g++" as default.
static std::string extractCcbin(const std::vector<std::string>& args) {
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == "-ccbin") return args[i + 1];
    }
    return "g++";
}

// ========== CompilationDB ==========

CompilationDB::CompilationDB(const std::string& dir, const std::string& modules_method)
    : modules_method_(modules_method) {
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

        // Skip assembly files — libclang can't parse them
        if (isAssemblyFile(entry.filename)) continue;

        // Pre-canonicalize the filename so relative source paths can be matched
        std::error_code fn_ec;
        std::string canonical_filename =
            std::filesystem::weakly_canonical(entry.filename, fn_ec).string();
        if (fn_ec) canonical_filename = entry.filename;

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
            // Check if relative/non-flag path resolves to the source file
            if (!arg.empty() && arg[0] != '-') {
                std::filesystem::path p(arg);
                if (!p.is_absolute()) p = std::filesystem::path(entry.directory) / p;
                std::error_code ec;
                auto canonical = std::filesystem::weakly_canonical(p, ec);
                if (!ec && canonical.string() == canonical_filename) continue;
            }

            entry.args.push_back(std::move(arg));
        }

        if (isNvccCompiler(compiler)) {
            adaptForCuda(entry, compiler);
        } else {
            filterFlags(entry.args, gcc_filter);
            addSystemIncludes(compiler, entry.args);
        }
        extractModuleName(entry);
        commands_.push_back(std::move(entry));
    }

    clang_CompileCommands_dispose(cmds);
}

CompilationDB::~CompilationDB() {
    if (db_) clang_CompilationDatabase_dispose(db_);
}

void CompilationDB::adaptForCuda(CompileCommand& entry, const std::string& compiler) {
    // Extract metadata from raw args before filtering removes the flags
    std::string host_compiler = extractCcbin(entry.args);
    std::string cuda_path = deriveCudaPath(compiler);
    std::string gpu_arch = extractGpuArch(entry.args);

    // Filter NVCC-specific flags first (handles pairs like -Xcompiler <val>),
    // then GCC flags (nvcc passes through to GCC for host compilation).
    filterFlags(entry.args, nvcc_filter);
    filterFlags(entry.args, gcc_filter);

    // Tell clang to parse as CUDA
    entry.args.insert(entry.args.begin(), {"-x", "cuda"});
    if (!cuda_path.empty())
        entry.args.push_back("--cuda-path=" + cuda_path);
    entry.args.push_back("--cuda-gpu-arch=" + gpu_arch);
    entry.args.push_back("--no-cuda-version-check");
    entry.args.push_back("-fcuda-rdc");

    // CUDA device code uses placement new without #include <new>
    entry.args.push_back("-include");
    entry.args.push_back("new");

    // nvcc assumes GNU extensions (typeof, asm, etc.); promote -std=c++NN to -std=gnu++NN
    for (auto& a : entry.args) {
        if (a.compare(0, 8, "-std=c++") == 0) {
            a = "-std=gnu++" + a.substr(8);
        }
    }

    addSystemIncludes(host_compiler, entry.args);
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
               "-E", "-x", "c++", "-v", "/dev/null", nullptr);
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
        if (path.empty()) continue;
        // Skip GCC's compiler-internal intrinsics path (lib/gcc/.../include).
        // Clang has its own intrinsics and GCC's use GCC-specific builtins
        // that clang doesn't understand (e.g. __builtin_ia32_*).
        if (path.find("/lib/gcc/") != std::string::npos) continue;
        includes.push_back(path);
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

void CompilationDB::extractModuleName(CompileCommand& entry) {
    if (modules_method_ == "kbuild") {
        bool has_dmodule = false;
        std::string modname;
        for (const auto& arg : entry.args) {
            if (arg == "-DMODULE") has_dmodule = true;
            static const std::string prefix = "-D__KBUILD_MODNAME=";
            if (arg.compare(0, prefix.size(), prefix) == 0)
                modname = arg.substr(prefix.size());
        }
        if (has_dmodule && !modname.empty())
            entry.modname = modname;
    }
}
