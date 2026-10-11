#include <algorithm>
#include "stage1.h"
#include "clang_raii.h"
#include "clang_utils.h"
#include "file_utils.h"
#include "symbol_table.h"
#include "symbol_types.h"

#include <cstdio>
#include <fstream>

Stage1::Stage1(SymbolTable& symbols, std::atomic<int64_t>& next_object_id)
    : symbols_(symbols), next_object_id_(next_object_id) {}

size_t Stage1::getOrCreateFile(CXFile file) {
    std::string path = getCanonicalPath(file);

    auto it = file_index_.find(path);
    if (it != file_index_.end()) {
        return it->second;
    }

    FileData fd;
    fd.object_local_id = next_object_id_.fetch_add(1);
    fd.filesystem_path = path;
    fd.module_path = path;
    fd.filetype = computeFiletype(path);

    std::ifstream in(path, std::ios::binary);
    if (in) {
        fd.content = std::vector<uint8_t>(
            (std::istreambuf_iterator<char>(in)),
            std::istreambuf_iterator<char>());
    }

    // Create FILE symbol
    auto [file_sym_id, _] = symbols_.getOrCreate(
        fd.module_path, SCOPE_GLOBAL, SYMTYPE_FILE);
    fd.instances.push_back({file_sym_id, 0, static_cast<int32_t>(fd.content.size()), filetypeToInstType(fd.filetype)});

    size_t idx = result_.files.size();
    result_.files.push_back(std::move(fd));
    file_index_[path] = idx;
    return idx;
}

CXChildVisitResult Stage1::macroVisitor(CXCursor cursor, CXCursor parent, CXClientData data) {
    auto* self = static_cast<Stage1*>(data);
    CXCursorKind kind = clang_getCursorKind(cursor);

    if (kind == CXCursor_MacroDefinition) {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) return CXChildVisit_Continue;

        // Get the file where the macro is defined
        CXSourceLocation loc = clang_getCursorLocation(cursor);
        CXFile file = nullptr;
        clang_getSpellingLocation(loc, &file, nullptr, nullptr, nullptr);
        if (!file) return CXChildVisit_Continue;

        // Create MACRO symbol
        auto [sym_id, _] = self->symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_MACRO);

        self->tu_macros_[sname] = sym_id;

        // Create instance at the #define body range
        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off)) {
            size_t fi = self->getOrCreateFile(range_file);
            self->addInstance(
                fi, {sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), INSTTYPE_DEFINITION},
                false);
            // The body is read for the macros it names once every macro in
            // this TU is known; see `linkMacroBodies`.
            self->macro_bodies_.push_back({range_file, fi, start_off, end_off, sname});
        }
    } else if (kind == CXCursor_MacroExpansion) {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) return CXChildVisit_Continue;

        // Look up the MACRO symbol (should have been created by MacroDefinition)
        auto [sym_id, _] = self->symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_MACRO);

        // Create an instance at the expansion site spanning the WHOLE
        // invocation -- `some_macro(bar)`, not just `some_macro`.
        //
        // This used to be clamped to the name, so that macro arguments like
        // `bar` fell OUTSIDE the macro's range and became children of the
        // enclosing scope.  That reasoning was range-based parentage, and
        // parentage no longer comes from ranges: refs are placed by scope (see
        // `insertRef`) and instances by the DFS interval.  So the clamp only
        // threw away the one span that records what the author actually wrote.
        //
        // What keeps the argument text the CALLER's is a rule where ownership
        // is decided, not a narrowed span: a folded instance owns no text.  A
        // macro expansion owns the symbols it generates, through the interval,
        // and no bytes at all -- every byte at the call site was typed by the
        // caller.  The name span below is what identifies the macro itself.
        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off)) {
            const unsigned name_end = start_off + sname.size();
            size_t fi = self->getOrCreateFile(range_file);
            // Opens a scope so the expansion-site refs addRef emits land inside
            // its interval; closed immediately below, since a MacroExpansion
            // cursor has no AST children of its own.
            InstanceData d{sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off),
                           INSTTYPE_EXPANSION};
            d.folded = true; // an expansion is the fold boundary itself
            d.name_start = static_cast<int32_t>(start_off);
            d.name_end = static_cast<int32_t>(name_end);
            d.name_end = static_cast<int32_t>(name_end);
            self->addInstance(fi, d, true);
            self->closeScopesTo(self->dfs_stack_.size() - 1);
        }
    }

    return CXChildVisit_Continue;
}

void Stage1::collectMacros(CXTranslationUnit tu) {
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, macroVisitor, this);
    linkMacroBodies(tu);
}

