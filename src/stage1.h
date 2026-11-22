#pragma once

#include "dedup_keys.h"
#include "macro_paste.h"
#include "symbol_types.h"

#include <atomic>
#include <clang-c/Index.h>
#include <cstdint>
#include <map>
#include <memory>
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

    // TRANSIENT, never on the wire: the two things a declaration says about
    // where it came from.
    //
    // `generated`: the whole extent lies in one macro invocation, so the
    // declaration has no text of its own (`Stage1::isWhollyAnInvocation`).
    // Containment reads it: such a declaration contains only what was read
    // inside it (`Stage1::numberTree`).  Folding reads it too, as one of the
    // reasons a SYMBOL was never written down (`Indexer::foldSymbols`) -- but
    // the two do not read each other.
    //
    // `name_typed`: the token that names it was typed outside every `#define`
    // body -- `lock` in `DEFINE_MUTEX(lock)`.  False for a name a macro body
    // wrote or pasted together.
    bool generated = false;
    bool name_typed = false;

    // TRANSIENT, never on the wire: THE INVOCATION THAT NAMES IT -- where, in
    // this file, the outermost macro invocation starts that its name came
    // through, or -1 when somebody typed the name outside every invocation.
    //
    //     static DEFINE_MUTEX(lock);
    //     PROFILE_FUNC(int, send, x) { return x; }
    //     #define MPI_Send PMPI_Send
    //     int MPI_Send(void) { ... }
    //
    // Each of these has text of its own, so none is `generated`; each got its
    // name from the invocation.  That invocation DECLARES it: the macro refers
    // to the declaration (`Stage1::linkDeclared`), and the invocation sits
    // where the declaration sits, not inside it (`Stage1::numberTree`).
    //
    // By the NAME, not the extent: `DEPRECATED int f(void)` starts on an
    // invocation that names nothing.
    //
    // `introduced`: this is a definition, or the first declaration the
    // compiler met.  `EXPORT_SYMBOL(foo)` redeclares `foo` and introduces
    // nothing; the macro does not lead to a symbol it merely mentioned again.
    int32_t named_at = -1;
    bool introduced = false;

    // TRANSIENT, never on the wire: the instance this one was READ INSIDE
    // according to the AST walk (nearest open scope in the same file).
    // `numberTree` uses it where text cannot say what contains what: among
    // declarations with one extent, and for a generated declaration, whose
    // extent is its invocation's and not text of its own.
    //
    // `kReadAtFileScope`: inside no declaration of this file.
    // `kReadDisputed`: two translation units read it inside DIFFERENT
    // declarations (one file compiled twice under two names).  Neither is
    // wrong and a tree has one parent, so the walk says nothing and text
    // decides -- keeping whichever unit arrived first would make the index
    // depend on thread timing.
    static constexpr int32_t kReadAtFileScope = -1;
    static constexpr int32_t kReadDisputed = -2;
    int32_t stack_parent = kReadAtFileScope;

    // TRANSIENT, never on the wire: the walk enters this declaration, so it
    // knows of everything it met whether that was read inside it.  False for
    // what is recorded without the walk: a file, a `#define`, a comment.
    bool scope = false;

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

    // ON THE WIRE (`SymbolRef.roles`): WHAT THE TOKEN DOES TO ITS TARGET, a
    // set of `REFROLE_*` bits.  Every bit is a positive fact about the text at
    // this token and an absent bit states nothing: zero is "refers to", never
    // "does not call".  So two readings of one token merge by UNION
    // (`mergeStated`), and a token that states any role states every role it
    // has: `CALL_AND_KEEP(fn)`, which calls its argument and hands it on, is
    // CALL|TAKE on the one `fn` the caller typed.
    uint32_t roles = 0;

    // ON THE WIRE (`SymbolRef.relation`): zero for a reference.  Otherwise
    // this row is not something a token says but a fact between two symbols
    // (`REFRELATION_*`), and it sits DIRECTLY IN the declaration of its
    // source: the engine gives a relation to the one instance exactly one
    // level above it, where a reference belongs to every declaration around
    // it.  The struct does not hold what its field holds.
    int32_t relation = REFRELATION_NONE;

    // TRANSIENT, never on the wire: what the range says about the NAME, for
    // folding.  A reference unfolds its target when its name was typed in
    // normal code, and for most references the range is the name, so where
    // the reference sits answers that.  Two kinds need saying separately:
    //
    // `pasted`: the range is the PART of a pasted name its caller chose
    // (`send` in `CALL(send)`), or the macro handed to a list.  Inside a
    // generated declaration such a reference is the generator's plumbing --
    // `KHASH_INIT(x, ...)` makes functions that call each other, all on the
    // one `x` -- and does not count.
    //
    // `typed_in_define`: that part was typed inside a `#define` body although
    // the reference sits in normal code -- `EACH(ENTRY)`, where the list
    // wrote `alpha` and `ENTRY` completed it.
    bool pasted = false;
    bool typed_in_define = false;

    // TRANSIENT, never on the wire: this reference was SYNTHESISED rather
    // than read from a use site, so its range deliberately does not spell its
    // symbol.  Two kinds: Stage2's, where a function-pointer field's
    // declaration claims the function assigned to it, and a macro's reference
    // to what its invocations declared, which sits on the macro's own name
    // (`Stage1::linkGenerated`).  Nobody typed the target's name at either, so
    // neither says the symbol was written down.  The flag also lets a test
    // state the range exemption instead of listing fixtures.
    bool synthetic = false;

    // TRANSIENT, never on the wire: the innermost declaration of this file the
    // walk was inside when it met the reference AND whose text holds the
    // token, as an index into the file's instances (`Stage1::scopeAround`).
    //
    // It is PART OF WHAT THE REFERENCE IS until the file is numbered: one
    // token read inside two declarations is two references (`RefKey`).  Like
    // `InstanceData::stack_parent`, it also chooses among instances with
    // IDENTICAL extents, which text cannot tell apart.
    //
    // `kNoScope` when the walk has nothing to say: the token is written
    // outside every declaration being read (a `#define` body, a macro use
    // recorded before the walk).
    static constexpr int32_t kNoScope = -1;
    int32_t scope_idx = kNoScope;

    // TRANSIENT, never on the wire: this reference IS a macro invocation, and
    // this is where the invocation starts.  A declaration the invocation names
    // (`InstanceData::named_at`) does not contain it.  -1 for every other
    // reference.
    int32_t invocation_at = -1;

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

