#include "indexer.h"
#include "clang_raii.h"
#include "dedup_keys.h"
#include "file_utils.h"
#include "proto_builder.h"
#include "stage1.h"
#include "stage2.h"
#include "symbol_types.h"


#include <algorithm>
#include <tuple>
#include <atomic>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <set>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#include <sys/wait.h>
#include <unistd.h>

Indexer::Indexer(const std::string& project_name, const std::string& compile_commands_dir,
                const std::string& root_path, int threads, const std::string& git_root,
                bool show_progress, bool show_warnings, const std::string& modules_method)
    : project_name_(project_name),
      root_path_(root_path),
      threads_(threads),
      git_root_(git_root),
      show_progress_(show_progress),
      show_warnings_(show_warnings),
      modules_method_(modules_method),
      compile_db_(compile_commands_dir, modules_method) {}

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

    if (show_warnings_) {
        fprintf(stderr, "--- TU: %s ---\n", cmd.filename.c_str());
        fprintf(stderr, "  clang args:");
        for (auto& arg : args_storage) fprintf(stderr, " %s", arg.c_str());
        fprintf(stderr, "\n");
    }

    ClangTU tu;
    CXErrorCode err = clang_parseTranslationUnit2(
        index, cmd.filename.c_str(), c_args.data(), c_args.size(),
        nullptr, 0, CXTranslationUnit_DetailedPreprocessingRecord, &tu.tu);

    if (err != CXError_Success || !tu) {
        fprintf(stderr, "Failed to parse: %s (error code: %d)\n", cmd.filename.c_str(), err);
        return;
    }

    // Print any errors/warnings (only when --warn is set)
    if (show_warnings_) {
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



    // Merge stage2 refs into the appropriate FileData using stage1's file index,
    // deduplicating against existing refs (Stage2 can produce identical synthetic
    // refs when the same function is assigned to the same field via multiple
    // patterns, e.g. designated init + direct assignment).
    for (auto& ref : stage2_result.refs) {
        auto it = results.file_index.find(ref.source_file);
        if (it != results.file_index.end()) {
            auto& file_refs = results.files[it->second].refs;
            bool dup = std::any_of(file_refs.begin(), file_refs.end(), [&](const RefData& r) {
                return r.to_symbol_local_id == ref.data.to_symbol_local_id &&
                       r.from_offset_start == ref.data.from_offset_start &&
                       r.from_offset_end == ref.data.from_offset_end;
            });
            if (!dup) {
                // Left unnumbered, like every other ref: the post-merge
                // numbering pass places all of them by the text they sit in.
                ref.data.synthetic = true;
                file_refs.push_back(ref.data);
            }
        }
    }

    // Pre-canonicalize outside the lock to avoid syscall under contention
    std::string canon_filename;
    if (!cmd.modname.empty()) {
        std::error_code ec;
        canon_filename = std::filesystem::weakly_canonical(cmd.filename, ec).string();
        if (ec) canon_filename = cmd.filename;
    }

    {
        std::lock_guard<std::mutex> lock(files_mutex_);
        for (auto& file : results.files) {
            auto it = file_index_.find(file.filesystem_path);
            if (it != file_index_.end()) {
                auto& existing = all_files_[it->second];

                // UNION THE INSTANCES, do not truncate them.
                //
                // A file's declarations are a property of its TEXT, not of any
                // translation unit, so the union over every TU that compiled it
                // is bounded by the text -- identical declarations from two TUs
                // are one declaration, which `InstKey` is what decides. Keeping
                // only the first TU's set discarded the rest by arrival order,
                // and because the SYMBOL table unions across TUs while this
                // truncated, the two halves of one index disagreed: a symbol
                // could be nameable and never locatable. Measured on criu, 129
                // such orphans with no macro explanation (issue #7).
                //
                // Two passes, because a child can arrive before its parent.
                // Pass one maps every incoming index to a merged one, inserting
                // what is missing; pass two rewrites `stack_parent` through
                // that map for the rows just inserted -- it is an index into
                // the INCOMING vector and would otherwise point at a stranger.
                // The map is KEPT across merges, not rebuilt from `existing`:
                // rebuilding made one merge cost O(everything accumulated) and
                // the run quadratic in how often a file is included.
                auto& have = merged_inst_keys_[it->second];

                std::vector<size_t> remap(file.instances.size(), 0);
                std::vector<size_t> inserted;
                for (size_t i = 0; i < file.instances.size(); ++i) {
                    const InstanceData& in = file.instances[i];
                    InstKey k{in.symbol_local_id, in.instance_type,
                              in.start_offset, in.end_offset};
                    auto [slot, fresh] = have.try_emplace(k, existing.instances.size());
                    if (fresh) {
                        existing.instances.push_back(in);
                        inserted.push_back(i);
                    } else {
                        // Same declaration, seen twice. First writer wins for
                        // every field -- except a span this TU knows and the
                        // other did not: -1 means "the indexer does not know
                        // one", and learning it is strictly better than
                        // keeping the absence.
                        InstanceData& e = existing.instances[slot->second];
                        if (e.name_start < 0 && in.name_start >= 0) {
                            e.name_start = in.name_start;
                            e.name_end = in.name_end;
                        }
                        if (e.sig_start < 0 && in.sig_start >= 0) {
                            e.sig_start = in.sig_start;
                            e.sig_end = in.sig_end;
                        }
                        if (e.content_start < 0 && in.content_start >= 0) {
                            e.content_start = in.content_start;
                            e.content_end = in.content_end;
                        }
                    }
                    remap[i] = slot->second;
                }
                for (size_t i : inserted) {
                    int32_t sp = file.instances[i].stack_parent;
                    existing.instances[remap[i]].stack_parent =
                        (sp >= 0 && static_cast<size_t>(sp) < remap.size())
                            ? static_cast<int32_t>(remap[static_cast<size_t>(sp)])
                            : -1;
                }
                if (!inserted.empty()) {
                    files_with_late_instances_.insert(it->second);
                    late_instance_count_ += inserted.size();
                }

                // Merge refs, deduplicating against what's already there --
                // again against a KEPT set, for the same reason.
                auto& seen = merged_ref_keys_[it->second];
                for (auto& ref : file.refs) {
                    if (seen.insert({ref.to_symbol_local_id, ref.from_offset_start, ref.from_offset_end}).second) {
                        // No renumbering: nothing is numbered yet.  A ref used
                        // to arrive carrying numbers computed against this TU's
                        // instances for the file, which were then discarded,
                        // so it had to be recomputed against the set it would
                        // actually live with.  Numbering the merged file once
                        // removes the problem instead of correcting it.
                        existing.refs.push_back(ref);
                    }
                }
            } else {
                const size_t idx = all_files_.size();
                file_index_[file.filesystem_path] = idx;
                all_files_.push_back(std::move(file));
                // Seed this file's dedup state from what it arrived with, once.
                merged_inst_keys_.resize(all_files_.size());
                merged_ref_keys_.resize(all_files_.size());
                const FileData& added = all_files_[idx];
                auto& ik = merged_inst_keys_[idx];
                ik.reserve(added.instances.size());
                for (size_t i = 0; i < added.instances.size(); ++i) {
                    const InstanceData& e = added.instances[i];
                    ik.emplace(InstKey{e.symbol_local_id, e.instance_type,
                                       e.start_offset, e.end_offset}, i);
                }
                auto& rk = merged_ref_keys_[idx];
                rk.reserve(added.refs.size());
                for (const RefData& r : added.refs)
                    rk.insert({r.to_symbol_local_id, r.from_offset_start, r.from_offset_end});
            }
        }

        // Set modname only on the main TU source file (not included headers)
        if (!cmd.modname.empty()) {
            auto it = file_index_.find(canon_filename);
            if (it != file_index_.end())
                all_files_[it->second].modname = cmd.modname;
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
        execlp("git", "git", "-C", git_root_.c_str(), "ls-files", "-z", nullptr);
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

        addFile(git_root_ + "/" + rel_path);
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
    fd.instances.push_back({file_sym_id, 0, static_cast<int32_t>(fd.content.size()), filetypeToInstType(fd.filetype)});

    file_index_[abs_path] = all_files_.size();
    all_files_.push_back(std::move(fd));
    // Keep the merge's per-file state the same length as `all_files_`.  These
    // files arrive after the translation-unit loop and are never merged into,
    // but the two must not drift apart.
    merged_inst_keys_.resize(all_files_.size());
    merged_ref_keys_.resize(all_files_.size());
}

void Indexer::createModuleSymbols() {
    std::set<std::string> modules;
    for (auto& file : all_files_) {
        if (!file.modname.empty())
            modules.insert(file.modname);
    }

    for (auto& mod : modules) {
        symbol_table_.getOrCreate(mod, SCOPE_GLOBAL, SYMTYPE_MODULE);
    }

    fprintf(stderr, "Modules: %zu\n", modules.size());
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

    // NUMBER EACH FILE ONCE, now that every TU has contributed to it.
    //
    // This used to run per TU, inside `Stage1::process`, and that is what made
    // a merged ref's numbers describe a numbering that no longer existed --
    // `placeRefByText` was the correction. A file's containment tree is a
    // property of its TEXT, so it can only be derived once the text's
    // contributors are all in.
    //
    // Files are independent -- containment never crosses an object -- so this
    // parallelises with no locking, and it is strictly less work than before:
    // a header included by K translation units was numbered K times.
    //
    // BEFORE `addGitTrackedFiles` on purpose. Those files get one instance and
    // no walk, so they keep `dfs_exit == 0`, which is exactly how
    // `ProtoBuilder::ensureObjectRoot` recognises "never numbered".
    {
        // A canonical instance order first, so the numbers below are a function
        // of what was found rather than of the order it arrived in.  Indexed by
        // local id, which is dense from 1.
        std::vector<SymbolOrder> order;
        for (const SymbolInfo& si : symbol_table_.allSymbols()) {
            if (si.local_id < 0) continue;
            if (static_cast<size_t>(si.local_id) >= order.size())
                order.resize(static_cast<size_t>(si.local_id) + 1);
            order[static_cast<size_t>(si.local_id)] = SymbolOrder{si.name, si.scope};
        }

        std::atomic<size_t> next_file{0};
        std::vector<std::thread> numberers;
        for (int t = 0; t < threads_; t++) {
            numberers.emplace_back([this, &next_file, &order]() {
                while (true) {
                    size_t idx = next_file.fetch_add(1);
                    if (idx >= all_files_.size()) break;
                    canonicaliseInstanceOrder(all_files_[idx], order);
                    Stage1::numberTree(all_files_[idx]);
                }
            });
        }
        for (auto& w : numberers) w.join();
    }

    if (!git_root_.empty()) {
        addGitTrackedFiles();
    }

    createDirectorySymbols();
    if (!modules_method_.empty())
        createModuleSymbols();

    fprintf(stderr, "Files: %zu\n", all_files_.size());
    fprintf(stderr, "Symbols: %zu\n", symbol_table_.allSymbols().size());

    // What the cross-TU union contributed. Always on, because the interesting
    // number is a SURPRISING one: a file two TUs saw differently is usually an
    // `#ifdef` only some of them satisfied, and both arms now co-exist in the
    // index with nothing saying which build each belongs to. That is the
    // accepted cost of describing the text rather than one configuration, and
    // it is worth being able to see how much of it there is.
    if (late_instance_count_ > 0)
        fprintf(stderr, "Cross-TU: %zu files seen differently by 2+ TUs, "
                        "%zu instances contributed by a later TU\n",
                files_with_late_instances_.size(), late_instance_count_);
}

static void write_file(const std::string& path, const std::string& data) {
    std::ofstream out(path, std::ios::binary);
    if (!out) {
        throw std::runtime_error("Failed to open output file: " + path);
    }
    out.write(data.data(), data.size());
    out.close();
    if (!out) {
        throw std::runtime_error("Failed to write output file: " + path);
    }
}

static std::string human_size(size_t bytes) {
    if (bytes >= 1024ULL * 1024 * 1024) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f GB", bytes / (1024.0 * 1024 * 1024));
        return buf;
    } else if (bytes >= 1024ULL * 1024) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f MB", bytes / (1024.0 * 1024));
        return buf;
    } else if (bytes >= 1024ULL) {
        char buf[32];
        snprintf(buf, sizeof(buf), "%.0f KB", bytes / 1024.0);
        return buf;
    }
    return std::to_string(bytes) + " B";
}