void Stage1::linkMacroBodies(CXTranslationUnit tu) {
    for (const MacroBody& b : macro_bodies_) {
        CXSourceLocation begin = clang_getLocationForOffset(tu, b.file, b.start);
        CXSourceLocation end = clang_getLocationForOffset(tu, b.file, b.end);
        CXSourceRange range = clang_getRange(begin, end);

        CXToken* toks = nullptr;
        unsigned n = 0;
        clang_tokenize(tu, range, &toks, &n);
        if (!toks) continue;

        // Parameter names are NOT macros, whatever else in the project shares
        // their spelling -- `#define WRAP(x) FOO(x)` must not gain an edge to
        // some unrelated `#define x`.  A macro is function-like exactly when
        // the `(` abuts the name with no space, so compare offsets rather than
        // guessing from the text.
        std::unordered_set<std::string> params;
        unsigned name_end_off = b.start + static_cast<unsigned>(b.name.size());
        bool in_params = false;
        for (unsigned i = 0; i < n; ++i) {
            CXSourceLocation tl = clang_getTokenLocation(tu, toks[i]);
            unsigned off = 0;
            clang_getSpellingLocation(tl, nullptr, nullptr, nullptr, &off);
            ClangString spell(clang_getTokenSpelling(tu, toks[i]));
            std::string tok = spell.to_string();
            if (i == 1 && tok == "(" && off == name_end_off) {
                in_params = true;
                continue;
            }
            if (in_params) {
                if (tok == ")") break;
                if (clang_getTokenKind(toks[i]) == CXToken_Identifier) params.insert(tok);
                continue;
            }
            break; // object-like macro: no parameter list to skip
        }

        for (unsigned i = 0; i < n; ++i) {
            if (clang_getTokenKind(toks[i]) != CXToken_Identifier) continue;
            ClangString spell(clang_getTokenSpelling(tu, toks[i]));
            std::string tok = spell.to_string();
            if (tok == b.name) continue;        // the macro being defined
            if (params.count(tok)) continue;    // a parameter, not a macro
            auto it = tu_macros_.find(tok);
            if (it == tu_macros_.end()) continue;

            CXSourceLocation tl = clang_getTokenLocation(tu, toks[i]);
            unsigned off = 0;
            clang_getSpellingLocation(tl, nullptr, nullptr, nullptr, &off);
            // No owner: `numberTree` resolves it from the text, and the
            // narrowest unfolded instance containing a token inside a
            // `#define` body is that macro's own definition.
            insertRef(b.file_idx, it->second, tok, static_cast<int32_t>(off),
                      static_cast<int32_t>(off + tok.size()));
        }
        clang_disposeTokens(tu, toks, n);
    }
}

void Stage1::collectTypedefTypes(CXTranslationUnit tu) {
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, [](CXCursor cursor, CXCursor, CXClientData data) {
        if (clang_getCursorKind(cursor) != CXCursor_TypedefDecl)
            return CXChildVisit_Continue;
        CXType underlying = clang_getTypedefDeclUnderlyingType(cursor);
        CXCursor type_decl = clang_getTypeDeclaration(underlying);
        CXCursorKind tk = clang_getCursorKind(type_decl);
        if (tk != CXCursor_EnumDecl && tk != CXCursor_StructDecl && tk != CXCursor_UnionDecl)
            return CXChildVisit_Continue;
        ClangString typedef_name(clang_getCursorSpelling(cursor));
        ClangString type_name(clang_getCursorSpelling(type_decl));
        std::string tn = typedef_name.to_string();
        if (!tn.empty() && tn == type_name.to_string())
            static_cast<Stage1*>(data)->typedef_type_names_.insert(tn);
        return CXChildVisit_Continue;
    }, this);
}

// Index a FieldDecl child cursor as a FIELD symbol with a compound name
// "parent.child".  Used for struct and union field visitors.
void Stage1::indexChildField(CXCursor child) {
    std::string compound_name = resolveFieldCompoundName(child);
    if (compound_name.empty()) return;

    auto [sym_id, _] = symbols_.getOrCreate(compound_name, SCOPE_GLOBAL, SYMTYPE_FIELD);

    CXFile file;
    unsigned start, end;
    if (getExpansionRange(child, file, start, end)) {
        size_t fi = getOrCreateFile(file);
        InstanceData d{sym_id, static_cast<int32_t>(start), static_cast<int32_t>(end), INSTTYPE_DEFINITION};
        fillSpans(child, file, d);
        d.folded = isMacroGenerated(child);
        addInstance(fi, d, true);
        closeScopesTo(dfs_stack_.size() - 1);
    }
}

// Create a ref at the spelling location.  When the cursor is inside a macro
// expansion and the spelling location is in a different file than the
// expansion location, also create a ref at the expansion site.  This ensures
// that symbols referenced in a macro body (e.g. a TypeRef to the variable's
// type) appear within the expanded declaration's instance range, enabling
// containment queries like `data @i {...} / #i {type}`.
void Stage1::fillSpans(CXCursor cursor, CXFile file, InstanceData& d) {
    auto record = [&](CXSourceRange range, int32_t& out_start, int32_t& out_end) {
        CXFile f = nullptr;
        unsigned s = 0, e = 0;
        if (!getExpansionRange(range, f, s, e, /*clamp_zero_width=*/true)) return;
        if (!f || !clang_File_isEqual(f, file)) return;
        // A span outside the instance's own extent is not a finer detail of it
        // -- that happens for ##-pasted names, which clang places in a virtual
        // <scratch space> buffer rather than in any file.
        if (static_cast<int32_t>(s) < d.start_offset || static_cast<int32_t>(e) > d.end_offset)
            return;
        out_start = static_cast<int32_t>(s);
        out_end = static_cast<int32_t>(e);
    };

    record(clang_Cursor_getSpellingNameRange(cursor, 0, 0), d.name_start, d.name_end);

    // The body, where there is one.  For a function that is the CompoundStmt
    // child; everything before it is the signature, which is what makes a
    // multi-line C declaration render correctly.
    CXCursor body = clang_getNullCursor();
    clang_visitChildren(
        cursor,
        [](CXCursor child, CXCursor, CXClientData data) {
            if (clang_getCursorKind(child) == CXCursor_CompoundStmt) {
                *static_cast<CXCursor*>(data) = child;
                return CXChildVisit_Break;
            }
            return CXChildVisit_Continue;
        },
        &body);

    if (!clang_Cursor_isNull(body)) {
        record(clang_getCursorExtent(body), d.content_start, d.content_end);
        if (d.content_start > d.start_offset) {
            d.sig_start = d.start_offset;
            d.sig_end = d.content_start;
        }
    }
}

int32_t Stage1::stackParent(size_t fi) const {
    for (auto it = dfs_stack_.rbegin(); it != dfs_stack_.rend(); ++it)
        if (it->file_idx == fi) return static_cast<int32_t>(it->instance_idx);
    return -1;
}

