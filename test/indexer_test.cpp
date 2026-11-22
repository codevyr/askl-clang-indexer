#include "clang_utils.h"
#include "compilation_db.h"
#include "indexer.h"
#include "symbol_table.h"
#include "stage1.h"
#include "proto_builder.h"
#include "symbol_types.h"
#include "proto/index.pb.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <map>
#include <set>
#include <tuple>
#include <string>
#include <unordered_map>
#include <vector>

namespace fs = std::filesystem;

// --- Helper types ---

struct ExpectedSymbol {
    std::string name;
    int scope;  // 1=LOCAL, 2=GLOBAL
    int type;   // 1=FUNCTION, 2=FILE, 4=DIRECTORY, 5=TYPE, 6=DATA

    bool operator==(const ExpectedSymbol& o) const {
        return name == o.name && scope == o.scope && type == o.type;
    }
    bool operator<(const ExpectedSymbol& o) const {
        if (name != o.name) return name < o.name;
        if (scope != o.scope) return scope < o.scope;
        return type < o.type;
    }
};

void PrintTo(const ExpectedSymbol& s, std::ostream* os) {
    auto scope_str = [](int s) {
        switch (s) { case 1: return "LOCAL"; case 2: return "GLOBAL"; default: return "?"; }
    };
    auto type_str = [](int t) {
        switch (t) {
            case 1: return "FUNCTION"; case 2: return "FILE"; case 4: return "DIRECTORY";
            case 5: return "TYPE"; case 6: return "DATA"; case 7: return "MACRO"; case 8: return "FIELD"; default: return "?";
        }
    };
    *os << "{name=" << s.name << ", scope=" << scope_str(s.scope)
        << ", type=" << type_str(s.type) << "}";
}

// Role bits and relations (matching proto/index.proto RefRole / RefRelation)
constexpr int CALL = 1;
constexpr int WRITE = 2;
constexpr int TAKE = 4;
constexpr int DECLARES = 1;
constexpr int HOLDS = 2;

struct ExpectedRef {
    std::string from_file;      // module_path of the file containing the reference
    std::string to_name;        // name of the referenced symbol
    int32_t from_start;         // byte offset of reference start
    int32_t from_end;           // byte offset of reference end
    // What the reference states.  -1 = not checked, which is what every
    // fixture written before roles existed says; 0 = it states nothing.
    int roles = -1;
    int relation = -1;

    bool operator==(const ExpectedRef& o) const {
        if (roles >= 0 && o.roles >= 0 && roles != o.roles) return false;
        if (relation >= 0 && o.relation >= 0 && relation != o.relation) return false;
        return from_file == o.from_file && to_name == o.to_name
            && from_start == o.from_start && from_end == o.from_end;
    }
    bool operator<(const ExpectedRef& o) const {
        if (from_file != o.from_file) return from_file < o.from_file;
        if (from_start != o.from_start) return from_start < o.from_start;
        if (from_end != o.from_end) return from_end < o.from_end;
        return to_name < o.to_name;
    }
};

void PrintTo(const ExpectedRef& r, std::ostream* os) {
    *os << "{\"" << r.from_file << "\", \"" << r.to_name
        << "\", " << r.from_start << ", " << r.from_end;
    if (r.roles >= 0 || r.relation >= 0) *os << ", " << r.roles << ", " << r.relation;
    *os << "}";
}

struct ExpectedInstance {
    std::string file;    // module_path of the file containing the instance
    std::string symbol;  // name of the symbol
    int instance_type = 0;  // 0 = skip check; 1=DEF, 2=DECL, 3=EXP, 6=SRC, 7=HDR
    int32_t start_offset = -1;  // -1 = skip check
    int32_t end_offset = -1;    // -1 = skip check

    bool operator==(const ExpectedInstance& o) const {
        if (file != o.file || symbol != o.symbol) return false;
        if (instance_type != 0 && o.instance_type != 0 && instance_type != o.instance_type) return false;
        if (start_offset >= 0 && o.start_offset >= 0 && start_offset != o.start_offset) return false;
        if (end_offset >= 0 && o.end_offset >= 0 && end_offset != o.end_offset) return false;
        return true;
    }
    bool operator<(const ExpectedInstance& o) const {
        if (file != o.file) return file < o.file;
        if (symbol != o.symbol) return symbol < o.symbol;
        if (instance_type != o.instance_type) return instance_type < o.instance_type;
        if (start_offset != o.start_offset) return start_offset < o.start_offset;
        return end_offset < o.end_offset;
    }
};

void PrintTo(const ExpectedInstance& i, std::ostream* os) {
    *os << "{file=" << i.file << ", sym=" << i.symbol << ", itype=" << i.instance_type;
    if (i.start_offset >= 0 || i.end_offset >= 0)
        *os << ", range=[" << i.start_offset << ", " << i.end_offset << ")";
    *os << "}";
}

// --- Helpers ---

void generateCompileCommands(const std::string& fixture_dir,
                             const std::string& output_dir,
                             const std::vector<std::string>& sources,
                             const std::string& extra_flags = "") {
    std::ofstream out(output_dir + "/compile_commands.json");
    out << "[\n";
    for (size_t i = 0; i < sources.size(); i++) {
        if (i > 0) out << ",\n";
        out << "  {\n"
            << "    \"directory\": \"" << fixture_dir << "\",\n"
            << "    \"command\": \"cc -c -I. "
            << (extra_flags.empty() ? "" : extra_flags + " ") << sources[i] << " -o "
            << sources[i].substr(0, sources[i].rfind('.')) << ".o\",\n"
            << "    \"file\": \"" << fixture_dir << "/" << sources[i] << "\"\n"
            << "  }";
    }
    out << "\n]\n";
}

struct RunResult {
    std::vector<ExpectedSymbol> symbols;
    std::vector<ExpectedRef> refs;
    std::vector<ExpectedInstance> instances;
};

struct TempDir {
    std::string path;
    ~TempDir() { if (!path.empty()) fs::remove_all(path); }
};

static TempDir makeTempDir() {
    std::string tmpl = (fs::temp_directory_path() / "askl-test-XXXXXX").string();
    std::vector<char> buf(tmpl.begin(), tmpl.end());
    buf.push_back('\0');
    char* dir = mkdtemp(buf.data());
    return TempDir{dir ? dir : ""};
}

RunResult runFixture(const std::string& fixture_name,
                     const std::vector<std::string>& sources) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/" + fixture_name;

    TempDir temp_dir = makeTempDir();

    generateCompileCommands(fixture_dir, temp_dir.path, sources);

    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();

    // Helper to strip the fixture_dir prefix from absolute paths for comparison.
    // Directory symbols for the fixture_dir ancestors are also filtered out.
    std::string root_prefix = fixture_dir + "/";
    auto strip_root = [&](const std::string& path) -> std::string {
        if (path.size() > root_prefix.size() &&
            path.substr(0, root_prefix.size()) == root_prefix) {
            return path.substr(root_prefix.size());
        }
        return path;
    };

    RunResult result;

    // Extract symbols and build id -> name map in one pass.
    // Directory symbols are skipped when they represent the fixture_dir itself
    // or any ancestor (e.g., /home, /home/user, ..., /path/to/fixture_dir).
    // A directory is an ancestor if fixture_dir starts with "name/".
    // A directory is the fixture_dir itself if name == fixture_dir.
    auto is_fixture_ancestor_dir = [&](const std::string& name) {
        return name == fixture_dir ||
               (fixture_dir.size() > name.size() &&
                fixture_dir[name.size()] == '/' &&
                fixture_dir.compare(0, name.size(), name) == 0);
    };

    std::unordered_map<int64_t, std::string> id_to_name;
    for (auto& sym : indexer.symbolTable().allSymbols()) {
        if (sym.type == 4 && is_fixture_ancestor_dir(sym.name)) {
            id_to_name[sym.local_id] = sym.name;
            continue;
        }
        result.symbols.push_back({strip_root(sym.name), sym.scope, sym.type});
        id_to_name[sym.local_id] = sym.name;
    }

    // Extract instances, refs, and validate offset ranges
    for (auto& file : indexer.allFiles()) {
        std::string rel_path = strip_root(file.module_path);
        for (auto& inst : file.instances) {
            EXPECT_LE(inst.start_offset, inst.end_offset)
                << "Bad instance range in " << file.module_path
                << ": start=" << inst.start_offset << " > end=" << inst.end_offset;
            auto it = id_to_name.find(inst.symbol_local_id);
            std::string sym_name = (it != id_to_name.end()) ? strip_root(it->second) : "UNKNOWN";
            result.instances.push_back({rel_path, sym_name, inst.instance_type, inst.start_offset, inst.end_offset});
        }
        for (auto& ref : file.refs) {
            EXPECT_LE(ref.from_offset_start, ref.from_offset_end)
                << "Bad ref range in " << file.module_path
                << ": start=" << ref.from_offset_start << " > end=" << ref.from_offset_end;
            auto it = id_to_name.find(ref.to_symbol_local_id);
            std::string to_name = (it != id_to_name.end()) ? it->second : "UNKNOWN";
            result.refs.push_back({rel_path, to_name, ref.from_offset_start, ref.from_offset_end,
                                   static_cast<int>(ref.roles), ref.relation});
        }
    }

    std::sort(result.symbols.begin(), result.symbols.end());
    std::sort(result.refs.begin(), result.refs.end());
    std::sort(result.instances.begin(), result.instances.end());

    return result;
}

// --- Fixture spec ---

struct FixtureSpec {
    std::string name;
    std::vector<std::string> sources;
    std::vector<ExpectedSymbol> symbols;
    std::vector<ExpectedRef> refs;
    std::vector<ExpectedInstance> instances; // empty = skip instance validation
};

void PrintTo(const FixtureSpec& spec, std::ostream* os) {
    *os << spec.name;
}

class IndexerFixtureTest : public testing::TestWithParam<FixtureSpec> {};

TEST_P(IndexerFixtureTest, SymbolsAndRefs) {
    auto result = runFixture(GetParam().name, GetParam().sources);

    auto expected_symbols = GetParam().symbols;
    auto expected_refs = GetParam().refs;
    std::sort(expected_symbols.begin(), expected_symbols.end());
    std::sort(expected_refs.begin(), expected_refs.end());

    EXPECT_EQ(result.symbols, expected_symbols);
    EXPECT_EQ(result.refs, expected_refs);

    // Validate instances when expected values are provided
    auto expected_instances = GetParam().instances;
    if (!expected_instances.empty()) {
        std::sort(expected_instances.begin(), expected_instances.end());
        EXPECT_EQ(result.instances, expected_instances);
    }

}

// Scope constants
constexpr int LOCAL = 1;
constexpr int GLOBAL = 2;

// Type constants (matching proto/index.proto SymbolType enum)
constexpr int FUNCTION = 1;
constexpr int FILETYPE = 2;
constexpr int DIRECTORY = 4;
constexpr int TYPE = 5;
constexpr int DATA = 6;
constexpr int MACRO = 7;
constexpr int FIELD = 8;

