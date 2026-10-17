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

    // FOLDING -- orthogonal to depth.  An instance is folded when it exists
    // only because of a macro expansion; fold_depth counts the folded
    // ancestors above it.  `X { Y }` reaches descendants at the same
    // fold_depth, so what a macro generates is one step out of reach by
    // default and `mod { macro { func } }` steps into it on purpose.
    bool folded = false;
    int32_t fold_depth = 0;

    // TRANSIENT, never on the wire: the instance this one was nested inside
    // according to the AST walk (nearest open scope in the same file), or -1.
    // `numberTree` uses it only to break ties between instances with IDENTICAL
    // ranges -- the one case ranges cannot decide.
    int32_t stack_parent = -1;

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

    // Fold state of the owning scope: the argument token in `printk(foo)`
    // stays at the caller's fold depth, each expanded use sits one deeper.
    bool folded = false;
    int32_t fold_depth = 0;

    // TRANSIENT, never on the wire: this reference was SYNTHESISED by Stage2
    // at a declaration site rather than read from a use site, so its range
    // deliberately does not spell its symbol -- a function-pointer field's
    // declaration claims the function assigned to it.  Stage2 does not go
    // through `insertRef`, so the producer's range check never sees these;
    // the flag lets a test state the same exemption instead of listing
    // fixtures.
    bool synthetic = false;

    // TRANSIENT, never on the wire: the instance that owns this reference.
    // Set from the AST scope for an ordinary ref and from the enclosing macro
    // for an expansion-site ref -- a distinction ranges cannot make, since both
    // lie inside the caller.  `numberTree` stamps the numbers above from it, so
    // "a ref lies inside its owner" holds by construction.
    int32_t owner_idx = -1;

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

/// THE ONE PLACEMENT RULE FOR REFERENCES: a reference belongs to the narrowest
/// UNFOLDED instance whose extent contains its text.  Returns an index into
/// `f.instances`, or -1 when nothing contains it.
///
/// Unfolded, because *a macro expansion owns no text*: every byte at the call
/// site of `foo(a)` -- the name, the parens, the argument -- was typed by the
/// CALLER.  An expansion owns the symbols it generates, through the DFS
/// interval, and no bytes at all.  So excluding folded instances is what keeps
/// `DEV_PRINT(threshold)` from producing an edge `DEV_PRINT -> threshold`.
///
/// This replaced placement by AST scope (`Stage1::stackParent`), which could
/// not see a macro body at all: the scope stack is consulted per FILE, and when
/// `handler` in main.c expands a macro defined in log.h, nothing from log.h is
/// on it.  So `printk`, written inside `DEV_PRINT`'s `#define` body, was
/// attributed to log.h's file root and the edge read `log.h -> printk` instead
/// of `DEV_PRINT -> printk`.  Text containment gets the same answer as the
/// scope for every ordinary reference and the right one here.
///
/// The one thing text alone could not tell used to be an argument token from
/// the macro's own expansion-site use of it, both lying inside the caller at
/// overlapping offsets.  There are no expansion-site references any more --
/// each layer records only what its own body names -- so the ambiguity is gone
/// with them.
int32_t ownerOfText(const FileData& f, int32_t start, int32_t end);

/// A SYMBOL'S PLACE IN THE CANONICAL ORDER, indexed by its local id.
///
/// The local id itself cannot be used: ids are handed out by `getOrCreate` in
/// the order symbols are first SEEN, which is translation-unit arrival order.
/// The name and the scope come from the source, so they are the same whatever
/// order the index was built in.
struct SymbolOrder {
    std::string name;
    int scope = 0;
};

