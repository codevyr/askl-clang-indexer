#include "indexer.h"
#include "clang_raii.h"
#include "proto_builder.h"
#include "stage1.h"
#include "stage2.h"
#include "symbol_types.h"

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <thread>
#include <set>

Indexer::Indexer(const std::string& project_name, const std::string& compile_commands_dir,
                const std::string& root_path, int threads)
    : project_name_(project_name),
      root_path_(root_path),
      threads_(threads),
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
        nullptr, 0, CXTranslationUnit_None, &tu.tu);

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
    Stage1 stage1(symbol_table_, root_path_, next_object_id_);
    stage1.process(tu, cmd.filename);

    // Stage 2: Function pointer assignment analysis
    Stage2 stage2(symbol_table_);
    stage2.process(tu, cmd.filename);

    // Collect results
    auto results = stage1.takeResults();
    auto stage2_result = stage2.takeResults();

    // Merge stage2 refs into the appropriate FileData
    for (auto& ref : stage2_result.refs) {
        for (auto& file : results.files) {
            if (file.filesystem_path == ref.source_file) {
                RefData rd;
                rd.to_symbol_local_id = ref.data.to_symbol_local_id;
                rd.from_offset_start = ref.data.from_offset_start;
                rd.from_offset_end = ref.data.from_offset_end;
                file.refs.push_back(rd);
                break;
            }
        }
    }

    {
        std::lock_guard<std::mutex> lock(files_mutex_);
        for (auto& file : results.files) {
            // Check if this file already exists in all_files_
            auto it = std::find_if(all_files_.begin(), all_files_.end(),
                [&](const FileData& f) { return f.filesystem_path == file.filesystem_path; });
            if (it != all_files_.end()) {
                // Merge refs into existing FileData (instances are identical across TUs)
                for (auto& ref : file.refs) it->refs.push_back(ref);
            } else {
                all_files_.push_back(std::move(file));
            }
        }
    }
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

    for (int t = 0; t < threads_; t++) {
        workers.emplace_back([this, &next_cmd]() {
            while (true) {
                size_t idx = next_cmd.fetch_add(1);
                if (idx >= compile_db_.commands().size()) break;
                processTU(compile_db_.commands()[idx]);
            }
        });
    }
    for (auto& w : workers) w.join();

    createDirectorySymbols();

    fprintf(stderr, "Files: %zu\n", all_files_.size());
    fprintf(stderr, "Symbols: %zu\n", symbol_table_.allSymbols().size());
}

void Indexer::write(const std::string& output_path) {
    std::string data = ProtoBuilder::build(
        project_name_, root_path_, symbol_table_, all_files_);

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