bool Stage1::isMacroGenerated(CXCursor cursor) {
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    CXFile spell_file = nullptr, exp_file = nullptr;
    unsigned spell_off = 0, exp_off = 0;
    clang_getSpellingLocation(loc, &spell_file, nullptr, nullptr, &spell_off);
    clang_getExpansionLocation(loc, &exp_file, nullptr, nullptr, &exp_off);
    if (!exp_file) return false;
    // No spelling file at all is the STRONGEST evidence, not the weakest: a
    // ##-pasted identifier exists in no file, clang materialises it in a
    // virtual <scratch space> buffer.  That is exactly `event_show` in
    // PMU_FORMAT_ATTR(event, ...).
    if (!spell_file) return true;
    // Otherwise the two differ exactly when the code was written somewhere
    // other than where it landed -- i.e. it came out of a macro.
    return spell_off != exp_off || !clang_File_isEqual(spell_file, exp_file);
}

void Stage1::addInstance(size_t fi, InstanceData d, bool opens_scope) {
    // Record structure, number later.  `numberTree` assigns dfs_enter,
    // dfs_exit, nesting_level and fold_depth together, so none of them can be
    // set while another is forgotten -- which is exactly how non-scope
    // instances ended up with dfs_exit == dfs_enter, able to contain nothing.
    d.stack_parent = stackParent(fi);

    size_t idx = result_.files[fi].instances.size();
    result_.files[fi].instances.push_back(d);
    // `opens_scope` now means only "later cursors nest inside this one", which
    // is all it ever should have meant.
    if (opens_scope) dfs_stack_.push_back({fi, idx});
}

void Stage1::closeScopesTo(size_t mark) {
    // Nothing to stamp any more: the exit number comes from the walk over the
    // derived tree, not from where the cursor happened to close.
    while (dfs_stack_.size() > mark) dfs_stack_.pop_back();
}

int32_t ownerOfText(const FileData& f, int32_t start, int32_t end) {
    int32_t best = -1;
    int64_t best_width = 0;
    for (size_t i = 0; i < f.instances.size(); ++i) {
        const InstanceData& inst = f.instances[i];
        if (inst.folded) continue; // a macro expansion owns no text
        if (inst.start_offset > start || inst.end_offset < end) continue;
        int64_t width = static_cast<int64_t>(inst.end_offset) - inst.start_offset;
        // Narrowest wins; ties broken by the earlier instance, so the choice
        // does not depend on the order cursors happened to arrive in.
        if (best < 0 || width < best_width) {
            best = static_cast<int32_t>(i);
            best_width = width;
        }
    }
    return best;
}