INSTANTIATE_TEST_SUITE_P(
    Fixtures,
    IndexerFixtureTest,
    testing::Values(
        FixtureSpec{
            "small_project",
            {"ops.c", "main.c"},
            // symbols
            {
                {"OPS_H", GLOBAL, MACRO},
                {"container", GLOBAL, TYPE},
                {"container.data", GLOBAL, FIELD},
                {"container.ops", GLOBAL, FIELD},
                {"default_ops", LOCAL, DATA},
                {"file_ops", GLOBAL, TYPE},
                {"file_ops.read", GLOBAL, FIELD},
                {"file_ops.write", GLOBAL, FIELD},
                {"main", GLOBAL, FUNCTION},
                {"main.c", GLOBAL, FILETYPE},
                {"my_container", GLOBAL, DATA},
                {"my_read", LOCAL, FUNCTION},
                {"my_write", LOCAL, FUNCTION},
                {"ops.c", GLOBAL, FILETYPE},
                {"ops.h", GLOBAL, FILETYPE},
                {"setup", GLOBAL, FUNCTION},
            },
            // refs (file, symbol, start_offset, end_offset)
            {
                {"main.c", "file_ops", 43, 51},
                {"main.c", "file_ops", 88, 96},
                {"main.c", "setup", 106, 111},
                {"main.c", "file_ops.read", 127, 131},     // MemberRefExpr: ops.read(0,0,0)
                {"ops.c", "file_ops", 207, 215},
                {"ops.c", "my_read", 244, 251},            // .read = my_read: the name token
                {"ops.c", "my_write", 266, 274},           // .write = my_write
                {"ops.c", "file_ops", 330, 338},
                {"ops.c", "file_ops.read", 356, 360},      // MemberRefExpr: ops->read = ...
                {"ops.c", "my_read", 363, 370},            // ops->read = my_read
                {"ops.c", "file_ops.write", 381, 386},     // MemberRefExpr: ops->write = ...
                {"ops.c", "my_write", 389, 397},           // ops->write = my_write
                {"ops.c", "container", 442, 451},
                {"ops.c", "my_read", 490, 497},            // .ops = { .read = my_read }
                {"ops.h", "my_read", 51, 91},              // synthetic: field impl ref (deduped across patterns)
                {"ops.h", "my_write", 97, 144},            // synthetic: field impl ref (deduped across patterns)
                {"ops.h", "file_ops", 180, 188},
            }
        },
        FixtureSpec{
            "enum_basic",
            {"main.c"},
            {
                {"BLUE", GLOBAL, DATA},
                {"COLORS_H", GLOBAL, MACRO},
                {"GREEN", GLOBAL, DATA},
                {"RED", GLOBAL, DATA},
                {"color", GLOBAL, TYPE},
                {"colors.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"pick_color", GLOBAL, FUNCTION},
            },
            {
                {"main.c", "color", 52, 57},
                {"main.c", "GREEN", 80, 85},
                {"main.c", "RED", 62, 65},
            },
            // instances — validates enum constant DATA definitions at correct offsets
            {
                {"colors.h", "BLUE", 1, 59, 63},
                {"colors.h", "COLORS_H", 1, 25, 33},
                {"colors.h", "GREEN", 1, 52, 57},
                {"colors.h", "RED", 1, 47, 50},
                {"colors.h", "color", 1, 34, 65},
                {"colors.h", "colors.h", 7, 0, 74},
                {"main.c", "main.c", 6, 0, 113},
                {"main.c", "pick_color", 1, 20, 112},
            }
        },
        FixtureSpec{
            "typedef_basic",
            {"main.c"},
            {
                {"TYPES_H", GLOBAL, MACRO},
                {"handle", GLOBAL, TYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"make_point", GLOBAL, FUNCTION},
                {"point", GLOBAL, TYPE},
                {"point.x", GLOBAL, FIELD},
                {"point.y", GLOBAL, FIELD},
                {"point_t", GLOBAL, TYPE},
                {"types.h", GLOBAL, FILETYPE},
            },
            {
                {"main.c", "handle", 38, 44},
                {"main.c", "point.x", 71, 72},
                {"main.c", "point.y", 84, 85},
                {"main.c", "point_t", 19, 26},
                {"main.c", "point_t", 54, 61},
                {"types.h", "point", 79, 84},
            }
        },
        FixtureSpec{
            "union_basic",
            {"main.c"},
            {
                {"main.c", GLOBAL, FILETYPE},
                {"set_int", GLOBAL, FUNCTION},
                {"value", GLOBAL, TYPE},
                {"value.f", GLOBAL, FIELD},
                {"value.i", GLOBAL, FIELD},
            },
            {
                {"main.c", "value", 52, 57},
                {"main.c", "value.i", 74, 75},
            }
        },
        FixtureSpec{
            "cross_file_variable",
            {"config.c", "main.c"},
            {
                {"CONFIG_H", GLOBAL, MACRO},
                {"app_name", GLOBAL, DATA},
                {"check", GLOBAL, FUNCTION},
                {"config.c", GLOBAL, FILETYPE},
                {"config.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"verbose", GLOBAL, DATA},
            },
            {
                {"main.c", "verbose", 47, 54},
                {"main.c", "app_name", 82, 90},
            }
        },
        FixtureSpec{
            "static_same_name",
            {"a.c", "b.c"},
            {
                {"a.c", GLOBAL, FILETYPE},
                {"b.c", GLOBAL, FILETYPE},
                {"count", LOCAL, DATA},
                {"count", LOCAL, DATA},
                {"get_a", GLOBAL, FUNCTION},
                {"get_b", GLOBAL, FUNCTION},
                {"helper", LOCAL, FUNCTION},
                {"helper", LOCAL, FUNCTION},
            },
            {
                {"a.c", "count", 56, 61},
                {"a.c", "helper", 90, 96},
                {"b.c", "count", 56, 61},
                {"b.c", "helper", 90, 96},
            }
        },
        FixtureSpec{
            "type_ref_chain",
            {"main.c"},
            {
                {"TYPES_H", GLOBAL, MACRO},
                {"list", GLOBAL, TYPE},
                {"list.head", GLOBAL, FIELD},
                {"list.length", GLOBAL, FIELD},
                {"main.c", GLOBAL, FILETYPE},
                {"node", GLOBAL, TYPE},
                {"node.next", GLOBAL, FIELD},
                {"node.value", GLOBAL, FIELD},
                {"node_ptr", GLOBAL, TYPE},
                {"node_t", GLOBAL, TYPE},
                {"push", GLOBAL, FUNCTION},
                {"types.h", GLOBAL, FILETYPE},
            },
            {
                {"main.c", "list", 36, 40},
                {"main.c", "node_t", 45, 51},
                {"types.h", "node", 64, 68},
                {"types.h", "node", 94, 98},
                {"types.h", "node_t", 115, 121},
                {"types.h", "node_ptr", 147, 155},
            }
        },
        FixtureSpec{
            "enum_in_switch",
            {"handler.c"},
            {
                {"ERR_IO", GLOBAL, DATA},
                {"ERR_MEM", GLOBAL, DATA},
                {"OK", GLOBAL, DATA},
                {"STATUS_H", GLOBAL, MACRO},
                {"handle_status", GLOBAL, FUNCTION},
                {"handler.c", GLOBAL, FILETYPE},
                {"last_error", GLOBAL, DATA},
                {"status", GLOBAL, TYPE},
                {"status.h", GLOBAL, FILETYPE},
            },
            {
                {"handler.c", "status", 43, 49},
                {"handler.c", "ERR_IO", 109, 115},
                {"handler.c", "ERR_IO", 218, 224},
                {"handler.c", "ERR_MEM", 137, 144},
                {"handler.c", "OK", 81, 83},
                {"handler.c", "status", 198, 204},
                {"status.h", "status", 94, 100},
            }
        },
        FixtureSpec{
            "cross_file_macro",
            {"main.c"},
            {
                {"DEFAULT_STATUS", GLOBAL, MACRO},
                {"DEFINE_HANDLER", GLOBAL, MACRO},
                {"MACROS_H", GLOBAL, MACRO},
                {"RESULT", GLOBAL, MACRO},
                {"S_ERR", GLOBAL, DATA},
                {"S_OK", GLOBAL, DATA},
                {"get_result", GLOBAL, FUNCTION},
                {"handle_close", GLOBAL, FUNCTION},
                {"handle_open", GLOBAL, FUNCTION},
                {"macros.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"result", GLOBAL, TYPE},
                {"result.code", GLOBAL, FIELD},
                {"status", GLOBAL, TYPE},
            },
            {
                // The BODY refs: each macro names these itself, in macros.h.
                {"macros.h", "result", 321, 327},
                {"macros.h", "S_OK", 411, 415},
                // The INVOCATION edges, at the macro name span in main.c.
                // These used to name the macro's body symbols instead -- a
                // `main.c -> result` edge for text that says `RESULT`.
                {"main.c", "DEFINE_HANDLER", 102, 116},
                {"main.c", "DEFINE_HANDLER", 156, 170},
                // ...and the two functions those invocations declared, on
                // DEFINE_HANDLER's own name in the header.
                {"macros.h", "handle_open", 187, 201},
                {"macros.h", "handle_close", 187, 201},
                {"main.c", "RESULT", 272, 278},
                {"main.c", "RESULT", 302, 308},
                {"main.c", "DEFAULT_STATUS", 325, 339},
                // Real text in main.c, unaffected.
                {"main.c", "result.code", 318, 322},
            }
        },
        FixtureSpec{
            "macro_struct_in_union",
            {"main.c"},
            {
                {"ALL_TYPES", GLOBAL, MACRO},
                {"ONE_TYPE", GLOBAL, MACRO},
                {"TYPES_H", GLOBAL, MACRO},
                {"any_type", GLOBAL, TYPE},
                {"any_type.__type_a__", GLOBAL, FIELD},
                {"any_type.__type_b__", GLOBAL, FIELD},
                {"any_type.__type_c__", GLOBAL, FIELD},
                {"main.c", GLOBAL, FILETYPE},
                {"type_a", GLOBAL, TYPE},
                {"type_b", GLOBAL, TYPE},
                {"type_c", GLOBAL, TYPE},
                {"types.h", GLOBAL, FILETYPE},
                {"use_any", GLOBAL, FUNCTION},
            },
            // refs — the body refs resolve inside ONE_TYPE's body, and the
            // call site at [398, 407) carries ONE invocation edge naming
            // ALL_TYPES.  It used to carry three, one per symbol the expansion
            // touched, all at the same offsets: three edges from `types.h` to
            // types the text at that position never mentions.
            {
                {"main.c", "any_type", 33, 41},
                // ALL_TYPES's body invokes ONE_TYPE three times: three
                // layerwise edges ALL_TYPES -> ONE_TYPE, from the body text.
                {"types.h", "ONE_TYPE", 277, 285},
                {"types.h", "ONE_TYPE", 300, 308},
                {"types.h", "ONE_TYPE", 323, 331},
                // ...and ONE_TYPE's own body names the types.
                {"types.h", "type_a", 286, 292},
                {"types.h", "type_b", 309, 315},
                {"types.h", "type_c", 332, 338},
                {"types.h", "ALL_TYPES", 398, 407},
                // What the one recorded invocation declared, on the name of
                // the macro that was invoked: three fields of the union
                // somebody typed, and the three structs their declarators
                // introduce.  ONE_TYPE wrote them, but only the outermost
                // invocation is on record.
                {"types.h", "any_type.__type_a__", 261, 270},
                {"types.h", "any_type.__type_b__", 261, 270},
                {"types.h", "any_type.__type_c__", 261, 270},
                {"types.h", "type_a", 261, 270},
                {"types.h", "type_b", 261, 270},
                {"types.h", "type_c", 261, 270},
            },
            // instances — a macro has ONE instance, its `#define`.  Using it
            // is a reference, so ALL_TYPES appears once, not once per use.
            {
                {"main.c", "main.c"},
                {"main.c", "use_any"},
                {"types.h", "ALL_TYPES"},
                {"types.h", "ONE_TYPE"},
                {"types.h", "TYPES_H"},
                {"types.h", "any_type"},
                {"types.h", "any_type.__type_a__"},
                {"types.h", "any_type.__type_b__"},
                {"types.h", "any_type.__type_c__"},
                // The three `struct t` the list forward-declares.  Each ends
                // on the argument `t`, so each used to collapse to zero width
                // and be dropped; they span the invocation `ALL_TYPES`.
                {"types.h", "type_a", 2, 398, 407},
                {"types.h", "type_b", 2, 398, 407},
                {"types.h", "type_c", 2, 398, 407},
                {"types.h", "types.h"},
            }
        },
        // A declaration that ends on a macro argument:
        // - alpha (`struct name`, generated, last token the argument) spans the
        //   invocation `ALL_STRUCTS` -- it used to be zero-width and dropped
        // - beta (source-level StructDecl) has its own range
        // - TypeRefs to BOTH from real source code have proper ranges
        FixtureSpec{
            "macro_zero_width",
            {"main.c"},
            {
                {"ALL_STRUCTS", GLOBAL, MACRO},
                {"ONE_STRUCT", GLOBAL, MACRO},
                {"TYPES_H", GLOBAL, MACRO},
                {"alpha", GLOBAL, TYPE},
                {"beta", GLOBAL, TYPE},
                {"beta.value", GLOBAL, FIELD},
                {"main.c", GLOBAL, FILETYPE},
                {"types.h", GLOBAL, FILETYPE},
                {"use_types", GLOBAL, FUNCTION},
                {"wrapper", GLOBAL, TYPE},
                {"wrapper.alpha_ptr", GLOBAL, FIELD},
                {"x", GLOBAL, DATA},
            },
            {
                // TypeRefs from real source have proper ranges (start < end)
                {"main.c", "alpha", 249, 254},
                {"main.c", "beta", 266, 270},
                // Single-char ref: proves clang returns half-open [start, start+len),
                // so 1-char token gives [289, 290) not [289, 289)
                {"main.c", "x", 289, 290},
                // ALL_STRUCTS's body invokes ONE_STRUCT: the layerwise edge
                // between the two, read from the `#define` body text.
                {"types.h", "ONE_STRUCT", 392, 402},
                // Macro-expanded TypeRef: spelling location resolves inside ONE_STRUCT body
                {"types.h", "alpha", 403, 408},
                // The invocation edge at the ALL_STRUCTS name span.
                {"types.h", "ALL_STRUCTS", 477, 488},
                // What that invocation declared, on ALL_STRUCTS's own name.
                {"types.h", "alpha", 374, 385},
                {"types.h", "wrapper.alpha_ptr", 374, 385},
            },
            {
                {"main.c", "main.c"},
                {"main.c", "use_types"},
                {"main.c", "x"},
                // beta instance present — source-level struct, proper range
                // ALL_STRUCTS once: the `#define`.  Its use is a reference.
                {"types.h", "ALL_STRUCTS"},
                {"types.h", "ONE_STRUCT"},
                {"types.h", "TYPES_H"},
                // alpha: generated, ends on the argument, spans `ALL_STRUCTS`.
                {"types.h", "alpha", 2, 477, 488},
                {"types.h", "beta"},
                {"types.h", "beta.value"},
                {"types.h", "types.h"},
                {"types.h", "wrapper"},
                {"types.h", "wrapper.alpha_ptr"},
            }
        },
        FixtureSpec{
            "shared_header_typedef",
            {"a.c", "b.c"},
            {
                {"TYPES_H", GLOBAL, MACRO},
                {"a.c", GLOBAL, FILETYPE},
                {"b.c", GLOBAL, FILETYPE},
                {"compute_a", GLOBAL, FUNCTION},
                {"make_pair", GLOBAL, FUNCTION},
                {"pair", GLOBAL, TYPE},
                {"pair.first", GLOBAL, FIELD},
                {"pair.second", GLOBAL, FIELD},
                {"pair_t", GLOBAL, TYPE},
                {"result_t", GLOBAL, TYPE},
                {"result_t.quot", GLOBAL, FIELD},
                {"result_t.rem", GLOBAL, FIELD},
                {"types.h", GLOBAL, FILETYPE},
            },
            {
                {"a.c", "result_t", 20, 28},
                {"a.c", "result_t", 52, 60},
                {"a.c", "result_t.quot", 70, 74},
                {"a.c", "result_t.rem", 90, 93},
                {"b.c", "pair_t", 20, 26},
                {"b.c", "pair_t", 57, 63},
                {"b.c", "pair.first", 73, 78},
                {"b.c", "pair.second", 90, 96},
            },
            // instances — validates dedup across 2 TUs sharing types.h
            {
                {"a.c", "a.c"},
                {"a.c", "compute_a"},
                {"b.c", "b.c"},
                {"b.c", "make_pair"},
                {"types.h", "TYPES_H"},
                {"types.h", "pair"},
                {"types.h", "pair.first"},
                {"types.h", "pair.second"},
                {"types.h", "pair_t"},
                {"types.h", "result_t"},
                {"types.h", "result_t.quot"},
                {"types.h", "result_t.rem"},
                {"types.h", "types.h"},
            }
        },
        // Proves that files with only macros (no declarations/refs) still appear
        // in the index via clang_getInclusions callback.
        // Basic macro-through-function chain: greet calls LOG macro, LOG calls puts.
        // LOG expansion ref in main.c; puts call's spelling lands in log.h (#define body).
        FixtureSpec{
            "macro_function_call",
            {"main.c"},
            {
                {"LOG", GLOBAL, MACRO},
                {"greet", GLOBAL, FUNCTION},
                {"log.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"puts", GLOBAL, FUNCTION},
            },
            {
                {"log.h", "puts", 49, 53},
                // The invocation edge.  `puts` is named by LOG's body, in
                // log.h, and appears there -- not here, where the text says
                // `LOG`.
                {"main.c", "LOG", 36, 39},
            }
        },
        // Nested macro chain: handler calls FATAL, which expands through ERR→MSG→output.
        // Only outermost MacroExpansion (FATAL) appears as TU-level child; nested
        // expansions (ERR, MSG) are not exposed by libclang. However, the output()
        // CallExpr's spelling location resolves into MSG's #define body in macros.h,
        // making it a child of MSG by offset containment.
        FixtureSpec{
            "macro_nested",
            {"main.c"},
            {
                {"ERR", GLOBAL, MACRO},
                {"FATAL", GLOBAL, MACRO},
                {"MSG", GLOBAL, MACRO},
                {"handler", GLOBAL, FUNCTION},
                {"macros.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"output", GLOBAL, FUNCTION},
            },
            {
                // The whole chain, one edge per layer, each read from the
                // `#define` body that names it:
                //
                //     handler -> FATAL -> ERR -> MSG -> output
                //
                // `handler -> output` is a TRAVERSAL of those four, not a
                // stored edge.  It used to be stored directly and the three
                // middle hops did not exist at all: libclang reports only the
                // OUTERMOST expansion at a use site, so `ERR` inside FATAL's
                // body is not an expansion anywhere, and `linkMacroBodies`
                // reads it from the text instead.
                {"macros.h", "output", 71, 77},
                {"macros.h", "MSG", 107, 110},
                {"macros.h", "ERR", 138, 141},
                {"main.c", "FATAL", 41, 46},
            }
        },
        // Proves that files with only macros (no declarations/refs) still appear
        // in the index via clang_getInclusions callback.
        FixtureSpec{
            "macro_only_header",
            {"main.c"},
            {
                {"MACROS_H", GLOBAL, MACRO},
                {"MAX_SIZE", GLOBAL, MACRO},
                {"MIN", GLOBAL, MACRO},
                {"buf", GLOBAL, DATA},
                {"macros.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"smallest", GLOBAL, FUNCTION},
            },
            // Using a macro is now visible.  This fixture had NO refs at all:
            // `MAX_SIZE` expands to a constant and `MIN` to an expression over
            // its arguments, so neither body names a symbol and the
            // expansion-site mechanism had nothing to attribute.  The use of
            // the macro itself was invisible to the graph.
            {
                {"main.c", "MAX_SIZE", 29, 37},
                {"main.c", "MIN", 76, 79},
            },
            {
                {"macros.h", "MACROS_H"},
                {"macros.h", "MAX_SIZE"},
                {"macros.h", "MIN"},
                {"macros.h", "macros.h"},
                // No MAX_SIZE / MIN here: main.c USES them, and a use is a
                // reference.  Both are still reachable from main.c through it.
                {"main.c", "buf"},
                {"main.c", "main.c"},
                {"main.c", "smallest"},
            }
        },
        // Macro invocation with a data argument: register_driver(my_drv).
        // The macro expansion site gets an instance so that the data ref
        // (my_drv) is contained within it, enabling containment queries
        // like: macro "register_driver" {data "my_drv"}
        FixtureSpec{
            "macro_expansion_arg",
            {"main.c"},
            {
                {"REGISTER_H", GLOBAL, MACRO},
                {"__exit", LOCAL, FUNCTION},
                {"__init", LOCAL, FUNCTION},
                {"driver_info", GLOBAL, TYPE},
                {"driver_info.id", GLOBAL, FIELD},
                {"main.c", GLOBAL, FILETYPE},
                {"my_drv", LOCAL, DATA},
                {"register.h", GLOBAL, FILETYPE},
                {"register_driver", GLOBAL, MACRO},
            },
            {
                {"main.c", "driver_info", 37, 48},
                // The invocation edge at the register_driver name span.  It
                // used to be two refs at these same offsets, naming
                // `driver_info.id` (from the body) and `my_drv` (the ARGUMENT)
                // -- the second an edge from a macro to text the caller wrote.
                {"main.c", "register_driver", 73, 88},
                // my_drv: spelling ref in the macro argument, owned by the
                // caller because every byte at a call site is the caller's.
                {"main.c", "my_drv", 89, 95},
                // ...and twice: `__init` and `__exit` each read that one
                // token, so each has its reference.
                {"main.c", "my_drv", 89, 95},
                // driver_info.id: spelling refs inside register_driver macro body
                {"register.h", "driver_info.id", 219, 221},
                {"register.h", "driver_info.id", 270, 272},
                // The two functions the invocation declared, on the macro's
                // own name.
                {"register.h", "__init", 153, 168},
                {"register.h", "__exit", 153, 168},
            },
            {
                {"main.c", "__exit"},
                {"main.c", "__init"},
                {"main.c", "main.c"},
                {"main.c", "my_drv"},
                // register_driver's only instance is its `#define`, below in
                // register.h; the invocation here is a reference.
                {"register.h", "REGISTER_H"},
                {"register.h", "driver_info"},
                {"register.h", "driver_info.id"},
                {"register.h", "register.h"},
                {"register.h", "register_driver"},
            }
        },
        FixtureSpec{
            "doc_comments",
            {"main.c"},
            {
                {"add", GLOBAL, FUNCTION},
                {"counter", GLOBAL, DATA},
                {"main.c", GLOBAL, FILETYPE},
                {"mul", GLOBAL, FUNCTION},
                {"no_doc", GLOBAL, FUNCTION},
                {"point", GLOBAL, TYPE},
                {"point.x", GLOBAL, FIELD},
                {"point.y", GLOBAL, FIELD},
            },
            {
                {"main.c", "counter", 305, 312},
                {"main.c", "add", 315, 318},
            },
            {
                {"main.c", "add", 1},
                {"main.c", "add", 10},
                {"main.c", "counter", 1},
                {"main.c", "counter", 10},
                {"main.c", "main.c", 6},
                {"main.c", "mul", 1},
                {"main.c", "no_doc", 1},
                {"main.c", "point", 1},
                {"main.c", "point", 10},
                {"main.c", "point.x", 1},
                {"main.c", "point.y", 1},
            }
        },
        // Proves that doc comments on header declarations produce documentation
        // instances in the header file, not in the .c file where the definition
        // lives.  Reproduces a bug where addDocComment used the definition's
        // file_idx instead of the comment's actual file.
        FixtureSpec{
            "cross_file_doc_comment",
            {"impl.c"},
            {
                {"API_H", GLOBAL, MACRO},
                {"api.h", GLOBAL, FILETYPE},
                {"do_work", GLOBAL, FUNCTION},
                {"impl.c", GLOBAL, FILETYPE},
            },
            {},
            {
                {"api.h", "API_H", 1},
                {"api.h", "api.h", 7},
                {"api.h", "do_work", 2},       // declaration
                // ONE doc comment in the text, so one instance -- and it is in
                // api.h, which is the claim: a definition in impl.c carries the
                // documentation its declaration was given in the header.
                // Both the declaration visit and the definition visit find this
                // same comment, and recording it per VISIT rather than per text
                // listed it twice.
                {"api.h", "do_work", 10},      // documentation
                {"impl.c", "do_work", 1},      // definition — no documentation here
                {"impl.c", "impl.c", 6},
            }
        },
        // Proves that symbols are still indexed when a translation unit
        // contains clang diagnostic errors.  The header has an undeclared
        // identifier inside an inline function body — clang emits an error
        // but still builds the AST.  All symbols (before and after the
        // error) must appear in the index.  Mirrors real-world cases like
        // __builtin_counted_by_ref errors in linux/slab.h.
        FixtureSpec{
            "diagnostic_error",
            {"main.c"},
            {
                {"ERRORS_H", GLOBAL, MACRO},
                {"broken_function", LOCAL, FUNCTION},
                {"config", GLOBAL, TYPE},
                {"config.debug", GLOBAL, FIELD},
                {"config.verbose", GLOBAL, FIELD},
                {"errors.h", GLOBAL, FILETYPE},
                {"global_cfg", GLOBAL, DATA},
                {"global_opts", GLOBAL, DATA},
                {"main", GLOBAL, FUNCTION},
                {"main.c", GLOBAL, FILETYPE},
                {"options", GLOBAL, TYPE},
                {"options.count", GLOBAL, FIELD},
                {"options.name", GLOBAL, FIELD},
                {"setup", GLOBAL, FUNCTION},
            },
            {
                {"errors.h", "config", 434, 440},
                {"errors.h", "config", 570, 576},
                {"main.c", "config", 28, 34},
                {"main.c", "config", 94, 100},
                {"main.c", "config.debug", 140, 145},
                {"main.c", "config.verbose", 118, 125},
                {"main.c", "global_cfg", 182, 192},
                {"main.c", "global_opts", 199, 210},
                {"main.c", "options", 54, 61},
                {"main.c", "options.count", 211, 216},
                {"main.c", "setup", 175, 180},
            }
        },
        // Proves that macro arguments become direct children of the enclosing
        // scope (via spelling ref outside the clamped macro instance) AND
        // has-children of the macro expansion (via expansion-site ref inside
        // the clamped macro instance).
        // some_macro(bar) inside foo:
        //   foo [35, ?)
        //     some_macro [56, 66)          ← expansion instance (name-only)
        //       bar        [56, 66)        ← expansion ref (inside macro)
        //       do_something [56, 66)      ← expansion ref (inside macro)
        //       GLOBAL_VARIABLE [56, 66)   ← expansion ref (inside macro)
        //     bar [67, 70)                 ← spelling ref (outside macro, child of foo)
        FixtureSpec{
            "macro_arg_direct",
            {"main.c"},
            {
                {"GLOBAL_VARIABLE", GLOBAL, DATA},
                {"bar", GLOBAL, DATA},
                {"do_something", GLOBAL, FUNCTION},
                {"foo", GLOBAL, FUNCTION},
                {"macros.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"some_macro", GLOBAL, MACRO},
            },
            {
                // The body refs, in macros.h, where some_macro names them.
                {"macros.h", "do_something", 86, 98},
                {"macros.h", "GLOBAL_VARIABLE", 105, 120},
                // ONE invocation edge at the some_macro name span [56, 66).
                // There used to be three here, all at these offsets: two
                // repeating the body refs above, and `bar` -- the argument,
                // which the caller wrote and which already has its own ref
                // below at its real position.
                {"main.c", "some_macro", 56, 66},
                // Spelling ref: bar in macro argument (direct child of foo)
                {"main.c", "bar", 67, 70},
            },
            {
                {"macros.h", "GLOBAL_VARIABLE", 2},
                {"macros.h", "do_something", 2},
                {"macros.h", "macros.h", 7},
                {"macros.h", "some_macro", 1},
                {"main.c", "bar", 1},
                {"main.c", "foo", 1},
                {"main.c", "main.c", 6},
                // Expansion instance clamped to macro name (not full call)
                // The expansion spans the WHOLE invocation `some_macro(bar)`,
                // not just the name.  It used to be clamped to `[56,66)` so
                // that `bar` fell outside it and became a child of the
                // enclosing scope BY RANGE; parentage comes from the DFS
                // interval now, so the clamp only discarded the span that
                // records what the author wrote.  What keeps `bar`'s text the
                // caller's is that a folded instance owns no text.
            }
        },
        // Validates fallback logic for anonymous/typedef enums vs named enums.
        // Anonymous and typedef-with-unnamed-inner enums produce DATA symbols;
        // named enums produce FIELD symbols with compound names.
        FixtureSpec{
            "enum_anonymous",
            {"main.c"},
            {
                {"ANON_A", GLOBAL, DATA},
                {"ANON_B", GLOBAL, DATA},
                {"N_ONE", GLOBAL, DATA},
                {"N_TWO", GLOBAL, DATA},
                {"TD_X", GLOBAL, DATA},
                {"TD_Y", GLOBAL, DATA},
                {"main.c", GLOBAL, FILETYPE},
                {"named", GLOBAL, TYPE},
                {"td_enum", GLOBAL, TYPE},
                {"use_enums", GLOBAL, FUNCTION},
            },
            {
                {"main.c", "ANON_A", 305, 311},
                {"main.c", "named", 344, 349},
                {"main.c", "N_ONE", 354, 359},
                {"main.c", "td_enum", 317, 324},
                {"main.c", "TD_X", 329, 333},
            },
            // instances — validates DATA definitions for all enum constants
            {
                {"main.c", "ANON_A", 1, 65, 71},
                {"main.c", "ANON_B", 1, 73, 79},
                {"main.c", "N_ONE", 1, 254, 259},
                {"main.c", "N_TWO", 1, 261, 266},
                {"main.c", "TD_X", 1, 163, 167},
                {"main.c", "TD_Y", 1, 169, 173},
                {"main.c", "main.c", 6, 0, 385},
                {"main.c", "named", 1, 241, 268},
                {"main.c", "td_enum", 1, 148, 183},
                {"main.c", "use_enums", 1, 271, 384},
            }
        },
        // Validates that typedef'd function pointers in struct fields are
        // recognized as FIELD symbols (requires resolving typedefs via
        // clang_getCanonicalType before checking for pointer-to-function).
        FixtureSpec{
            "typedef_func_ptr",
            {"main.c"},
            {
                {"OPS_H", GLOBAL, MACRO},
                {"default_ops", LOCAL, DATA},
                {"iface_ops", GLOBAL, TYPE},
                {"iface_ops.flags", GLOBAL, FIELD},
                {"iface_ops.read", GLOBAL, FIELD},
                {"iface_ops.write", GLOBAL, FIELD},
                {"main.c", GLOBAL, FILETYPE},
                {"my_read", LOCAL, FUNCTION},
                {"my_write", LOCAL, FUNCTION},
                {"ops.h", GLOBAL, FILETYPE},
                {"read_func_t", GLOBAL, TYPE},
                {"use", GLOBAL, FUNCTION},
                {"write_func_t", GLOBAL, TYPE},
            },
            {
                {"main.c", "iface_ops", 170, 179},
                {"main.c", "iface_ops", 276, 285},
                {"main.c", "iface_ops.read", 303, 307},     // MemberRefExpr: ops->read(...)
                {"main.c", "iface_ops.write", 327, 332},    // MemberRefExpr: ops->write(...)
                {"main.c", "my_read", 208, 215},            // .read = my_read: the name token
                {"main.c", "my_write", 230, 238},           // .write = my_write
                {"ops.h", "my_read", 174, 191},              // field impl ref
                {"ops.h", "my_write", 197, 215},             // field impl ref
                {"ops.h", "read_func_t", 174, 185},          // TypeRef in field decl
                {"ops.h", "write_func_t", 197, 209},         // TypeRef in field decl
            },
        },
        // Validates that positional (non-designated) struct initializers create
        // field impl refs by matching init list children to fields by position.
        FixtureSpec{
            "positional_init_func_ptr",
            {"main.c"},
            {
                {"OPS_H", GLOBAL, MACRO},
                {"default_ops", LOCAL, DATA},
                {"iface_ops", GLOBAL, TYPE},
                {"iface_ops.read", GLOBAL, FIELD},
                {"iface_ops.write", GLOBAL, FIELD},
                {"main.c", GLOBAL, FILETYPE},
                {"my_read", LOCAL, FUNCTION},
                {"my_write", LOCAL, FUNCTION},
                {"ops.h", GLOBAL, FILETYPE},
                {"read_func_t", GLOBAL, TYPE},
                {"write_func_t", GLOBAL, TYPE},
            },
            {
                {"main.c", "iface_ops", 170, 179},
                {"main.c", "my_read", 200, 207},              // positional init funcPtrRef
                {"main.c", "my_write", 213, 221},             // positional init funcPtrRef
                {"ops.h", "my_read", 174, 191},               // field impl ref (positional)
                {"ops.h", "my_write", 197, 215},              // field impl ref (positional)
                {"ops.h", "read_func_t", 174, 185},           // TypeRef in field decl
                {"ops.h", "write_func_t", 197, 209},          // TypeRef in field decl
            },
        },
        // The layerwise model stated as a symbol and reference set.  Three
        // references, one per layer boundary -- see
        // `ObjectRoots.MacroEdgesAreLayerwise` for the OWNERS, which is what
        // makes them edges.
        FixtureSpec{
            "macro_layerwise",
            {"main.c"},
            {
                {"DEV_PRINT", GLOBAL, MACRO},
                {"LEVEL", GLOBAL, MACRO},
                {"LOG_H", GLOBAL, MACRO},
                {"handler", GLOBAL, FUNCTION},
                {"log.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"printk", GLOBAL, FUNCTION},
                {"threshold", GLOBAL, DATA},
            },
            {
                // The body: DEV_PRINT names printk, in log.h.
                {"log.h", "printk", 178, 184},
                // The invocations, at the macro name span.
                {"main.c", "DEV_PRINT", 59, 68},
                {"main.c", "DEV_PRINT", 85, 94},
                // The arguments, at their own positions -- the caller wrote
                // them, so the caller owns them, and no edge runs from
                // DEV_PRINT to either.  `LEVEL` is the interesting one: it is
                // a macro, so it has an expansion of its own, nested by RANGE
                // inside DEV_PRINT's.
                {"main.c", "threshold", 69, 78},
                {"main.c", "LEVEL", 95, 100},
            },
        },
        // What each reference states: the roles of its token, or the
        // relation the row is.  (The tests under "What a reference says about
        // its target" ask the same by the text at each token.)
        FixtureSpec{
            "reference_roles",
            {"main.c"},
            {
                {"BODY_CALL", GLOBAL, MACRO},
                {"CALL_AND_KEEP", GLOBAL, MACRO},
                {"DEFINE_COUNTER", GLOBAL, MACRO},
                {"MODE_A", GLOBAL, DATA},
                {"MODE_B", GLOBAL, DATA},
                {"OPS_H", GLOBAL, MACRO},
                {"USE", GLOBAL, MACRO},
                {"both", GLOBAL, FUNCTION},
                {"either", GLOBAL, FUNCTION},
                {"file_ops", GLOBAL, TYPE},
                {"file_ops.read", GLOBAL, FIELD},
                {"file_ops.write", GLOBAL, FIELD},
                {"fn_t", GLOBAL, TYPE},
                {"helper", GLOBAL, FUNCTION},
                {"hits", GLOBAL, DATA},
                {"keep", GLOBAL, FUNCTION},
                {"main.c", GLOBAL, FILETYPE},
                {"mode", GLOBAL, TYPE},
                {"my_read", GLOBAL, FUNCTION},
                {"ops", LOCAL, DATA},
                {"ops.h", GLOBAL, FILETYPE},
                {"other", GLOBAL, FUNCTION},
                {"pick", GLOBAL, FUNCTION},
                {"run", GLOBAL, FUNCTION},
                {"two_units", GLOBAL, FUNCTION},
                {"unlabelled", GLOBAL, FUNCTION},
            },
            // refs (file, symbol, start, end, roles, relation)
            {
                {"main.c", "DEFINE_COUNTER", 252, 266, 0, 0},             // DEFINE_COUNTER(hits);
                {"main.c", "file_ops", 289, 297, 0, 0},                   // static struct file_ops ops = {
                {"main.c", "my_read", 318, 325, TAKE, 0},                 // .read = my_read,
                {"main.c", "fn_t", 358, 362, 0, 0},                       // fn_t pick(void) { return other; }
                {"main.c", "other", 383, 388, TAKE, 0},                   // fn_t pick(void) { return other; }
                {"main.c", "mode", 406, 410, 0, 0},                       // int run(enum mode m)
                {"main.c", "MODE_A", 442, 448, TAKE, 0},                  // case MODE_A:
                {"main.c", "helper", 465, 471, CALL, 0},                  // return helper(1);
                {"main.c", "keep", 514, 518, CALL, 0},                    // keep(helper);
                {"main.c", "helper", 519, 525, TAKE, 0},                  // keep(helper);
                {"main.c", "CALL_AND_KEEP", 539, 552, 0, 0},              // return CALL_AND_KEEP(both) + BODY_CALL();
                {"main.c", "both", 553, 557, CALL | TAKE, 0},             // return CALL_AND_KEEP(both) + BODY_CALL();
                {"main.c", "BODY_CALL", 561, 570, 0, 0},                  // return CALL_AND_KEEP(both) + BODY_CALL();
                {"main.c", "hits", 604, 608, 0, 0},                       // hits++;
                {"main.c", "ops", 623, 626, 0, 0},                        // return ops.read(2);
                {"main.c", "file_ops.read", 627, 631, 0, 0},              // return ops.read(2);
                {"main.c", "USE", 668, 671, 0, 0},                        // int two_units(void) { return USE(either); }
                {"main.c", "either", 672, 678, TAKE, 0},                  // int two_units(void) { return USE(either); }
                {"ops.h", "my_read", 51, 67, 0, HOLDS},                   // int (*read)(int);
                {"ops.h", "hits", 131, 145, 0, DECLARES},                 // #define DEFINE_COUNTER(name) int name = 0
                {"ops.h", "keep", 200, 204, CALL, 0},                     // #define CALL_AND_KEEP(fn) (fn(1) + keep(fn))
                {"ops.h", "helper", 230, 236, CALL, 0},                   // #define BODY_CALL() helper(3)
                {"ops.h", "keep", 300, 304, CALL, 0},                     // #define USE(fn) keep(fn)
            }
        }
    ),
    [](const testing::TestParamInfo<FixtureSpec>& info) {
        return info.param.name;
    }
);

// --- Absolute path test ---
// When compile_commands.json uses a relative -I flag (e.g. -Ilib/include),
// clang may report included headers with paths relative to the compilation
// directory.  All filesystem_path values must still be absolute.
// This reproduces the nccl bug where object 79 had filesystem_path
// "transport/net_ib/gdaki/..." instead of an absolute path.
TEST(AbsolutePaths, RelativeIncludePath) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/relative_include";
    std::string src_dir = fixture_dir + "/src";

    TempDir temp_dir = makeTempDir();
    ASSERT_FALSE(temp_dir.path.empty());

    // Key: use relative -I flag, just like nccl's compile_commands.json
    // uses "-Itransport/net_ib/gdaki/doca-gpunetio/include"
    {
        std::ofstream out(temp_dir.path + "/compile_commands.json");
        out << "[\n"
            << "  {\n"
            << "    \"directory\": \"" << src_dir << "\",\n"
            << "    \"command\": \"cc -c -Ilib/include main.c -o main.o\",\n"
            << "    \"file\": \"" << src_dir << "/main.c\"\n"
            << "  }\n"
            << "]\n";
    }

    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();

    for (auto& file : indexer.allFiles()) {
        EXPECT_FALSE(file.filesystem_path.empty())
            << "filesystem_path is empty for object " << file.object_local_id;
        EXPECT_EQ(file.filesystem_path[0], '/')
            << "filesystem_path is not absolute: \"" << file.filesystem_path
            << "\" for object " << file.object_local_id;
    }
}

// --- Symlink deduplication test ---
// When two TUs include the same header through different paths (one via a
// symlink), clang_getFileName returns the path that was used to find the file
// — producing two different names for the same physical file.  The indexer
// must resolve both to the same canonical (real) path so the header appears
// exactly once.  This is the root cause of the OMPI tree-view bug where
// headers appeared under multiple bogus directories.
TEST(AbsolutePaths, SymlinkIncludeDedup) {
    TempDir temp_dir = makeTempDir();
    ASSERT_FALSE(temp_dir.path.empty());

    std::string project = temp_dir.path + "/project";
    fs::create_directories(project + "/lib");

    // helper.h — a small header declaring one function
    {
        std::ofstream out(project + "/lib/helper.h");
        out << "#ifndef HELPER_H\n#define HELPER_H\n"
            << "int helper_fn(int x);\n"
            << "#endif\n";
    }

    // Create a symlink: project/alt -> project/lib
    fs::create_directory_symlink(project + "/lib", project + "/alt");

    // a.c includes helper.h through the real path (-I lib)
    {
        std::ofstream out(project + "/a.c");
        out << "#include \"helper.h\"\n"
            << "int a(void) { return helper_fn(1); }\n";
    }

    // b.c includes the same helper.h through the symlink (-I alt)
    {
        std::ofstream out(project + "/b.c");
        out << "#include \"helper.h\"\n"
            << "int b(void) { return helper_fn(2); }\n";
    }

    // Write compile_commands.json — two TUs with different -I paths
    {
        std::ofstream out(temp_dir.path + "/compile_commands.json");
        out << "[\n"
            << "  {\n"
            << "    \"directory\": \"" << project << "\",\n"
            << "    \"command\": \"cc -c -Ilib a.c -o a.o\",\n"
            << "    \"file\": \"" << project << "/a.c\"\n"
            << "  },\n"
            << "  {\n"
            << "    \"directory\": \"" << project << "\",\n"
            << "    \"command\": \"cc -c -Ialt b.c -o b.o\",\n"
            << "    \"file\": \"" << project << "/b.c\"\n"
            << "  }\n"
            << "]\n";
    }

    Indexer indexer("test", temp_dir.path, project, 1);
    indexer.run();

    // Collect all file paths
    std::vector<std::string> paths;
    for (auto& file : indexer.allFiles()) {
        paths.push_back(file.filesystem_path);
    }
    std::sort(paths.begin(), paths.end());

    // helper.h must appear exactly once, via its real path (through lib/, not alt/)
    std::string real_helper = project + "/lib/helper.h";
    std::vector<std::string> helper_paths;
    for (auto& p : paths) {
        if (p.find("helper.h") != std::string::npos)
            helper_paths.push_back(p);
    }
    ASSERT_EQ(helper_paths.size(), 1u)
        << "helper.h should appear exactly once; got " << helper_paths.size() << ":";
    EXPECT_EQ(helper_paths[0], real_helper);

    // The symlink path must NOT appear
    std::string symlink_helper = project + "/alt/helper.h";
    EXPECT_EQ(std::count(paths.begin(), paths.end(), symlink_helper), 0)
        << "symlink path " << symlink_helper << " should not appear in file list";
}

// --- canonicalizePath tests ---

TEST(CanonicalizePath, ResolvesDotDot) {
    EXPECT_EQ(canonicalizePath("/a/b/../c"), "/a/c");
}

TEST(CanonicalizePath, ResolvesDot) {
    EXPECT_EQ(canonicalizePath("/a/./b"), "/a/b");
}

TEST(CanonicalizePath, ResolvesMultipleDotDot) {
    EXPECT_EQ(canonicalizePath("/a/b/../../c"), "/c");
}

TEST(CanonicalizePath, DotDotAtRoot) {
    EXPECT_EQ(canonicalizePath("/../a"), "/a");
}

TEST(CanonicalizePath, RealWorldBugReport) {
    EXPECT_EQ(
        canonicalizePath("/home/user/project/common/mmu/../../include/header.h"),
        "/home/user/project/include/header.h"
    );
}

// ========== CompilationDB tests ==========

// Write a single-entry compile_commands.json using the arguments array format.
static void writeCompileCommands(const std::string& dir, const std::string& compiler,
                                 const std::string& file,
                                 const std::vector<std::string>& extra_args) {
    std::ofstream out(dir + "/compile_commands.json");
    out << "[{\"directory\":\"" << dir << "\","
        << "\"arguments\":[\"" << compiler << "\",\"-c\"";
    for (auto& arg : extra_args) out << ",\"" << arg << "\"";
    out << ",\"-o\",\"out.o\",\"" << file << "\"],"
        << "\"file\":\"" << file << "\"}]\n";
}

// Strip -isystem pairs from args (host-dependent, not under our control).
static std::vector<std::string> stripSystemIncludes(const std::vector<std::string>& args) {
    std::vector<std::string> result;
    for (size_t i = 0; i < args.size(); i++) {
        if (args[i] == "-isystem") { i++; continue; }
        result.push_back(args[i]);
    }
    return result;
}

static bool hasArg(const std::vector<std::string>& args, const std::string& val) {
    return std::find(args.begin(), args.end(), val) != args.end();
}

static bool hasArgPair(const std::vector<std::string>& args,
                       const std::string& a, const std::string& b) {
    for (size_t i = 0; i + 1 < args.size(); i++) {
        if (args[i] == a && args[i + 1] == b) return true;
    }
    return false;
}

TEST(CompilationDB, FiltersGccFlags) {
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "gcc", "/tmp/test.c",
        {"-I.", "-fno-var-tracking-assignments", "-mrecord-mcount", "-O2"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);
    EXPECT_TRUE(hasArg(args, "-I."));
    EXPECT_TRUE(hasArg(args, "-O2"));
    EXPECT_FALSE(hasArg(args, "-fno-var-tracking-assignments"));
    EXPECT_FALSE(hasArg(args, "-mrecord-mcount"));
}

TEST(CompilationDB, FiltersWarnings) {
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "gcc", "/tmp/test.c",
        {"-I.", "-Wall", "-Wextra", "-Werror"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);
    EXPECT_TRUE(hasArg(args, "-I."));
    EXPECT_FALSE(hasArg(args, "-Wall"));
    EXPECT_FALSE(hasArg(args, "-Wextra"));
    EXPECT_FALSE(hasArg(args, "-Werror"));
}

TEST(CompilationDB, FiltersGccPrefixes) {
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "gcc", "/tmp/test.c",
        {"-I.", "-fplugin=some_plugin.so", "-mabi=lp64"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);
    EXPECT_TRUE(hasArg(args, "-I."));
    EXPECT_FALSE(hasArg(args, "-fplugin=some_plugin.so"));
    EXPECT_FALSE(hasArg(args, "-mabi=lp64"));
}

TEST(CompilationDB, FiltersGccPluginDefines) {
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "gcc", "/tmp/test.c",
        {"-DLATENT_ENTROPY_PLUGIN", "-DRANDSTRUCT_PLUGIN", "-DMODULE", "-DFOO=1"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);
    EXPECT_FALSE(hasArg(args, "-DLATENT_ENTROPY_PLUGIN"));
    EXPECT_FALSE(hasArg(args, "-DRANDSTRUCT_PLUGIN"));
    // Non-plugin defines should survive
    EXPECT_TRUE(hasArg(args, "-DMODULE"));
    EXPECT_TRUE(hasArg(args, "-DFOO=1"));
}

TEST(CompilationDB, NvccBasicAdaptation) {
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "/usr/local/cuda/bin/nvcc", "/tmp/test.cu",
        {"-std=c++14", "-I.",
         "--expt-extended-lambda",
         "-Xcompiler", "-fPIC",
         "-ccbin", "g++",
         "-gencode=arch=compute_80,code=sm_80",
         "-dc", "-dw"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);

    // CUDA flags added
    EXPECT_TRUE(hasArgPair(args, "-x", "cuda"));
    EXPECT_TRUE(hasArg(args, "--cuda-path=/usr/local/cuda"));
    EXPECT_TRUE(hasArg(args, "--cuda-gpu-arch=sm_80"));
    EXPECT_TRUE(hasArg(args, "--no-cuda-version-check"));
    EXPECT_TRUE(hasArg(args, "-fcuda-rdc"));
    EXPECT_TRUE(hasArgPair(args, "-include", "new"));

    // -std promotion: c++14 → gnu++14
    EXPECT_FALSE(hasArg(args, "-std=c++14"));
    EXPECT_TRUE(hasArg(args, "-std=gnu++14"));

    // NVCC-specific flags stripped
    EXPECT_FALSE(hasArg(args, "--expt-extended-lambda"));
    EXPECT_FALSE(hasArg(args, "-dc"));
    EXPECT_FALSE(hasArg(args, "-dw"));
    EXPECT_FALSE(hasArg(args, "-Xcompiler"));
    EXPECT_FALSE(hasArg(args, "-fPIC"));
    EXPECT_FALSE(hasArg(args, "-ccbin"));
    EXPECT_FALSE(hasArg(args, "-gencode=arch=compute_80,code=sm_80"));

    // Regular flags survive
    EXPECT_TRUE(hasArg(args, "-I."));
}

TEST(CompilationDB, NvccFlagPairs) {
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "/usr/local/cuda/bin/nvcc", "/tmp/test.cu",
        {"-std=c++14",
         "-Xptxas", "-v",
         "-Xfatbin", "--compress-all",
         "--compiler-options", "-fvisibility=hidden"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);

    // All pair flags and their values stripped
    EXPECT_FALSE(hasArg(args, "-Xptxas"));
    EXPECT_FALSE(hasArg(args, "-v"));
    EXPECT_FALSE(hasArg(args, "-Xfatbin"));
    EXPECT_FALSE(hasArg(args, "--compress-all"));
    EXPECT_FALSE(hasArg(args, "--compiler-options"));
    EXPECT_FALSE(hasArg(args, "-fvisibility=hidden"));
}

TEST(CompilationDB, NvccCudaPathDerivation) {
    auto tmp = makeTempDir();
    // Non-standard CUDA install path
    writeCompileCommands(tmp.path, "/opt/nvidia/cuda-12.0/bin/nvcc", "/tmp/test.cu",
        {"-std=c++17"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);
    EXPECT_TRUE(hasArg(args, "--cuda-path=/opt/nvidia/cuda-12.0"));
    EXPECT_TRUE(hasArg(args, "-std=gnu++17"));
}

TEST(CompilationDB, NvccGpuArchDefault) {
    auto tmp = makeTempDir();
    // No -gencode → default sm_70
    writeCompileCommands(tmp.path, "/opt/cuda/bin/nvcc", "/tmp/test.cu",
        {"-std=c++14"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);
    EXPECT_TRUE(hasArg(args, "--cuda-gpu-arch=sm_70"));
}

TEST(CompilationDB, NvccAlsoFiltersGccFlags) {
    // NVCC TUs should have BOTH nvcc and gcc flags filtered
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "/usr/local/cuda/bin/nvcc", "/tmp/test.cu",
        {"-std=c++14", "-fno-var-tracking-assignments", "--expt-extended-lambda"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);
    EXPECT_FALSE(hasArg(args, "-fno-var-tracking-assignments"));  // gcc filter
    EXPECT_FALSE(hasArg(args, "--expt-extended-lambda"));          // nvcc filter
}

TEST(CompilationDB, SkipsAssemblyFiles) {
    auto tmp = makeTempDir();
    {
        std::ofstream out(tmp.path + "/compile_commands.json");
        out << "["
            << "{\"directory\":\"" << tmp.path << "\","
            << "\"arguments\":[\"gcc\",\"-c\",\"/tmp/code.c\",\"-o\",\"code.o\"],"
            << "\"file\":\"/tmp/code.c\"},"
            << "{\"directory\":\"" << tmp.path << "\","
            << "\"arguments\":[\"gcc\",\"-c\",\"/tmp/boot.S\",\"-o\",\"boot.o\"],"
            << "\"file\":\"/tmp/boot.S\"},"
            << "{\"directory\":\"" << tmp.path << "\","
            << "\"arguments\":[\"gcc\",\"-c\",\"/tmp/entry.s\",\"-o\",\"entry.o\"],"
            << "\"file\":\"/tmp/entry.s\"}"
            << "]\n";
    }
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    EXPECT_EQ(db.commands()[0].filename, "/tmp/code.c");
}

TEST(CompilationDB, StripsOutputAndSourceFile) {
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "gcc", "/tmp/test.c", {"-I.", "-O2"});
    CompilationDB db(tmp.path);
    ASSERT_EQ(db.commands().size(), 1u);
    auto args = stripSystemIncludes(db.commands()[0].args);
    // -c, -o, out.o, and the source file should all be stripped
    EXPECT_FALSE(hasArg(args, "-c"));
    EXPECT_FALSE(hasArg(args, "-o"));
    EXPECT_FALSE(hasArg(args, "out.o"));
    EXPECT_FALSE(hasArg(args, "/tmp/test.c"));
}

TEST(CompilationDB, KbuildModuleName) {
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "gcc", "/tmp/test.c",
        {"-DMODULE", "-D__KBUILD_MODNAME=my_driver"});
    CompilationDB db(tmp.path, "kbuild");
    ASSERT_EQ(db.commands().size(), 1u);
    EXPECT_EQ(db.commands()[0].modname, "my_driver");
}

TEST(CompilationDB, KbuildNoModuleDefine) {
    // Without -DMODULE, modname should be empty even with __KBUILD_MODNAME
    auto tmp = makeTempDir();
    writeCompileCommands(tmp.path, "gcc", "/tmp/test.c",
        {"-D__KBUILD_MODNAME=my_driver"});
    CompilationDB db(tmp.path, "kbuild");
    ASSERT_EQ(db.commands().size(), 1u);
    EXPECT_TRUE(db.commands()[0].modname.empty());
}

// --- Object root intervals -----------------------------------------------
//
// Containment is `parent.dfs_enter < child.dfs_enter < parent.dfs_exit`, so an
// object whose root instance is left at [0,0) can contain nothing at all --
// silently, since a zero-width interval is not an error anywhere.  Two kinds
// of object bypass Stage1's numbering walk and used to end up that way: files
// pulled in by --include-git-files (their file instance comes from
// Indexer::addFile) and directory sentinels (built from the symbol table).

TEST(ObjectRoots, UnparsedFileGetsNonEmptyRootInterval) {
    SymbolTable symbols;
    const std::string path = "/proj/README.md";
    const std::string text = "hello\n";

    auto [file_sym, _] = symbols.getOrCreate(path, SCOPE_GLOBAL, SYMTYPE_FILE);

    FileData fd;
    fd.object_local_id = 1;
    fd.module_path = path;
    fd.filesystem_path = path;
    fd.filetype = "documentation";
    fd.content.assign(text.begin(), text.end());
    // Exactly what Indexer::addFile builds: four fields, no DFS numbers.
    fd.instances.push_back(
        {file_sym, 0, static_cast<int32_t>(text.size()), INSTTYPE_FILE});
    ASSERT_EQ(fd.instances[0].dfs_exit, 0) << "fixture must start unnumbered";

    auto built = ProtoBuilder::build("test", "/proj", symbols, {fd}, 1);
    askl::index::Project project;
    ASSERT_TRUE(project.ParseFromString(built.project_data));
    ASSERT_GE(project.objects_size(), 1);

    const auto& root = project.objects(0).symbol_instances(0);
    EXPECT_GT(root.dfs_exit(), root.dfs_enter())
        << "an unparsed file's root must be able to contain something";
}

TEST(ObjectRoots, EveryObjectInARealProjectIsNumbered) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/small_project";
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"ops.c", "main.c"});

    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();

    auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                     indexer.allFiles(), 1);
    askl::index::Project project;
    ASSERT_TRUE(project.ParseFromString(built.project_data));
    ASSERT_GT(project.objects_size(), 0);

    for (const auto& obj : project.objects()) {
        ASSERT_GT(obj.symbol_instances_size(), 0)
            << "object " << obj.module_path() << " has no instances";
        const auto& root = obj.symbol_instances(0);
        EXPECT_GT(root.dfs_exit(), root.dfs_enter())
            << "degenerate root interval on " << obj.module_path()
            << " (filetype " << obj.filetype() << ")";

        // A ref is a point in the same DFS order, and the engine finds it by
        // `container.dfs_enter < ref.dfs_enter < container.dfs_exit`.  A ref
        // outside every interval is as invisible as an unnumbered instance.
        for (const auto& ref : obj.refs()) {
            bool inside = false;
            for (const auto& inst : obj.symbol_instances())
                inside = inside || (inst.dfs_enter() < ref.dfs_enter() &&
                                    ref.dfs_enter() < inst.dfs_exit());
            EXPECT_TRUE(inside)
                << "ref at dfs_enter " << ref.dfs_enter() << " on "
                << obj.module_path() << " lies outside every instance interval";
        }
    }
}

// The invariants askld's `validate_object_tree` enforces at upload, checked
// here at the producer so the two cannot drift.  Every one of them was a real
// bug at some point: zero-width intervals (three call sites that never stamped
// an exit), unnumbered roots and refs, and a nesting_level that disagreed with
// the interval.  An index violating any of them is REJECTED at upload, so this
// test is what stands between a change here and a reindex that cannot be
// uploaded.
// A macro expansion belongs to the function it is written in.
//
// `collectMacros` runs before the AST walk with an empty scope stack, so an
// expansion used to be numbered at file level -- in `macro_nested/main.c`,
// `handler` got [4096,5120) while `FATAL("boom")` inside it got [2048,3072),
// a SIBLING numbered earlier.  `func("handler") { macro }` could not work, and
// only `mod { macro { func } }` did, because that route goes mod -> file ->
// macro.  Deriving parentage from ranges fixes it; this pins that it stays
// fixed, since the cause (a pre-pass with no stack) is still there.
// `func { macro }` and `mod("amdgpu") { macro }` must keep working now that a
// macro use is a REFERENCE rather than an instance -- which means the
// invocation reference has to land inside the enclosing function, at one level
// down, exactly where the expansion instance used to be numbered.
//
// This is the same mechanism as `func("main") { "printk" }`: printk has no
// instance in main either.  Containment reaches a symbol through a reference
// sitting inside the scope, so the deleted instance was never what made the
// query work.
TEST(ObjectRoots, AMacroInvocationIsInsideItsEnclosingFunction) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/macro_nested";
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"main.c"});
    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();
    auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                     indexer.allFiles(), 1);
    askl::index::Project project;
    ASSERT_TRUE(project.ParseFromString(built.project_data));

    // Every MACRO symbol, by local id, so a reference can be identified as an
    // invocation rather than by guessing from its range.
    std::set<int64_t> macro_ids;
    for (const auto& sym : project.symbols())
        if (sym.type() == askl::index::SymbolType::MACRO)
            macro_ids.insert(sym.local_id());
    ASSERT_FALSE(macro_ids.empty()) << "fixture must define a macro";

    bool checked = false;
    for (const auto& obj : project.objects()) {
        if (obj.module_path().find("main.c") == std::string::npos) continue;

        // No expansion instance survives anywhere.
        for (const auto& i : obj.symbol_instances())
            EXPECT_NE(i.instance_type(), askl::index::InstanceType::EXPANSION)
                << "a macro use is a reference, not an instance";

        const askl::index::SymbolInstance* func = nullptr;
        for (const auto& i : obj.symbol_instances())
            if (i.instance_type() == askl::index::InstanceType::DEFINITION)
                func = &i;
        ASSERT_NE(func, nullptr) << "fixture must define a function";

        for (const auto& ref : obj.refs()) {
            if (!macro_ids.count(ref.to_symbol_local_id())) continue;
            ASSERT_LT(func->start_offset(), ref.from_offset_start())
                << "the invocation must sit textually inside the function";
            EXPECT_TRUE(func->dfs_enter() < ref.dfs_enter() &&
                        ref.dfs_enter() < func->dfs_exit())
                << "the invocation at " << ref.dfs_enter()
                << " must be inside the function at [" << func->dfs_enter()
                << "," << func->dfs_exit() << ")";
            EXPECT_EQ(ref.nesting_level(), func->nesting_level() + 1)
                << "one level down, not a sibling";
            // The full span is the whole invocation `some_macro(bar)`, while
            // the reference's own range is just the name.
            EXPECT_GT(ref.full().end(), ref.from_offset_end())
                << "the full span must cover the argument list";
            checked = true;
        }
    }
    ASSERT_TRUE(checked) << "no macro invocation reference in main.c";
}