/// Put a file's instances in a canonical order, so that everything downstream
/// of them is a function of the SET rather than of the order translation units
/// happened to contribute.
///
/// Two things read the vector's order and would otherwise inherit the merge's:
/// `numberTree` builds each node's child list by walking it, which fixes the
/// preorder sequence and therefore every DFS number; and `ownerOfText` breaks
/// ties between equally narrow candidates by taking the earlier one.
///
/// Measured before this existed: reversing criu's 211 translation units moved
/// 2,953 instances and 3,006 references to different numbers. The TREE was
/// identical -- not one of 34,018 instances changed `nesting_level`,
/// `fold_depth` or `folded` -- so nothing was wrong with the graph; it simply
/// could not be reproduced, which rules out comparing two indices or
/// addressing a row by position across a reindex.
///
/// The order is `(start, end, instance_type, symbol name, symbol scope)`, and
/// it is TOTAL because of instance dedup: `InstKey` makes
/// `(symbol, type, extent)` unique per file, so two instances can only tie here
/// if they share a symbol, and they cannot. Every component is needed --
/// measured on criu, `(start, end, type)` alone collides 49 times on
/// co-extensive macro-generated symbols whose name spans are both absent, and
/// adding the name still leaves one collision, a `static`/`extern` pair of the
/// same declaration that is its own open defect.
void canonicaliseInstanceOrder(FileData& f, const std::vector<SymbolOrder>& order);

struct Stage1Result {
    std::vector<FileData> files;
    std::unordered_map<std::string, size_t> file_index; // filesystem_path -> index in files
};

class Stage1 {
public:
    Stage1(SymbolTable& symbols, std::atomic<int64_t>& next_object_id);

    void process(CXTranslationUnit tu, const std::string& tu_filename);
    Stage1Result takeResults();

    /// Derive one file's containment tree from the ranges the walk collected,
    /// and number it.  See the definition for the parentage rule.
    ///
    /// Public, and called from OUTSIDE Stage1 -- by `Indexer::run`, once every
    /// translation unit has contributed to the file. It is a function of the
    /// file alone, which is the point: a containment tree is a property of the
    /// text, and no single TU holds more than one view of it.
    static void numberTree(FileData& f);

private:
    SymbolTable& symbols_;
    std::atomic<int64_t>& next_object_id_;
    Stage1Result result_;

    // Per-TU state
    std::unordered_map<std::string, size_t> file_index_; // filepath -> index in result_.files
    std::unordered_map<size_t, std::unordered_set<EntryKey, EntryKeyHash>> file_ref_sets_; // per-file ref dedup
    /// Per-file instance index, key -> position in that file's `instances`.
    ///
    /// A set would be enough to drop a repeat, but `addInstance` needs the
    /// SURVIVOR'S INDEX: the second parse of a header must push that onto
    /// `dfs_stack_`, so its children nest under the instance that stayed
    /// instead of under one that was never appended.
    ///
    /// Within a file this makes the key a primary key, which is what lets the
    /// cross-TU merge be a set union (see `Indexer::processTU`).
    std::unordered_map<size_t, std::unordered_map<InstKey, size_t, InstKeyHash>> file_inst_index_;
    /// Every macro this TU defined, by name.  Scoped to the TU on purpose: a
    /// macro body can only name a macro visible in the same TU, so resolving
    /// against this rather than the project-global symbol table is both
    /// cheaper and stricter -- it cannot invent an edge to a same-named macro
    /// from an unrelated file.
    std::unordered_map<std::string, int64_t> tu_macros_;

    /// A `#define` body awaiting the token pass that finds the macros it
    /// names.  Deferred to the end of `collectMacros` so that `tu_macros_` is
    /// complete first: `#define A B` is legal before `B` is defined.
    struct MacroBody {
        /// THE CURSOR, kept.  `linkMacroBodies` needs a source RANGE to
        /// tokenise, and the only cheap way to get one is from the cursor that
        /// already has it: `clang_getCursorExtent` is pure translation.
        ///
        /// Recovering the range from `(file, start, end)` instead costs
        /// `clang_getLocationForOffset`, which is O(SLocEntries IN THE
        /// TRANSLATION UNIT) -- `ASTUnit::getLocation` asks
        /// `SourceManager::translateFileLineCol(File, 1, 1)`, and
        /// `translateFile` linear-scans every entry looking for the file.  One
        /// entry exists per inclusion AND per macro expansion, so with a
        /// detailed preprocessing record a kernel translation unit has a very
        /// large number of them, and this ran twice for every `#define` the
        /// unit could see.  Measured: 81.5% of all cycles in that one scan
        /// loop, 7.2 CPU-seconds per translation unit against under one to
        /// parse it.
        ///
        /// A CXCursor is a small value valid for its translation unit's
        /// lifetime, and `macro_bodies_` never outlives the unit it came from.
        CXCursor cursor;
        size_t file_idx;
        unsigned start, end;   // the whole `#define` body extent
        std::string name;      // the macro being defined
    };
    std::vector<MacroBody> macro_bodies_;