void Stage1::numberTree(FileData& f) {
    const size_t n = f.instances.size();
    if (n == 0) return;

    // --- PARENTAGE -----------------------------------------------------
    //
    //   parent(x) = the innermost instance containing x, where "containing"
    //   means (a) its range STRICTLY contains x's, or (b) its range EQUALS x's
    //   and it is x's stack ancestor.  Innermost = narrowest range, then
    //   deepest on the stack.
    //
    // (a) is what ranges are good at, and it is what the walk could not do: a
    // macro expansion comes from `collectMacros`, a pass that runs before the
    // AST walk with an empty stack, so it was numbered as a sibling of the
    // function it textually sits in and `func { macro }` could not work.  The
    // same blindness made every `opens_scope = false` instance a tree leaf
    // whose range none the less contained other instances.
    //
    // (b) is the only thing ranges cannot do, and the stack is exactly the
    // information that can.  Everything `PMU_FORMAT_ATTR(...)` generates shares
    // one extent; the stack knows the assert is inside `event_show`'s body
    // while `format_attr_event` is beside it.  An equal range is narrower than
    // any strict container, so (b) outranks (a) -- but ONLY for a stack
    // ancestor, which is what keeps `event_show` a child of the enclosing
    // function at fold depth 0 instead of sliding under the macro and
    // disappearing from `mod { func }`.
    std::vector<int32_t> parent(n, -1);
    for (size_t i = 0; i < n; ++i) {
        const InstanceData& x = f.instances[i];
        int32_t s = x.stack_parent;
        while (s >= 0) {
            const InstanceData& a = f.instances[static_cast<size_t>(s)];
            if (a.start_offset == x.start_offset && a.end_offset == x.end_offset) break;
            s = a.stack_parent;
        }
        if (s >= 0) {
            parent[i] = s;
            continue;
        }
        int64_t best_w = 0;
        int32_t best = -1;
        for (size_t j = 0; j < n; ++j) {
            if (j == i) continue;
            const InstanceData& c = f.instances[j];
            // AN EXPANSION IS NEVER A PARENT BY RANGE, because a macro
            // expansion owns no text: every byte at the call site of `foo(a)`
            // was typed by the caller.  Without this, a macro passed as an
            // ARGUMENT nests under the outer invocation it was written inside
            // -- `ucs_unlikely(kh_end(x))` made `kh_end`'s expansion a child
            // of `ucs_unlikely`'s, so `macro("ucs_unlikely") { }` reported 31
            // unrelated macros as its contents.  They are the caller's, and
            // they become siblings of the outer expansion under it.
            //
            // Measured on ucx: across 41,487 expansions every single child
            // was another folded expansion -- 2,594 of them, and not one
            // DEFINITION -- so this relation carried arguments and nothing
            // else.
            //
            // NOT "a folded instance is never a parent", which would be the
            // tidier sentence and would break the fold axis where it earns its
            // keep: a macro-GENERATED symbol is a folded DEFINITION that does
            // own its text, and `__compiletime_assert_1121` sits inside the
            // generated `event_show` at fold_depth 1.  That is what keeps it
            // out of `mod { func }` while `event_show` itself stays in.
            if (c.instance_type == INSTTYPE_EXPANSION) continue;
            if (c.start_offset > x.start_offset || c.end_offset < x.end_offset) continue;
            if (c.start_offset == x.start_offset && c.end_offset == x.end_offset)
                continue; // equal, not strict -- only (b) may use it
            int64_t w = static_cast<int64_t>(c.end_offset) - c.start_offset;
            if (best < 0 || w < best_w) {
                best = static_cast<int32_t>(j);
                best_w = w;
            }
        }
        parent[i] = best;
    }

    // --- THE INVOCATION EDGE -------------------------------------------
    // `handler() { DEV_PRINT(x); }` must produce a reference `handler ->
    // DEV_PRINT`, and before this nothing referenced a macro at all: the
    // preprocessor pass creates an INSTANCE for an expansion and never a
    // REFERENCE.  Measured on ucx, none of 231,091 references targeted any of
    // 14,946 macro symbols, so the first hop of `handler -> DEV_PRINT ->
    // printk` simply did not exist and containment (`unnest`, and with it the
    // whole fold axis on references) was the only way across.
    //
    // Emitted HERE, not in `macroVisitor`, because that pass runs before the
    // AST walk with an empty scope stack (`collectMacros`), so there is no
    // open scope to attribute it to at the time the expansion is seen.
    //
    // The owner is left UNSET so the reference is placed by `ownerOfText`
    // below, like every other one.  It must NOT come from `parent[]`: tree
    // parentage of an expansion is range-derived, and a macro passed as an
    // ARGUMENT expands inside the text of the outer invocation, so
    // `DEV_PRINT(LEVEL)` makes LEVEL's expansion a tree child of DEV_PRINT's
    // and produced an edge `DEV_PRINT -> LEVEL`.  That is the argument edge
    // this whole model exists to remove -- `LEVEL` is text the CALLER wrote.
    // Excluding folded instances, which is what `ownerOfText` does, gives
    // `handler -> LEVEL` instead.
    //
    // The chain between two macros does not come from here at all: libclang
    // never reports an expansion written inside a `#define` body, so
    // `FATAL -> ERR` is emitted by `linkMacroBodies` from the body text, and
    // lands inside FATAL's own unfolded DEFINITION instance.
    for (size_t i = 0; i < n; ++i) {
        const InstanceData& x = f.instances[i];
        if (x.instance_type != INSTTYPE_EXPANSION) continue;
        if (x.name_start < 0) continue;
        RefData r{x.symbol_local_id, x.name_start, x.name_end};
        r.full_start = x.start_offset;
        r.full_end = x.end_offset;
        f.refs.push_back(r);
    }

    // --- NUMBERING -----------------------------------------------------
    // One preorder walk, one stride per event, every field assigned together.
    std::vector<std::vector<int32_t>> kids(n);
    std::vector<int32_t> roots;
    for (size_t i = 0; i < n; ++i) {
        if (parent[i] < 0)
            roots.push_back(static_cast<int32_t>(i));
        else
            kids[static_cast<size_t>(parent[i])].push_back(static_cast<int32_t>(i));
    }

    int64_t counter = 0;
    size_t numbered = 0;
    std::vector<std::pair<int32_t, size_t>> st; // (node, next child)
    for (int32_t r : roots) {
        counter += kDfsStride;
        f.instances[static_cast<size_t>(r)].dfs_enter = counter;
        f.instances[static_cast<size_t>(r)].nesting_level = 0;
        f.instances[static_cast<size_t>(r)].fold_depth = 0;
        ++numbered;
        st.push_back({r, 0});
        while (!st.empty()) {
            int32_t node = st.back().first;
            size_t next = st.back().second;
            if (next < kids[static_cast<size_t>(node)].size()) {
                st.back().second = next + 1;
                int32_t c = kids[static_cast<size_t>(node)][next];
                const InstanceData& p = f.instances[static_cast<size_t>(node)];
                counter += kDfsStride;
                InstanceData& ci = f.instances[static_cast<size_t>(c)];
                ci.dfs_enter = counter;
                ci.nesting_level = p.nesting_level + 1;
                ci.fold_depth = p.fold_depth + (p.folded ? 1 : 0);
                ++numbered;
                st.push_back({c, 0});
            } else {
                counter += kDfsStride;
                f.instances[static_cast<size_t>(node)].dfs_exit = counter;
                st.pop_back();
            }
        }
    }
    // Parentage is acyclic by construction -- (a) strictly narrows the range,
    // (b) follows the scope stack -- so every instance is reached exactly once.
    // Assert it rather than trust it: a cycle would silently leave instances at
    // [0,0), which is the invisible-row failure all over again.
    if (numbered != n) {
        fprintf(stderr,
                "Warning: numbered %zu of %zu instances in %s; containment tree is not acyclic\n",
                numbered, n, f.module_path.c_str());
    }

    // --- REFS ----------------------------------------------------------
    // A ref is a point one step inside its owner, on every axis.  Stamping it
    // here from `owner_idx` is what makes "a ref lies inside its owner" true by
    // construction, instead of two independently computed numbers that agreed
    // only while a reference sat inside exactly one scope.
    for (RefData& r : f.refs) {
        // EVERY reference is placed by TEXT, by the one rule in `ownerOfText`:
        // the narrowest UNFOLDED instance containing it.  `insertRef` no longer
        // guesses an owner from the AST scope -- it could not see a macro body,
        // and the case that needed a scope (an argument token vs the macro's
        // own expansion-site use of it) no longer exists.
        r.owner_idx = ownerOfText(f, r.from_offset_start, r.from_offset_end);
        if (r.owner_idx < 0 || static_cast<size_t>(r.owner_idx) >= n) continue;
        const InstanceData& o = f.instances[static_cast<size_t>(r.owner_idx)];
        r.dfs_enter = o.dfs_enter + 1;
        r.nesting_level = o.nesting_level + 1;
        r.fold_depth = o.fold_depth + (o.folded ? 1 : 0);
    }
}