// A ref merged from a LATER translation unit is renumbered against the
// instances it will actually live with.
//
// `Indexer::processTU` keeps the FIRST TU's instances for a shared header and
// merges only later TUs' refs -- so a merged ref's numbers were computed
// against an instance set that is then discarded.  It worked only while two TUs
// happened to walk the header identically.
//
// This fixture makes them differ: `ops.h` declares `struct extra_decl` under
// `#ifdef WANT_EXTRA`, which only `b.c` defines, and it sits BEFORE
// `struct ops` so every later instance shifts in b.c's walk.  Both TUs then
// attribute a Stage2 synthetic ref to the SAME field (`run`), so both refs land
// on one range and must resolve to one owner.  Un-renumbered, b.c's ref came
// out at dfs 11265 against a.c's numbering -- outside the field entirely --
// while a.c's sat correctly at 5121.
// A MACRO-BODY REFERENCE AFTER A LINE CONTINUATION STILL SPELLS ITS MACRO.
//
// `linkMacroBodies` tokenises a `#define` body and emits a reference for each
// identifier naming a macro.  The range came from `clang_getTokenLocation` for
// the start and `start + name.size()` for the end -- and for the FIRST token
// after a `\` line splice that start lands on the backslash, so the whole range
// slides two bytes and spells `\<newline>` plus a truncated name.
//
// The range guard in `insertRef` then refuses the reference, so the edge is lost
// even though its TARGET was always right.  Measured on a full reindex:
// 865,728 references dropped on linux, 14,654 on rdma-core, 7,741 on
// open-gpu-kernel-modules -- and ZERO on ucx, which writes its macro bodies
// mid-line and therefore cannot exhibit it.  That zero is what made the guard
// look clean when it shipped.
TEST(ObjectRoots, AMacroBodyRefAfterALineContinuationSpellsItsMacro) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/macro_continuation";
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"main.c"});
    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();

    int64_t inner = -1;
    for (const auto& si : indexer.symbolTable().allSymbols())
        if (si.name == "INNER") inner = si.local_id;
    ASSERT_NE(inner, -1) << "fixture must define INNER";

    bool checked = false;
    for (const auto& f : indexer.allFiles()) {
        if (f.module_path.find("m.h") == std::string::npos) continue;
        std::string text(reinterpret_cast<const char*>(f.content.data()), f.content.size());

        std::vector<const RefData*> to_inner;
        for (const auto& r : f.refs)
            if (r.to_symbol_local_id == inner) to_inner.push_back(&r);

        // OUTER names INNER after a continuation, MIDLINE names it mid-line.
        // Both are references from a macro body, so both must survive.
        EXPECT_EQ(to_inner.size(), 2u)
            << "both macro bodies naming INNER must produce an edge";
        for (const auto* r : to_inner) {
            ASSERT_GE(r->from_offset_start, 0);
            ASSERT_LE(static_cast<size_t>(r->from_offset_end), text.size());
            EXPECT_EQ(text.substr(r->from_offset_start,
                                  r->from_offset_end - r->from_offset_start),
                      "INNER")
                << "a reference's range must spell its symbol, at ["
                << r->from_offset_start << "," << r->from_offset_end << ")";
            checked = true;
        }
    }
    ASSERT_TRUE(checked) << "no reference to INNER was recorded at all";
}

