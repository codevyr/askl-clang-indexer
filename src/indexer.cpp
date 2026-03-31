#include "indexer.h"
#include "clang_raii.h"
#include "file_utils.h"
#include "proto_builder.h"
#include "stage1.h"
#include "stage2.h"
#include "symbol_types.h"

#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>

#include <sys/wait.h>
#include <unistd.h>

Indexer::Indexer(const std::string& project_name, const std::string& compile_commands_dir,
                const std::string& root_path, int threads, bool include_git_files,
                bool show_progress)
    : project_name_(project_name),
      root_path_(root_path),
      threads_(threads),
      include_git_files_(include_git_files),
      show_progress_(show_progress),
      compile_db_(compile_commands_dir) {}

std::string Indexer::computeCommonAncestor(const std::vector<CompileCommand>& cmds) {
    if (cmds.empty()) return ".";

    // Extract directory part of first path
    std::string common = cmds[0].filename;
    auto last_slash = common.rfind('/');
    if (last_slash != std::string::npos) {
        common = common.substr(0, last_slash);
    }

    for (size_t i = 1; i < cmds.size(); i++) {
        // Extract directory part of this path too
        std::string dir = cmds[i].filename;
        auto slash = dir.rfind('/');
        if (slash != std::string::npos) {
            dir = dir.substr(0, slash);
        }

        size_t j = 0;
        while (j < common.size() && j < dir.size() && common[j] == dir[j]) {
            j++;
        }
        // Only back up if we're not at a directory boundary
        bool at_boundary = (j == common.size() || common[j] == '/') &&
                           (j == dir.size() || dir[j] == '/');
        if (!at_boundary) {
            while (j > 0 && common[j - 1] != '/') j--;
            if (j > 0) j--; // remove trailing slash
        }
        common = common.substr(0, j);
    }

    if (common.empty()) common = "/";
    return common;
}

void Indexer::processTU(const CompileCommand& cmd) {
    ClangIndex index;

    // Build args: add -working-directory so relative paths resolve correctly
    std::vector<std::string> args_storage;
    args_storage.push_back("-working-directory");
    args_storage.push_back(cmd.directory);
    for (auto& arg : cmd.args) args_storage.push_back(arg);

    std::vector<const char*> c_args;
    for (auto& arg : args_storage) c_args.push_back(arg.c_str());

    ClangTU tu;
    CXErrorCode err = clang_parseTranslationUnit2(
        index, cmd.filename.c_str(), c_args.data(), c_args.size(),
        nullptr, 0, CXTranslationUnit_DetailedPreprocessingRecord, &tu.tu);

    if (err != CXError_Success || !tu) {
        fprintf(stderr, "Failed to parse: %s (error code: %d)\n", cmd.filename.c_str(), err);
        return;
    }

    // Print any errors/warnings
    unsigned num_diag = clang_getNumDiagnostics(tu);
    for (unsigned i = 0; i < num_diag; i++) {
        CXDiagnostic diag = clang_getDiagnostic(tu, i);
        CXDiagnosticSeverity sev = clang_getDiagnosticSeverity(diag);
        if (sev >= CXDiagnostic_Error) {
            ClangString msg(clang_formatDiagnostic(diag, CXDiagnostic_DisplaySourceLocation));
            fprintf(stderr, "%s\n", msg.c_str());
        }
        clang_disposeDiagnostic(diag);
    }

    // Stage 1: Extract symbols and direct references
    Stage1 stage1(symbol_table_, next_object_id_);
    stage1.process(tu, cmd.filename);

    // Stage 2: Function pointer assignment analysis
    Stage2 stage2(symbol_table_);
    stage2.process(tu, cmd.filename);

    // Collect results
    auto results = stage1.takeResults();
    auto stage2_result = stage2.takeResults();

    // Merge stage2 refs into the appropriate FileData using stage1's file index
    for (auto& ref : stage2_result.refs) {
        auto it = results.file_index.find(ref.source_file);
        if (it != results.file_index.end()) {
            results.files[it->second].refs.push_back(ref.data);
        }
    }

    {
        std::lock_guard<std::mutex> lock(files_mutex_);
        for (auto& file : results.files) {
            auto it = file_index_.find(file.filesystem_path);
            if (it != file_index_.end()) {
                // Merge refs into existing FileData (instances are identical across TUs)
                auto& existing = all_files_[it->second];
                for (auto& ref : file.refs) existing.refs.push_back(ref);
            } else {
                file_index_[file.filesystem_path] = all_files_.size();
                all_files_.push_back(std::move(file));
            }
        }
    }
}

