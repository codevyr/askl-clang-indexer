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

        // Create instance at the #define body range
        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off)) {
            size_t fi = self->getOrCreateFile(range_file);
            self->result_.files[fi].instances.push_back(
                {sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), INSTTYPE_DEFINITION});
        }
    } else if (kind == CXCursor_MacroExpansion) {
        ClangString name(clang_getCursorSpelling(cursor));
        std::string sname = name.to_string();
        if (sname.empty()) return CXChildVisit_Continue;

        // Look up the MACRO symbol (should have been created by MacroDefinition)
        auto [sym_id, _] = self->symbols_.getOrCreate(sname, SCOPE_GLOBAL, SYMTYPE_MACRO);

        // Create an instance at the expansion site, clamped to the macro
        // name only (excluding arguments).  This way macro arguments like
        // `bar` in `some_macro(bar)` fall outside the macro instance range
        // and become direct children of the enclosing scope, while still
        // appearing as has-children of the macro via expansion-site refs.
        CXFile range_file;
        unsigned start_off, end_off;
        if (getSpellingRange(cursor, range_file, start_off, end_off)) {
            end_off = start_off + sname.size();  // clamp to macro name only
            size_t fi = self->getOrCreateFile(range_file);
            self->result_.files[fi].instances.push_back(
                {sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), INSTTYPE_EXPANSION});
            // Record macro name end for expansion-site ref clamping in addRef
            uint64_t key = fileOffsetKey(fi, start_off);
            self->macro_name_ends_[key] = end_off;
        }
    }

    return CXChildVisit_Continue;
}

void Stage1::collectMacros(CXTranslationUnit tu) {
    CXCursor root = clang_getTranslationUnitCursor(tu);
    clang_visitChildren(root, macroVisitor, this);
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

// Index a child cursor (FieldDecl or EnumConstantDecl) as a FIELD symbol
// with a compound name "parent.child".  Shared by struct field and enum
// constant visitors.
void Stage1::indexChildField(CXCursor child, CXCursorKind expected_parent_kind) {
    std::string compound_name = resolveCompoundName(child, expected_parent_kind);
    if (compound_name.empty()) return;

    auto [sym_id, _] = symbols_.getOrCreate(compound_name, SCOPE_GLOBAL, SYMTYPE_FIELD);

    CXFile file;
    unsigned start, end;
    if (getExpansionRange(child, file, start, end)) {
        size_t fi = getOrCreateFile(file);
        result_.files[fi].instances.push_back(
            {sym_id, static_cast<int32_t>(start), static_cast<int32_t>(end), INSTTYPE_DEFINITION});
    }
}

// Create a ref at the spelling location.  When the cursor is inside a macro
// expansion and the spelling location is in a different file than the
// expansion location, also create a ref at the expansion site.  This ensures
// that symbols referenced in a macro body (e.g. a TypeRef to the variable's
// type) appear within the expanded declaration's instance range, enabling
// containment queries like `data @i {...} / #i {type}`.
void Stage1::insertRef(size_t file_idx, int64_t sym_id, int32_t start, int32_t end) {
    if (file_ref_sets_[file_idx].insert({sym_id, start, end}).second)
        result_.files[file_idx].refs.push_back({sym_id, start, end});
}

void Stage1::addRef(CXCursor cursor, int64_t sym_id, unsigned name_len) {
    CXFile spelling_file;
    unsigned spelling_start, spelling_end;
    if (getSpellingRange(cursor, spelling_file, spelling_start, spelling_end, name_len)) {
        size_t fi = getOrCreateFile(spelling_file);
        insertRef(fi, sym_id, static_cast<int32_t>(spelling_start), static_cast<int32_t>(spelling_end));
    }

    // Use cursor location expansion (not extent) for tight expansion-site refs.
    // This gives a single point at the macro call start, which we clamp to the
    // macro name width using the lookup map populated during collectMacros.
    // Triggers for both cross-file refs (macro body) and same-file-different-offset
    // refs (macro arguments).
    CXSourceLocation exp_loc = clang_getCursorLocation(cursor);
    CXFile expansion_file = nullptr;
    unsigned exp_off;
    clang_getExpansionLocation(exp_loc, &expansion_file, nullptr, nullptr, &exp_off);
    if (expansion_file) {
        bool diff_file = !spelling_file || !clang_File_isEqual(spelling_file, expansion_file);
        // For same-file macro arguments: check if cursor location's spelling
        // differs from its expansion (i.e., cursor is inside a macro expansion).
        bool same_file_macro = false;
        if (!diff_file) {
            CXFile spell_loc_file = nullptr;
            unsigned spell_loc_off;
            clang_getSpellingLocation(exp_loc, &spell_loc_file, nullptr, nullptr, &spell_loc_off);
            same_file_macro = spell_loc_file &&
                (spell_loc_off != exp_off || !clang_File_isEqual(spell_loc_file, expansion_file));
        }
        if (diff_file || same_file_macro) {
            size_t fi = getOrCreateFile(expansion_file);
            // Clamp to enclosing macro name width
            uint64_t key = fileOffsetKey(fi, exp_off);
            auto it = macro_name_ends_.find(key);
            int32_t exp_end = (it != macro_name_ends_.end())
                ? static_cast<int32_t>(it->second)
                : static_cast<int32_t>(exp_off + 1);  // fallback
            insertRef(fi, sym_id, static_cast<int32_t>(exp_off), exp_end);
        }
    }
}

void Stage1::addDocComment(CXCursor cursor, int64_t sym_id, size_t /*file_idx*/) {
    CXSourceRange comment_range = clang_Cursor_getCommentRange(cursor);
    if (clang_Range_isNull(comment_range)) return;

    CXFile file;
    unsigned start_off, end_off;
    if (!getExpansionRange(comment_range, file, start_off, end_off)) return;

    size_t comment_file_idx = getOrCreateFile(file);
    result_.files[comment_file_idx].instances.push_back(
        {sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off),
         INSTTYPE_DOCUMENTATION});
}