/// TWO READINGS OF ONE REFERENCE STATE WHAT EITHER STATES.
///
/// The one merge of roles and relation, used wherever two records of the same
/// reference meet: within a unit (`Stage1::insertRef`), when Stage2's are
/// added, across units (`mergeRefView`), after numbering (`numberTree`) and in
/// `ProtoBuilder`.  Roles are positive facts, so they OR, and the result does
/// not depend on which reading came first.  A relation is what the row IS; two
/// readings of one row agree on it, and where only one states it, it stands.
inline void mergeStated(RefData& kept, const RefData& in) {
    kept.roles |= in.roles;
    if (kept.relation == REFRELATION_NONE) kept.relation = in.relation;
}

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

/// TWO READINGS OF WHERE ONE INSTANCE WAS READ, reconciled.
///
/// clang meets a struct written inside a declarator twice: hoisted, as a
/// sibling before `typedef struct { ... } T;`, and again under the typedef.
/// The second reading has the first one's place on its chain and is the more
/// specific of the two, so it is taken.  Two readings with neither inside the
/// other -- one header included twice, one file compiled twice -- are a
/// disagreement, `InstanceData::kReadDisputed`.  Symmetric, so the result does
/// not depend on which reading came first.
///
/// `self` is the instance the readings are of.  A reading that lies inside
/// `self` is dropped, so `stack_parent` never leads back to where it started:
/// `numberTree` follows that chain and relies on it ending.
int32_t reconcileReadIn(const std::vector<InstanceData>& instances, int32_t self, int32_t kept,
                        int32_t incoming);

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
/// preorder sequence and therefore every DFS number; and where two typed
/// declarations share one extent, the earlier of them is an instance's parent.
///
/// Measured before this existed: reversing criu's 211 translation units moved
/// 2,953 instances and 3,006 references to different numbers. The TREE was
/// identical -- not one of 34,018 instances changed `nesting_level` -- so
/// nothing was wrong with the graph; it simply could not be reproduced, which
/// rules out comparing two indices or addressing a row by position across a
/// reindex.
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

    /// Where a reference goes, and what its range says about the name.
    struct RefPlace {
        size_t fi = 0;
        int32_t start = -1, end = -1;
        /// The range is a macro that BUILDS the name rather than a part of it
        /// (`PasteSite::by_callback`), so the text there cannot be expected
        /// to spell the symbol.
        bool unspelled = false;
        /// See `RefData::pasted` and `RefData::typed_in_define`.
        bool pasted = false;
        bool typed_in_define = false;
        /// See `RefData::synthetic`.
        bool synthetic = false;
        /// The reference is a macro invocation; see `RefData::invocation_at`.
        bool invocation = false;
        /// What the reference states; see `RefData::roles` and
        /// `RefData::relation`.
        uint32_t roles = 0;
        int32_t relation = REFRELATION_NONE;
    };

    /// Would a reference to the symbol `name`, made by this cursor, get a
    /// range that names it -- the token that spells it, or for a pasted name
    /// the part its caller typed?  The same decision `addRef` makes, without
    /// recording anything.  Valid only while this unit's cursors are.
    bool placesReference(CXCursor cursor, const std::string& name);