// ONE RECORD REACHED TWO WAYS IS ONE SYMBOL.
//
// A record with no tag has no name of its own, so clang names it after where it
// is written -- `struct (anonymous at inc/types.h:7:5)` -- using the PRESUMED
// path, i.e. whatever the `#include` resolved to.  `inc/self -> .` here makes
// one header reachable as `types.h` and as `self/types.h`, which is the shape
// criu has as `compel/include/uapi/compel -> .`.
//
// File identity was never fooled by this: objects are keyed on
// `getCanonicalPath`, so the header is one object either way.  Only the NAMES
// bypassed it, and on criu that produced 23 redundant symbols across 18
// records, one union appearing under four names.
TEST(ObjectRoots, OneRecordReachedTwoWaysIsOneSymbol) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/symlinked_header";
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"a.c", "b.c"}, "-Iinc");
    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();

    std::vector<std::string> anon;
    for (const auto& si : indexer.symbolTable().allSymbols())
        if (si.name.find("anonymous at ") != std::string::npos ||
            si.name.find("unnamed at ") != std::string::npos)
            anon.push_back(si.name);

    // The fixture is only testing what it claims if both TUs were indexed and
    // the union is actually anonymous.
    ASSERT_FALSE(anon.empty()) << "fixture must declare an anonymous record";
    std::set<std::string> distinct(anon.begin(), anon.end());
    EXPECT_EQ(distinct.size(), 1u)
        << "one record, one symbol -- got " << distinct.size() << ": "
        << [&] { std::string j; for (auto& n : distinct) j += "\n    " + n; return j; }();

    // And the surviving name is canonical: the symlinked spelling must not be
    // the one that won.
    for (const auto& n : distinct)
        EXPECT_EQ(n.find("/self/"), std::string::npos)
            << "the name kept a symlinked spelling: " << n;
}

// THE NUMBERING IS A FUNCTION OF WHAT WAS FOUND, NOT OF THE ORDER IT ARRIVED.
//
// The union made the instance SET order-independent; it did not make the
// numbers so.  `numberTree` builds each node's child list by walking the
// instance vector, so the preorder sequence -- and therefore every dfs_enter in
// the object -- followed whichever translation unit contributed each row first.
// Measured on criu before `canonicaliseInstanceOrder`: reversing 211
// translation units moved 2,953 instances and 3,006 references to different
// numbers, while not one of 34,018 instances changed `nesting_level`.
// Nothing was wrong with the graph; it just could
// not be reproduced, which rules out diffing two indices or addressing a row by
// position across a reindex.
//
// Keyed by symbol NAME and SCOPE rather than local_id on purpose: ids are
// handed out in the order symbols are first seen, so they legitimately permute
// between runs.  This test is about the graph, not its labels.
TEST(ObjectRoots, NumberingDoesNotDependOnTuOrder) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/cross_tu_field_refs";
    using InstKeyT = std::tuple<std::string, std::string, int, int, int32_t, int32_t>;
    using RefKeyT = std::tuple<std::string, std::string, int, int32_t, int32_t>;
    std::map<InstKeyT, std::tuple<int64_t, int64_t, int32_t, int32_t, bool>> inst_of[2];
    std::map<RefKeyT, std::tuple<int64_t, int32_t, int32_t>> ref_of[2];

    std::vector<std::string> orders[2] = {{"a.c", "b.c"}, {"b.c", "a.c"}};
    for (int run = 0; run < 2; ++run) {
        TempDir temp_dir = makeTempDir();
        generateCompileCommands(fixture_dir, temp_dir.path, orders[run]);
        Indexer indexer("test", temp_dir.path, fixture_dir, 1);
        indexer.run();

        std::unordered_map<int64_t, std::pair<std::string, int>> sym;
        for (const auto& si : indexer.symbolTable().allSymbols())
            sym[si.local_id] = {si.name, si.scope};

        for (const auto& f : indexer.allFiles()) {
            for (const auto& i : f.instances) {
                auto& s = sym[i.symbol_local_id];
                inst_of[run][{f.module_path, s.first, s.second, i.instance_type,
                              i.start_offset, i.end_offset}] =
                    {i.dfs_enter, i.dfs_exit, i.nesting_level, i.generated, i.name_typed};
            }
            for (const auto& r : f.refs) {
                auto& s = sym[r.to_symbol_local_id];
                ref_of[run][{f.module_path, s.first, s.second,
                             r.from_offset_start, r.from_offset_end}] =
                    {r.dfs_enter, r.nesting_level, r.pasted};
            }
        }
    }

    ASSERT_FALSE(inst_of[0].empty());
    ASSERT_FALSE(ref_of[0].empty());
    EXPECT_EQ(inst_of[0].size(), inst_of[1].size())
        << "the two orders must find the same declarations";
    EXPECT_EQ(ref_of[0].size(), ref_of[1].size());
    // Whole-map equality, so a differing NUMBER fails just as loudly as a
    // missing declaration -- the two failures this is guarding are distinct and
    // the second is the one that was silent.
    EXPECT_EQ(inst_of[0], inst_of[1])
        << "same declarations must get the same intervals in either TU order";
    EXPECT_EQ(ref_of[0], ref_of[1])
        << "same references must get the same positions in either TU order";
}

// A SHARED HEADER'S INSTANCES ARE THE UNION OF EVERY TU'S VIEW, NOT THE FIRST
// TU'S -- codevyr/askl-clang-indexer#7.
//
// `ops.h` declares `struct extra_decl` under `#ifdef WANT_EXTRA`, which only
// `b.c` defines. The merge used to keep the first TU's instances and discard
// the rest, so in TU order (a.c, b.c) the declaration had NO instance in any
// object while the project-global symbol table named it anyway: an orphan
// symbol the engine can name and never locate. In order (b.c, a.c) it had one.
// The answer depended on `compile_commands.json` order, and alphabetical --
// what a real build system emits -- was the losing one.
//
// Both orders are run here because one order passes for the wrong reason.
TEST(ObjectRoots, ASharedHeaderUnionsEveryTusView) {
    for (std::vector<std::string> order :
         {std::vector<std::string>{"a.c", "b.c"}, std::vector<std::string>{"b.c", "a.c"}}) {
        SCOPED_TRACE("TU order: " + order[0] + ", " + order[1]);
        std::string fixture_dir = std::string(FIXTURE_DIR) + "/cross_tu_field_refs";
        TempDir temp_dir = makeTempDir();
        generateCompileCommands(fixture_dir, temp_dir.path, order);
        Indexer indexer("test", temp_dir.path, fixture_dir, 1);
        indexer.run();

        std::unordered_map<int64_t, std::string> name;
        for (const auto& sym : indexer.symbolTable().allSymbols())
            name[sym.local_id] = sym.name;

        // Every symbol that has an instance ANYWHERE, so this is the invariant
        // issue #7 asked for rather than a claim about one file.
        std::set<std::string> located;
        for (const auto& f : indexer.allFiles())
            for (const auto& i : f.instances)
                located.insert(name[i.symbol_local_id]);

        EXPECT_EQ(located.count("extra_decl"), 1u)
            << "the #ifdef'd declaration only b.c sees must still be locatable";
        EXPECT_EQ(located.count("extra_decl.pad"), 1u)
            << "and so must its field";
        // The control: what both TUs see was never in doubt, and must not have
        // been disturbed by unioning.
        EXPECT_EQ(located.count("ops"), 1u);
        EXPECT_EQ(located.count("worker_a"), 1u);
        EXPECT_EQ(located.count("worker_b"), 1u);

        // The union must not duplicate what both TUs saw: `struct ops` is one
        // declaration in the text however many TUs compiled the header.
        for (const auto& f : indexer.allFiles()) {
            if (f.module_path.find("ops.h") == std::string::npos) continue;
            std::map<std::string, int> seen;
            for (const auto& i : f.instances) seen[name[i.symbol_local_id]]++;
            EXPECT_EQ(seen["ops"], 1) << "one declaration, one instance";
            EXPECT_EQ(seen["extra_decl"], 1);
        }
    }
}

TEST(ObjectRoots, ARefMergedFromAnotherTuIsRenumbered) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/cross_tu_field_refs";
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"a.c", "b.c"});
    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();

    bool checked = false;
    for (const auto& f : indexer.allFiles()) {
        if (f.module_path.find("ops.h") == std::string::npos) continue;

        // Both TUs contributed, so the merge path really ran.  Three refs, not
        // two: `#ifdef WANT_EXTRA` is a MacroExpansion of a macro that b.c has
        // DEFINED, so it carries an invocation edge like any other expansion --
        // which is the point, `macro("CONFIG_NUMA")` is a question worth
        // answering -- and that one is not a field ref.
        std::vector<const InstanceData*> fields;
        for (const auto& i : f.instances)
            if (i.instance_type == INSTTYPE_DEFINITION) fields.push_back(&i);
        std::vector<RefData> field_refs;
        for (const auto& r : f.refs)
            if (r.from_offset_start > 900) field_refs.push_back(r); // struct ops, at the end
        ASSERT_EQ(field_refs.size(), 2u)
            << "each TU attributes one synthetic ref to ops.h's field";
        ASSERT_EQ(field_refs[0].from_offset_start, field_refs[1].from_offset_start)
            << "and both to the SAME field declaration";
        ASSERT_NE(field_refs[0].to_symbol_local_id, field_refs[1].to_symbol_local_id)
            << "pointing at different workers, which is why dedup keeps both";

        // One range, one owner: the numbers must agree.
        EXPECT_EQ(field_refs[0].dfs_enter, field_refs[1].dfs_enter);
        EXPECT_EQ(field_refs[0].nesting_level, field_refs[1].nesting_level);

        // And that owner is the field they are attributed to, in THIS file's
        // surviving instances.
        for (const auto& r : field_refs) {
            const InstanceData* owner = nullptr;
            for (const auto& i : f.instances)
                if (i.dfs_enter < r.dfs_enter && r.dfs_enter < i.dfs_exit &&
                    (!owner || i.dfs_enter > owner->dfs_enter))
                    owner = &i;
            ASSERT_NE(owner, nullptr)
                << "ref at dfs " << r.dfs_enter << " is outside every interval";
            EXPECT_EQ(r.nesting_level, owner->nesting_level + 1);
            EXPECT_EQ(owner->start_offset, r.from_offset_start)
                << "the owner is the field the ref was attributed to";
        }
        checked = true;
    }
    ASSERT_TRUE(checked) << "ops.h was not indexed";
}

// Macro edges are LAYERWISE: each layer owns only what its own body names.
//
//     log.h:  #define DEV_PRINT(x) do { printk(x); } while (0)
//     main.c: void handler(void) { DEV_PRINT(threshold); }
//
// An edge is a reference plus the instance that owns it, so the edge set this
// has to produce is exactly three:
//
//     handler   -> threshold    the argument, written at the call site
//     handler   -> DEV_PRINT    the invocation
//     DEV_PRINT -> printk       the body
//
// and `handler -> printk` is a TRAVERSAL of two of them, not a stored edge --
// the same as it already is for a chain of functions.
//
// All three were wrong before.  The body ref `printk` lives at log.h [178,184),
// which is inside DEV_PRINT's `#define` body [160,200), but `insertRef` took
// its owner from `stackParent` -- the innermost open scope IN ITS OWN FILE --
// and nothing from log.h was ever on the stack, so it landed on the header's
// file root and the edge read `log.h -> printk`.  Meanwhile `addRef` emitted a
// SECOND ref at the expansion site owned by the expansion, giving a duplicate
// `DEV_PRINT -> printk`, and a third owned the same way giving
// `DEV_PRINT -> threshold` -- an edge from a macro to text the macro never
// contained, since the argument is typed by the caller.  And nothing referenced
// DEV_PRINT at all, so the first hop did not exist: `unnest` was the only way
// across.
// An expansion contains NOTHING.
//
// A macro expansion owns no text, so it cannot be a parent by range either --
// the same doctrine that keeps `DEV_PRINT -> threshold` out of the edges keeps
// the argument out of its CONTENTS.  Before this, `DEV_PRINT(LEVEL)` spanned
// [85,101) and `LEVEL`'s own expansion spanned [95,100), so range parentage
// made the argument a child of the macro and `macro("ucs_unlikely") { }`
// answered with 31 unrelated macros -- every one of them an argument at one of
// its call sites.
//
// Measured on ucx: across 41,487 expansions, every child was another folded
// expansion (2,594 of them) and not one was a DEFINITION.  So nothing is lost.
// In particular a macro-GENERATED symbol is not a child of the expansion that
// made it -- it is a folded DEFINITION beside it, which is why the narrow rule
// is "an expansion is never a parent" and not "a folded instance is never a
// parent": the latter would re-admit `__compiletime_assert_1121` into
// `mod { func }`.
TEST(ObjectRoots, AnExpansionContainsNothing) {
    for (const char* fx : {"macro_layerwise", "macro_nested", "macro_arg_direct",
                           "macro_expansion_arg", "macro_struct_in_union",
                           "macro_zero_width", "reincluded_header"}) {
        std::string fixture_dir = std::string(FIXTURE_DIR) + "/" + fx;
        TempDir temp_dir = makeTempDir();
        std::vector<std::string> srcs;
        for (auto& e : fs::directory_iterator(fixture_dir))
            if (e.path().extension() == ".c") srcs.push_back(e.path().filename().string());
        std::sort(srcs.begin(), srcs.end());
        generateCompileCommands(fixture_dir, temp_dir.path, srcs);
        Indexer indexer("test", temp_dir.path, fixture_dir, 1);
        indexer.run();
        auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                         indexer.allFiles(), 1);
        askl::index::Project project;
        ASSERT_TRUE(project.ParseFromString(built.project_data));

        for (const auto& obj : project.objects()) {
            for (const auto& child : obj.symbol_instances()) {
                const askl::index::SymbolInstance* owner = nullptr;
                for (const auto& c : obj.symbol_instances()) {
                    if (&c == &child) continue;
                    if (c.dfs_enter() < child.dfs_enter() &&
                        child.dfs_enter() < c.dfs_exit() &&
                        (!owner || c.dfs_enter() > owner->dfs_enter()))
                        owner = &c;
                }
                if (!owner) continue;
                EXPECT_NE(owner->instance_type(), askl::index::InstanceType::EXPANSION)
                    << fx << " " << obj.module_path() << ": instance ["
                    << child.dfs_enter() << "," << child.dfs_exit()
                    << ") is inside an EXPANSION at [" << owner->dfs_enter() << ","
                    << owner->dfs_exit() << ") -- an expansion owns no text";
            }
        }
    }
}

