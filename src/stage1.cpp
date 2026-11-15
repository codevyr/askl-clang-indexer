#include <tuple>
#include <algorithm>
#include "stage1.h"
#include "clang_raii.h"
#include "clang_utils.h"
#include "file_utils.h"
#include "symbol_table.h"
#include "symbol_types.h"

#include <cstdio>
#include <fstream>
#include <iterator>

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
    const int32_t file_inst_type = filetypeToInstType(fd.filetype);
    fd.instances.push_back(
        {file_sym_id, 0, static_cast<int32_t>(fd.content.size()), file_inst_type});

    size_t idx = result_.files.size();
    result_.files.push_back(std::move(fd));
    file_index_[path] = idx;
    // The file's own instance is pushed here rather than through `addInstance`
    // -- there is no scope stack yet, and it must be instances[0], which every
    // root-numbering path assumes.  Register it in the index anyway, so "the
    // index names every instance in the vector" holds without exception.
    file_inst_index_[idx].emplace(
        InstKey{file_sym_id, file_inst_type, 0,
                static_cast<int32_t>(result_.files[idx].content.size())},
        size_t{0});
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
            self->macro_bodies_.push_back({cursor, fi, start_off, end_off, sname});
            self->macro_def_index_[sname] = self->macro_bodies_.size() - 1;
            // Where normal code stops; see `insideDefine`.
            self->define_spans_[fi][static_cast<int32_t>(start_off)] =
                static_cast<int32_t>(end_off);
        }
    } else if (kind == CXCursor_MacroExpansion) {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) return CXChildVisit_Continue;

        // Look up the MACRO symbol (should have been created by MacroDefinition)
        auto [sym_id, _] = self->symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_MACRO);

        // USING a macro is a REFERENCE, not an instance.
        //
        // A macro is declared once -- at its `#define` -- and that line is its
        // only instance.  Every use is one site naming it, which is what a
        // reference is, and recording a use as an instance says a second time
        // what the reference already says.  It used to be the only record, so
        // the duplication was invisible; the invocation reference below was
        // derived from this instance in `numberTree`, both lived at the same
        // position, and the engine's own renderer dropped the instances again
        // on the way out ("a widely-used macro has hundreds of them").
        //
        // What the deletion costs: nothing on either axis the instance looked
        // like it carried.
        //
        //   - CONTAINMENT.  `func { macro }` and `mod("amdgpu") { macro }`
        //     keep working, through the reference, exactly as
        //     `func("main") { "printk" }` does -- printk has no instance in
        //     main either.  The reference sits in the enclosing function,
        //     which is where the expansion instance was nested by range.
        //   - FOLDING.  Whether a generated symbol is listed is decided for
        //     the symbol (`Indexer::foldSymbols`), and an expansion was
        //     already barred from being a parent -- measured on ucx, across
        //     41,487 expansions not one DEFINITION was a child.
        //
        // What it buys: 41,490 of ucx's ~97,664 instances were expansions, 42%
        // of the index.
        //
        // The reference's own range is the macro's NAME; its `full` range is
        // the whole invocation `some_macro(bar)`, the span a reader wants.
        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off)) {
            size_t fi = self->getOrCreateFile(range_file);
            RefPlace at;
            at.fi = fi;
            at.start = static_cast<int32_t>(start_off);
            at.end = static_cast<int32_t>(start_off + sname.size());
            self->insertRef(at, sym_id, sname, static_cast<int32_t>(start_off),
                            static_cast<int32_t>(end_off));
            // The same span is what later decides whether a declaration has
            // any text of its own; see `isWhollyAnInvocation`.
            self->noteInvocation(fi, static_cast<int32_t>(start_off),
                                 static_cast<int32_t>(end_off), cursor);
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
        // Straight from the cursor.  Rebuilding this range from byte offsets
        // cost a linear scan of every SLocEntry in the translation unit, twice
        // per `#define` -- see `MacroBody::cursor`.
        CXSourceRange range = clang_getCursorExtent(b.cursor);

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

            // ANCHOR ON THE TOKEN'S END, NOT ITS START.
            //
            // A line splice belongs to the token that follows it: clang reports
            // `INNER` written at column 1 after a `\\` as extent [341,348),
            // seven bytes wide for a five-character name, covering the
            // backslash and the newline.  Its START is therefore not where the
            // identifier is -- but its END is, so take the end from the extent
            // and step back by the spelling's length.
            //
            // `clang_getTokenLocation` has the same offset as the inflated
            // start, so the previous `[loc, loc + size)` slid the whole range
            // onto the splice and spelled `\\<newline>INN`.  `insertRef`'s range
            // guard then refused the reference and the edge was lost, though
            // its target had always been right.  Measured on a full reindex:
            // 865,728 references dropped on linux, 14,654 on rdma-core, 7,741
            // on open-gpu-kernel-modules -- the kernel opens almost every
            // multi-line macro body at column 1 -- and ZERO on ucx, which
            // writes them mid-line, which is why the guard looked clean.
            //
            // An identifier spliced INTERNALLY (`IN\\<newline>NER`) is still
            // beyond this: the spelling is shorter than the bytes it occupies,
            // so the computed start lands inside the name and the guard drops
            // it exactly as it does today.  No worse, and legal-but-absurd.
            CXSourceRange extent = clang_getTokenExtent(tu, toks[i]);
            unsigned tok_end = 0;
            CXFile tok_file = nullptr;
            clang_getSpellingLocation(clang_getRangeEnd(extent), &tok_file, nullptr,
                                      nullptr, &tok_end);
            if (!tok_file || tok_end < tok.size()) continue;
            const unsigned tok_start = tok_end - static_cast<unsigned>(tok.size());
            // No owner: `numberTree` resolves it from the text, and the
            // innermost instance containing a token inside a `#define` body
            // is that macro's own definition.
            RefPlace at;
            at.fi = b.file_idx;
            at.start = static_cast<int32_t>(tok_start);
            at.end = static_cast<int32_t>(tok_end);
            insertRef(at, it->second, tok);
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

// Index a FieldDecl cursor as a FIELD symbol with a compound name
// "parent.child".
//
// THE FIELD STAYS OPEN while the walk reads what is below it, like every
// other declaration: its type, its bit width.  It used to be closed at once,
// so whatever a field named was read "inside the struct".  Text put it in the
// field anyway -- except where text cannot: `PAIR(struct cfg, a, b)` declares
// two fields on one extent, and which of them a `cfg` belongs to is something
// only the walk knows.
void Stage1::indexChildField(CXCursor child) {
    std::string compound_name = resolveFieldCompoundName(child);
    if (compound_name.empty()) return;

    auto [sym_id, _] = symbols_.getOrCreate(compound_name, SCOPE_GLOBAL, SYMTYPE_FIELD);

    CXFile file;
    unsigned start, end;
    if (declExtent(child, file, start, end)) {
        size_t fi = getOrCreateFile(file);
        InstanceData d{sym_id, static_cast<int32_t>(start), static_cast<int32_t>(end), INSTTYPE_DEFINITION};
        fillSpans(child, file, d);
        markOrigin(child, fi, d);
        addInstance(fi, d, true);
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

int32_t Stage1::scopeAround(size_t fi, int32_t start, int32_t end) const {
    const auto& instances = result_.files[fi].instances;
    for (auto it = dfs_stack_.rbegin(); it != dfs_stack_.rend(); ++it) {
        if (it->file_idx != fi) continue;
        const InstanceData& d = instances[it->instance_idx];
        if (d.start_offset <= start && end <= d.end_offset)
            return static_cast<int32_t>(it->instance_idx);
    }
    return -1;
}

void Stage1::noteInvocation(size_t fi, int32_t start, int32_t end, CXCursor cursor) {
    if (end <= start) return;
    auto& spans = invocation_spans_[fi];
    // Invocations nest or are disjoint -- one is written in another's
    // arguments or it is not -- so the outermost ones are a set of disjoint
    // intervals and "is this range inside one" is a single predecessor lookup.
    auto next = spans.upper_bound(start);
    if (next != spans.begin() && std::prev(next)->second.end >= end) return;
    for (auto it = spans.lower_bound(start); it != spans.end() && it->first < end;)
        it = spans.erase(it);
    spans[start] = {end, cursor};
}

bool Stage1::isWhollyAnInvocation(size_t fi, int32_t start, int32_t end) const {
    auto file = invocation_spans_.find(fi);
    if (file == invocation_spans_.end()) return false;
    auto next = file->second.upper_bound(start);
    if (next == file->second.begin()) return false;
    return std::prev(next)->second.end >= end;
}

bool Stage1::declExtent(CXCursor cursor, CXFile& out_file, unsigned& out_start,
                        unsigned& out_end) {
    CXSourceRange range = clang_getCursorExtent(cursor);
    CXSourceLocation end = clang_getRangeEnd(range);
    CXFile start_file = nullptr, end_file = nullptr;
    clang_getExpansionLocation(clang_getRangeStart(range), &start_file, nullptr, nullptr,
                               &out_start);
    clang_getExpansionLocation(end, &end_file, nullptr, nullptr, &out_end);
    if (!start_file || !end_file || !clang_File_isEqual(start_file, end_file)) return false;

    // An end that is still inside an expansion is spelled somewhere other
    // than where it expands: at the argument, or nowhere (a pasted token).
    // Then `out_end` is the START of the outermost invocation it came from,
    // and the declaration really ends where that invocation does.
    CXFile spelled_file = nullptr;
    unsigned spelled_at = 0;
    clang_getSpellingLocation(end, &spelled_file, nullptr, nullptr, &spelled_at);
    if (!spelled_file || !clang_File_isEqual(spelled_file, end_file) || spelled_at != out_end) {
        auto file = invocation_spans_.find(getOrCreateFile(end_file));
        if (file != invocation_spans_.end()) {
            auto inv = file->second.find(static_cast<int32_t>(out_end));
            if (inv != file->second.end()) out_end = static_cast<unsigned>(inv->second.end);
        }
    }

    if (out_start >= out_end) return false;
    out_file = start_file;
    return true;
}

bool Stage1::continuesAsInvocation(CXCursor cursor, size_t fi, int32_t start,
                                   int32_t end) const {
    auto file = invocation_spans_.find(fi);
    if (file == invocation_spans_.end()) return false;
    auto inv = file->second.find(start);
    if (inv == file->second.end() || inv->second.end >= end) return false;

    const std::vector<uint8_t>& content = result_.files[fi].content;
    if (end <= 0 || static_cast<size_t>(end) > content.size()) return false;
    int32_t open = inv->second.end;
    while (open < end && (content[open] == ' ' || content[open] == '\t' ||
                          content[open] == '\n' || content[open] == '\r'))
        ++open;
    if (open >= end || content[open] != '(' || content[end - 1] != ')') return false;

    // Now ask clang whether `(` ... `)` is ONE list: the parenthesis that
    // opens it is closed by the last token and not before.  A string or a
    // comment holding a parenthesis is clang's to recognise, not ours.
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXFile cxfile = clang_getFile(tu, result_.files[fi].filesystem_path.c_str());
    if (!cxfile) return false;
    CXSourceRange range =
        clang_getRange(clang_getLocationForOffset(tu, cxfile, static_cast<unsigned>(open)),
                       clang_getLocationForOffset(tu, cxfile, static_cast<unsigned>(end)));
    CXToken* toks = nullptr;
    unsigned n = 0;
    clang_tokenize(tu, range, &toks, &n);
    if (!toks) return false;
    int depth = 0;
    bool one_list = n > 0;
    for (unsigned i = 0; i < n && one_list; ++i) {
        if (clang_getTokenKind(toks[i]) != CXToken_Punctuation) continue;
        ClangString spell(clang_getTokenSpelling(tu, toks[i]));
        const std::string t = spell.to_string();
        if (t == "(") {
            ++depth;
        } else if (t == ")") {
            --depth;
            if (depth < 0 || (depth == 0 && i + 1 != n)) one_list = false;
        }
    }
    clang_disposeTokens(tu, toks, n);
    return one_list && depth == 0;
}

bool Stage1::insideDefine(size_t fi, int32_t offset) const {
    auto file = define_spans_.find(fi);
    if (file == define_spans_.end()) return false;
    auto next = file->second.upper_bound(offset);
    if (next == file->second.begin()) return false;
    return std::prev(next)->second > offset;
}

bool Stage1::nameTypedInNormalCode(CXCursor cursor) {
    CXSourceRange nr = clang_Cursor_getSpellingNameRange(cursor, 0, 0);
    if (clang_Range_isNull(nr)) return false;
    CXFile file = nullptr;
    unsigned offset = 0;
    clang_getSpellingLocation(clang_getRangeStart(nr), &file, nullptr, nullptr, &offset);
    // No spelling file: the name was pasted together and lives in clang's
    // scratch buffer.  Nobody typed it anywhere.
    if (!file) return false;
    return !insideDefine(getOrCreateFile(file), static_cast<int32_t>(offset));
}

void Stage1::markOrigin(CXCursor cursor, size_t fi, InstanceData& d) {
    d.generated = isWhollyAnInvocation(fi, d.start_offset, d.end_offset) ||
                  continuesAsInvocation(cursor, fi, d.start_offset, d.end_offset);
    // A declaration with text of its own has been written down already; the
    // name only decides anything for one that has none.
    d.name_typed = d.generated && nameTypedInNormalCode(cursor);
}

void Stage1::addInstance(size_t fi, InstanceData d, bool opens_scope) {
    // Record structure, number later.  `numberTree` assigns dfs_enter,
    // dfs_exit and nesting_level together, so none of them can be set while
    // another is forgotten -- which is exactly how non-scope instances ended
    // up with dfs_exit == dfs_enter, able to contain nothing.
    d.stack_parent = stackParent(fi);

    // ONE DECLARATION IN THE TEXT IS ONE INSTANCE, however many times the text
    // is parsed.  A header without an include guard is parsed once per
    // inclusion, and each parse used to record the same declaration again (see
    // `InstKey`).
    //
    // On a repeat the SURVIVOR's index goes on the scope stack, not the one a
    // push would have produced, so the second parse's children nest under the
    // instance that stayed.  Nothing points at an instance yet -- numbering and
    // refs both come later -- which is the whole difference from the dedup this
    // reinstates: that one ran in `ProtoBuilder` AFTER numbering and orphaned
    // the refs already stamped against the rows it dropped.
    auto& index = file_inst_index_[fi];
    auto& instances = result_.files[fi].instances;
    auto [it, fresh] = index.try_emplace(
        InstKey{d.symbol_local_id, d.instance_type, d.start_offset, d.end_offset},
        instances.size());
    if (fresh) {
        instances.push_back(d);
    } else {
        // The same declaration parsed again may have been seen differently
        // (a header re-included under other macros).  Evidence that it was
        // written down wins, whichever parse brought it -- the same rule the
        // cross-TU merge applies, so the answer does not depend on order.
        InstanceData& kept = instances[it->second];
        kept.generated = kept.generated && d.generated;
        kept.name_typed = kept.name_typed || d.name_typed;
    }
    // `opens_scope` now means only "later cursors nest inside this one", which
    // is all it ever should have meant.
    if (opens_scope) dfs_stack_.push_back({fi, it->second});
}

void Stage1::closeScopesTo(size_t mark) {
    // Nothing to stamp any more: the exit number comes from the walk over the
    // derived tree, not from where the cursor happened to close.
    while (dfs_stack_.size() > mark) dfs_stack_.pop_back();
}

void canonicaliseInstanceOrder(FileData& f, const std::vector<SymbolOrder>& order) {
    const size_t n = f.instances.size();
    if (n < 2) return;

    auto key = [&](size_t i) {
        const InstanceData& d = f.instances[i];
        const SymbolOrder* so =
            (d.symbol_local_id >= 0 && static_cast<size_t>(d.symbol_local_id) < order.size())
                ? &order[static_cast<size_t>(d.symbol_local_id)]
                : nullptr;
        return std::tuple<int32_t, int32_t, int32_t, const std::string*, int>(
            d.start_offset, d.end_offset, d.instance_type,
            so ? &so->name : nullptr, so ? so->scope : 0);
    };

    std::vector<size_t> perm(n);
    for (size_t i = 0; i < n; ++i) perm[i] = i;
    std::sort(perm.begin(), perm.end(), [&](size_t a, size_t b) {
        auto ka = key(a), kb = key(b);
        if (std::get<0>(ka) != std::get<0>(kb)) return std::get<0>(ka) < std::get<0>(kb);
        if (std::get<1>(ka) != std::get<1>(kb)) return std::get<1>(ka) < std::get<1>(kb);
        if (std::get<2>(ka) != std::get<2>(kb)) return std::get<2>(ka) < std::get<2>(kb);
        const std::string* na = std::get<3>(ka);
        const std::string* nb = std::get<3>(kb);
        if (na && nb && *na != *nb) return *na < *nb;
        if (static_cast<bool>(na) != static_cast<bool>(nb)) return nb != nullptr;
        return std::get<4>(ka) < std::get<4>(kb);
    });

    // instances[0] must stay the file's own instance: every root-numbering path
    // assumes it, and `ProtoBuilder::ensureObjectRoot` numbers exactly that row
    // for a file nothing walked.  Its extent is [0, content size), so the sort
    // already puts it first unless another instance starts at 0 and ends no
    // later -- a file whose first declaration spans the whole text.  Pin it
    // rather than rely on that.
    auto zero = std::find(perm.begin(), perm.end(), size_t{0});
    if (zero != perm.end()) {
        perm.erase(zero);
        perm.insert(perm.begin(), size_t{0});
    }

    std::vector<size_t> rank(n);
    for (size_t newpos = 0; newpos < n; ++newpos) rank[perm[newpos]] = newpos;

    std::vector<InstanceData> sorted;
    sorted.reserve(n);
    for (size_t i = 0; i < n; ++i) sorted.push_back(f.instances[perm[i]]);
    // `stack_parent` is an index into the vector being reordered, so it has to
    // move with it, and so does a ref's `scope_idx`.  A ref's owner_idx is
    // assigned later, by `numberTree`.
    for (InstanceData& d : sorted) {
        if (d.stack_parent >= 0 && static_cast<size_t>(d.stack_parent) < n)
            d.stack_parent = static_cast<int32_t>(rank[static_cast<size_t>(d.stack_parent)]);
        else if (d.stack_parent != InstanceData::kReadDisputed)
            d.stack_parent = InstanceData::kReadAtFileScope;
    }
    for (RefData& r : f.refs) {
        if (r.scope_idx < 0) continue; // none: nothing to move
        r.scope_idx = static_cast<size_t>(r.scope_idx) < n
                          ? static_cast<int32_t>(rank[static_cast<size_t>(r.scope_idx)])
                          : RefData::kNoScope;
    }
    f.instances.swap(sorted);
}

void Stage1::numberTree(FileData& f) {
    const size_t n = f.instances.size();
    if (n == 0) return;

    // --- WHAT CONTAINS WHAT --------------------------------------------
    //
    //   A declaration somebody typed contains what lies in its text.
    //   A declaration a macro generated has no text of its own, so it
    //   contains only what the compiler read inside it.
    //
    // One law, for an instance and for a reference.  The extents that hold a
    // piece of text are asked innermost first, and for each of them:
    //
    //   1. If it was READ INSIDE a declaration with that extent, it sits there.
    //      Several declarations can have one extent -- everything
    //      `KHASH_INIT(...)` generates, or one function compiled twice under
    //      two names -- and text cannot tell them apart.  The walk can.
    //   2. Otherwise it sits in the declarations with that extent that have
    //      TEXT OF THEIR OWN.  A generated one is passed over: its extent is
    //      the invocation's, which says where the macro was used and nothing
    //      about what the declaration is made of.
    //   3. Otherwise the next wider extent is asked.
    //
    // `BUILD_BUG_ON(sizeof(struct x))` in `f` generates a declaration on the
    // invocation's extent.  `struct x` lies in that extent and was read in
    // `f`, so it sits in `f`.  The three functions `KHASH_INIT(x, int, hash)`
    // generates each read `hash` and each have it; the `KHASH_INIT` token was
    // read by none of them and sits in the file.
    //
    // It used to be "innermost extent, then deepest, then earliest", which
    // gave a generated declaration whatever its invocation mentioned and, for
    // several on one extent, gave it to an arbitrary one of them.
    //
    // `read_in` is the innermost declaration it was read inside, followed
    // outwards through `stack_parent`.  `self` is the instance being placed,
    // or -1 for a reference: an instance is never inside itself, and one with
    // the SAME extent holds it only by (1) -- two declarations on one extent
    // are beside each other unless the walk nested them.
    auto extentOf = [&](int32_t i) {
        const InstanceData& d = f.instances[static_cast<size_t>(i)];
        return std::pair<int32_t, int32_t>(d.start_offset, d.end_offset);
    };
    std::vector<int32_t> around;
    auto containersOf = [&](int32_t start, int32_t end, int32_t read_in, int32_t self,
                            std::vector<int32_t>& out) {
        out.clear();
        around.clear();
        for (size_t j = 0; j < n; ++j) {
            const InstanceData& c = f.instances[j];
            if (static_cast<int32_t>(j) != self && c.start_offset <= start && end <= c.end_offset)
                around.push_back(static_cast<int32_t>(j));
        }
        // Innermost first.  Among equals the canonical order decides, which is
        // a property of the file and not of the order units arrived in.
        std::sort(around.begin(), around.end(), [&](int32_t a, int32_t b) {
            const InstanceData& x = f.instances[static_cast<size_t>(a)];
            const InstanceData& y = f.instances[static_cast<size_t>(b)];
            const int64_t wx = static_cast<int64_t>(x.end_offset) - x.start_offset;
            const int64_t wy = static_cast<int64_t>(y.end_offset) - y.start_offset;
            if (wx != wy) return wx < wy;
            if (x.start_offset != y.start_offset) return x.start_offset < y.start_offset;
            return a < b;
        });
        for (size_t g = 0; g < around.size();) {
            size_t h = g + 1;
            while (h < around.size() && extentOf(around[h]) == extentOf(around[g])) ++h;
            for (int32_t s = read_in; s >= 0 && static_cast<size_t>(s) < n;
                 s = f.instances[static_cast<size_t>(s)].stack_parent)
                if (s != self && extentOf(s) == extentOf(around[g])) {
                    out.push_back(s);
                    return;
                }
            const bool beside = self >= 0 && extentOf(self) == extentOf(around[g]);
            for (size_t k = g; !beside && k < h; ++k)
                if (!f.instances[static_cast<size_t>(around[k])].generated)
                    out.push_back(around[k]);
            if (!out.empty()) return;
            g = h;
        }
    };

    // --- PARENTAGE -----------------------------------------------------
    // A tree has one parent per node.  Where the law names several -- one
    // function compiled twice under two names, with a declaration in its body
    // -- the first in canonical order is taken.
    std::vector<int32_t> parent(n, -1);
    std::vector<int32_t> holders;
    for (size_t i = 0; i < n; ++i) {
        const InstanceData& x = f.instances[i];
        containersOf(x.start_offset, x.end_offset, x.stack_parent, static_cast<int32_t>(i),
                     holders);
        if (!holders.empty()) parent[i] = holders.front();
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
    // A ref is a point one step inside what contains it, on every axis.
    // Stamping it here from `owner_idx` is what makes "a ref lies inside its
    // container" true by construction.
    //
    // A reference nothing read -- a macro use, recorded before the walk -- that
    // lies in the text of two typed declarations sits in both: each of them
    // contains its text.  That is one more reference, not a choice.
    const size_t given = f.refs.size();
    for (size_t ri = 0; ri < given; ++ri) {
        containersOf(f.refs[ri].from_offset_start, f.refs[ri].from_offset_end,
                     f.refs[ri].scope_idx, -1, holders);
        for (size_t k = 0; k < holders.size(); ++k) {
            if (k > 0) f.refs.push_back(f.refs[ri]);
            RefData& r = k > 0 ? f.refs.back() : f.refs[ri];
            const InstanceData& o = f.instances[static_cast<size_t>(holders[k])];
            r.owner_idx = holders[k];
            r.dfs_enter = o.dfs_enter + 1;
            r.nesting_level = o.nesting_level + 1;
        }
        if (holders.empty()) f.refs[ri].owner_idx = -1;
    }

    // ONE REFERENCE PER (target, token, place).  Until here a reference was
    // told apart by the declaration it was read inside, which is finer than
    // where it sits: a `#define` body token is read in every function that
    // expands the macro and sits in the macro's definition.  Readings that
    // ended in one place are one reference; they keep what any of them knew.
    {
        std::unordered_map<RefKey, size_t, RefKeyHash> seen;
        std::vector<RefData> kept;
        kept.reserve(f.refs.size());
        for (const RefData& r : f.refs) {
            auto [it, fresh] = seen.try_emplace(
                RefKey{r.to_symbol_local_id, r.from_offset_start, r.from_offset_end, r.owner_idx},
                kept.size());
            if (fresh) {
                kept.push_back(r);
                continue;
            }
            RefData& k = kept[it->second];
            k.pasted = k.pasted && r.pasted;
            k.typed_in_define = k.typed_in_define && r.typed_in_define;
            k.synthetic = k.synthetic && r.synthetic;
            if (k.full_start < 0 && r.full_start >= 0) {
                k.full_start = r.full_start;
                k.full_end = r.full_end;
            }
        }
        f.refs.swap(kept);
    }
}

std::vector<PasteToken> Stage1::pasteTokens(CXCursor cursor) {
    std::vector<PasteToken> out;
    CXTranslationUnit tu = clang_Cursor_getTranslationUnit(cursor);
    CXSourceRange range = clang_getCursorExtent(cursor);
    // One invocation, or one `#define`, is written in one file.
    CXFile file = nullptr;
    clang_getSpellingLocation(clang_getRangeStart(range), &file, nullptr, nullptr, nullptr);
    if (!file) return out;
    const auto fi = static_cast<uint32_t>(getOrCreateFile(file));

    CXToken* toks = nullptr;
    unsigned n = 0;
    clang_tokenize(tu, range, &toks, &n);
    if (!toks) return out;
    out.reserve(n);
    for (unsigned i = 0; i < n; ++i) {
        ClangString spell(clang_getTokenSpelling(tu, toks[i]));
        const std::string text = spell.to_string();
        PasteToken t;
        t.text = paste_strings_.intern(text);
        const CXTokenKind kind = clang_getTokenKind(toks[i]);
        t.ident = kind == CXToken_Identifier || kind == CXToken_Keyword;
        // Anchored on the token's END, for the reason `linkMacroBodies` gives:
        // a line splice belongs to the token after it, so the start can be
        // two bytes early while the end is always the identifier's.
        CXFile tok_file = nullptr;
        unsigned tok_end = 0;
        clang_getSpellingLocation(clang_getRangeEnd(clang_getTokenExtent(tu, toks[i])),
                                  &tok_file, nullptr, nullptr, &tok_end);
        if (tok_file && clang_File_isEqual(tok_file, file) && tok_end >= text.size()) {
            t.origin.file_idx = fi;
            t.origin.start = static_cast<int32_t>(tok_end - text.size());
            t.origin.end = static_cast<int32_t>(tok_end);
        }
        out.push_back(t);
    }
    clang_disposeTokens(tu, toks, n);
    return out;
}

std::unique_ptr<PasteMacro> Stage1::parsePasteMacro(CXCursor definition) {
    std::vector<PasteToken> toks = pasteTokens(definition);
    if (toks.empty()) return nullptr;

    auto macro = std::make_unique<PasteMacro>();
    macro->name = toks[0].text;
    macro->function_like = clang_Cursor_isMacroFunctionLike(definition) != 0;
    size_t i = 1;
    if (macro->function_like) {
        if (i >= toks.size() || toks[i].text != paste_strings_.lparen) return nullptr;
        for (++i; i < toks.size() && toks[i].text != paste_strings_.rparen; ++i) {
            if (toks[i].text == paste_strings_.ellipsis) {
                // `args...` names the variadic parameter; a bare `...` is
                // `__VA_ARGS__`.
                const bool named = !macro->params.empty() && toks[i - 1].ident &&
                                   toks[i - 1].text == macro->params.back();
                if (!named) macro->params.push_back(paste_strings_.va_args);
                macro->variadic = true;
            } else if (toks[i].ident) {
                macro->params.push_back(toks[i].text);
            }
        }
        if (i >= toks.size()) return nullptr;
        ++i; // the `)`
    }
    macro->body.assign(toks.begin() + static_cast<std::ptrdiff_t>(i), toks.end());
    return macro;
}

const PasteMacro* Stage1::pasteMacroAt(size_t index) {
    if (paste_macros_.size() < macro_bodies_.size()) {
        paste_macros_.resize(macro_bodies_.size());
        paste_macro_tried_.resize(macro_bodies_.size(), 0);
    }
    if (!paste_macro_tried_[index]) {
        paste_macro_tried_[index] = 1;
        paste_macros_[index] = parsePasteMacro(macro_bodies_[index].cursor);
    }
    return paste_macros_[index].get();
}

const PasteMacro* Stage1::pasteMacroByName(PasteStrings::Id name) {
    if (macro_of_string_.size() <= name) macro_of_string_.resize(paste_strings_.size(), -2);
    int32_t& slot = macro_of_string_[name];
    if (slot == -2) {
        auto it = macro_def_index_.find(paste_strings_.str(name));
        slot = it == macro_def_index_.end() ? -1 : static_cast<int32_t>(it->second);
    }
    return slot < 0 ? nullptr : pasteMacroAt(static_cast<size_t>(slot));
}

const std::unordered_map<PasteStrings::Id, PasteSite>* Stage1::pastesAt(size_t fi,
                                                                        int32_t start) {
    auto& per_file = paste_cache_[fi];
    auto hit = per_file.find(start);
    if (hit != per_file.end()) return &hit->second;
    // Cached even when empty: an invocation that pastes nothing a caller
    // chose is asked about once per reference it generated.
    auto& pastes = per_file[start];

    auto file = invocation_spans_.find(fi);
    if (file == invocation_spans_.end()) return &pastes;
    auto inv = file->second.find(start);
    if (inv == file->second.end()) return &pastes;
    const CXCursor invocation = inv->second.cursor;

    // The definition THIS invocation used, which a name lookup could get
    // wrong: `#define X(a) ...` / `#undef X` / `#define X(a) ...` around a
    // list is an ordinary idiom.  It is nearly always the latest one all the
    // same, and then the parse is already in hand.
    CXCursor definition = clang_getCursorReferenced(invocation);
    if (clang_getCursorKind(definition) != CXCursor_MacroDefinition) return &pastes;
    const PasteMacro* macro = nullptr;
    std::unique_ptr<PasteMacro> earlier_definition;
    {
        ClangString def_name(clang_getCursorSpelling(definition));
        auto latest = macro_def_index_.find(def_name.to_string());
        if (latest != macro_def_index_.end() &&
            clang_equalCursors(macro_bodies_[latest->second].cursor, definition)) {
            macro = pasteMacroAt(latest->second);
        } else {
            earlier_definition = parsePasteMacro(definition);
            macro = earlier_definition.get();
        }
    }
    if (!macro) return &pastes;

    std::vector<PasteToken> tokens = pasteTokens(invocation);
    if (tokens.empty()) return &pastes;
    tokens.erase(tokens.begin()); // the macro's own name

    pastes = replayPastes(*macro, tokens, paste_strings_,
                          [this](PasteStrings::Id name) { return pasteMacroByName(name); });
    return &pastes;
}

bool Stage1::pastedNameSite(CXCursor cursor, const std::string& sym_name, RefPlace& out) {
    CXSourceRange nr = clang_Cursor_getSpellingNameRange(cursor, 0, 0);
    if (clang_Range_isNull(nr)) return false;
    CXSourceLocation at = clang_getRangeStart(nr);

    // A pasted identifier is the one kind of token with no spelling file:
    // clang keeps it in a scratch buffer.
    CXFile spell_file = nullptr;
    clang_getSpellingLocation(at, &spell_file, nullptr, nullptr, nullptr);
    if (spell_file) return false;

    // What it does have is the OUTERMOST invocation it came out of, however
    // many macros deep the paste itself was.
    CXFile exp_file = nullptr;
    unsigned exp_off = 0;
    clang_getExpansionLocation(at, &exp_file, nullptr, nullptr, &exp_off);
    if (!exp_file) return false;

    const auto* pastes = pastesAt(getOrCreateFile(exp_file), static_cast<int32_t>(exp_off));
    if (!pastes || pastes->empty()) return false;

    // By leaf, as `refTextNamesSymbol` reads a name: `mod.pml_send` is
    // written `pml_send`.  A name no replay ever built was never interned.
    std::string leaf = sym_name;
    auto cut = leaf.find_last_of("./");
    if (cut != std::string::npos) leaf = leaf.substr(cut + 1);
    PasteStrings::Id leaf_id = 0;
    if (!paste_strings_.find(leaf, leaf_id)) return false;
    auto it = pastes->find(leaf_id);
    if (it == pastes->end()) return false;

    // WHERE IT SITS, not where its part was typed.  The two differ for a name
    // completed by a macro passed as an argument (`PasteSite`); `typed` is
    // then in a `#define` body, which is what says the name was never
    // written in normal code.
    const PasteSite& found = it->second;
    out.fi = found.site.file_idx;
    out.start = found.site.start;
    out.end = found.site.end;
    out.unspelled = found.by_callback;
    out.pasted = true;
    out.typed_in_define =
        found.typed.valid() && insideDefine(found.typed.file_idx, found.typed.start);
    return true;
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

void Stage1::insertRef(const RefPlace& at, int64_t sym_id, const std::string& sym_name,
                       int32_t full_start, int32_t full_end) {
    const size_t file_idx = at.fi;
    const int32_t start = at.start, end = at.end;
    // WHERE IT WAS READ IS PART OF WHAT IT IS.  `KHASH_INIT(x, int, my_hash)`
    // generates three functions that each call `my_hash`, on the one token the
    // caller typed: three references, one in each.  Keyed by the token alone,
    // the first function read kept the call and the other two had none.
    const int32_t scope = scopeAround(file_idx, start, end);
    if (!file_ref_sets_[file_idx].insert({sym_id, start, end, scope}).second)
        return;
    // Where it sits is left UNSET: `numberTree` decides, once every instance
    // in the file is known.  It used to come
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
    // pass through `insertRef` at all -- they are merged in `indexer.cpp` and
    // numbered with everything else after the merge -- so the ones it
    // deliberately parks at a field's declaration site stay exempt by
    // construction.
    //
    // One kind of range is right without spelling its symbol: the macro that
    // builds a pasted name, `ENTRY` in `EACH(ENTRY)`.  The replay has already
    // shown that this token, applied to the list, produces the name -- which
    // is a stronger statement than the text resembling it.
    if (!at.unspelled && !refTextNamesSymbol(file_idx, sym_name, start, end, /*warn=*/true))
        return;

    RefData r{sym_id, start, end};
    r.scope_idx = scope;
    r.pasted = at.pasted;
    r.typed_in_define = at.typed_in_define;
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

    // The token has to begin exactly where the name does, in the same file, or
    // it is not the token we asked about.  That test is what lets the location
    // be GUESSED cheaply first and only computed expensively on a miss.
    auto token_end_at = [&](CXSourceLocation at, int32_t& out) {
        CXToken* tok = clang_getToken(tu, at);
        if (!tok) return false;
        CXSourceRange te = clang_getTokenExtent(tu, *tok);
        CXFile tf = nullptr;
        unsigned ts = 0, tend = 0;
        clang_getSpellingLocation(clang_getRangeStart(te), &tf, nullptr, nullptr, &ts);
        clang_getSpellingLocation(clang_getRangeEnd(te), nullptr, nullptr, nullptr, &tend);
        clang_disposeTokens(tu, tok, 1);
        if (!tf || !clang_File_isEqual(tf, start_file) || ts != start_off ||
            tend <= start_off)
            return false;
        out = static_cast<int32_t>(tend);
        return true;
    };

    // The location already in hand costs nothing.  It is a RAW location
    // though: inside a macro it is the expansion's and lexes somewhere else
    // entirely, which the test above then rejects -- so it is an attempt, not a
    // replacement.
    if (token_end_at(clang_getRangeStart(nr), out_end)) return true;

    // On a miss, build the location from the offset after all.  This is the
    // expensive way round: `clang_getLocationForOffset` linear-scans every
    // SLocEntry in the translation unit (see `MacroBody::cursor`), and once the
    // macro-body path stopped using it, it was still 23% of the run --
    // `SourceManager::translateFile` 16% plus `getFileIDLocal` 6.7% underneath
    // it, profiling a kernel slice.  Reached only when the cheap guess missed,
    // and the answer is the same either way.
    if (token_end_at(clang_getLocationForOffset(tu, start_file, start_off), out_end))
        return true;

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
bool Stage1::refRange(CXCursor cursor, const std::string& sym_name, RefPlace& out) {
    out = RefPlace{};
    if (refNameRange(cursor, sym_name, out.fi, out.start, out.end) &&
        refTextNamesSymbol(out.fi, sym_name, out.start, out.end, /*warn=*/false))
        return true;

    // No token spells the name: a macro pasted it together.  Then the
    // reference sits on the part its caller typed, when a caller typed one.
    // Before the extent fallback below, which for such a name is the start of
    // an expression in some `#define` body and names nothing.
    RefPlace pasted;
    if (pastedNameSite(cursor, sym_name, pasted)) {
        out = pasted;
        return true;
    }

    CXFile ef = nullptr;
    unsigned es = 0, ee = 0;
    if (!getSpellingRange(cursor, ef, es, ee)) return out.start >= 0;
    size_t efi = getOrCreateFile(ef);
    if (!refTextNamesSymbol(efi, sym_name, static_cast<int32_t>(es),
                            static_cast<int32_t>(ee), /*warn=*/false))
        return out.start >= 0;  // neither names it; keep the name range and let
                                // `insertRef` warn about the one we chose
    out.fi = efi;
    out.start = static_cast<int32_t>(es);
    out.end = static_cast<int32_t>(ee);
    return true;
}

bool Stage1::placesReference(CXCursor cursor, const std::string& name) {
    RefPlace at;
    return refRange(cursor, name, at) &&
           (at.unspelled || refTextNamesSymbol(at.fi, name, at.start, at.end, /*warn=*/false));
}

void Stage1::addRef(CXCursor cursor, int64_t sym_id, const std::string& sym_name) {
    RefPlace at;
    if (refRange(cursor, sym_name, at)) {
        const size_t fi = at.fi;
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
        insertRef(at, sym_id, sym_name, full_start, full_end);
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
    size_t calls = self->open_calls_.size();
    self->visitCursor(cursor, parent);
    clang_visitChildren(cursor, visitor, self);
    self->closeScopesTo(mark);
    while (self->open_calls_.size() > calls) self->open_calls_.pop_back();
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
        if (declExtent(cursor, range_file, start_off, end_off)) {
            size_t fi = getOrCreateFile(range_file);
            InstanceData d{sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), inst_type};
            fillSpans(cursor, range_file, d);
            markOrigin(cursor, fi, d);
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

        // ONE RECORD, ONE NAME.  A record with no tag is named by clang after
        // where it is written, using the path the `#include` resolved to, so
        // the same declaration reached two ways got two names.  Done after the
        // typedef check above, which compares against identifiers a generated
        // name can never equal.
        sname = canonicaliseEmbeddedPaths(sname, cursor);

        if (!skip_type) {
            auto [sym_id, _] = symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_TYPE);
            int inst_type = clang_isCursorDefinition(cursor) ? INSTTYPE_DEFINITION : INSTTYPE_DECLARATION;

            CXFile range_file;
            unsigned start_off, end_off;
            if (declExtent(cursor, range_file, start_off, end_off)) {
                size_t fi = getOrCreateFile(range_file);
                InstanceData d{sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), inst_type};
                fillSpans(cursor, range_file, d);
                markOrigin(cursor, fi, d);
                addInstance(fi, d, true);
                addDocComment(cursor, sym_id, fi);
            }
        }

        // Fields and enum constants are met below this cursor, by the walk
        // (`CXCursor_FieldDecl`, `CXCursor_EnumConstantDecl`) -- also when
        // the record itself was skipped above.
        break;
    }
    case CXCursor_FieldDecl:
        indexChildField(cursor);
        break;
    case CXCursor_EnumConstantDecl: {
        // Enum constants are standalone DATA symbols: in C they live in the
        // global namespace whether or not the enum is named.
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) break;
        auto [sym_id, _] = symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_DATA);
        CXFile range_file;
        unsigned start_off, end_off;
        if (declExtent(cursor, range_file, start_off, end_off)) {
            size_t fi = getOrCreateFile(range_file);
            InstanceData d{sym_id, static_cast<int32_t>(start_off),
                           static_cast<int32_t>(end_off), INSTTYPE_DEFINITION};
            markOrigin(cursor, fi, d);
            addInstance(fi, d, true);
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
        if (declExtent(cursor, range_file, start_off, end_off)) {
            size_t fi = getOrCreateFile(range_file);
            InstanceData d{sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), inst_type};
            fillSpans(cursor, range_file, d);
            markOrigin(cursor, fi, d);
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
        // Its callee is about to be visited as a DeclRefExpr to the same
        // function; see `open_calls_`.  Closed by `visitor` on the way out.
        open_calls_.push_back({referenced, false});
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
        } else if (ref_kind == CXCursor_FunctionDecl) {
            // A FUNCTION NAMED WITHOUT BEING CALLED: `register_cb(helper)`,
            // `return helper;`, `&helper`, `.open = helper`.  Only the
            // CallExpr case above used to record a function, so none of these
            // produced an edge -- and `INIT(fn)`, where the macro uses its
            // argument as a value, left `fn` with no reference at all.
            //
            // Except the one mention that IS a call: the callee of the call
            // being walked.  That is the call's reference, already recorded.
            if (!open_calls_.empty() && !open_calls_.back().callee_seen &&
                clang_equalCursors(open_calls_.back().callee, referenced)) {
                open_calls_.back().callee_seen = true;
                break;
            }

            ClangString ref_name(clang_getCursorSpelling(referenced));
            std::string sname = ref_name.to_string();
            if (sname.empty()) break;

            int64_t sym_id = resolveSymbol(symbols_, referenced, sname, SYMTYPE_FUNCTION);
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

        // Canonicalised against `referenced`, the DECLARATION, not against the
        // reference site: the name has to come out identical to the one the
        // declaration registered, and it is the declaration's file the path
        // names.
        sname = canonicaliseEmbeddedPaths(sname, referenced);

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
    // Structure only.  NUMBERING HAPPENS AFTER THE MERGE, in `Indexer::run`,
    // because a file's instances are a property of its TEXT and this TU holds
    // only one view of it: numbering here meant a second TU's refs had to be
    // renumbered against instances they never shipped with, which is what
    // `placeRefByText` existed to do.  One file, one numbering, once.
}

Stage1Result Stage1::takeResults() {
    result_.file_index = std::move(file_index_);
    return std::move(result_);
}