void Indexer::write(const std::string& output_dir) {
    auto result = ProtoBuilder::build(
        project_name_, root_path_, symbol_table_, all_files_,
        next_object_id_.load());

    std::filesystem::create_directories(output_dir);

    // Remove stale contents-*.pb from prior runs
    for (auto& entry : std::filesystem::directory_iterator(output_dir)) {
        auto fname = entry.path().filename().string();
        if (fname.rfind("contents-", 0) == 0 && fname.size() > 3 && fname.compare(fname.size() - 3, 3, ".pb") == 0) {
            std::filesystem::remove(entry.path());
        }
    }

    // Write project.pb
    std::string project_path = output_dir + "/project.pb";
    write_file(project_path, result.project_data);
    fprintf(stderr, "Wrote %s (%s, content %s)\n", project_path.c_str(),
            human_size(result.project_data.size()).c_str(),
            result.content_inlined ? "inlined" : "in batch files");

    // Write contents-NNNN.pb only when content is NOT inlined
    if (!result.content_inlined) {
        for (size_t i = 0; i < result.content_batches.size(); ++i) {
            char name[64];
            snprintf(name, sizeof(name), "contents-%04zu.pb", i + 1);
            std::string batch_path = output_dir + "/" + name;
            write_file(batch_path, result.content_batches[i]);
            fprintf(stderr, "Wrote %s (%s)\n", batch_path.c_str(),
                    human_size(result.content_batches[i].size()).c_str());
        }
    }
}
