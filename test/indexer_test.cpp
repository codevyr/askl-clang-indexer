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

struct ExpectedRef {
    std::string from_file;      // module_path of the file containing the reference
    std::string to_name;        // name of the referenced symbol
    int32_t from_start;         // byte offset of reference start
    int32_t from_end;           // byte offset of reference end

    bool operator==(const ExpectedRef& o) const {
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
        << "\", " << r.from_start << ", " << r.from_end << "}";
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
            result.refs.push_back({rel_path, to_name, ref.from_offset_start, ref.from_offset_end});
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
                {"ops.c", "my_read", 236, 251},
                {"ops.c", "my_write", 257, 274},
                {"ops.c", "file_ops", 330, 338},
                {"ops.c", "file_ops.read", 356, 360},      // MemberRefExpr: ops->read = ...
                {"ops.c", "my_read", 351, 370},
                {"ops.c", "file_ops.write", 381, 386},     // MemberRefExpr: ops->write = ...
                {"ops.c", "my_write", 376, 397},
                {"ops.c", "container", 442, 451},
                {"ops.c", "my_read", 482, 497},
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
                {"types.h", "types.h"},
            }
        },
        // Proves zero-width filtering is correct:
        // - alpha (macro-generated StructDecl) has zero-width range → no instance
        // - beta (source-level StructDecl) has proper range → has instance
        // - TypeRefs to BOTH from real source code have proper ranges → both kept
        // This shows the filter only drops macro-generated declarations,
        // not references to macro-defined types.
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
                // driver_info.id: spelling refs inside register_driver macro body
                {"register.h", "driver_info.id", 219, 221},
                {"register.h", "driver_info.id", 270, 272},
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
                {"main.c", "my_read", 200, 215},
                {"main.c", "my_write", 221, 238},
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
            // At the FUNCTION's fold depth, not one past it: the caller wrote
            // this invocation, so it is not behind a fold.  What the macro
            // GENERATES is what stays one level out of reach.
            EXPECT_EQ(ref.fold_depth(), func->fold_depth());
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
// numbers, while not one of 34,018 instances changed `nesting_level`,
// `fold_depth` or `folded`.  Nothing was wrong with the graph; it just could
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
                    {i.dfs_enter, i.dfs_exit, i.nesting_level, i.fold_depth, i.folded};
            }
            for (const auto& r : f.refs) {
                auto& s = sym[r.to_symbol_local_id];
                ref_of[run][{f.module_path, s.first, s.second,
                             r.from_offset_start, r.from_offset_end}] =
                    {r.dfs_enter, r.nesting_level, r.fold_depth};
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
        EXPECT_EQ(field_refs[0].fold_depth, field_refs[1].fold_depth);

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
                           "macro_layerwise"}) {
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

            // (enter, exit, nesting_level, folded, fold_depth), outermost
            // first; exit DESC breaks ties so a parent precedes a child that
            // starts at the same number.
            struct Node { int64_t enter, exit; int32_t level; bool folded; int32_t fold; };
            std::vector<Node> nodes;
            for (const auto& i : obj.symbol_instances())
                nodes.push_back({i.dfs_enter(), i.dfs_exit(), i.nesting_level(),
                                 i.folded(), i.fold_depth()});
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
                    ASSERT_EQ(n.fold, p.fold + (p.folded ? 1 : 0))
                        << where << ": [" << n.enter << ", " << n.exit
                        << ") fold_depth disagrees with its parent";
                } else {
                    ASSERT_EQ(n.fold, 0)
                        << where << ": root [" << n.enter << ", " << n.exit
                        << ") has fold_depth " << n.fold;
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
            // inside some instance -- and its depth and fold depth have to
            // agree with the instance its NUMBER puts it in.
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
                EXPECT_EQ(ref.fold_depth(), owner->fold + (owner->folded ? 1 : 0))
                    << where << ": ref at " << ref.dfs_enter() << " says fold_depth "
                    << ref.fold_depth() << ", but its owner [" << owner->enter << ", "
                    << owner->exit << ") has fold_depth " << owner->fold
                    << " folded=" << owner->folded;
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