CXChildVisitResult Stage1::visitor(CXCursor cursor, CXCursor parent, CXClientData data) {
    auto* self = static_cast<Stage1*>(data);
    self->visitCursor(cursor, parent);
    return CXChildVisit_Recurse;
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
            result_.files[fi].instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), inst_type});
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
                result_.files[fi].instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), inst_type});
                addDocComment(cursor, sym_id, fi);
            }
        }

        // Index children as FIELD symbols with compound "parent.child" names.
        // Struct fields: only function-pointer fields (for call-graph analysis).
        // Enum constants: all constants (they are the enum's "members").
        if (kind == CXCursor_StructDecl) {
            clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
                if (clang_getCursorKind(child) != CXCursor_FieldDecl)
                    return CXChildVisit_Continue;
                if (!isFunctionPointerType(clang_getCursorType(child)))
                    return CXChildVisit_Continue;
                static_cast<Stage1*>(data)->indexChildField(child, CXCursor_StructDecl);
                return CXChildVisit_Continue;
            }, this);
        } else if (kind == CXCursor_EnumDecl) {
            clang_visitChildren(cursor, [](CXCursor child, CXCursor, CXClientData data) {
                if (clang_getCursorKind(child) != CXCursor_EnumConstantDecl)
                    return CXChildVisit_Continue;
                static_cast<Stage1*>(data)->indexChildField(child, CXCursor_EnumDecl);
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
            result_.files[fi].instances.push_back({sym_id, static_cast<int32_t>(start_off), static_cast<int32_t>(end_off), inst_type});
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
        addRef(cursor, sym_id, sname.size());
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

            std::string compound_name = resolveCompoundName(referenced, CXCursor_EnumDecl);
            if (!compound_name.empty()) {
                auto [sym_id, _] = symbols_.getOrCreate(compound_name, SCOPE_GLOBAL, SYMTYPE_FIELD);
                addRef(cursor, sym_id, sname.size());
            } else {
                int64_t sym_id = resolveSymbol(symbols_, referenced, sname, SYMTYPE_DATA);
                addRef(cursor, sym_id, sname.size());
            }
        } else if (ref_kind == CXCursor_VarDecl) {
            CXCursor ref_parent = clang_getCursorSemanticParent(referenced);
            if (clang_getCursorKind(ref_parent) != CXCursor_TranslationUnit) break;

            ClangString ref_name(clang_getCursorSpelling(referenced));
            std::string sname = ref_name.to_string();
            if (sname.empty()) break;

            int64_t sym_id = resolveSymbol(symbols_, referenced, sname, SYMTYPE_DATA);
            addRef(cursor, sym_id, sname.size());
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
        addRef(cursor, sym_id, sname.size());
        break;
    }
    case CXCursor_MemberRefExpr: {
        CXCursor referenced = clang_getCursorReferenced(cursor);
        if (clang_Cursor_isNull(referenced)) break;
        if (clang_getCursorKind(referenced) != CXCursor_FieldDecl) break;
        if (!isFunctionPointerType(clang_getCursorType(referenced))) break;

        std::string compound_name = resolveCompoundName(referenced, CXCursor_StructDecl);
        if (compound_name.empty()) break;

        // Extract just the field name for the ref span length
        std::string::size_type dot = compound_name.rfind('.');
        unsigned name_len = (dot != std::string::npos)
            ? compound_name.size() - dot - 1 : compound_name.size();

        auto [field_sym_id, _] = symbols_.getOrCreate(compound_name, SCOPE_GLOBAL, SYMTYPE_FIELD);
        addRef(cursor, field_sym_id, name_len);
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
}

Stage1Result Stage1::takeResults() {
    result_.file_index = std::move(file_index_);
    return std::move(result_);
}
