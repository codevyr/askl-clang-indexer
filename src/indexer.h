#pragma once

#include "compilation_db.h"
#include "symbol_table.h"
#include "stage1.h"

#include <atomic>
#include <mutex>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class Indexer {
public:
    Indexer(const std::string& project_name, const std::string& compile_commands_dir,
            const std::string& root_path, int threads, const std::string& git_root = "",
            bool show_progress = false, bool show_warnings = false,
            const std::string& modules_method = "");

    void run();
    void write(const std::string& output_path);

    const SymbolTable& symbolTable() const { return symbol_table_; }
    const std::vector<FileData>& allFiles() const { return all_files_; }

private:
    std::string project_name_;
    std::string root_path_;
    int threads_;
    std::string git_root_;
    bool show_progress_;
    bool show_warnings_;
    std::string modules_method_;
    std::atomic<size_t> processed_tus_{0};

    CompilationDB compile_db_;
    SymbolTable symbol_table_;

    std::atomic<int64_t> next_object_id_{1};
    std::mutex files_mutex_;
    std::vector<FileData> all_files_;
    std::unordered_map<std::string, size_t> file_index_; // guarded by files_mutex_

    /// THE MERGE'S DEDUP STATE, KEPT, not rebuilt.
    ///
    /// Both of these used to be locals, rebuilt from `existing` on every merge
    /// -- so the cost of folding one translation unit's view of a file into the
    /// accumulated one was O(everything accumulated so far), and the whole run
    /// was quadratic in how often a file is included.  A kernel header reached
    /// by 20,000 translation units with 3,000 instances and 10,000 references
    /// cost 20,000 x 13,000 hash insertions by itself.
    ///
    /// Measured on 26,077 kernel translation units: ~21 TUs a minute and
    /// falling, against 124 a minute on a small sample.  Per-TU cost tracked
    /// ACCUMULATED FILES, not TU size -- 893ms at 2,978 files, 979ms at 4,156 --
    /// which is the signature of this and not of anything in the parse.
    ///
    /// Indexed in lockstep with `all_files_`; both are guarded by files_mutex_.
    std::vector<std::unordered_map<InstKey, size_t, InstKeyHash>> merged_inst_keys_;
    std::vector<std::unordered_set<EntryKey, EntryKeyHash>> merged_ref_keys_;

    /// How much the cross-TU union actually contributed, for the summary line.
    /// A file counted here was seen DIFFERENTLY by two or more translation
    /// units -- usually an `#ifdef` that only some of them satisfied. Both are
    /// guarded by files_mutex_.
    std::unordered_set<size_t> files_with_late_instances_;
    size_t late_instance_count_ = 0;

    void processTU(const CompileCommand& cmd);
    void addGitTrackedFiles();
    void addFile(const std::string& abs_path);
    void createDirectorySymbols();
    void createModuleSymbols();
    std::string computeCommonAncestor(const std::vector<CompileCommand>& cmds);
};