static std::set<std::pair<std::string, std::string>> edgesOf(const char* fixture);

namespace {
/// One fixture, indexed and read back from the proto it would ship as.
struct BuiltFixture {
    askl::index::Project project;
    std::unordered_map<int64_t, std::string> name;

    /// (source, extra flags), one translation unit each, in this order -- for
    /// a file compiled more than once.
    struct Units {
        std::vector<std::pair<std::string, std::string>> list;
    };

    BuiltFixture(const char* fixture, const Units& units) {
        const auto& commands = units.list;
        std::string fixture_dir = std::string(FIXTURE_DIR) + "/" + fixture;
        TempDir temp_dir = makeTempDir();
        {
            std::ofstream out(temp_dir.path + "/compile_commands.json");
            out << "[\n";
            for (size_t i = 0; i < commands.size(); i++) {
                out << (i ? ",\n" : "") << "  {\"directory\": \"" << fixture_dir
                    << "\", \"command\": \"cc -c -I. " << commands[i].second << " "
                    << commands[i].first << " -o u" << i << ".o\", \"file\": \""
                    << fixture_dir << "/" << commands[i].first << "\"}";
            }
            out << "\n]\n";
        }
        build(fixture_dir, temp_dir.path);
    }

    BuiltFixture(const char* fixture, const std::vector<std::string>& sources) {
        std::string fixture_dir = std::string(FIXTURE_DIR) + "/" + fixture;
        TempDir temp_dir = makeTempDir();
        generateCompileCommands(fixture_dir, temp_dir.path, sources);
        build(fixture_dir, temp_dir.path);
    }

    void build(const std::string& fixture_dir, const std::string& commands_dir) {
        Indexer indexer("test", commands_dir, fixture_dir, 1);
        indexer.run();
        auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                         indexer.allFiles(), 1);
        EXPECT_TRUE(project.ParseFromString(built.project_data));
        for (const auto& sym : project.symbols()) {
            auto cut = sym.name().rfind('/');
            name[sym.local_id()] =
                cut == std::string::npos ? sym.name() : sym.name().substr(cut + 1);
        }
    }

    /// The names of the symbols the index says are not listed by default.
    std::set<std::string> folded() const {
        std::set<std::string> out;
        for (const auto& sym : project.symbols())
            if (sym.folded()) out.insert(name.at(sym.local_id()));
        return out;
    }

    /// (instance, target) for EVERY instance whose interval contains the
    /// reference, not only the innermost: what `X { Y }` can reach.
    std::set<std::pair<std::string, std::string>> reach() const {
        std::set<std::pair<std::string, std::string>> out;
        for (const auto& obj : project.objects())
            for (const auto& ref : obj.refs())
                for (const auto& i : obj.symbol_instances())
                    if (i.dfs_enter() < ref.dfs_enter() && ref.dfs_enter() < i.dfs_exit())
                        out.insert({name.at(i.symbol_local_id()),
                                    name.at(ref.to_symbol_local_id())});
        return out;
    }

    /// (instance, the instance it sits in), by name.
    std::set<std::pair<std::string, std::string>> parents() const {
        std::set<std::pair<std::string, std::string>> out;
        for (const auto& obj : project.objects())
            for (const auto& x : obj.symbol_instances()) {
                const askl::index::SymbolInstance* p = nullptr;
                for (const auto& i : obj.symbol_instances())
                    if (i.dfs_enter() < x.dfs_enter() && x.dfs_exit() < i.dfs_exit() &&
                        (!p || i.dfs_enter() > p->dfs_enter()))
                        p = &i;
                if (p)
                    out.insert({name.at(x.symbol_local_id()), name.at(p->symbol_local_id())});
            }
        return out;
    }

    /// (innermost instance, target): where each reference sits.
    std::set<std::pair<std::string, std::string>> sits() const {
        std::set<std::pair<std::string, std::string>> out;
        for (const auto& obj : project.objects())
            for (const auto& ref : obj.refs()) {
                const askl::index::SymbolInstance* owner = nullptr;
                for (const auto& i : obj.symbol_instances())
                    if (i.dfs_enter() < ref.dfs_enter() && ref.dfs_enter() < i.dfs_exit() &&
                        (!owner || i.dfs_enter() > owner->dfs_enter()))
                        owner = &i;
                if (owner)
                    out.insert({name.at(owner->symbol_local_id()),
                                name.at(ref.to_symbol_local_id())});
            }
        return out;
    }
};
} // namespace


// A reference's range is the spelling of the token that NAMES it, and that one
// rule places every case without a discriminator.
//
//   m.h:    #define FIELD_OF(_t, _m)  ((unsigned long)&(((_t*)0)->_m))
//           #define MODE_OFF          ((unsigned long)&(((struct cfg*)0)->mode))
//   main.c: FIELD_OF(struct cfg, mode)   and   MODE_OFF
//
// Both expand to byte-identical text, so the ONLY thing separating them is
// where each token was written: FIELD_OF gets `mode` from the caller as an
// argument, MODE_OFF spells it in its own body.  So FIELD_OF must own no field
// edge at all, and MODE_OFF must keep one.
//
// Before this, a `MemberRefExpr`'s extent START was used, and that sits in the
// macro BODY whenever the member came from an argument, with the end taken
// from the symbol's name length.  `ucs_offsetof(uct_mm_md_config_t,
// hugetlb_mode)` recorded the field at compiler_def.h [3757,3769), where the
// text reads `((_type*)0)-` -- a range spelling no text in either file.
TEST(ObjectRoots, ARefSitsWhereItsNameIsWritten) {
    EXPECT_EQ(edgesOf("macro_arg_member"),
              (std::set<std::pair<std::string, std::string>>{
                  // (a) the argument case: the CALLER names the field and the
                  // type, and FIELD_OF names nothing.
                  {"from_arg", "FIELD_OF"},
                  {"from_arg", "cfg.mode"},
                  {"from_arg", "cfg"},
                  // (c) the body case: MODE_OFF names them, reachable in two
                  // hops.  This is the control -- a fix that relocated every
                  // reference to the call site would empty it.
                  {"from_body", "MODE_OFF"},
                  {"MODE_OFF", "cfg.mode"},
                  {"MODE_OFF", "cfg"},
              }));
}

// Every AST reference's range spells the symbol it references.
//
// The invariant the chimera range broke, checked here over fixtures and at the
// producer by `Stage1::refTextNamesSymbol`.  Stage2's synthetic references are
// deliberately parked at a field's DECLARATION site rather than the
// assignment, so they do not satisfy it -- and they never pass through
// `insertRef`, which is why guarding there exempts them by construction.
TEST(ObjectRoots, RefRangesSpellTheirSymbol) {
    for (const char* fx : {"macro_arg_member", "macro_layerwise", "macro_nested",
                           "macro_arg_direct", "macro_expansion_arg",
                           "macro_struct_in_union", "reincluded_header",
                           "small_project", "typedef_basic", "union_basic"}) {
        std::string fixture_dir = std::string(FIXTURE_DIR) + "/" + fx;
        TempDir temp_dir = makeTempDir();
        std::vector<std::string> srcs;
        for (auto& e : fs::directory_iterator(fixture_dir))
            if (e.path().extension() == ".c") srcs.push_back(e.path().filename().string());
        std::sort(srcs.begin(), srcs.end());
        generateCompileCommands(fixture_dir, temp_dir.path, srcs);
        Indexer indexer("test", temp_dir.path, fixture_dir, 1);
        indexer.run();

        std::unordered_map<int64_t, std::string> name;
        for (auto& sym : indexer.symbolTable().allSymbols()) name[sym.local_id] = sym.name;

        for (const auto& f : indexer.allFiles()) {
            for (const auto& r : f.refs) {
                // Stage2 places a function-pointer assignment at the FIELD's
                // declaration, so `int (*read)(int fd, ...)` claims `my_read`
                // on purpose.  Those never reach `insertRef`, so the producer
                // check never sees them either.
                if (r.synthetic) continue;
                if (r.from_offset_start < 0 || r.from_offset_end <= r.from_offset_start ||
                    static_cast<size_t>(r.from_offset_end) > f.content.size())
                    continue;
                std::string leaf = name[r.to_symbol_local_id];
                auto cut = leaf.find_last_of("./");
                if (cut != std::string::npos) leaf = leaf.substr(cut + 1);
                // Only identifiers are checkable: clang names an anonymous
                // struct by its source location, so the "leaf" of
                // `(anonymous struct at /p/x.h:122:2)` is not a name at all.
                static const char* kIdent =
                    "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789_";
                if (leaf.empty() || leaf.find_first_not_of(kIdent) != std::string::npos)
                    continue;
                std::string text(
                    reinterpret_cast<const char*>(f.content.data()) + r.from_offset_start,
                    static_cast<size_t>(r.from_offset_end - r.from_offset_start));
                EXPECT_NE(text.find(leaf), std::string::npos)
                    << fx << " " << f.module_path << "[" << r.from_offset_start << ","
                    << r.from_offset_end << ") reads \"" << text
                    << "\" but references \"" << leaf << "\"";
            }
        }
    }
}


TEST(ObjectRoots, MacroEdgesAreLayerwise) {
    EXPECT_EQ(edgesOf("macro_layerwise"),
              (std::set<std::pair<std::string, std::string>>{
                  {"handler", "threshold"},
                  {"handler", "DEV_PRINT"},
                  {"DEV_PRINT", "printk"},
                  // `DEV_PRINT(LEVEL)` -- an argument that is itself a macro.
                  // Its expansion is a tree CHILD of DEV_PRINT's expansion,
                  // because it is written inside that invocation's text, so
                  // owning the invocation edge by tree parentage produced
                  // `DEV_PRINT -> LEVEL`.  The text is the caller's.
                  {"handler", "LEVEL"},
              }));

    // And a CHAIN: one edge per layer, so `handler -> output` is four hops.
    // The three middle ones do not exist in anything libclang reports -- the
    // preprocessing record carries only the outermost expansion at a use site,
    // so `ERR` written inside FATAL's body is an expansion nowhere.
    // `linkMacroBodies` reads them from the `#define` text.
    EXPECT_EQ(edgesOf("macro_nested"),
              (std::set<std::pair<std::string, std::string>>{
                  {"handler", "FATAL"},
                  {"FATAL", "ERR"},
                  {"ERR", "MSG"},
                  {"MSG", "output"},
              }));
}

static std::set<std::pair<std::string, std::string>> edgesOf(const char* fixture) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/" + fixture;
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"main.c"});
    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();
    auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                     indexer.allFiles(), 1);
    askl::index::Project project;
    EXPECT_TRUE(project.ParseFromString(built.project_data));

    std::unordered_map<int64_t, std::string> name;
    for (const auto& sym : project.symbols()) name[sym.local_id()] = sym.name();
    auto leaf = [](const std::string& p) {
        auto i = p.rfind('/');
        return i == std::string::npos ? p : p.substr(i + 1);
    };

    // An edge is (owner of the ref) -> (target of the ref).  The owner is the
    // instance whose interval innermost contains the ref's position, which is
    // how the engine reads it.
    std::set<std::pair<std::string, std::string>> edges;
    for (const auto& obj : project.objects()) {
        if (obj.filetype() == "directory") continue;
        for (const auto& ref : obj.refs()) {
            const askl::index::SymbolInstance* owner = nullptr;
            for (const auto& i : obj.symbol_instances())
                if (i.dfs_enter() < ref.dfs_enter() && ref.dfs_enter() < i.dfs_exit() &&
                    (!owner || i.dfs_enter() > owner->dfs_enter()))
                    owner = &i;
            EXPECT_NE(owner, nullptr);
            if (!owner) continue;
            edges.insert({leaf(name[owner->symbol_local_id()]),
                          leaf(name[ref.to_symbol_local_id()])});
        }
    }
    return edges;
}

// A DEFINITION WHOSE NAME PASSES THROUGH A MACRO STILL CONTAINS ITS BODY.
//
// `PROFILE_FUNC(int, wrapped, (a), int a) { ... }` and `#define alias renamed`
// / `int alias(int a) { ... }` both hand the function's NAME to the
// preprocessor, and nothing else: the body is ordinary text the author typed.
// Folding used to be decided by the name token alone, so both definitions came
// out folded, a folded instance took no reference, and every call written in
// those bodies landed on the file.  On ucx that was `ucp_tag_send_nbx` with 71
// references written inside it and none placed in it -- a call graph with the
// function's callees hanging off `tag_send.c`.
//
// What a macro wholly generated is different, and is told apart by EXTENT: the
// declaration's extent is the invocation and nothing more.  `DEFINE_SHOW(event)`
// is that case, and `event_show`, which nobody wrote down, is the one symbol
// here that is not listed by default.
TEST(ObjectRoots, ADefinitionNamedThroughAMacroContainsItsBody) {
    EXPECT_EQ(edgesOf("macro_named_definition"),
              (std::set<std::pair<std::string, std::string>>{
                  {"plain", "helper"},
                  {"wrapped", "helper"},
                  {"renamed", "helper"},
                  // The invocation that supplies a definition's name declares
                  // it: the macro leads to the definition, and the invocation
                  // sits where the definition sits, not inside it.  The body
                  // somebody typed stays the definition's.
                  {"PROFILE_FUNC", "wrapped"},
                  {"main.c", "PROFILE_FUNC"},
                  {"alias", "renamed"},
                  {"main.c", "alias"},
                  // Wholly generated: the same rule.  What the generated body
                  // calls is named by the `#define`.
                  {"DEFINE_SHOW", "event_show"},
                  {"main.c", "DEFINE_SHOW"},
                  {"DEFINE_SHOW", "shown"},
              }));

    BuiltFixture built("macro_named_definition", {"main.c"});
    EXPECT_EQ(built.folded(), (std::set<std::string>{"event_show"}));
    EXPECT_TRUE(built.reach().count({"main.c", "DEFINE_SHOW"}));
}

// ONE INVOCATION, SEVERAL DECLARATIONS, AND A BODY TYPED AFTER IT.
//
// The shape of the kernel's `SYSCALL_DEFINE`: the macro emits a prototype and
// a wrapper, and ends on the header of the function whose body the author
// writes next.  Only that last declaration reaches past the invocation, so
// only it has text of its own -- and the calls in the body are its calls.
TEST(ObjectRoots, ABodyTypedAfterAGeneratingMacroBelongsToItsDeclaration) {
    auto edges = edgesOf("macro_trailing_body");
    EXPECT_TRUE(edges.count({"do_read", "helper"}));
    EXPECT_FALSE(edges.count({"main.c", "helper"}));

    // `do_read` has a body the author typed; `entry_read` is the macro's.
    BuiltFixture built("macro_trailing_body", {"main.c"});
    EXPECT_EQ(built.folded(), (std::set<std::string>{"entry_read"}));
}

// A FUNCTION NAMED WITHOUT BEING CALLED IS STILL REFERENCED.
//
// Only a CALL used to produce a reference to a function.  Passing one as a
// callback, returning it, taking its address or storing it in a local did not,
// so the graph had no edge for any of them -- and `INIT(fn)`, where the macro
// uses its argument as a value, recorded nothing at all for `fn`.
//
// The reference sits where the name is written, like every other: in the
// function that mentions it, and for a macro argument at the argument.
TEST(ObjectRoots, AFunctionUsedAsAValueIsReferenced) {
    EXPECT_EQ(edgesOf("function_as_value"),
              (std::set<std::pair<std::string, std::string>>{
                  {"pass", "register_cb"},
                  {"pass", "helper"},     // register_cb(helper)
                  {"ret", "helper"},      // return helper;
                  {"addr", "other"},      // return &other;
                  {"local", "other"},     // fn_t f = other;
                  {"call", "helper"},     // helper(1), as before
                  {"twice", "apply"},     // apply((void *)apply)
                  // `INIT(other)` generates `get_init`.  It read `other`,
                  // which it returns; the `INIT` token nothing read, and a
                  // generated declaration has no text of its own to hold it.
                  {"main.c", "INIT"},
                  {"get_init", "other"},
                  // The macro leads to the function its invocation declared.
                  {"INIT", "get_init"},
                  // The types, unchanged by any of this.
                  {"ret", "fn_t"},
                  {"addr", "fn_t"},
                  {"local", "fn_t"},
                  {"register_cb", "fn_t"},
                  {"INIT", "fn_t"},
              }));
}

// A CALL IS STILL ONE REFERENCE, AND STILL THE CALL.
//
// `helper(1)` is a CallExpr whose callee is a DeclRefExpr to the same
// function.  The callee is not a second mention: the call's reference is the
// only one, and its FULL span is the whole `helper(1)` rather than the bare
// name.
//
// A function passed to itself IS mentioned twice -- `apply((void *)apply)` --
// and only the first of the two is the callee.
TEST(ObjectRoots, ACallIsNotAlsoAValueReference) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/function_as_value";
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"main.c"});
    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();
    auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                     indexer.allFiles(), 1);
    askl::index::Project project;
    ASSERT_TRUE(project.ParseFromString(built.project_data));

    int64_t helper_id = -1;
    for (const auto& sym : project.symbols())
        if (sym.name() == "helper") helper_id = sym.local_id();
    ASSERT_GE(helper_id, 0);

    int in_call = 0;
    for (const auto& obj : project.objects()) {
        const std::string& text = obj.content();
        auto at = text.find("helper(1)");
        if (at == std::string::npos) continue;
        const auto name_start = static_cast<int32_t>(at);
        const auto name_end = static_cast<int32_t>(at + std::string("helper").size());
        const auto call_end = static_cast<int32_t>(at + std::string("helper(1)").size());
        for (const auto& ref : obj.refs()) {
            if (ref.to_symbol_local_id() != helper_id) continue;
            if (ref.from_offset_start() != name_start) continue;
            ++in_call;
            EXPECT_EQ(ref.from_offset_end(), name_end);
            ASSERT_TRUE(ref.has_full());
            EXPECT_EQ(ref.full().start(), name_start);
            EXPECT_EQ(ref.full().end(), call_end) << "the call's span, not the name's";
        }
    }
    EXPECT_EQ(in_call, 1) << "one token, one reference";

    int64_t apply_id = -1;
    for (const auto& sym : project.symbols())
        if (sym.name() == "apply") apply_id = sym.local_id();
    ASSERT_GE(apply_id, 0);
    for (const auto& obj : project.objects()) {
        const std::string& text = obj.content();
        auto at = text.find("apply((void *)apply)");
        if (at == std::string::npos) continue;
        const auto callee = static_cast<int32_t>(at);
        const auto argument = static_cast<int32_t>(at + std::string("apply((void *)").size());
        std::set<int32_t> starts;
        for (const auto& ref : obj.refs())
            if (ref.to_symbol_local_id() == apply_id) starts.insert(ref.from_offset_start());
        EXPECT_EQ(starts, (std::set<int32_t>{callee, argument}))
            << "the callee and the argument, each once";
    }
}

// A FUNCTION STORED IN A FIELD BY A MACRO.
//
// Three ways the function's name can reach `.open = ...` through a macro.
// Each is one reference, and it sits where the name was written:
//
//   * passed as an argument: the token is the caller's, at the call site;
//   * pasted (`_n##_open`): the part the caller typed, `ev`, at the call site;
//   * written in the `#define` body: in the body, and the macro's.  The
//     invocation site names nothing, so it has no reference of its own --
//     `main.c -> STATIC_OPS -> fixed_open` is two edges, as it is for a call.
//
// The second pass used to add a reference over the whole invocation for the
// last two, beside whatever the first pass placed.
TEST(ObjectRoots, AFunctionStoredByAMacroIsReferencedOnce) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/function_value_from_macro";
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"main.c"});
    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();
    auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                     indexer.allFiles(), 1);
    askl::index::Project project;
    ASSERT_TRUE(project.ParseFromString(built.project_data));

    std::unordered_map<int64_t, std::string> name;
    for (const auto& sym : project.symbols()) name[sym.local_id()] = sym.name();

    // (file leaf, function) -> the text each reference covers.
    std::map<std::pair<std::string, std::string>, std::vector<std::string>> texts;
    for (const auto& obj : project.objects()) {
        const std::string& path = obj.module_path();
        auto slash = path.rfind('/');
        std::string file = slash == std::string::npos ? path : path.substr(slash + 1);
        for (const auto& ref : obj.refs()) {
            const std::string& to = name[ref.to_symbol_local_id()];
            if (to != "ev_open" && to != "fixed_open" && to != "arg_open") continue;
            // Stage2 also parks a reference at the FIELD's declaration; that
            // one is the field's edge, not a use site.
            std::string text = obj.content().substr(
                ref.from_offset_start(), ref.from_offset_end() - ref.from_offset_start());
            if (text.rfind("int (*open)", 0) == 0) continue;
            texts[{file, to}].push_back(text);
        }
    }

    using V = std::vector<std::string>;
    EXPECT_EQ(texts[std::make_pair(std::string("main.c"), std::string("arg_open"))],
              V{"arg_open"})
        << "the argument token, and nothing wider beside it";
    EXPECT_EQ(texts[std::make_pair(std::string("main.c"), std::string("ev_open"))], V{"ev"})
        << "the part of the pasted name the caller typed";
    EXPECT_EQ(texts[std::make_pair(std::string("ops.h"), std::string("fixed_open"))],
              V{"fixed_open"});
    EXPECT_EQ(texts[std::make_pair(std::string("main.c"), std::string("fixed_open"))], V{})
        << "written by the macro's body, so the body's reference and not the call site's";
}