bool Stage1::refTextNamesSymbol(size_t file_idx, const std::string& sym_name,
                                int32_t start, int32_t end, bool warn) const {
    const std::vector<uint8_t>& content = result_.files[file_idx].content;
    if (content.empty() || start < 0 || end <= start ||
        static_cast<size_t>(end) > content.size())
        return true; // nothing to check against; not a reason to drop a ref

    // The name comes from the caller rather than from a `symbols_` lookup: it
    // is already in hand at every call site, and looking it up would take the
    // symbol table's mutex once per reference on the hot path.
    //
    // A compound name is spelled by its leaf: `ucs_list_link.next` is written
    // `next`.  But not every name is compound-or-identifier: clang names an
    // anonymous struct by its source location, so
    // `(anonymous struct at /path/infect.h:122:2)` splits on the `.` in `.h`
    // and leaves `h:122:2)`, which matches nothing and dropped 234 good
    // references on criu.
    //
    // So only check when the leaf is a plain identifier.  The guard exists to
    // catch a range that spells no text; where it cannot tell, it must stay
    // out of the way rather than guess.
    std::string name = sym_name;
    auto cut = name.find_last_of("./");
    if (cut != std::string::npos) name = name.substr(cut + 1);
    if (name.empty()) return true;
    auto ident = [](unsigned char c) {
        return c == '_' || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
               (c >= '0' && c <= '9');
    };
    for (unsigned char c : name)
        if (!ident(c)) return true;

    std::string text(reinterpret_cast<const char*>(content.data()) + start,
                     static_cast<size_t>(end - start));
    // Either may contain the other.  A symbol's name is sometimes a DECORATED
    // form of what the source writes: clang resolves `__sync_and_and_fetch` to
    // the sized builtin `__sync_and_and_fetch_1`, so the text is a prefix of
    // the name and "text contains name" rejected 1048 good references on ucx.
    // What this is for is a range that spells nothing related at all --
    // `((_type*)0)-` claiming `hugetlb_mode`.
    if (text.find(name) != std::string::npos || name.find(text) != std::string::npos)
        return true;

    // Silent when a caller is merely CHOOSING a range -- only the final
    // decision in `insertRef` is worth a warning.
    if (warn)
        fprintf(stderr,
                "Warning: dropping ref to '%s' at %s[%d,%d): that text reads '%s'\n",
                name.c_str(), result_.files[file_idx].module_path.c_str(), start, end,
                text.c_str());
    return false;
}

void Stage1::insertRef(size_t file_idx, int64_t sym_id, const std::string& sym_name,
                       int32_t start, int32_t end, int32_t full_start, int32_t full_end) {
    if (!file_ref_sets_[file_idx].insert({sym_id, start, end}).second)
        return;
    // The owner is left UNSET: `numberTree` resolves it by text, through
    // `ownerOfText`, once every instance in the file is known.  It used to come
    // from `stackParent(file_idx)` here, and that was wrong for exactly the
    // references a macro body contributes -- the scope stack is consulted per
    // FILE, and when a function in one file expands a macro defined in
    // another, nothing from the defining file is on it, so a body reference
    // landed on that file's root.
    // A REFERENCE'S RANGE MUST SPELL ITS SYMBOL.  The range is the naming
    // token's own spelling now, so the text there is that token -- and when it
    // is not, the reference is describing a position it does not occupy and is
    // worse than absent: `ucs_offsetof`'s 427 field references read
    // `((_type*)0)-` in compiler_def.h and sent "who uses this field" to a
    // subsystem that never touched it.
    //
    // CONTAINS, not equals: a symbol's name may be compound (`struct.field`)
    // while the token spells only the leaf, and a few constructs legitimately
    // carry a wider range.  Containment is enough to catch a chimera.
    //
    // Checked here rather than in `addRef` so it also covers the references
    // `linkMacroBodies` inserts directly.  Stage2's synthetic references do not
    // pass through `insertRef` at all -- they are placed by `placeRefByText`
    // after being merged in `indexer.cpp` -- so the ones it deliberately parks
    // at a field's declaration site stay exempt by construction.
    if (!refTextNamesSymbol(file_idx, sym_name, start, end, /*warn=*/true)) return;

    RefData r{sym_id, start, end};
    r.full_start = full_start;
    r.full_end = full_end;
    result_.files[file_idx].refs.push_back(r);
}

