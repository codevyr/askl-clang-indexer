#pragma once

#include "dedup_keys.h"

#include <atomic>
#include <clang-c/Index.h>
#include <cstdint>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>

class SymbolTable;

struct InstanceData {
    int64_t symbol_local_id;
    int32_t start_offset;
    int32_t end_offset;
    int32_t instance_type;

    // Position in this file's containment tree.  Descendants are exactly the
    // instances whose dfs_enter lies strictly between dfs_enter and dfs_exit,
    // and a direct child is additionally at nesting_level + 1.
    //
    // This is what orders compiler-synthesised symbols, which ranges cannot:
    // everything a macro generates shares one extent.  Defaults keep older
    // brace-initialisation working for callers that do not number.
    int32_t nesting_level = 0;
    int64_t dfs_enter = 0;
    int64_t dfs_exit = 0;

    // Finer spans inside [start_offset, end_offset).  -1 = the indexer does not
    // know one: a ##-pasted identifier has no file location at all (its name
    // lives in clang's <scratch space> buffer), and not every kind has a
    // signature or a body.  Absent must stay distinguishable from empty.
    int32_t name_start = -1, name_end = -1;
    int32_t sig_start = -1, sig_end = -1;
    int32_t content_start = -1, content_end = -1;
};

struct RefData {
    int64_t to_symbol_local_id;
    int32_t from_offset_start;
    int32_t from_offset_end;

    // A ref is a POINT in the same sequence, not an interval: it is owned by
    // the instance whose [dfs_enter, dfs_exit] contains this number at
    // nesting_level - 1.
    int32_t nesting_level = 0;
    int64_t dfs_enter = 0;

    // FULL span of the referencing construct -- `printk(foo)` where
    // [from_offset_start, from_offset_end) covers only `printk`.  Display only.
    int32_t full_start = -1, full_end = -1;
};

struct FileData {
    int64_t object_local_id;
    std::string module_path;
    std::string filesystem_path;
    std::string filetype;
    std::string modname;  // kernel module name, empty if not part of a module
    std::vector<uint8_t> content;
    std::vector<InstanceData> instances;
    std::vector<RefData> refs;
};

struct Stage1Result {
    std::vector<FileData> files;
    std::unordered_map<std::string, size_t> file_index; // filesystem_path -> index in files
};

class Stage1 {
public:
    Stage1(SymbolTable& symbols, std::atomic<int64_t>& next_object_id);

    void process(CXTranslationUnit tu, const std::string& tu_filename);
    Stage1Result takeResults();

private:
    SymbolTable& symbols_;
    std::atomic<int64_t>& next_object_id_;
    Stage1Result result_;

    // Per-TU state
    std::unordered_map<std::string, size_t> file_index_; // filepath -> index in result_.files
    std::unordered_map<size_t, std::unordered_set<EntryKey, EntryKeyHash>> file_ref_sets_; // per-file ref dedup
    std::unordered_map<uint64_t, unsigned> macro_name_ends_; // fileOffsetKey(fi, off) -> end_offset

    // --- DFS numbering -------------------------------------------------
    //
    // Numbers are SPARSE so an ephemeral layer can later insert an instance
    // between two neighbours without renumbering: 10 successive inserts at one
    // position (1024 -> 512 -> ... -> 1) before a local renumber is needed.
    // Counters are per FILE, because containment never crosses objects.
    static constexpr int64_t kDfsStride = 1024;

    struct DfsScope {
        size_t file_idx;
        size_t instance_idx;
    };
    std::vector<DfsScope> dfs_stack_;
    std::unordered_map<size_t, int64_t> dfs_counter_; // file idx -> last number used

    /// Depth an instance created right now in `fi` would sit at.  The file's
    /// own instance is the root at depth 0, so everything the walk produces
    /// starts at 1.
    int32_t currentDepth(size_t fi) const;
    /// Append `d` to `fi`, numbering it into the enclosing scope.  When
    /// `opens_scope`, it becomes the parent of whatever is created next until
    /// the matching closeScopesTo.
    void addInstance(size_t fi, InstanceData d, bool opens_scope);
    /// Close scopes until the stack is back to `mark`, stamping each exit.
    void closeScopesTo(size_t mark);
    /// Give every file's root instance an interval covering the whole file.
    void finalizeFileRoots();

    static uint64_t fileOffsetKey(size_t file_idx, unsigned offset) {
        return (static_cast<uint64_t>(file_idx) << 32) | offset;
    }

    size_t getOrCreateFile(CXFile file);

    void collectMacros(CXTranslationUnit tu);
    static CXChildVisitResult macroVisitor(CXCursor cursor, CXCursor parent, CXClientData data);

    static CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData data);
    static void inclusionCallback(CXFile included_file, CXSourceLocation* stack,
                                  unsigned len, CXClientData data);
    void visitCursor(CXCursor cursor, CXCursor parent);
    void addRef(CXCursor cursor, int64_t sym_id, unsigned name_len);
    void insertRef(size_t file_idx, int64_t sym_id, int32_t start, int32_t end,
                   int32_t full_start = -1, int32_t full_end = -1);
    void addDocComment(CXCursor cursor, int64_t sym_id, size_t file_idx);
    void indexChildField(CXCursor child);
    /// Fill in the name / signature / content spans for `cursor`, leaving any
    /// the indexer cannot determine at -1.  Only spans that land in `file` and
    /// inside the instance's own extent are recorded.
    static void fillSpans(CXCursor cursor, CXFile file, InstanceData& d);

    // Names of types whose hoisted EnumDecl/StructDecl/UnionDecl should skip
    // TYPE instance creation because a TypedefDecl already covers it.
    std::unordered_set<std::string> typedef_type_names_;
    void collectTypedefTypes(CXTranslationUnit tu);
};