// A NAME BUILT BY `##` IS REFERENCED WHERE ITS CHOSEN PART WAS TYPED.
//
// `CALL(send(x))` with `#define CALL(a) mca_pml.pml_ ## a` refers to the field
// `pml_send`, and no token anywhere spells that.  What the caller wrote is
// `send` -- the part of the name that makes it this field and not `pml_recv`
// -- and that is where the reference sits, so the edge is the caller's:
// `direct -> pml_send`.
//
// It used to be the macro's, and by accident: the reference fell back to the
// first byte of the expression in the `#define` body, and survived the range
// guard only when that byte happened to occur in the name.  Every call site
// of `CALL` merged onto that one byte, so `CALL` appeared to use every field
// any caller had ever picked and no caller appeared to use any.
//
// The part is followed to wherever it was typed.  Through a wrapper that
// passes its argument on (`OUTER`) that is still the call site; when a
// wrapper's own body supplies it (`PROBE`), it is that body, and the edge is
// the wrapper's.  When no parameter feeds the paste at all (`FIXED`), nobody
// chose anything and the reference stays with the macro that wrote both
// halves.
//
// `EACH(ENTRY)` is an X-macro list.  Its body types `alpha` and `beta`, but
// that alone names nothing -- the same list applied to another macro would
// declare, or stringify.  What makes `handle_alpha` a reference here is
// `ENTRY`, the macro handed to the list, so the reference sits on `ENTRY` at
// the call site and the edge is `table -> handle_alpha`, not the list's.
//
// `OP` is the two-level `CONCAT` idiom, which exists because an argument
// beside `##` is pasted as written and anywhere else is expanded first:
// `CONCAT(op_##name, SUFFIX)` builds `op_add_avx`, where pasting directly
// would build `op_addSUFFIX`.  `LOG` has `, ## __VA_ARGS__`, which is not a
// paste and must not swallow the comma the inner call's arguments need.
TEST(ObjectRoots, APastedNameIsReferencedWhereItsPartWasTyped) {
    EXPECT_EQ(edgesOf("macro_pasted_ref"),
              (std::set<std::pair<std::string, std::string>>{
                  {"direct", "CALL"},
                  {"direct", "mod.pml_send"},
                  {"layered", "OUTER"},
                  {"layered", "mod.pml_recv"},
                  {"via_body", "PROBE"},
                  {"PROBE", "mod.pml_probe"},
                  {"func", "kh_put"},
                  {"func", "kh_put_tbl"},
                  {"typed", "khash_t"},
                  {"typed", "kh_tbl_t"},
                  {"fixed", "FIXED"},
                  {"FIXED", "mod.pml_send"},
                  {"indirect", "OP"},
                  {"indirect", "op_add_avx"},
                  {"variadic", "LOG"},
                  {"variadic", "log_at_info"},
                  {"table", "EACH"},
                  {"table", "handle_alpha"},
                  {"table", "handle_beta"},
                  // What each `#define` body names in plain tokens.
                  {"OUTER", "CALL"},
                  {"PROBE", "CALL"},
                  {"OP", "CONCAT"},
                  {"OP", "SUFFIX"},
                  {"CONCAT", "CONCAT_"},
                  {"CALL", "mca_pml"},
                  {"FIXED", "mca_pml"},
                  {"mca_pml", "mod"},
              }));
}

// WHAT THE REPLAY CANNOT FOLLOW IS STILL RECORDED.
//
// The replay resolves a macro an expansion goes on to invoke by NAME, so it
// reads the latest definition.  `SEL` is defined twice here; for the first
// `MK(fn)` the replay builds `fn_b` where the compiler built `fn_a`, finds no
// such name among the pastes, and has no token to put the reference on.
//
// A function stored in a field has a second record for exactly this: the use
// over the whole construct.  It is the fallback, emitted only when there is
// no token -- so `fn_a` is still referenced, once, and `fn_b`, which the
// replay does follow, sits on the `fn` its caller typed and nowhere else.
TEST(ObjectRoots, APastedNameTheReplayCannotFollowKeepsItsWideReference) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/macro_pasted_fallback";
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"main.c"});
    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();
    auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                     indexer.allFiles(), 1);
    askl::index::Project project;
    ASSERT_TRUE(project.ParseFromString(built.project_data));

    std::unordered_map<int64_t, std::string> name;
    for (const auto& sym : project.symbols()) name[sym.local_id()] = sym.name();

    std::map<std::string, std::vector<std::pair<int32_t, std::string>>> uses;
    for (const auto& obj : project.objects()) {
        for (const auto& ref : obj.refs()) {
            const std::string& to = name[ref.to_symbol_local_id()];
            if (to != "fn_a" && to != "fn_b") continue;
            std::string text = obj.content().substr(
                ref.from_offset_start(), ref.from_offset_end() - ref.from_offset_start());
            if (text.rfind("int (*open)", 0) == 0) continue; // the field's own edge
            uses[to].push_back({ref.from_offset_start(), text});
        }
    }

    std::string src;
    for (const auto& obj : project.objects())
        if (obj.content().find("struct ops first") != std::string::npos) src = obj.content();
    const auto first = static_cast<int32_t>(src.find("MK(fn)"));
    const auto second = static_cast<int32_t>(src.rfind("MK(fn)"));
    ASSERT_NE(first, second);

    using V = std::vector<std::pair<int32_t, std::string>>;
    EXPECT_EQ(uses["fn_a"], (V{{first, "MK(fn)"}})) << "the fallback, over the invocation";
    EXPECT_EQ(uses["fn_b"], (V{{second + 3, "fn"}})) << "the part the caller typed";
}

// A DECLARATION THAT ENDS ON A MACRO ARGUMENT STILL HAS AN EXTENT.
//
// `DEFINE_COUNTER(hits)` with `#define DEFINE_COUNTER(name) int name;`
// declares `hits`, and the last token of that declaration is the argument.
// libclang maps an extent's end to the END of the expansion when the last
// token came from the macro's BODY, and leaves it alone when it came from an
// ARGUMENT -- and an argument's expansion location is where the invocation
// STARTS.  So the extent came out `[start, start)`, zero bytes wide, and the
// declaration was dropped: `hits` was a symbol with no instance anywhere.
//
// The same thing truncated an extent that merely ended in an invocation:
// `int plain = ID(7);` stopped at `ID`, with its initialiser outside it.
//
// What the declaration covers is the text through the end of that invocation.
TEST(ObjectRoots, ADeclarationEndingOnAMacroArgumentKeepsItsExtent) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/macro_decl_ends_on_arg";
    std::ifstream in(fixture_dir + "/main.c");
    std::string src((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());

    auto result = runFixture("macro_decl_ends_on_arg", {"main.c"});
    std::map<std::string, std::string> text;
    for (const auto& i : result.instances)
        if (i.file == "main.c" && i.symbol != "main.c")
            text[i.symbol] = src.substr(i.start_offset, i.end_offset - i.start_offset);

    EXPECT_EQ(text["hits"], "DEFINE_COUNTER(hits)");
    EXPECT_EQ(text["plain"], "int plain = ID(7)");
    EXPECT_EQ(text["box.width"], "FIELD(width)");
}

// FOLDING IS WHAT IS LISTED BY DEFAULT, AND IT IS A PROPERTY OF THE SYMBOL.
//
// A symbol is listed unless nobody ever wrote it down in normal code -- text
// outside every `#define` body.  Three things count as writing it down:
//
//   * declaring it with text of its own (`plain`, `struct x`, `user`);
//   * typing its whole name where it is declared, though a macro wrote the
//     rest (`DEFINE_COUNTER(idle)` -- which nothing refers to, so the
//     declaration is all there is to go on);
//   * typing its name, or the part of a pasted name a caller chooses, in a
//     reference (`SHOW(event)` for `event_show`; `SHOW(far)` in ANOTHER
//     translation unit for `far_show`).
//
// Everything else is the preprocessor's own business and is folded:
//
//   * `unused_show` -- pasted, and used by nobody;
//   * `hidden_impl`, `check_failed` -- named only inside a `#define`;
//   * `pair_first` -- referenced, but only by `pair_second`, which the same
//     invocation generated.  The `pair` it was pasted from was typed to
//     GENERATE, not to refer; a pasted reference inside a generated
//     declaration is that declaration's plumbing;
//   * `handle_alpha` -- referenced from `table` through `EACH(ENTRY)`, and the
//     edge is there, but `alpha` was typed in the list's `#define`.  Where a
//     reference SITS and where its name was TYPED are different facts, and
//     folding follows the second.
//
// It is per symbol because the question is asked across the whole project:
// `far_show` is unfolded by a file that neither declares nor defines it.
TEST(ObjectRoots, ASymbolIsFoldedUnlessNormalCodeWroteItDown) {
    BuiltFixture built("symbol_folding", {"main.c", "other.c"});
    EXPECT_EQ(built.folded(),
              (std::set<std::string>{"unused_show", "hidden_impl", "check_failed",
                                     "pair_first", "pair_second", "handle_alpha",
                                     "handle_beta"}));

    // Folding hides; it does not disconnect.
    auto reach = built.reach();
    EXPECT_TRUE(reach.count({"table", "handle_alpha"}));
    EXPECT_TRUE(reach.count({"pair_second", "pair_first"}));
}

// AMONG DECLARATIONS WITH ONE EXTENT, A REFERENCE SITS IN THE ONE IT WAS
// PARSED INSIDE.
//
// Everything `DEFINE_PAIR(pair)` generates has the invocation as its extent,
// and the call `pair_second` makes to `pair_first` sits on the `pair` both
// were made from.  Text cannot say which of the two it is in.  Left to a
// tie-break it lands in whichever sorts first -- `pair_first`, calling itself
// -- and a family of generated functions that call each other (`KHASH_INIT`)
// turns into one function calling all of them.
//
// The walk knows, exactly as it knows for an instance (`numberTree`, rule b).
TEST(ObjectRoots, AReferenceAmongEqualExtentsSitsWhereItWasParsed) {
    BuiltFixture built("symbol_folding", {"main.c", "other.c"});
    auto sits = built.sits();
    EXPECT_TRUE(sits.count({"pair_second", "pair_first"}));
    EXPECT_FALSE(sits.count({"pair_first", "pair_first"}));
}

// ONE TOKEN READ INSIDE THREE DECLARATIONS IS THREE REFERENCES.
//
// `DEFINE_FAMILY(tab, my_key_t, my_hash)` generates `tab_get`, `tab_resize`
// and `tab_put`, and each of them calls `my_hash` and takes a `my_key_t`.  The
// caller typed each name once, so all three uses are on one token.  While a
// reference was identified by its target and its token alone, the first
// function read kept the call and the other two called nothing.
//
// A reference is what it names, the token that names it, and where it sits --
// as an instance already is.
TEST(ObjectRoots, OneTokenReadInSeveralDeclarationsIsAReferenceInEach) {
    BuiltFixture built("generated_family", {"main.c"});
    auto sits = built.sits();
    for (const char* fn : {"tab_get", "tab_resize", "tab_put"}) {
        EXPECT_TRUE(sits.count({fn, "my_hash"})) << fn;
        EXPECT_TRUE(sits.count({fn, "my_key_t"})) << fn;
    }
    // Three rows on the one token, told apart by their place alone.
    std::map<std::pair<int32_t, int32_t>, std::set<int32_t>> places;
    for (const auto& obj : built.project.objects())
        for (const auto& ref : obj.refs())
            if (built.name.at(ref.to_symbol_local_id()) == "my_hash")
                places[{ref.from_offset_start(), ref.from_offset_end()}].insert(ref.dfs_enter());
    ASSERT_EQ(places.size(), 1u);
    EXPECT_EQ(places.begin()->second.size(), 3u);
    // What was already right stays: the call between two of them is where it
    // was read, and nothing is read twice in one place.
    EXPECT_TRUE(sits.count({"tab_put", "tab_resize"}));
    EXPECT_FALSE(sits.count({"tab_get", "tab_resize"}));
}

// A FIELD AND AN ENUM CONSTANT ARE READ INSIDE, LIKE A FUNCTION.
//
// `PAIR(struct cfg, first, second)` declares two fields on one extent, and
// each is a `struct cfg`; `TWO(LEFT, RIGHT, START)` does the same for two enum
// constants that are both computed from `START`.  The walk used to close a
// field the moment it opened it and never opened an enum constant, so
// everything they named was read "inside the struct" -- one reference, which
// text then gave to the first of the two.
TEST(ObjectRoots, AFieldAndAnEnumConstantHoldWhatWasReadInsideThem) {
    BuiltFixture built("generated_fields", {"main.c"});
    auto sits = built.sits();
    EXPECT_TRUE(sits.count({"holder.first", "cfg"}));
    EXPECT_TRUE(sits.count({"holder.second", "cfg"}));
    EXPECT_TRUE(sits.count({"LEFT", "START"}));
    EXPECT_TRUE(sits.count({"RIGHT", "START"}));
}

// ...AND SO IS ONE TOKEN READ BY TWO TRANSLATION UNITS IN DIFFERENT ONES.
//
// One file compiled twice, the second time with `-DVARIANT`, which renames
// the function: `work` in one unit and `work_variant` in the other, on the
// same bytes.  (ompi does this for its checksum and PMPI variants.)  The call
// to `helper` is one token, read inside `work` by one unit and inside
// `work_variant` by the other.
//
// Both are true, and both are kept.  There used to be one reference for the
// two, and since neither unit was wrong it went to the earlier function in the
// tree's order, leaving the other calling nothing.
TEST(ObjectRoots, AReferenceReadInTwoPlacesByTwoUnitsIsInBoth) {
    using Units = BuiltFixture::Units;
    BuiltFixture plain_first("dual_compile", Units{{{"main.c", ""}, {"main.c", "-DVARIANT"}}});
    BuiltFixture variant_first("dual_compile", Units{{{"main.c", "-DVARIANT"}, {"main.c", ""}}});

    EXPECT_EQ(plain_first.sits(), variant_first.sits());
    EXPECT_TRUE(plain_first.sits().count({"work", "helper"}));
    EXPECT_TRUE(plain_first.sits().count({"work_variant", "helper"}));
}

// AN INVOCATION THROUGH AN ALIAS IS STILL ONE INVOCATION.
//
// `#define CHECK REAL_CHECK` makes `CHECK(x)` an invocation of `REAL_CHECK`,
// but not one clang records: its preprocessing record keeps `CHECK`, eight
// characters wide, and drops the expansion of `REAL_CHECK` because its name
// came out of another macro.  So what `CHECK(x)` generates extends past the
// only invocation on record, and looked like a declaration with text of its
// own.  The kernel's `#define dev_vdbg dev_dbg` unfolded 431 of its 71,277
// `__compiletime_assert_N` this way.
//
// A macro name followed by exactly one parenthesised list is an invocation,
// whoever's name it turns out to be.  `PREFIX int own(...) { ... }` also
// starts on a macro and runs past it, and is not: what follows is not an
// argument list, and its body is the author's.
TEST(ObjectRoots, AnInvocationThroughAnAliasGeneratesWhatItGenerates) {
    BuiltFixture built("macro_alias_invocation", {"main.c"});
    EXPECT_EQ(built.folded(), (std::set<std::string>{"check_failed"}));
}

// A GENERATED DECLARATION CONTAINS ONLY WHAT WAS READ INSIDE IT.
//
// A declaration somebody typed contains what lies in its text.  One a macro
// generated has no text of its own: the extent clang reports for it is the
// invocation's, which says where the macro was used and nothing about what the
// declaration is made of.
//
// `CHECK(sizeof(struct x) > 4)` in `user` generates the declaration
// `check_failed` on the invocation's extent.  `struct x` and the `CHECK` token
// lie in that extent, and neither was read inside `check_failed` -- the
// condition is `user`'s code.  Both sit in `user`.
//
// The rule used to be "the innermost extent, whatever it is", which put both
// in `check_failed`.  `user` still reached them, one level further down, and a
// declaration that calls nothing appeared to use a type and a macro.
TEST(ObjectRoots, AGeneratedDeclarationContainsOnlyWhatWasReadInsideIt) {
    BuiltFixture built("symbol_folding", {"main.c", "other.c"});
    auto sits = built.sits();
    EXPECT_TRUE(sits.count({"user", "x"}));
    EXPECT_TRUE(sits.count({"user", "CHECK"}));
    EXPECT_FALSE(sits.count({"check_failed", "x"}));
    EXPECT_FALSE(sits.count({"check_failed", "CHECK"}));
    // The declaration itself is where it was: inside `user`, which read it.
    bool inside = false;
    for (const auto& obj : built.project.objects())
        for (const auto& outer : obj.symbol_instances())
            for (const auto& inner : obj.symbol_instances())
                if (built.name.at(outer.symbol_local_id()) == "user" &&
                    built.name.at(inner.symbol_local_id()) == "check_failed" &&
                    outer.dfs_enter() < inner.dfs_enter() && inner.dfs_exit() < outer.dfs_exit())
                    inside = true;
    EXPECT_TRUE(inside);
}

// ...AND SEVERAL GENERATED ON ONE EXTENT ARE NOT GIVEN WHAT NONE OF THEM READ.
//
// `DEFINE_FAMILY(tab, my_key_t, my_hash)` generates three functions.  Each
// read `my_hash`, and each has it.  The `DEFINE_FAMILY` token was read by
// none: a macro use is recorded before the walk.  It used to go to the first
// of the three, which then alone appeared to use the macro that made all of
// them.  It sits in the file.
TEST(ObjectRoots, WhatNoGeneratedDeclarationReadSitsBesideThem) {
    BuiltFixture built("generated_family", {"main.c"});
    auto sits = built.sits();
    EXPECT_TRUE(sits.count({"main.c", "DEFINE_FAMILY"}));
    for (const char* fn : {"tab_get", "tab_resize", "tab_put"})
        EXPECT_FALSE(sits.count({fn, "DEFINE_FAMILY"})) << fn;
}

// TWO TYPED DECLARATIONS ON ONE TEXT BOTH CONTAIN IT.
//
// One file compiled twice declares `work` and `work_variant` on the same
// bytes.  Both were typed, so both contain what lies in their text.  `TWICE`
// in the body is a macro use, read by neither: it is in both, as the call to
// `helper` is in both because each read it.  It used to be in whichever came
// first in the tree's order.
TEST(ObjectRoots, TwoTypedDeclarationsOnOneTextBothContainIt) {
    using Units = BuiltFixture::Units;
    BuiltFixture plain_first("dual_compile", Units{{{"main.c", ""}, {"main.c", "-DVARIANT"}}});
    BuiltFixture variant_first("dual_compile", Units{{{"main.c", "-DVARIANT"}, {"main.c", ""}}});
    EXPECT_EQ(plain_first.sits(), variant_first.sits());
    EXPECT_TRUE(plain_first.sits().count({"work", "TWICE"}));
    EXPECT_TRUE(plain_first.sits().count({"work_variant", "TWICE"}));
}

// A GENERATED DECLARATION SITS WHERE IT WAS READ.
//
//     #define PROFILE_FUNC(type, name, param) int blah; type blah_##name(int param)
//     PROFILE_FUNC(int, send, x) { return helper(x); }
//
// `blah` is made of the macro's own tokens, and the only extent libclang has
// for it is the invocation.  `blah_send` starts at that invocation and runs on
// through a body the author typed, so the invocation -- and with it `blah` --
// lies in `blah_send`'s text.  Placed by text, a global variable ended up
// inside a function.
//
// It has no text there to be placed by.  The compiler read it at file scope,
// before `blah_send` was opened, and that is where it sits.
TEST(ObjectRoots, AGeneratedDeclarationSitsWhereItWasRead) {
    BuiltFixture built("generated_sibling", {"main.c"});
    auto parents = built.parents();
    EXPECT_TRUE(parents.count({"blah", "main.c"}));
    EXPECT_FALSE(parents.count({"blah", "blah_send"}));
    // What was typed is unchanged, but for the invocation that named
    // `blah_send`: it is beside the function it declared
    // (`AMacroRefersToWhatItsInvocationsDeclared`).
    EXPECT_TRUE(built.sits().count({"main.c", "PROFILE_FUNC"}));
    EXPECT_TRUE(built.sits().count({"blah_send", "helper"}));

    // The kernel's SYSCALL_DEFINE shape: the prototypes are beside the
    // function that carries the body, not inside it.
    BuiltFixture sys("macro_trailing_body", {"main.c"});
    EXPECT_FALSE(sys.parents().count({"entry_read", "do_read"}));
}