bool Stage1::refNameRange(CXCursor cursor, const std::string& sym_name, size_t& out_fi,
                          int32_t& out_start, int32_t& out_end) {
    // A REFERENCE'S RANGE IS THE SPELLING OF THE TOKEN THAT NAMES IT.
    //
    // Not the cursor's extent, which is the whole construct: for
    // `((_t*)0)->_m` the extent STARTS at the base expression, and that is in
    // the macro BODY whenever `_m` came from an argument.  The old code took
    // the extent's endpoints and, when they landed in different files, pasted
    // the body's start onto the symbol name's length -- a range that spells no
    // text in either file.  `ucs_offsetof(uct_mm_md_config_t, hugetlb_mode)`
    // recorded `uct_mm_md_config.hugetlb_mode` at compiler_def.h [3757,3769),
    // which reads `((_type*)0)-`.
    //
    // `clang_Cursor_getSpellingNameRange` gives the name instead, and its
    // START is reliably the identifier's first byte -- at the call site when
    // the author wrote it there, in the `#define` body when the macro did.
    // That one distinction places every case with no discriminator.
    CXSourceRange nr = clang_Cursor_getSpellingNameRange(cursor, 0, 0);
    if (clang_Range_isNull(nr)) return false;

    CXFile start_file = nullptr;
    unsigned start_off = 0;
    clang_getSpellingLocation(clang_getRangeStart(nr), &start_file, nullptr, nullptr,
                              &start_off);
    if (!start_file) return false;

    out_fi = getOrCreateFile(start_file);
    out_start = static_cast<int32_t>(start_off);

    // THE END.  Prefer clang's own name range, which is a proper half-open
    // span -- `mode` at a call site comes back [103,107) and `std::min` covers
    // the whole qualified name, both exact.  It has two failure modes, both
    // measured:
    //
    //   * it STRADDLES.  For a token spelled in a macro body the start
    //     resolves into the body while the end is a plain location at the
    //     invocation, so `MODE_OFF`'s `mode` is [m.h:481 .. main.c:159] --
    //     two endpoints in different coordinate systems, not a range.
    //   * it RUNS AWAY.  Even within one file it can be 2669 bytes:
    //     `atomic.h[766,3435)` for a reference to `__sync_and_and_fetch`.
    //
    // A name sits on one line, which separates the good case from the runaway
    // one without a magic width.
    CXFile end_file = nullptr;
    unsigned end_off = 0;
    clang_getSpellingLocation(clang_getRangeEnd(nr), &end_file, nullptr, nullptr, &end_off);
    const std::vector<uint8_t>& content = result_.files[out_fi].content;
    if (end_file && clang_File_isEqual(start_file, end_file) && end_off > start_off &&
        static_cast<size_t>(end_off) <= content.size() &&
        std::find(content.begin() + start_off, content.begin() + end_off,
                  static_cast<uint8_t>('\n')) == content.begin() + end_off) {
        out_end = static_cast<int32_t>(end_off);
        return true;
    }

    // Otherwise the name is one token, so ask clang for the token AT that
    // position rather than re-lexing it here: `clang_getToken` runs the same
    // lexer that produced the cursor, so the bounds cannot disagree with it.
    // Measured on criu, this beats a hand-rolled identifier scan -- 73,950
    // exact ranges against 73,748, and an imprecise bucket 8x smaller.
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXSourceLocation at = clang_getLocationForOffset(tu, start_file, start_off);
    if (CXToken* tok = clang_getToken(tu, at)) {
        CXSourceRange te = clang_getTokenExtent(tu, *tok);
        CXFile tf = nullptr;
        unsigned ts = 0, tend = 0;
        clang_getSpellingLocation(clang_getRangeStart(te), &tf, nullptr, nullptr, &ts);
        clang_getSpellingLocation(clang_getRangeEnd(te), nullptr, nullptr, nullptr, &tend);
        clang_disposeTokens(tu, tok, 1);
        if (tf && clang_File_isEqual(tf, start_file) && ts == start_off &&
            tend > start_off) {
            out_end = static_cast<int32_t>(tend);
            return true;
        }
    }

    // If clang will not lex there, give up rather than re-lex it ourselves.
    // `refRange` then falls through to the cursor's EXTENT, which is also
    // clang's answer and is validated by the same predicate -- so every range
    // this produces comes from clang, and there is no second lexer to disagree
    // with the one that built the AST.
    //
    // Never observed to fire: zero times across criu's 74,737 references, a
    // C++ reproduction, and all 55 fixtures.  An earlier shape of this
    // function had no name-range tier, and `clang_getToken` then returned null
    // 3375 times on criu -- but those are exactly the positions the name-range
    // tier now handles first, so they no longer reach here.
    return false;
}

/// Take the name range, but only if the text there actually names the symbol;
/// otherwise fall back to the cursor's EXTENT.
///
/// The name range is not always the name.  For a qualified C++ callee it is the
/// nested-name-specifier: `std::min(a, b)` comes back as `std`, a single piece
/// (there is no second piece to ask for -- pieceIndex 1 is NULL).  Narrowing to
/// that would be a regression, because the extent at least contains `min`.
///
/// So the same predicate that guards `insertRef` picks the range here, which
/// also makes the choice self-correcting for cases nobody has met yet.  Order
/// matters: the name range is PREFERRED, because for
/// `ucs_offsetof(T, hugetlb_mode)` it is the only one that is right -- the
/// extent is the chimera this whole change exists to remove.
bool Stage1::refRange(CXCursor cursor, const std::string& sym_name, size_t& out_fi,
                      int32_t& out_start, int32_t& out_end) {
    if (refNameRange(cursor, sym_name, out_fi, out_start, out_end) &&
        refTextNamesSymbol(out_fi, sym_name, out_start, out_end, /*warn=*/false))
        return true;

    CXFile ef = nullptr;
    unsigned es = 0, ee = 0;
    if (!getSpellingRange(cursor, ef, es, ee)) return out_start >= 0;
    size_t efi = getOrCreateFile(ef);
    if (!refTextNamesSymbol(efi, sym_name, static_cast<int32_t>(es),
                            static_cast<int32_t>(ee), /*warn=*/false))
        return out_start >= 0;  // neither names it; keep the name range and let
                                // `insertRef` warn about the one we chose
    out_fi = efi;
    out_start = static_cast<int32_t>(es);
    out_end = static_cast<int32_t>(ee);
    return true;
}