private:
    SymbolTable& symbols_;
    std::atomic<int64_t>& next_object_id_;
    Stage1Result result_;

    // Per-TU state
    std::unordered_map<std::string, size_t> file_index_; // filepath -> index in result_.files
    /// Per file: one reference per (target, token, scope), and WHERE IT IS in
    /// that file's `refs`.
    ///
    /// A set was enough to drop a second reading, and that is what it did:
    /// the second reading was thrown away with whatever it knew.  A token read
    /// twice can do two things (`CALL_AND_KEEP(fn)` calls its argument and
    /// hands it on), so the second reading has to reach the first.
    /// `kRefDropped` for a reference that was refused (its range does not
    /// spell its symbol): a repeat of it is refused the same way.
    static constexpr int32_t kRefDropped = -1;
    std::unordered_map<size_t, std::unordered_map<RefKey, int32_t, RefKeyHash>> file_ref_index_;
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
        /// It takes arguments: `(` follows the name with nothing between.
        bool function_like = false;
    };
    std::vector<MacroBody> macro_bodies_;

    // --- Pasted names ----------------------------------------------------
    //
    // A reference whose name a macro built with `##` sits where the caller's
    // part of that name was typed; see macro_paste.h for why and how.  What is
    // kept here is what the replay reads: every invocation's cursor, so its
    // tokens can be had without rebuilding a source range, and every
    // definition by name, for the macros an expansion goes on to invoke.
    //
    // All of it is lazy.  Nothing is tokenised until a reference turns up
    // whose name has no spelling, and then only the one invocation it came
    // from, once.

    /// Token spellings for this unit, interned; see `PasteStrings`.
    PasteStrings paste_strings_;
    /// Macro name -> index in `macro_bodies_` of its LATEST definition.
    std::unordered_map<std::string, size_t> macro_def_index_;
    /// The same, by interned name: -2 not asked yet, -1 not a macro.  This is
    /// the question a replay asks of every identifier it meets.
    std::vector<int32_t> macro_of_string_;
    /// Parsed on first use, parallel to `macro_bodies_`; null when unreadable.
    std::vector<std::unique_ptr<PasteMacro>> paste_macros_;
    std::vector<char> paste_macro_tried_;
    /// File -> invocation start offset -> the names that invocation pastes.
    std::unordered_map<size_t, std::unordered_map<
                                   int32_t, std::unordered_map<PasteStrings::Id, PasteSite>>>
        paste_cache_;

    /// The tokens of a macro invocation or definition, each with the place it
    /// was typed.
    std::vector<PasteToken> pasteTokens(CXCursor cursor);
    std::unique_ptr<PasteMacro> parsePasteMacro(CXCursor definition);
    const PasteMacro* pasteMacroAt(size_t index);
    const PasteMacro* pasteMacroByName(PasteStrings::Id name);
    const std::unordered_map<PasteStrings::Id, PasteSite>* pastesAt(size_t fi, int32_t start);
    /// The range to give a reference whose naming token was pasted together:
    /// the part of the name its caller typed.  False when the name has a
    /// spelling of its own, or no caller chose any part of it.
    bool pastedNameSite(CXCursor cursor, const std::string& sym_name, RefPlace& out);

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

    /// The calls the walk is currently inside, innermost last: the function
    /// each one calls, and whether its callee has been seen yet.
    ///
    /// A call names its function once, and the AST says so twice -- the
    /// CallExpr refers to it, and so does the DeclRefExpr that is its callee.
    /// The call's reference is the one to keep (its full span is the whole
    /// `helper(1)`), so the first DeclRefExpr to that function inside the call
    /// is the callee and is skipped; any later one is a real second mention,
    /// as in `apply(apply)`.
    ///
    /// By IDENTITY, not by the two happening to get the same range: for an
    /// overloaded C++ operator they do not -- the call covers `a != b` and the
    /// callee is the `!=` token -- and one use came out as two references.
    struct OpenCall {
        CXCursor callee;
        bool callee_seen;
    };
    std::vector<OpenCall> open_calls_;

    /// Index of the innermost open scope in `fi`, or -1.  Scopes from other
    /// files are skipped: one TU walk descends into headers, and each file has
    /// its own tree.
    int32_t stackParent(size_t fi) const;

    /// The innermost scope of file `fi` on the stack whose text holds
    /// [start, end), or -1.  This is where a reference at that range was READ.
    ///
    /// The extent test is what separates a token read where it is written
    /// from one a `#define` in the same file contributed: the body token is
    /// read inside whichever function expanded the macro and written in none
    /// of them, and a reference sits where its name was typed.
    int32_t scopeAround(size_t fi, int32_t start, int32_t end) const;

    /// Every macro invocation this TU saw in a file, keyed by where it starts:
    /// the end of the whole `NAME(args)` text, and the invocation's cursor.
    /// Only the OUTERMOST are kept: an invocation written in another's
    /// arguments lies inside it and can cover nothing the outer one does not.
    ///
    /// Filled by `macroVisitor`, so it is complete before the AST walk asks.
    /// The cursor is for `pastesAt`, which needs the invocation's tokens; it
    /// is the outermost invocation there too, because that is the only one a
    /// pasted name can be traced back to.
    struct Invocation {
        int32_t end;
        CXCursor cursor;
    };
    std::unordered_map<size_t, std::map<int32_t, Invocation>> invocation_spans_;
    void noteInvocation(size_t fi, int32_t start, int32_t end, CXCursor cursor);

    /// True when [start, end) in file `fi` is nothing but a macro invocation,
    /// which is what "the preprocessor generated this declaration" means.
    ///
    /// EXTENT, NOT NAME.  This used to ask where the declaration's NAME token
    /// was spelled, and that answers a different question.  A name reaches a
    /// declaration through a macro in two ordinary ways that generate nothing:
    ///
    ///     PROFILE_FUNC(int, wrapped, (a), int a) { return helper(a); }
    ///
    ///     #define alias renamed
    ///     int alias(int a) { return helper(a); }
    ///
    /// Both bodies are text the author typed, and both definitions were folded
    /// anyway.  A folded instance took no reference then, so their
    /// calls landed on the file: ucx's `ucp_tag_send_nbx` had 71 references
    /// written inside it and none placed in it.
    ///
    /// The extent tells the two apart with no heuristic.  `DEFINE_SHOW(event)`
    /// generates `event_show` and its extent IS the invocation; `wrapped` and
    /// `renamed` extend past theirs, to the closing brace.  Nothing weaker
    /// works: libclang maps a macro end to the end of the expansion, so "the
    /// extent ends inside a macro" is never observable.
    bool isWhollyAnInvocation(size_t fi, int32_t start, int32_t end) const;

    /// The other way a range is nothing but a macro invocation: it STARTS on
    /// a recorded invocation and the rest of it is one parenthesised list.
    ///
    ///     #define dev_vdbg dev_dbg
    ///     dev_vdbg(dev, "intr\n");
    ///
    /// invokes `dev_dbg`, but clang's preprocessing record keeps only
    /// `dev_vdbg`, eight characters wide: an expansion whose NAME came out of
    /// another macro is not recorded.  So what the line generates runs past
    /// the only invocation on record, and read as a declaration with text of
    /// its own.
    ///
    /// The list is read with clang's own tokens, and only once the bytes say
    /// it could be one: the range must continue with `(` and end with `)`.
    /// That excludes `PREFIX int f(void) { ... }` for free, which also starts
    /// on a macro and runs past it.
    bool continuesAsInvocation(CXCursor cursor, size_t fi, int32_t start, int32_t end) const;

    /// The extent of a declaration: where its text starts and ends in the
    /// file it was written in.  False when it has none there.
    ///
    /// libclang's extent, with ONE correction.  An extent whose last token
    /// came out of a macro's BODY already ends where the invocation ends.
    /// One whose last token is a macro ARGUMENT does not: libclang leaves
    /// that end inside the argument's expansion, and the expansion location
    /// of anything in an expansion is where the invocation STARTS.  So
    ///
    ///     #define DEFINE_COUNTER(name) int name;
    ///     DEFINE_COUNTER(hits)
    ///
    /// declared `hits` on `[start, start)` -- zero bytes, which was then
    /// dropped as no extent at all, leaving a symbol with no instance -- and
    /// `int plain = ID(7);` ended at `ID`, its initialiser outside it.  The
    /// declaration ends where that invocation ends.
    bool declExtent(CXCursor cursor, CXFile& out_file, unsigned& out_start, unsigned& out_end);

    /// Every `#define` this TU saw in a file, start -> end.  They cannot
    /// nest, so "is this offset inside one" is a predecessor lookup.  Filled
    /// by `macroVisitor`, so complete before the AST walk asks.
    std::unordered_map<size_t, std::map<int32_t, int32_t>> define_spans_;
    bool insideDefine(size_t fi, int32_t offset) const;
    /// Was the token that names this declaration typed in normal code?  Not
    /// when a `#define` body wrote it, and not when it was pasted together,
    /// which leaves no token at all.
    bool nameTypedInNormalCode(CXCursor cursor);
    /// Record on `d` what its declaration says about folding.
    void markOrigin(CXCursor cursor, size_t fi, InstanceData& d);
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
    /// A MACRO REFERS TO WHAT ITS INVOCATIONS DECLARED.
    ///
    ///     #define DEFINE_LOCK(name) int name = 0
    ///     static DEFINE_LOCK(lock);
    ///
    /// Nothing somebody typed leads to `lock` except the invocation, and the
    /// invocation is the macro's doing.  For every declaration an invocation
    /// names (`InstanceData::named_at`), the invoked macro's `#define` gets a
    /// reference to it, on the macro's name -- the only text a macro has.
    ///
    /// DIRECTLY: a declaration read inside another one the same invocation
    /// named is reached through that one, like anything else something
    /// contains.  `KHASH_INIT` leads to the struct and the functions, not to
    /// each field of the struct.
    ///
    /// INTRODUCED: not for a redeclaration; see `InstanceData::introduced`.
    ///
    /// Only the OUTERMOST invocation is known (`invocation_spans_`), so that
    /// is the macro credited: `READ_ONCE` for the assertion `BUILD_BUG_ON`
    /// declares inside it.
    ///
    /// Run after the walk, when each instance's `stack_parent` is this unit's
    /// final answer and the invocations' cursors still live.
    void linkDeclared();
    /// See `InstanceData::named_at`.
    int32_t namedAt(CXCursor cursor, size_t fi, const InstanceData& d);
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
    bool refRange(CXCursor cursor, const std::string& sym_name, RefPlace& out);
    /// `roles`: what this cursor's token does to the symbol (`REFROLE_*`),
    /// zero when the walk cannot say.
    void addRef(CXCursor cursor, int64_t sym_id, const std::string& sym_name,
                uint32_t roles = 0);
    /// Does the text at a reference's range actually spell the symbol it
    /// references?  A range that does not is describing a position it does not
    /// occupy, which is worse than the reference being absent.
    bool refTextNamesSymbol(size_t file_idx, const std::string& sym_name,
                            int32_t start, int32_t end, bool warn) const;
    /// `at.unspelled` is for the one kind of range that is right without
    /// spelling its symbol: the macro that builds a pasted name.
    void insertRef(const RefPlace& at, int64_t sym_id, const std::string& sym_name,
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
    /// One pass over the unit's top-level declarations, before anything else
    /// reads it: the typedef'd type names above, and `decl_extents_`.
    void prescanDeclarations(CXTranslationUnit tu);

    // --- `#include` inside a declaration ----------------------------------
    //
    //   WHAT AN `#include` INSIDE A DECLARATION BRINGS IN IS READ THERE.
    //
    //     long x64_sys_call(const struct pt_regs *regs, unsigned int nr)
    //     {
    //         switch (nr) {
    //         #include <asm/syscalls_64.h>
    //         default: return __x64_sys_ni_syscall(regs);
    //         }
    //     }
    //
    // The header is a list of `__SYSCALL(0, sys_read)` lines, and the function
    // calls every one of them.  A reference sits where its text is, and that
    // text is in the header: containment never crosses files, so the function
    // contained none of it and appeared to call `__x64_sys_ni_syscall` alone,
    // while the header "used" every system call.
    //
    // The author wrote those calls into the function with one line, the
    // `#include`.  So each reference read through it is recorded a second
    // time on that line, where the declaration around it contains it.  The
    // header keeps its own: the text is there too, and at file scope that is
    // the whole story.
    //
    // The second record is `synthetic` -- the directive does not spell the
    // name, and nobody typed the name there.

    struct Inclusion {
        int32_t end;
        size_t included;
    };
    /// File -> where an `#include` starts -> where it ends, and the file it
    /// resolved to.
    std::unordered_map<size_t, std::map<int32_t, Inclusion>> inclusions_;
    /// File -> the files it includes, directly.
    std::unordered_map<size_t, std::unordered_set<size_t>> includes_of_;
    std::unordered_map<uint64_t, bool> include_reaches_;
    /// Does including `from` bring in `to`, itself or through its own
    /// `#include`s?
    bool includeReaches(size_t from, size_t to);
    /// For a reference the walk met at `at`: the `#include`, inside a
    /// declaration the walk is in, that brought that text in.
    bool includedInside(const RefPlace& at, RefPlace& out) const;

    /// File -> start -> end of each top-level declaration that spans lines,
    /// outermost only.  A macro use is recorded before the walk, when no
    /// declaration is open, and this is what says its `#include` is inside
    /// one.
    std::unordered_map<size_t, std::map<int32_t, int32_t>> decl_extents_;
    void noteDeclExtent(size_t fi, int32_t start, int32_t end);

    /// The `#include`s the preprocessing record is inside, outermost first.
    /// Its entries come in source order, so a directive is followed by what
    /// its file contains and then by the rest of the file it is written in:
    /// an entry from any other file than the innermost one closes it.
    struct IncludeFrame {
        size_t fi;
        int32_t start, end;
        size_t included;
        bool inside_declaration;
    };
    std::vector<IncludeFrame> include_stack_;
    /// The next entry of the record is written in `fi`.
    void enterFile(size_t fi);
};