// A MACRO LEADS TO WHAT ITS INVOCATIONS DECLARED.
//
//     #define DEFINE_LOCK(name) int name = 0
//     static DEFINE_LOCK(slock);
//
// Nothing somebody typed leads to `slock` except the invocation, and the
// invocation is the macro's doing.  So:
//
//   An invocation that supplies a declaration's name declares it: the macro
//   refers to the declaration, and the invocation sits where the declaration
//   sits, not inside it.
//
// By the NAME, not the extent.  `static DEFINE_LOCK(slock)` starts on text
// somebody typed and `PROFILE_FUNC(int, send, x) { ... }` ends on it; both got
// their name from the invocation, and 1,012 of the kernel's 1,098 file-scope
// `DEFINE_MUTEX` variables are written the first way.  `DEPRECATED int f()`
// starts on an invocation that names nothing, and is unchanged.
//
// DIRECTLY: what a declared struct or function holds in turn is reached
// through it, like anything else something contains.  INTRODUCED: a macro
// that only redeclares what exists (`EXPORT_SYMBOL(foo)`) does not lead to it.
//
// The reference sits on the macro's name in its `#define`, which is the only
// text the macro has.  It says nothing about whether the declaration was ever
// written down, so it unfolds nothing.
TEST(ObjectRoots, AMacroRefersToWhatItsInvocationsDeclared) {
    BuiltFixture built("reference_from_define", {"main.c"});
    auto sits = built.sits();
    EXPECT_TRUE(sits.count({"DEFINE_LOCK", "lock"}));
    // Typed text before the invocation: the same answer, one edge.
    EXPECT_TRUE(sits.count({"DEFINE_LOCK", "slock"}));
    EXPECT_FALSE(sits.count({"slock", "DEFINE_LOCK"}));
    EXPECT_TRUE(sits.count({"main.c", "DEFINE_LOCK"}));
    // A field declared inside a struct somebody typed: the macro leads to
    // the field, and the field to the type its declaration names.
    EXPECT_TRUE(sits.count({"HOOK", "hooks.open"}));
    EXPECT_TRUE(sits.count({"HOOK", "other.close"}));
    EXPECT_TRUE(sits.count({"hooks", "HOOK"}));
    EXPECT_TRUE(sits.count({"hooks.open", "ops_ret"}));
    // Typed text after the invocation: the function keeps its body and what
    // the body names; the invocation that named it is beside it.
    EXPECT_TRUE(sits.count({"PROFILE_FUNC", "blah"}));
    EXPECT_TRUE(sits.count({"PROFILE_FUNC", "blah_send"}));
    EXPECT_FALSE(sits.count({"blah_send", "PROFILE_FUNC"}));
    EXPECT_TRUE(sits.count({"main.c", "PROFILE_FUNC"}));
    EXPECT_TRUE(sits.count({"blah_send", "helper"}));
    // An invocation that names nothing stays in the declaration around it.
    EXPECT_TRUE(sits.count({"old_api", "DEPRECATED"}));
    EXPECT_FALSE(sits.count({"DEPRECATED", "old_api"}));
    // The first declaration is introduced; a redeclaration is not.
    EXPECT_TRUE(sits.count({"DECLARE_COUNTER", "hits"}));
    EXPECT_FALSE(sits.count({"EXPORT", "lock"}));
    // Directly: the struct and the function, not what they hold.
    EXPECT_TRUE(sits.count({"FAMILY", "pair_s"}));
    EXPECT_TRUE(sits.count({"FAMILY", "pair_get"}));
    EXPECT_FALSE(sits.count({"FAMILY", "pair_s.n"}));
    // Inside a function too.
    EXPECT_TRUE(sits.count({"CHECK", "check_failed"}));

    // The reference is on the macro's name, in the file that defines it.
    bool found = false;
    for (const auto& obj : built.project.objects()) {
        const std::string& text = obj.content();
        for (const auto& ref : obj.refs()) {
            if (built.name.at(ref.to_symbol_local_id()) != "lock") continue;
            const std::string at = text.substr(
                static_cast<size_t>(ref.from_offset_start()),
                static_cast<size_t>(ref.from_offset_end() - ref.from_offset_start()));
            if (at != "DEFINE_LOCK") continue;
            found = true;
            EXPECT_NE(obj.filesystem_path().find("defs.h"), std::string::npos);
        }
    }
    EXPECT_TRUE(found);

    // It unfolds nothing: what only a macro wrote stays unlisted.
    auto folded = built.folded();
    EXPECT_TRUE(folded.count("blah"));
    EXPECT_TRUE(folded.count("pair_get"));
    EXPECT_TRUE(folded.count("check_failed"));
    EXPECT_FALSE(folded.count("lock"));
}

// A VARIABLE WITHOUT AN INITIALIZER IS DEFINED WHERE IT IS NOT `extern`.
//
// `int x;` at file scope is a definition in C -- a tentative one, which the
// compiler turns into the definition unless another unit supplies an
// initializer.  libclang answers "not a definition" for it, so after an
// `extern int x;` in a header the indexer recorded `int x;` as one more
// declaration: the variable had no definition anywhere, and the macro that
// defined it (`DEFINE_PER_CPU(struct rq, runqueues)`) was not credited with
// it while the one that only declared it was.
TEST(ObjectRoots, AVariableWithoutAnInitializerIsDefinedWhereItIsNotExtern) {
    BuiltFixture built("tentative_definition", {"main.c"});

    // (name, file) of every DEFINITION instance.
    std::set<std::pair<std::string, std::string>> defined;
    for (const auto& obj : built.project.objects()) {
        const std::string& path = obj.filesystem_path();
        const std::string file = path.substr(path.rfind('/') + 1);
        for (const auto& i : obj.symbol_instances())
            if (i.instance_type() == askl::index::InstanceType::DEFINITION)
                defined.insert({built.name.at(i.symbol_local_id()), file});
    }
    EXPECT_TRUE(defined.count({"initialised", "main.c"}));
    EXPECT_TRUE(defined.count({"tentative", "main.c"}));
    EXPECT_TRUE(defined.count({"plain", "main.c"}));
    EXPECT_TRUE(defined.count({"counter", "main.c"}));
    // An `extern` declaration stays one, in the header and with no definition
    // anywhere.
    EXPECT_FALSE(defined.count({"tentative", "defs.h"}));
    EXPECT_FALSE(defined.count({"plain", "defs.h"}));
    EXPECT_FALSE(defined.count({"only_declared", "defs.h"}));

    // The macro that defined it is credited, like the one with an
    // initializer; the first declaration still is too.
    auto sits = built.sits();
    EXPECT_TRUE(sits.count({"DEFINE_VI", "initialised"}));
    EXPECT_TRUE(sits.count({"DEFINE_V", "tentative"}));
    EXPECT_TRUE(sits.count({"DECLARE_V", "tentative"}));
}

// A MACRO'S BODY NAMES A FUNCTION-LIKE MACRO ONLY WHERE IT CALLS IT OR HANDS
// IT ON.
//
// `#define __printf(a, b) __attribute__((__format__(printf, a, b)))` writes
// the identifier `printf`, and some file of the project has
// `#define printf(fmt, args...) printk(...)`.  The preprocessor expands a
// function-like macro only where its name is followed by `(`, so that body
// uses no macro -- but every identifier of a body that named a macro of the
// unit was linked, and the kernel's `__printf` led to a debug macro of the
// BFS filesystem and from there to `printk`.
//
// What stays: the name called in the body; the name a parameter follows,
// which may supply the list (`PAIR _tuple`); the name ending the body, which
// the text after the invocation completes (`#define alias printf`); and the
// name handed to a macro or to a parameter, which may apply it
// (`EACH(ENTRY)`, `f(ENTRY)`).
TEST(ObjectRoots, AMacroBodyNamesAFunctionLikeMacroOnlyWhereItCallsItOrHandsItOn) {
    BuiltFixture built("macro_name_not_called", {"main.c"});
    auto sits = built.sits();

    EXPECT_FALSE(sits.count({"__printf", "printf"}));
    EXPECT_FALSE(sits.count({"PAREN", "printf"}));
    // Nor is a parameter the macro that shares its spelling.
    EXPECT_FALSE(sits.count({"PAD", "width"}));

    EXPECT_TRUE(sits.count({"CALLS", "printf"}));
    EXPECT_TRUE(sits.count({"alias", "printf"}));
    EXPECT_TRUE(sits.count({"ALL", "EACH"}));
    EXPECT_TRUE(sits.count({"ALL", "ENTRY"}));
    EXPECT_TRUE(sits.count({"APPLY", "ENTRY"}));
    EXPECT_TRUE(sits.count({"ZIP", "PAIR"}));
    EXPECT_TRUE(sits.count({"printf", "printk"}));
    if (::testing::Test::HasFailure())
        for (const auto& e : sits) std::cerr << "  " << e.first << " -> " << e.second << "\n";
}

// A NAME PASTED IN A DESIGNATOR IS REFERENCED LIKE ANY OTHER PASTED NAME.
//
//   #define REQ_OP_NAME(name) [REQ_OP_##name] = #name
//   static const char *const blk_op_name[] = { REQ_OP_NAME(READ), ... };
//
// The table is indexed by `REQ_OP_READ`, completed by the `READ` its author
// typed.
TEST(ObjectRoots, ANamePastedInADesignatorIsReferenced) {
    BuiltFixture built("macro_pasted_designator", {"main.c"});
    auto sits = built.sits();
    EXPECT_TRUE(sits.count({"blk_op_name", "REQ_OP_READ"}));
    EXPECT_TRUE(sits.count({"blk_op_name", "REQ_OP_WRITE"}));
    EXPECT_TRUE(sits.count({"blk_op_name", "REQ_OP_FLUSH"}));
    if (::testing::Test::HasFailure())
        for (const auto& e : sits) std::cerr << "  " << e.first << " -> " << e.second << "\n";
}

// WHAT AN `#include` INSIDE A DECLARATION BRINGS IN IS READ THERE.
//
//     int dispatch(int nr, int x) {
//         switch (nr) {
//     #include "syscalls.h"          /* __SYSCALL(0, sys_read) ... */
//         default: return -1;
//         }
//     }
//
// The function calls everything the header lists, and so does the table
// built the same way.  The text of those calls is in the header and
// containment does not cross files, so both contained nothing and the header
// alone "used" the functions -- the kernel's `x64_sys_call` called one
// function and `sys_call_table` held none.
//
// Each such reference is also recorded on the `#include` line, which is how
// its author wrote it into the declaration.  That covers a header the
// included one includes in turn, and the macros used in that text.  The
// header keeps its own references, and an `#include` at file scope adds
// none to anything.
TEST(ObjectRoots, WhatAnIncludeInsideADeclarationBringsInIsReadThere) {
    BuiltFixture built("include_inside_declaration", {"main.c"});
    auto sits = built.sits();
    for (const char* fn : {"sys_read", "sys_write", "sys_close"}) {
        EXPECT_TRUE(sits.count({"table", fn})) << fn;
        EXPECT_TRUE(sits.count({"dispatch", fn})) << fn;
    }
    EXPECT_TRUE(sits.count({"table", "__SYSCALL"}));
    EXPECT_TRUE(sits.count({"dispatch", "__SYSCALL"}));

    // The text is still the header's.
    EXPECT_TRUE(sits.count({"syscalls.h", "sys_read"}));
    EXPECT_TRUE(sits.count({"syscalls.h", "__SYSCALL"}));
    EXPECT_TRUE(sits.count({"more.h", "sys_close"}));

    // File scope is not a declaration: the first `#include` of the header
    // gives the file nothing.
    for (const auto& edge : sits) EXPECT_NE(edge.first, "main.c") << edge.second;

    // Nobody typed a name on the `#include` line, so being reached through
    // one is not what gets a symbol listed.
    EXPECT_TRUE(built.folded().empty());
}

// ...AND WHERE TWO UNITS READ IT IN DIFFERENT PLACES, THE WALK SAYS NOTHING.
//
// One file compiled twice declares `work` and `work_variant` on the same
// bytes, and `CHECK(a > 0)` in the body generates `check_failed`.  It is one
// instance, read inside `work` by one unit and inside `work_variant` by the
// other, and a tree gives it one parent.  Taking whichever unit arrived first
// would make the index depend on thread timing.  A disagreement is no
// information, and it is placed by text: in the first of the two typed
// declarations, in the tree's order -- the same answer in either order.
TEST(ObjectRoots, AnInstanceReadInTwoPlacesDoesNotDependOnUnitOrder) {
    using Units = BuiltFixture::Units;
    BuiltFixture plain_first("dual_compile_generated",
                             Units{{{"main.c", ""}, {"main.c", "-DVARIANT"}}});
    BuiltFixture variant_first("dual_compile_generated",
                               Units{{{"main.c", "-DVARIANT"}, {"main.c", ""}}});
    EXPECT_EQ(plain_first.parents(), variant_first.parents());
    EXPECT_TRUE(plain_first.parents().count({"check_failed", "work"}));
}

// WHERE A DECLARATION WAS READ NEVER LEADS BACK TO ITSELF.
//
//     #define struct_group(NAME, MEMBERS...) \
//         union { struct { MEMBERS }; struct { MEMBERS } NAME; }
//
// The kernel's `struct_group` expands its members into two anonymous structs,
// both at the invocation's location, so they are ONE instance -- and clang
// meets every record twice besides, hoisted and under its declarator.  The
// union is therefore met again while a struct that was read inside it is
// open.  Taking that as "the union was read inside the struct" made each the
// other's parent: neither was reachable from the file, neither was numbered,
// and the engine refused the whole index (seven headers in Linux, `skbuff.h`
// among them).  A reading that puts a declaration inside itself says nothing.
TEST(ObjectRoots, ADeclarationMetAgainWhileOpenIsNotInsideItself) {
    BuiltFixture built("struct_group", {"main.c"});
    size_t instances = 0;
    for (const auto& obj : built.project.objects())
        for (const auto& inst : obj.symbol_instances()) {
            ++instances;
            EXPECT_GT(inst.dfs_exit(), inst.dfs_enter())
                << built.name.at(inst.symbol_local_id()) << " was left out of the tree";
        }
    EXPECT_GT(instances, 0u);
    // Everything the invocation generated is inside the struct it was typed in.
    EXPECT_TRUE(built.sits().count({"outer", "struct_group"}));
}

// ...AND TWO UNITS THAT READ A PAIR IN OPPOSITE ORDER AGREE ON NEITHER.
//
// One file compiled twice; the second time `first` and `second` swap places,
// so `struct first { struct second { ... } f; }` in one unit is
// `struct second { struct first { ... } f; }` in the other, on the same
// bytes.  Each unit read one inside the other.  Following both would be a
// loop; keeping the first unit's answer would depend on which unit that was.
// Both lose the walk's answer and text places them.
TEST(ObjectRoots, TwoUnitsThatNestAPairOppositelyDoNotDependOnOrder) {
    using Units = BuiltFixture::Units;
    BuiltFixture plain_first("dual_compile_swapped",
                             Units{{{"main.c", ""}, {"main.c", "-DSWAPPED"}}});
    BuiltFixture swapped_first("dual_compile_swapped",
                               Units{{{"main.c", "-DSWAPPED"}, {"main.c", ""}}});
    EXPECT_EQ(plain_first.parents(), swapped_first.parents());
    for (const auto& obj : plain_first.project.objects())
        for (const auto& inst : obj.symbol_instances())
            EXPECT_GT(inst.dfs_exit(), inst.dfs_enter())
                << plain_first.name.at(inst.symbol_local_id()) << " was left out of the tree";
}

// A header included twice under different macro definitions.
//
// `multi.h` has no include guard and holds one line, `DECL(alpha)`.  `a.c`
// includes it twice, once with `SUFFIX` defined to `_f` and once to `_d`, so
// the preprocessor turns that single line into TWO different functions:
//
//     int alpha_f(int v) { return helper(v); }
//     int alpha_d(int v) { return helper(v); }
//
// Both expansions are of the macro `DECL` and both occupy the same extent, so
// `(symbol, extent)` -- what identified an instance before -- says they are the
// same thing.  They are not: one made `alpha_f` and the other made `alpha_d`.
//
// This used to lose one of them, and the loss was not quiet.  `macro_sites_` is
// keyed by (file, offset), so the LAST expansion at an offset wins and owns the
// expansion-site refs; the dedup kept the FIRST.  So the surviving instance was
// never the one the refs had been numbered against, and every such ref pointed
// into the stride the dropped instance used to occupy.
// ---------------------------------------------------------------------------
// What a reference says about its target
// ---------------------------------------------------------------------------

namespace {
/// One reference of a built fixture, with where it sits.
struct StatedRef {
    const askl::index::SymbolRef* ref = nullptr;
    /// The innermost instance around it, and that instance's depth.
    std::string owner;
    int32_t owner_level = -1;
};

/// The reference to `target` that starts where `anchor` first occurs in
/// `file`, `skip` bytes in.  Asked by text, so the fixture can be read beside
/// the assertion.
StatedRef refAt(const BuiltFixture& built, const std::string& file, const std::string& anchor,
                size_t skip, const std::string& target) {
    StatedRef out;
    for (const auto& obj : built.project.objects()) {
        const std::string& path = obj.module_path();
        if (path.size() < file.size() + 1 ||
            path.compare(path.size() - file.size() - 1, file.size() + 1, "/" + file) != 0)
            continue;
        const std::string& text = obj.content();
        auto at = text.find(anchor);
        if (at == std::string::npos) {
            ADD_FAILURE() << "`" << anchor << "` is not in " << file;
            return out;
        }
        const auto start = static_cast<int32_t>(at + skip);
        for (const auto& ref : obj.refs()) {
            if (ref.from_offset_start() != start) continue;
            if (built.name.at(ref.to_symbol_local_id()) != target) continue;
            EXPECT_EQ(out.ref, nullptr)
                << "two references to " << target << " at `" << anchor << "`";
            out.ref = &ref;
            const askl::index::SymbolInstance* owner = nullptr;
            for (const auto& i : obj.symbol_instances())
                if (i.dfs_enter() < ref.dfs_enter() && ref.dfs_enter() < i.dfs_exit() &&
                    (!owner || i.dfs_enter() > owner->dfs_enter()))
                    owner = &i;
            if (owner) {
                out.owner = built.name.at(owner->symbol_local_id());
                out.owner_level = owner->nesting_level();
            }
        }
    }
    if (!out.ref) ADD_FAILURE() << "no reference to " << target << " at `" << anchor << "` in " << file;
    return out;
}
} // namespace

// A REFERENCE CARRIES THE SET OF THINGS ITS TOKEN DOES TO THE TARGET.
//
// The indexer knows, where it makes a reference, whether the token is the
// callee of a call or a function handed on as a value, and it used to write
// the same bare reference for both: `request_irq(n, handler)` was drawn like a
// call.  It writes what it knows:
//
//   * CALL: the token is the callee of a call;
//   * TAKE: a function named and not called, an enum constant.
//
// Each is a positive fact about the text.  A token that states nothing here
// (a type, a macro invocation, and until they can be told apart a variable or
// a field) is not thereby "not called": it says nothing.
TEST(ReferenceRoles, AReferenceSaysWhatItsTokenDoes) {
    BuiltFixture built("reference_roles", std::vector<std::string>{"main.c"});
    struct Case {
        const char* file;
        const char* anchor;
        size_t skip;
        const char* target;
        unsigned roles;
        const char* owner;
    };
    for (const Case& c : std::vector<Case>{
             // The callee of a call.
             {"main.c", "helper(1)", 0, "helper", CALL, "run"},
             {"main.c", "keep(helper)", 0, "keep", CALL, "run"},
             // A callback passed to a function, and a function returned.
             {"main.c", "keep(helper)", 5, "helper", TAKE, "run"},
             {"main.c", "return other", 7, "other", TAKE, "pick"},
             // A function stored in a field: the token is a value.
             {"main.c", ".read = my_read", 8, "my_read", TAKE, "ops"},
             // An enum constant.
             {"main.c", "case MODE_A", 5, "MODE_A", TAKE, "run"},
             // A call written in a `#define` body is a call there too.
             {"ops.h", "helper(3)", 0, "helper", CALL, "BODY_CALL"},
             {"ops.h", "keep(fn))", 0, "keep", CALL, "CALL_AND_KEEP"},
             // Nothing is stated of a type, a macro invocation, a variable or
             // a field.
             {"main.c", "enum mode m", 5, "mode", 0, "run"},
             {"main.c", "DEFINE_COUNTER(hits)", 0, "DEFINE_COUNTER", 0, "main.c"},
             {"main.c", "BODY_CALL()", 0, "BODY_CALL", 0, "run"},
             {"main.c", "hits++", 0, "hits", 0, "unlabelled"},
             {"main.c", "ops.read(2)", 0, "ops", 0, "unlabelled"},
             {"main.c", "ops.read(2)", 4, "file_ops.read", 0, "unlabelled"},
         }) {
        StatedRef r = refAt(built, c.file, c.anchor, c.skip, c.target);
        if (!r.ref) continue;
        EXPECT_EQ(r.ref->roles(), c.roles) << c.target << " at `" << c.anchor << "`";
        EXPECT_EQ(r.ref->relation(), askl::index::REF_RELATION_NONE)
            << c.target << " at `" << c.anchor << "`";
        EXPECT_EQ(r.owner, c.owner) << c.target << " at `" << c.anchor << "`";
    }
}

// ONE TOKEN THAT DOES TWO THINGS STATES BOTH.
//
// `CALL_AND_KEEP(both)` expands to `(both(1) + keep(both))`: the one `both`
// the caller typed is the callee of a call and a value handed on.  The two
// readings land on one reference, and a reference keeps what either knew --
// it used to keep whichever was read first.
TEST(ReferenceRoles, OneTokenReadTwiceStatesBothRoles) {
    BuiltFixture built("reference_roles", std::vector<std::string>{"main.c"});
    StatedRef r = refAt(built, "main.c", "CALL_AND_KEEP(both)", 14, "both");
    ASSERT_NE(r.ref, nullptr);
    EXPECT_EQ(r.ref->roles(), static_cast<unsigned>(CALL | TAKE));
    EXPECT_EQ(r.owner, "run");
}

// TWO UNITS THAT READ ONE TOKEN DIFFERENTLY STATE BOTH, IN EITHER ORDER.
//
// `USE(either)` calls its argument when the file is compiled with -DAS_CALL
// and hands it on otherwise.  The file is one text, so the token is one
// reference; what it states is the union of what the two units read.
TEST(ReferenceRoles, RolesFromTwoUnitsAreAUnion) {
    using Units = BuiltFixture::Units;
    for (const Units& units :
         {Units{{{"main.c", "-DAS_CALL"}, {"main.c", ""}}},
          Units{{{"main.c", ""}, {"main.c", "-DAS_CALL"}}}}) {
        BuiltFixture built("reference_roles", units);
        StatedRef r = refAt(built, "main.c", "USE(either)", 4, "either");
        ASSERT_NE(r.ref, nullptr);
        EXPECT_EQ(r.ref->roles(), static_cast<unsigned>(CALL | TAKE))
            << "first unit: " << units.list[0].second;
        EXPECT_EQ(r.owner, "two_units");
    }
    // One unit alone states what that unit read.
    BuiltFixture call("reference_roles", Units{{{"main.c", "-DAS_CALL"}}});
    StatedRef r = refAt(call, "main.c", "USE(either)", 4, "either");
    ASSERT_NE(r.ref, nullptr);
    EXPECT_EQ(r.ref->roles(), static_cast<unsigned>(CALL));
    BuiltFixture value("reference_roles", std::vector<std::string>{"main.c"});
    r = refAt(value, "main.c", "USE(either)", 4, "either");
    ASSERT_NE(r.ref, nullptr);
    EXPECT_EQ(r.ref->roles(), static_cast<unsigned>(TAKE));
}