void Stage1::addRef(CXCursor cursor, int64_t sym_id, const std::string& sym_name) {
    size_t fi;
    int32_t spelling_start, spelling_end;
    spelling_start = -1;
    if (refRange(cursor, sym_name, fi, spelling_start, spelling_end)) {
        // The FULL span is the whole referencing construct -- `foo(a, b)` where
        // the name span covers only `foo`.  Display only; ownership comes from
        // the DFS position.  Absent when the construct straddles files, which
        // is exactly the macro case.
        int32_t full_start = -1, full_end = -1;
        CXFile full_file = nullptr;
        unsigned fs = 0, fe = 0;
        CXFile name_file = clang_getFile(
            clang_Cursor_getTranslationUnit(cursor),
            result_.files[fi].filesystem_path.c_str());
        if (getExpansionRange(cursor, full_file, fs, fe) && full_file && name_file &&
            clang_File_isEqual(full_file, name_file)) {
            full_start = static_cast<int32_t>(fs);
            full_end = static_cast<int32_t>(fe);
        }
        insertRef(fi, sym_id, sym_name, spelling_start, spelling_end, full_start,
                  full_end);
    }

    // NO EXPANSION-SITE REFERENCE.  There used to be a second reference here,
    // emitted at the macro call site and owned by the expansion instance, for
    // every symbol an expansion touched.  Macro edges are layerwise now: each
    // layer records only what its OWN body names, and the body reference --
    // the one `insertRef` above already emitted, at the text the macro author
    // wrote -- is that record.  `handler -> printk` is a traversal of
    // `handler -> DEV_PRINT -> printk`, not a stored edge, exactly as it
    // already is for a chain of functions.
    //
    // Deleting it removes two wrongs and one duplicate.  On ucx it was 71,833
    // references collapsing to 14,753 distinct (macro, target) pairs, so the
    // same edge was materialised once per call site.  It also produced
    // `DEV_PRINT -> threshold` for `DEV_PRINT(threshold)` -- an edge from a
    // macro to an argument the macro never contained, since every byte at a
    // call site is the caller's.  And the hop it was standing in for,
    // `handler -> DEV_PRINT`, never existed; `numberTree` emits it now.
}

void Stage1::addDocComment(CXCursor cursor, int64_t sym_id, size_t /*file_idx*/) {
    CXSourceRange comment_range = clang_Cursor_getCommentRange(cursor);
    if (clang_Range_isNull(comment_range)) return;

    CXFile file;
    unsigned start_off, end_off;
    if (!getExpansionRange(comment_range, file, start_off, end_off)) return;

    size_t comment_file_idx = getOrCreateFile(file);
    addInstance(comment_file_idx,
                {sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off),
                 INSTTYPE_DOCUMENTATION},
                false);
}

CXChildVisitResult Stage1::visitor(CXCursor cursor, CXCursor parent, CXClientData data) {
    auto* self = static_cast<Stage1*>(data);
    // Recurse explicitly rather than returning CXChildVisit_Recurse: libclang
    // gives no "leaving this cursor" callback, and the DFS exit number has to
    // be stamped after the subtree is done.  Traversal is otherwise identical.
    size_t mark = self->dfs_stack_.size();
    self->visitCursor(cursor, parent);
    clang_visitChildren(cursor, visitor, self);
    self->closeScopesTo(mark);
    return CXChildVisit_Continue;
}