void Indexer::addGitTrackedFiles() {
    int pipefd[2];
    if (pipe(pipefd) == -1) {
        fprintf(stderr, "Warning: failed to create pipe for git ls-files\n");
        return;
    }

    pid_t pid = fork();
    if (pid == -1) {
        close(pipefd[0]);
        close(pipefd[1]);
        fprintf(stderr, "Warning: failed to fork for git ls-files\n");
        return;
    }

    if (pid == 0) {
        // Child: redirect stdout to pipe, exec git
        close(pipefd[0]);
        dup2(pipefd[1], STDOUT_FILENO);
        close(pipefd[1]);
        execlp("git", "git", "-C", root_path_.c_str(), "ls-files", "-z", nullptr);
        _exit(127);
    }

    // Parent: read from pipe
    close(pipefd[1]);
    std::vector<char> buf(4096);
    std::string accum;
    while (true) {
        ssize_t n = read(pipefd[0], buf.data(), buf.size());
        if (n <= 0) break;
        accum.append(buf.data(), n);
    }
    close(pipefd[0]);

    int status = 0;
    waitpid(pid, &status, 0);
    if (!WIFEXITED(status) || WEXITSTATUS(status) != 0) {
        fprintf(stderr, "Warning: git ls-files failed (exit %d)\n",
                WIFEXITED(status) ? WEXITSTATUS(status) : -1);
        return;
    }

    size_t before = all_files_.size();
    size_t pos = 0;
    while (pos < accum.size()) {
        size_t nul = accum.find('\0', pos);
        if (nul == std::string::npos) nul = accum.size();
        std::string rel_path = accum.substr(pos, nul - pos);
        pos = nul + 1;

        if (rel_path.empty()) continue;

        addFile(root_path_ + "/" + rel_path);
    }

    fprintf(stderr, "Git-tracked files added: %zu\n", all_files_.size() - before);
}

void Indexer::addFile(const std::string& abs_path) {
    // Skip if already indexed
    if (file_index_.count(abs_path)) return;

    // Skip non-regular files (directories, submodules, symlinks to dirs, etc.)
    std::error_code ec;
    if (!std::filesystem::is_regular_file(abs_path, ec)) return;

    // Read file content
    std::ifstream in(abs_path, std::ios::binary);
    if (!in) return;
    std::vector<uint8_t> content(
        (std::istreambuf_iterator<char>(in)),
        std::istreambuf_iterator<char>());

    FileData fd;
    fd.object_local_id = next_object_id_.fetch_add(1);
    fd.filesystem_path = abs_path;
    fd.module_path = abs_path;
    fd.filetype = computeFiletype(abs_path);
    fd.content = std::move(content);

    // Create FILE symbol and instance
    auto [file_sym_id, _] = symbol_table_.getOrCreate(
        fd.module_path, SCOPE_GLOBAL, SYMTYPE_FILE);
    fd.instances.push_back({file_sym_id, 0, static_cast<int32_t>(fd.content.size())});

    file_index_[abs_path] = all_files_.size();
    all_files_.push_back(std::move(fd));
}

void Indexer::createDirectorySymbols() {
    std::set<std::string> dirs;
    for (auto& file : all_files_) {
        std::string path = file.module_path;
        while (true) {
            auto last_slash = path.rfind('/');
            if (last_slash == std::string::npos) break;
            path = path.substr(0, last_slash);
            if (!path.empty()) {
                dirs.insert(path);
            }
        }
    }

    for (auto& dir : dirs) {
        symbol_table_.getOrCreate(dir, SCOPE_GLOBAL, SYMTYPE_DIRECTORY);
    }
}

void Indexer::run() {
    if (root_path_.empty()) {
        root_path_ = computeCommonAncestor(compile_db_.commands());
    }
    // Ensure root_path_ ends without trailing slash
    while (root_path_.size() > 1 && root_path_.back() == '/') {
        root_path_.pop_back();
    }

    fprintf(stderr, "Project: %s\n", project_name_.c_str());
    fprintf(stderr, "Root: %s\n", root_path_.c_str());
    fprintf(stderr, "Translation units: %zu\n", compile_db_.commands().size());
    fprintf(stderr, "Threads: %d\n", threads_);

    // Process TUs in parallel
    std::atomic<size_t> next_cmd{0};
    std::vector<std::thread> workers;

    size_t total = compile_db_.commands().size();
    for (int t = 0; t < threads_; t++) {
        workers.emplace_back([this, &next_cmd, total]() {
            while (true) {
                size_t idx = next_cmd.fetch_add(1);
                if (idx >= compile_db_.commands().size()) break;
                processTU(compile_db_.commands()[idx]);
                if (show_progress_) {
                    size_t done = processed_tus_.fetch_add(1) + 1;
                    fprintf(stderr, "\rProcessed: %zu/%zu", done, total);
                }
            }
        });
    }
    for (auto& w : workers) w.join();
    if (show_progress_) fprintf(stderr, "\n");

    if (include_git_files_) {
        addGitTrackedFiles();
    }

    createDirectorySymbols();

    fprintf(stderr, "Files: %zu\n", all_files_.size());
    fprintf(stderr, "Symbols: %zu\n", symbol_table_.allSymbols().size());
}

void Indexer::write(const std::string& output_path) {
    std::string data = ProtoBuilder::build(
        project_name_, root_path_, symbol_table_, all_files_,
        next_object_id_.load());

    std::ofstream out(output_path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Failed to open output file: " + output_path);
    }
    out.write(data.data(), data.size());
    out.close();
    if (!out) {
        throw std::runtime_error("Failed to write output file: " + output_path);
    }

    fprintf(stderr, "Wrote %zu bytes to %s\n", data.size(), output_path.c_str());
}