    // --- DFS numbering -------------------------------------------------
    //
    // Numbers are SPARSE so an ephemeral layer can later insert an instance
    // between two neighbours without renumbering; one stride per instance is
    // what `DFS_STRIDE` on the askld side requires.  Numbering is per FILE,
    // because containment never crosses objects.
    //
    // The WALK only records structure -- a parent index per instance, an owner
    // index per ref.  `numberTree` then assigns every tree field in one place,
    // which is what makes it impossible to set three of four and forget the
    // fourth (the zero-width-leaf bug).
    static constexpr int64_t kDfsStride = 1024;

    struct DfsScope {
        size_t file_idx;
        size_t instance_idx;
    };
    std::vector<DfsScope> dfs_stack_;

    /// Index of the innermost open scope in `fi`, or -1.  Scopes from other
    /// files are skipped: one TU walk descends into headers, and each file has
    /// its own tree.
    int32_t stackParent(size_t fi) const;

    /// True when the cursor exists only because of a macro expansion -- its
    /// spelling location (where the text is) differs from its expansion
    /// location (where the code lands).
    static bool isMacroGenerated(CXCursor cursor);
    /// Append `d` to `fi`, numbering it into the enclosing scope.  When
    /// `opens_scope`, it becomes the parent of whatever is created next until
    /// the matching closeScopesTo.
    void addInstance(size_t fi, InstanceData d, bool opens_scope);
    /// Close scopes until the stack is back to `mark`, stamping each exit.
    void closeScopesTo(size_t mark);
    /// Give every file's root instance an interval covering the whole file.

    size_t getOrCreateFile(CXFile file);

    void collectMacros(CXTranslationUnit tu);
    /// Emit a reference for every macro a `#define` body names, which is the
    /// layerwise edge between two macros.  libclang does not report it: the
    /// preprocessing record carries only the OUTERMOST expansion at a use
    /// site, so `#define FATAL(m) ERR(m)` produces no expansion of `ERR`
    /// anywhere, and `FATAL -> ERR -> MSG -> output` had a hole in the middle
    /// where only the ends were visible.  On criu 2193 of 12704 macro
    /// definitions (17%) name another macro, so the hole is the common case,
    /// not an edge case.
    void linkMacroBodies(CXTranslationUnit tu);
    static CXChildVisitResult macroVisitor(CXCursor cursor, CXCursor parent, CXClientData data);

    static CXChildVisitResult visitor(CXCursor cursor, CXCursor parent, CXClientData data);
    static void inclusionCallback(CXFile included_file, CXSourceLocation* stack,
                                  unsigned len, CXClientData data);
    void visitCursor(CXCursor cursor, CXCursor parent);
    /// The spelling range of the token that NAMES this reference's symbol --
    /// which is what a reference's range means.  Its start comes from
    /// `clang_Cursor_getSpellingNameRange`; its end is measured in the file's
    /// own text, because clang reports the name range's end at the EXPANSION
    /// point for a token spelled inside a macro body.
    bool refNameRange(CXCursor cursor, const std::string& sym_name, size_t& out_fi,
                      int32_t& out_start, int32_t& out_end);
    /// The name range when it names the symbol, else the cursor's extent.
    bool refRange(CXCursor cursor, const std::string& sym_name, size_t& out_fi,
                  int32_t& out_start, int32_t& out_end);
    void addRef(CXCursor cursor, int64_t sym_id, const std::string& sym_name);
    /// Does the text at a reference's range actually spell the symbol it
    /// references?  A range that does not is describing a position it does not
    /// occupy, which is worse than the reference being absent.
    bool refTextNamesSymbol(size_t file_idx, const std::string& sym_name,
                            int32_t start, int32_t end, bool warn) const;
    void insertRef(size_t file_idx, int64_t sym_id, const std::string& sym_name,
                   int32_t start, int32_t end,
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