void Stage1::visitCursor(CXCursor cursor, CXCursor parent) {
    CXCursorKind kind = clang_getCursorKind(cursor);
    CXSourceLocation loc = clang_getCursorLocation(cursor);
    if (clang_equalLocations(loc, clang_getNullLocation())) return;

    CXFile file = nullptr;
    clang_getExpansionLocation(loc, &file, nullptr, nullptr, nullptr);
    if (!file) return;

    switch (kind) {
    case CXCursor_FunctionDecl: {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        int64_t sym_id = resolveSymbol(symbols_, cursor, sname, SYMTYPE_FUNCTION);
        int inst_type = clang_isCursorDefinition(cursor) ? INSTTYPE_DEFINITION : INSTTYPE_DECLARATION;

        CXFile range_file;
        unsigned start_off, end_off;
        if (getExpansionRange(cursor, range_file, start_off, end_off)) {
            size_t fi = getOrCreateFile(range_file);
            InstanceData d{sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), inst_type};
            fillSpans(cursor, range_file, d);
            // A declaration the preprocessor produced is a fold boundary: it is
            // still a real symbol of this module, but what IT contains is macro
            // plumbing and stays out of reach by default.
            d.folded = isMacroGenerated(cursor);
            addInstance(fi, d, true);
            addDocComment(cursor, sym_id, fi);
        }
        break;
    }
    case CXCursor_StructDecl:
    case CXCursor_UnionDecl:
    case CXCursor_EnumDecl:
    case CXCursor_TypedefDecl: {
        // Skip type decls that are children of a TypedefDecl — clang visits
        // them both as hoisted TU children and as TypedefDecl children during
        // recursion, which would create duplicate instances.
        if (kind != CXCursor_TypedefDecl &&
            clang_getCursorKind(parent) == CXCursor_TypedefDecl) break;

        // Decide whether to skip TYPE symbol/instance creation:
        // - Anonymous enums: synthetic name (e.g. "enum (unnamed at ...)") is useless
        // - Hoisted typedef types: the TypedefDecl already creates the TYPE instance
        // In both cases, still fall through to index child fields below.
        bool skip_type = false;
        if (kind == CXCursor_EnumDecl && clang_Cursor_isAnonymous(cursor))
            skip_type = true;

        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        if (kind != CXCursor_TypedefDecl && typedef_type_names_.count(sname))
            skip_type = true;

        if (!skip_type) {
            auto [sym_id, _] = symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_TYPE);
            int inst_type = clang_isCursorDefinition(cursor) ? INSTTYPE_DEFINITION : INSTTYPE_DECLARATION;

            CXFile range_file;
            unsigned start_off, end_off;
            if (getExpansionRange(cursor, range_file, start_off, end_off)) {
                size_t fi = getOrCreateFile(range_file);
                InstanceData d{sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), inst_type};
                fillSpans(cursor, range_file, d);
                d.folded = isMacroGenerated(cursor);
                addInstance(fi, d, true);
                addDocComment(cursor, sym_id, fi);
            }
        }

        // Index struct/union fields as FIELD symbols with compound "parent.child" names.
        // Enum constants are indexed as standalone DATA symbols — in C they
        // live in the global namespace regardless of whether the enum is named.
        if (kind == CXCursor_StructDecl || kind == CXCursor_UnionDecl) {
            clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
                if (clang_getCursorKind(child) != CXCursor_FieldDecl)
                    return CXChildVisit_Continue;
                static_cast<Stage1*>(data)->indexChildField(child);
                return CXChildVisit_Continue;
            }, this);
        } else if (kind == CXCursor_EnumDecl) {
            clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
                if (clang_getCursorKind(child) != CXCursor_EnumConstantDecl)
                    return CXChildVisit_Continue;
                auto* self = static_cast<Stage1*>(data);
                ClangString name(clang_getCursorSpelling(child));
                std::string sname = name.to_string();
                if (sname.empty()) return CXChildVisit_Continue;
                auto [sym_id, _] = self->symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_DATA);
                CXFile file;
                unsigned start, end;
                if (getExpansionRange(child, file, start, end)) {
                    size_t fi = self->getOrCreateFile(file);
                    self->addInstance(
                        fi, {sym_id, static_cast<int32_t>(start), static_cast<int32_t>(end), INSTTYPE_DEFINITION},
                        false);
                }
                return CXChildVisit_Continue;
            }, this);
        }
        break;
    }
    case CXCursor_VarDecl: {
        CXCursor semantic_parent = clang_getCursorSemanticParent(cursor);
        if (clang_getCursorKind(semantic_parent) != CXCursor_TranslationUnit) break;

        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;

        int64_t sym_id = resolveSymbol(symbols_, cursor, sname, SYMTYPE_DATA);
        int inst_type = clang_isCursorDefinition(cursor) ? INSTTYPE_DEFINITION : INSTTYPE_DECLARATION;

        CXFile range_file;
        unsigned start_off, end_off;
        if (getExpansionRange(cursor, range_file, start_off, end_off)) {
            size_t fi = getOrCreateFile(range_file);
            InstanceData d{sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), inst_type};
            fillSpans(cursor, range_file, d);
            // A declaration the preprocessor produced is a fold boundary: it is
            // still a real symbol of this module, but what IT contains is macro
            // plumbing and stays out of reach by default.
            d.folded = isMacroGenerated(cursor);
            addInstance(fi, d, true);
            addDocComment(cursor, sym_id, fi);
        }
        break;
    }
    case CXCursor_CallExpr: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;
        if (clang_getCursorKind(referenced) != CXCursor_FunctionDecl) break;

        ClangString ref_name(clang_getCursorSpelling(referenced));
        std::string sname = ref_name.to_string();
        if (sname.empty()) break;

        int64_t sym_id = resolveSymbol(symbols_, referenced, sname, SYMTYPE_FUNCTION);
        addRef(cursor, sym_id, sname);
        break;
    }
    case CXCursor_DeclRefExpr: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;

        CXCursorKind ref_kind = clang_getCursorKind(referenced);

        if (ref_kind == CXCursor_EnumConstantDecl) {
            ClangString ref_name(clang_getCursorSpelling(referenced));
            std::string sname = ref_name.to_string();
            if (sname.empty()) break;

            auto [sym_id, _] = symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_DATA);
            addRef(cursor, sym_id, sname);
        } else if (ref_kind == CXCursor_VarDecl) {
            CXCursor ref_parent = clang_getCursorSemanticParent(referenced);
            if (clang_getCursorKind(ref_parent) != CXCursor_TranslationUnit) break;

            ClangString ref_name(clang_getCursorSpelling(referenced));
            std::string sname = ref_name.to_string();
            if (sname.empty()) break;

            int64_t sym_id = resolveSymbol(symbols_, referenced, sname, SYMTYPE_DATA);
            addRef(cursor, sym_id, sname);
        }
        break;
    }
    case CXCursor_TypeRef: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;

        ClangString ref_name(clang_getCursorSpelling(referenced));
        std::string sname = ref_name.to_string();
        if (sname.empty()) break;

        auto [sym_id, _] = symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_TYPE);
        // Note: clang visits TypeRef children of anonymous types through both
        // the TypedefDecl and the underlying type (UnionDecl/StructDecl),
        // producing duplicate refs. Deduplicated in addRef via file_ref_sets_.
        addRef(cursor, sym_id, sname);
        break;
    }
    case CXCursor_MemberRefExpr: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;
        if (clang_getCursorKind(referenced) != CXCursor_FieldDecl) break;

        std::string compound_name = resolveFieldCompoundName(referenced);
        if (compound_name.empty()) break;

        auto [field_sym_id, _] = symbols_.getOrCreate(compound_name, SCOPE_GLOBAL, SYMTYPE_FIELD);
        addRef(cursor, field_sym_id, compound_name);
        break;
    }
    default:
        break;
    }
}

void Stage1::inclusionCallback(CXFile included_file, CXSourceLocation*,
                               unsigned, CXClientData data) {
    auto* self = static_cast<Stage1*>(data);
    self->getOrCreateFile(included_file);
}

void Stage1::process(CXTranslationUnit tu, const std::string& tu_filename) {
    // First, ensure all included files get FileData entries (with content + FILE symbol),
    // even if they contain no handled cursor kinds (e.g., macro-only headers).
    clang_getInclusions(tu, inclusionCallback, this);

    // Then also ensure the TU's own source file is indexed.
    CXFile tu_file = clang_getFile(tu, tu_filename.c_str());
    if (tu_file) getOrCreateFile(tu_file);

    // Collect macro definitions and expansions (requires DetailedPreprocessingRecord)
    collectMacros(tu);

    // Pre-scan for typedef'd types to avoid duplicate TYPE instances
    collectTypedefTypes(tu);

    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, visitor, this);
    closeScopesTo(0);
    // Every instance and ref now carries its structure; derive the tree and
    // number it.  Per TU, which is where it has to stay: the Indexer merges a
    // second TU's REFS into a file whose instances came from the first, so a
    // ref's numbers must be computed against the instances it shipped with.
    for (auto& f : result_.files) numberTree(f);
}

Stage1Result Stage1::takeResults() {
    result_.file_index = std::move(file_index_);
    return std::move(result_);
}