// A RELATION IS A FACT BETWEEN TWO SYMBOLS THAT NO TOKEN OF THE SOURCE SPELLS,
// AND IT SITS DIRECTLY IN THE DECLARATION OF ITS SOURCE.
//
// "The macro declared it" and "the slot holds it" were written as references,
// parked on the macro's name and on the field's declaration.  A reference
// belongs to every declaration around it, so the struct appeared to refer to
// the function its field holds: `file_operations (type) -> bt_bmc_open`.  The
// row says what it is now, and the engine gives a relation to the one
// declaration exactly one level above it.
TEST(ReferenceRoles, ARelationSitsDirectlyInItsSource) {
    BuiltFixture built("reference_roles", std::vector<std::string>{"main.c"});

    // The macro's invocation declared `hits`.
    StatedRef declares = refAt(built, "ops.h", "DEFINE_COUNTER(name)", 0, "hits");
    ASSERT_NE(declares.ref, nullptr);
    EXPECT_EQ(declares.ref->relation(), askl::index::REF_RELATION_DECLARES);
    EXPECT_EQ(declares.ref->roles(), 0u) << "a relation is no token and states no role";
    EXPECT_EQ(declares.owner, "DEFINE_COUNTER");
    EXPECT_EQ(declares.ref->nesting_level(), declares.owner_level + 1);

    // The slot holds the function stored in it.
    StatedRef holds = refAt(built, "ops.h", "int (*read)(int);", 0, "my_read");
    ASSERT_NE(holds.ref, nullptr);
    EXPECT_EQ(holds.ref->relation(), askl::index::REF_RELATION_HOLDS);
    EXPECT_EQ(holds.ref->roles(), 0u);
    EXPECT_EQ(holds.owner, "file_ops.read");
    EXPECT_EQ(holds.ref->nesting_level(), holds.owner_level + 1);

    // Every relation row of the fixture, not only the two looked up: it
    // states no role, and its source is one level above it.
    int relations = 0;
    for (const auto& obj : built.project.objects())
        for (const auto& ref : obj.refs()) {
            if (ref.relation() == askl::index::REF_RELATION_NONE) continue;
            ++relations;
            EXPECT_EQ(ref.roles(), 0u);
            int direct = 0;
            for (const auto& i : obj.symbol_instances())
                if (i.dfs_enter() < ref.dfs_enter() && ref.dfs_enter() < i.dfs_exit() &&
                    i.nesting_level() + 1 == ref.nesting_level())
                    ++direct;
            EXPECT_EQ(direct, 1) << "a relation to "
                                 << built.name.at(ref.to_symbol_local_id())
                                 << " has no single source";
        }
    EXPECT_EQ(relations, 2);
}

// A SLOT ASSIGNED FROM A SLOT HOLDS IT.
//
// `to->write = from->read` stores whatever `read` holds in `write`.  The same
// relation as a function stored directly, on the declaration of the field
// assigned to.
TEST(ReferenceRoles, ASlotAssignedFromASlotHoldsIt) {
    BuiltFixture built("reference_roles", std::vector<std::string>{"fields.c"});
    StatedRef holds = refAt(built, "ops.h", "int (*write)(int);", 0, "file_ops.read");
    ASSERT_NE(holds.ref, nullptr);
    EXPECT_EQ(holds.ref->relation(), askl::index::REF_RELATION_HOLDS);
    EXPECT_EQ(holds.ref->roles(), 0u);
    EXPECT_EQ(holds.owner, "file_ops.write");
    EXPECT_EQ(holds.ref->nesting_level(), holds.owner_level + 1);
}

// A COPY OF A REFERENCE STATES WHAT ITS OWN READING DOES.
//
// A reference read through an `#include` inside a declaration is also
// recorded on that `#include` line.  `table` stores the functions the header
// lists and `dispatch` calls them: each copy says which, and the header's own
// token, read both ways, says both.
TEST(ReferenceRoles, ACopyAtAnIncludeStatesWhatWasReadThere) {
    BuiltFixture built("include_inside_declaration", std::vector<std::string>{"main.c"});
    std::map<std::string, std::set<unsigned>> roles_in;
    for (const auto& obj : built.project.objects())
        for (const auto& ref : obj.refs()) {
            if (built.name.at(ref.to_symbol_local_id()) != "sys_read") continue;
            const askl::index::SymbolInstance* owner = nullptr;
            for (const auto& i : obj.symbol_instances())
                if (i.dfs_enter() < ref.dfs_enter() && ref.dfs_enter() < i.dfs_exit() &&
                    (!owner || i.dfs_enter() > owner->dfs_enter()))
                    owner = &i;
            ASSERT_NE(owner, nullptr);
            roles_in[built.name.at(owner->symbol_local_id())].insert(ref.roles());
        }
    EXPECT_EQ(roles_in["table"], (std::set<unsigned>{TAKE}));
    EXPECT_EQ(roles_in["dispatch"], (std::set<unsigned>{CALL}));
    EXPECT_EQ(roles_in["syscalls.h"], (std::set<unsigned>{CALL | TAKE}));
}

// THE FALLBACK USE OF A FUNCTION STORED IN A FIELD IS A VALUE.
//
// Where a macro pasted the function's name together and the replay could not
// follow it, no token carries the reference and the second pass records the
// use over the whole `.open = ...`.  It is the same use: the function is
// stored, not called.  The slot's own row is a relation, as everywhere.
TEST(ReferenceRoles, TheWideFallbackReferenceTakesTheFunction) {
    BuiltFixture built("macro_pasted_fallback", std::vector<std::string>{"main.c"});
    int wide = 0, holds = 0;
    for (const auto& obj : built.project.objects())
        for (const auto& ref : obj.refs()) {
            if (built.name.at(ref.to_symbol_local_id()) != "fn_a") continue;
            if (ref.relation() == askl::index::REF_RELATION_HOLDS) {
                ++holds;
                EXPECT_EQ(ref.roles(), 0u);
            } else {
                ++wide;
                EXPECT_EQ(ref.roles(), static_cast<unsigned>(TAKE));
            }
        }
    EXPECT_EQ(wide, 1);
    EXPECT_EQ(holds, 1);
}

// EVERY PLACE TWO RECORDS OF ONE REFERENCE MEET, THEY STATE WHAT EITHER DID.
//
// The two merges above are the ones a fixture reaches.  Two more make one
// reference out of several records: `numberTree`, where readings that were
// told apart by the declaration they were read inside turn out to sit in the
// same place, and `ProtoBuilder`, for a caller that hands over a repeat.
// Asked directly, with records built by hand.
TEST(ReferenceRoles, RecordsThatBecomeOneReferenceAreAUnion) {
    SymbolTable symbols;
    const std::string path = "/proj/a.c";
    const std::string text = "int f(void) { return g(g); }\n";
    auto [file_sym, _f] = symbols.getOrCreate(path, SCOPE_GLOBAL, SYMTYPE_FILE);
    auto [f_sym, _a] = symbols.getOrCreate("f", SCOPE_GLOBAL, SYMTYPE_FUNCTION);
    auto [g_sym, _b] = symbols.getOrCreate("g", SCOPE_GLOBAL, SYMTYPE_FUNCTION);
    const auto at = static_cast<int32_t>(text.find("g(g)"));

    FileData fd;
    fd.object_local_id = 1;
    fd.module_path = path;
    fd.filesystem_path = path;
    fd.filetype = "c";
    fd.content.assign(text.begin(), text.end());
    fd.instances.push_back({file_sym, 0, static_cast<int32_t>(text.size()), INSTTYPE_FILE});
    fd.instances.push_back(
        {f_sym, 0, static_cast<int32_t>(text.size()) - 1, INSTTYPE_DEFINITION});

    // One token, read once inside `f` and once with nothing to say about
    // where: both sit in `f`.
    RefData inside{g_sym, at, at + 1};
    inside.scope_idx = 1;
    inside.roles = REFROLE_CALL;
    RefData nowhere{g_sym, at, at + 1};
    nowhere.roles = REFROLE_TAKE;
    for (bool call_first : {true, false}) {
        FileData numbered = fd;
        numbered.refs = call_first ? std::vector<RefData>{inside, nowhere}
                                   : std::vector<RefData>{nowhere, inside};
        Stage1::numberTree(numbered);
        ASSERT_EQ(numbered.refs.size(), 1u) << "one token in one place is one reference";
        EXPECT_EQ(numbered.refs[0].owner_idx, 1);
        EXPECT_EQ(numbered.refs[0].roles, REFROLE_CALL | REFROLE_TAKE)
            << (call_first ? "the call was read first" : "the value was read first");
    }

    // A relation stands where only one record states it, and is not
    // overwritten by another.
    RefData holds{g_sym, 0, 3};
    holds.relation = REFRELATION_HOLDS;
    RefData plain{g_sym, 0, 3};
    for (bool relation_first : {true, false}) {
        FileData numbered = fd;
        numbered.refs = relation_first ? std::vector<RefData>{holds, plain}
                                       : std::vector<RefData>{plain, holds};
        Stage1::numberTree(numbered);
        ASSERT_EQ(numbered.refs.size(), 1u);
        EXPECT_EQ(numbered.refs[0].relation, REFRELATION_HOLDS);
    }

    // ProtoBuilder: the same reference handed over twice.
    FileData twice = fd;
    twice.refs = {inside, nowhere};
    Stage1::numberTree(twice);
    twice.refs.push_back(twice.refs[0]);
    twice.refs[0].roles = REFROLE_CALL;
    twice.refs[1].roles = REFROLE_TAKE;
    auto built = ProtoBuilder::build("test", "/proj", symbols, {twice}, 2);
    askl::index::Project project;
    ASSERT_TRUE(project.ParseFromString(built.project_data));
    int found = 0;
    for (const auto& obj : project.objects())
        for (const auto& ref : obj.refs()) {
            ++found;
            EXPECT_EQ(ref.roles(), static_cast<unsigned>(CALL | TAKE));
            EXPECT_EQ(ref.relation(), askl::index::REF_RELATION_NONE);
        }
    EXPECT_EQ(found, 1);
}

TEST(ObjectRoots, ReincludedHeaderKeepsBothResults) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/reincluded_header";
    TempDir temp_dir = makeTempDir();
    generateCompileCommands(fixture_dir, temp_dir.path, {"a.c"});
    Indexer indexer("test", temp_dir.path, fixture_dir, 1);
    indexer.run();
    auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                     indexer.allFiles(), 1);
    askl::index::Project project;
    ASSERT_TRUE(project.ParseFromString(built.project_data));

    std::unordered_map<int64_t, std::string> name;
    for (const auto& sym : project.symbols()) name[sym.local_id()] = sym.name();

    bool checked = false;
    for (const auto& obj : project.objects()) {
        if (obj.module_path().find("multi.h") == std::string::npos) continue;
        checked = true;

        // One `DECL(alpha)` line, included twice, declares two different
        // functions AT ONE EXTENT.  Nothing about their ranges tells them
        // apart; the DFS position does, which is why instance identity is the
        // position and not `(symbol, extent)`.
        std::map<std::string, const askl::index::SymbolInstance*> generated;
        std::map<std::string, int> seen;
        for (const auto& i : obj.symbol_instances()) {
            EXPECT_NE(i.instance_type(), askl::index::InstanceType::EXPANSION)
                << "a macro use is a reference, not an instance";
            seen[name[i.symbol_local_id()]]++;
            if (i.instance_type() == askl::index::InstanceType::DEFINITION)
                generated[name[i.symbol_local_id()]] = &i;
        }

        // The other half of instance identity, and the reason the key carries
        // the SYMBOL: `int plain(int v);` does not depend on SUFFIX, so both
        // inclusions parse one declaration of one symbol at one extent.  One
        // declaration in the text is one instance, however many times the text
        // is parsed.  glibc's `libc-header-start.h` is this at scale -- 146
        // keys carrying 366 extra instances on criu before this held.
        EXPECT_EQ(seen["plain"], 1)
            << "a declaration parsed twice is still one declaration";

        ASSERT_EQ(generated.count("alpha_f"), 1u) << "first inclusion lost";
        ASSERT_EQ(generated.count("alpha_d"), 1u) << "second inclusion lost";
        const auto* f = generated["alpha_f"];
        const auto* d = generated["alpha_d"];
        EXPECT_EQ(f->start_offset(), d->start_offset())
            << "they must share an extent -- otherwise the fixture is not "
               "testing what it claims";
        EXPECT_EQ(f->end_offset(), d->end_offset());
        EXPECT_NE(f->dfs_enter(), d->dfs_enter())
            << "the DFS position is what tells them apart, so it must differ";

        // Two expansions of one invocation TEXT give one invocation edge: a
        // reference is identified by the text it occupies, and there is one
        // such text.  (It used to be two instances here, which is the pair
        // this fixture was built to keep; the generated symbols above carry
        // that claim now.)
        int decl_refs = 0;
        for (const auto& ref : obj.refs())
            if (name[ref.to_symbol_local_id()] == "DECL") decl_refs++;
        EXPECT_EQ(decl_refs, 1) << "one invocation text, one invocation edge";

        // Every ref inside some interval is checked for every fixture by
        // `EveryInstanceFormsAValidTree`, which `reincluded_header` is in.
    }
    ASSERT_TRUE(checked) << "multi.h was not in the built project";
}

// ProtoBuilder serialises; it does not filter.
//
// It used to drop instances sharing (symbol, extent), which runs AFTER
// `numberTree` has stamped refs against them -- so a drop silently orphaned
// references.  Whatever the identity of an instance is, deciding it here is
// too late: the numbers are already assigned.
TEST(ProtoBuilder, EmitsEveryInstanceItWasGiven) {
    for (const char* fx : {"reincluded_header", "shared_header_typedef",
                           "macro_nested", "small_project"}) {
        std::string fixture_dir = std::string(FIXTURE_DIR) + "/" + fx;
        TempDir temp_dir = makeTempDir();
        std::vector<std::string> srcs;
        for (auto& e : fs::directory_iterator(fixture_dir))
            if (e.path().extension() == ".c") srcs.push_back(e.path().filename().string());
        std::sort(srcs.begin(), srcs.end());
        generateCompileCommands(fixture_dir, temp_dir.path, srcs);
        Indexer indexer("test", temp_dir.path, fixture_dir, 1);
        indexer.run();

        // Keyed on what the input HAS -- the DFS position is assigned before
        // this point, so it is the one field that distinguishes two instances
        // sharing a symbol and an extent.  Counted, not set-collapsed, so
        // dropping one of two identical rows is caught too.
        using Key = std::tuple<int64_t, int32_t, int32_t, int>;
        std::map<std::string, std::map<Key, int>> given;
        for (const auto& f : indexer.allFiles())
            for (const auto& i : f.instances)
                given[f.module_path][{i.symbol_local_id, i.start_offset,
                                      i.end_offset, i.instance_type}]++;

        auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                         indexer.allFiles(), 1);
        askl::index::Project project;
        ASSERT_TRUE(project.ParseFromString(built.project_data));

        for (const auto& obj : project.objects()) {
            auto it = given.find(obj.module_path());
            if (it == given.end()) continue; // a container object ProtoBuilder adds
            std::map<Key, int> emitted;
            for (const auto& i : obj.symbol_instances())
                emitted[{i.symbol_local_id(), i.start_offset(), i.end_offset(),
                         static_cast<int>(i.instance_type())}]++;
            // Subset, not equality: ProtoBuilder ADDS the module/directory
            // container instances that `nestUnderContainers` nests under.
            for (const auto& [key, n] : it->second)
                EXPECT_GE(emitted[key], n)
                    << fx << " " << obj.module_path() << ": symbol "
                    << std::get<0>(key) << " at [" << std::get<1>(key) << ","
                    << std::get<2>(key) << ") given " << n << " times, emitted "
                    << emitted[key];
        }
    }
}

TEST(ObjectRoots, EveryInstanceFormsAValidTree) {
    for (const char* fx : {"small_project", "doc_comments", "typedef_basic",
                           "enum_basic", "enum_in_switch", "macro_nested",
                           "macro_expansion_arg", "shared_header_typedef",
                           "union_basic", "type_ref_chain",
                           // Two TUs sharing a header they number differently:
                           // covers the cross-TU ref merge.
                           "cross_tu_field_refs",
                           // One header included twice under different macro
                           // definitions: two expansions at ONE extent.
                           "reincluded_header",
                           // Layerwise macro edges: an invocation edge at a
                           // call site, a body edge at the `#define`.
                           "macro_layerwise",
                           // One declaration met again while it is open.
                           "struct_group"}) {
        std::string fixture_dir = std::string(FIXTURE_DIR) + "/" + fx;
        TempDir temp_dir = makeTempDir();
        std::vector<std::string> srcs;
        for (auto& e : fs::directory_iterator(fixture_dir))
            if (e.path().extension() == ".c") srcs.push_back(e.path().filename().string());
        ASSERT_FALSE(srcs.empty()) << "fixture " << fx << " has no .c sources";
        std::sort(srcs.begin(), srcs.end());
        generateCompileCommands(fixture_dir, temp_dir.path, srcs);

        Indexer indexer("test", temp_dir.path, fixture_dir, 1);
        indexer.run();
        auto built = ProtoBuilder::build("test", fixture_dir, indexer.symbolTable(),
                                         indexer.allFiles(), 1);
        askl::index::Project project;
        ASSERT_TRUE(project.ParseFromString(built.project_data));
        ASSERT_GT(project.objects_size(), 0) << fx;

        for (const auto& obj : project.objects()) {
            std::string where = std::string(fx) + " " + obj.module_path();

            // (enter, exit, nesting_level), outermost first; exit DESC breaks
            // ties so a parent precedes a child that starts at the same
            // number.
            struct Node { int64_t enter, exit; int32_t level; };
            std::vector<Node> nodes;
            for (const auto& i : obj.symbol_instances())
                nodes.push_back({i.dfs_enter(), i.dfs_exit(), i.nesting_level()});
            std::sort(nodes.begin(), nodes.end(), [](const Node& a, const Node& b) {
                return a.enter != b.enter ? a.enter < b.enter : a.exit > b.exit;
            });

            std::vector<Node> stack;
            int64_t prev_enter = -1;
            for (const auto& n : nodes) {
                ASSERT_GT(n.exit, n.enter)
                    << where << ": empty interval [" << n.enter << ", " << n.exit
                    << ") can never contain anything";
                ASSERT_NE(n.enter, prev_enter)
                    << where << ": two instances share dfs_enter " << n.enter;
                prev_enter = n.enter;

                while (!stack.empty() && stack.back().exit <= n.enter) stack.pop_back();
                if (!stack.empty()) {
                    const Node& p = stack.back();
                    ASSERT_LE(n.exit, p.exit)
                        << where << ": [" << n.enter << ", " << n.exit
                        << ") escapes its parent [" << p.enter << ", " << p.exit << ")";
                }
                ASSERT_EQ(n.level, static_cast<int32_t>(stack.size()))
                    << where << ": [" << n.enter << ", " << n.exit
                    << ") says nesting_level " << n.level << " but sits "
                    << stack.size() << " levels deep";
                stack.push_back(n);
            }

            // The tree AGREES WITH RANGE NESTING, with the DFS order as the
            // tie-break for the equal ranges synthesised code produces.  After
            // W7 that is the parentage rule's definition rather than an
            // aspiration, so this holds with NO exceptions -- including for
            // macro expansions, which used to be numbered as siblings of the
            // function they textually sit in.
            //
            // It was written once before W7 and failed twice: on the macro
            // expansion, and on every `opens_scope = false` instance that was
            // a tree leaf whose range still contained other instances.
            for (int a = 0; a < obj.symbol_instances_size(); ++a) {
                const auto& ia = obj.symbol_instances(a);
                // ...with ONE exception, and it is the doctrine rather than a
                // carve-out: an EXPANSION contains nothing by range, because a
                // macro expansion owns no text.  `DEV_PRINT(LEVEL)` spans
                // [85,101) and the argument's own expansion spans [95,100),
                // so range nesting says the macro contains it -- and the
                // caller wrote both, so the tree says they are siblings.
                if (ia.instance_type() == askl::index::InstanceType::EXPANSION)
                    continue;
                for (int b = 0; b < obj.symbol_instances_size(); ++b) {
                    if (a == b) continue;
                    const auto& ib = obj.symbol_instances(b);
                    bool strictly_contains =
                        ia.start_offset() <= ib.start_offset() &&
                        ib.end_offset() <= ia.end_offset() &&
                        (ia.start_offset() != ib.start_offset() ||
                         ia.end_offset() != ib.end_offset());
                    if (!strictly_contains) continue;
                    EXPECT_TRUE(ia.dfs_enter() < ib.dfs_enter() &&
                                ib.dfs_enter() < ia.dfs_exit())
                        << where << ": range [" << ia.start_offset() << ","
                        << ia.end_offset() << ") strictly contains ["
                        << ib.start_offset() << "," << ib.end_offset()
                        << ") but interval [" << ia.dfs_enter() << ","
                        << ia.dfs_exit() << ") does not contain ["
                        << ib.dfs_enter() << "," << ib.dfs_exit() << ")";
                }
            }

            // A ref is a point in the same order, so it has to land strictly
            // inside some instance -- and its depth has to agree with the
            // instance its NUMBER puts it in.
            //
            // Those can drift, because they come from different places: the
            // expansion-site branch of `addRef` takes a ref's dfs_enter from
            // the macro it landed in, while `insertRef` already set its
            // nesting_level from whatever AST scope was open.  They agree only
            // while a reference sits inside exactly one scope, which is the
            // common case and the only one these fixtures have.  Asserting it
            // is what turns "happens to hold" into "is checked".
            for (const auto& ref : obj.refs()) {
                const Node* owner = nullptr;
                for (const auto& n : nodes)
                    if (n.enter < ref.dfs_enter() && ref.dfs_enter() < n.exit &&
                        (!owner || n.enter > owner->enter))
                        owner = &n;
                ASSERT_NE(owner, nullptr)
                    << where << ": ref at dfs_enter " << ref.dfs_enter()
                    << " lies outside every instance interval";
                EXPECT_EQ(ref.nesting_level(), owner->level + 1)
                    << where << ": ref at " << ref.dfs_enter() << " says nesting_level "
                    << ref.nesting_level() << ", but its interval puts it inside ["
                    << owner->enter << ", " << owner->exit << ") at level " << owner->level;
            }

            // One full stride between enter and exit, which numbering by
            // stride gives for free.  That gap is where query-time rows are
            // numbered, so an instance narrower than a stride cannot hold its
            // own search matches -- they land past its dfs_exit and silently
            // stop being its descendants.  Mirrors askld's DFS_STRIDE, which
            // the upload validator enforces.
            for (const auto& n : nodes)
                ASSERT_GE(n.exit - n.enter, 1024)
                    << where << ": [" << n.enter << ", " << n.exit
                    << ") is narrower than one DFS stride";
        }
    }
}
