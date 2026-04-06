#include "clang_utils.h"
#include "indexer.h"
#include "symbol_table.h"
#include "stage1.h"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
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
                             const std::vector<std::string>& sources) {
    std::ofstream out(output_dir + "/compile_commands.json");
    out << "[\n";
    for (size_t i = 0; i < sources.size(); i++) {
        if (i > 0) out << ",\n";
        out << "  {\n"
            << "    \"directory\": \"" << fixture_dir << "\",\n"
            << "    \"command\": \"cc -c -I. " << sources[i] << " -o "
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

RunResult runFixture(const std::string& fixture_name,
                     const std::vector<std::string>& sources) {
    std::string fixture_dir = std::string(FIXTURE_DIR) + "/" + fixture_name;

    // Create temp directory for compile_commands.json
    std::string tmp_template = (fs::temp_directory_path() / "askl-test-XXXXXX").string();
    std::vector<char> tmp_buf(tmp_template.begin(), tmp_template.end());
    tmp_buf.push_back('\0');
    char* tmp_dir = mkdtemp(tmp_buf.data());
    EXPECT_NE(tmp_dir, nullptr) << "Failed to create temp directory";
    TempDir temp_dir{tmp_dir ? tmp_dir : ""};

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
                {"main.c", "setup", 106, 117},
                {"main.c", "file_ops.read", 123, 131},     // MemberRefExpr: ops.read(0,0,0)
                {"ops.c", "file_ops", 207, 215},
                {"ops.c", "my_read", 236, 251},
                {"ops.c", "my_write", 257, 274},
                {"ops.c", "file_ops", 330, 338},
                {"ops.c", "file_ops.read", 351, 360},      // MemberRefExpr: ops->read = ...
                {"ops.c", "my_read", 351, 370},
                {"ops.c", "file_ops.write", 376, 386},     // MemberRefExpr: ops->write = ...
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
                {"main.c", "RED", 62, 65},
                {"main.c", "GREEN", 80, 85},
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
                {"point_t", GLOBAL, TYPE},
                {"types.h", GLOBAL, FILETYPE},
            },
            {
                {"main.c", "point_t", 19, 26},
                {"main.c", "handle", 38, 44},
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
            },
            {
                {"main.c", "value", 52, 57},
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
                {"a.c", "helper", 90, 98},
                {"b.c", "count", 56, 61},
                {"b.c", "helper", 90, 98},
            }
        },
        FixtureSpec{
            "type_ref_chain",
            {"main.c"},
            {
                {"TYPES_H", GLOBAL, MACRO},
                {"list", GLOBAL, TYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"node", GLOBAL, TYPE},
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
                {"handler.c", "OK", 81, 83},
                {"handler.c", "ERR_IO", 109, 115},
                {"handler.c", "ERR_MEM", 137, 144},
                {"handler.c", "status", 198, 204},
                {"handler.c", "ERR_IO", 218, 224},
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
                {"S_OK", GLOBAL, DATA},
                {"get_result", GLOBAL, FUNCTION},
                {"handle_close", GLOBAL, FUNCTION},
                {"handle_open", GLOBAL, FUNCTION},
                {"macros.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"result", GLOBAL, TYPE},
                {"status", GLOBAL, TYPE},
            },
            {
                {"macros.h", "S_OK", 411, 415},
                {"macros.h", "result", 321, 327},
                // Expansion-site refs: clamped to macro name width
                {"main.c", "S_OK", 325, 339},
                {"main.c", "result", 272, 278},
                {"main.c", "result", 302, 308},
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
                {"main.c", GLOBAL, FILETYPE},
                {"type_a", GLOBAL, TYPE},
                {"type_b", GLOBAL, TYPE},
                {"type_c", GLOBAL, TYPE},
                {"types.h", GLOBAL, FILETYPE},
                {"use_any", GLOBAL, FUNCTION},
            },
            // refs — spelling refs resolve inside ONE_TYPE body;
            // expansion refs clamped to ALL_TYPES name at call site [398, 407)
            {
                {"main.c", "any_type", 33, 41},
                {"types.h", "type_a", 286, 292},
                {"types.h", "type_a", 398, 407},
                {"types.h", "type_b", 309, 315},
                {"types.h", "type_b", 398, 407},
                {"types.h", "type_c", 332, 338},
                {"types.h", "type_c", 398, 407},
            },
            // instances — macro symbols now have instances too
            // ALL_TYPES appears twice: once for #define, once for expansion site
            {
                {"main.c", "main.c"},
                {"main.c", "use_any"},
                {"types.h", "ALL_TYPES"},
                {"types.h", "ALL_TYPES"},
                {"types.h", "ONE_TYPE"},
                {"types.h", "TYPES_H"},
                {"types.h", "any_type"},
                {"types.h", "any_type"},
                {"types.h", "any_type"},
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
                {"main.c", GLOBAL, FILETYPE},
                {"types.h", GLOBAL, FILETYPE},
                {"use_types", GLOBAL, FUNCTION},
                {"wrapper", GLOBAL, TYPE},
                {"x", GLOBAL, DATA},
            },
            {
                // TypeRefs from real source have proper ranges (start < end)
                {"main.c", "alpha", 249, 254},
                {"main.c", "beta", 266, 270},
                // Single-char ref: proves clang returns half-open [start, start+len),
                // so 1-char token gives [289, 290) not [289, 289)
                {"main.c", "x", 289, 290},
                // Macro-expanded TypeRef: spelling location resolves inside ONE_STRUCT body
                {"types.h", "alpha", 403, 408},
                // Expansion-site ref: clamped to ALL_STRUCTS name width
                {"types.h", "alpha", 477, 488},
            },
            {
                {"main.c", "main.c"},
                {"main.c", "use_types"},
                {"main.c", "x"},
                // beta instance present — source-level struct, proper range
                // ALL_STRUCTS appears twice: #define + expansion site
                {"types.h", "ALL_STRUCTS"},
                {"types.h", "ALL_STRUCTS"},
                {"types.h", "ONE_STRUCT"},
                {"types.h", "TYPES_H"},
                {"types.h", "beta"},
                {"types.h", "types.h"},
                {"types.h", "wrapper"},
                {"types.h", "wrapper"},
                {"types.h", "wrapper"},
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
                {"pair_t", GLOBAL, TYPE},
                {"result_t", GLOBAL, TYPE},
                {"types.h", GLOBAL, FILETYPE},
            },
            {
                {"a.c", "result_t", 20, 28},
                {"a.c", "result_t", 52, 60},
                {"b.c", "pair_t", 20, 26},
                {"b.c", "pair_t", 57, 63},
            },
            // instances — validates dedup across 2 TUs sharing types.h
            {
                {"a.c", "a.c"},
                {"a.c", "compute_a"},
                {"b.c", "b.c"},
                {"b.c", "make_pair"},
                {"types.h", "TYPES_H"},
                {"types.h", "pair"},
                {"types.h", "pair"},
                {"types.h", "pair_t"},
                {"types.h", "result_t"},
                {"types.h", "result_t"},
                {"types.h", "result_t"},
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
                // Expansion-site ref: clamped to LOG macro name width
                {"main.c", "puts", 36, 39},
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
                {"macros.h", "output", 71, 77},
                // Expansion-site ref: clamped to FATAL macro name width
                {"main.c", "output", 41, 46},
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
            {},
            {
                {"macros.h", "MACROS_H"},
                {"macros.h", "MAX_SIZE"},
                {"macros.h", "MIN"},
                {"macros.h", "macros.h"},
                // MAX_SIZE and MIN also get instances at expansion sites
                {"main.c", "MAX_SIZE"},
                {"main.c", "MIN"},
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
                {"main.c", GLOBAL, FILETYPE},
                {"my_drv", LOCAL, DATA},
                {"register.h", GLOBAL, FILETYPE},
                {"register_driver", GLOBAL, MACRO},
            },
            {
                {"main.c", "driver_info", 37, 48},
                // my_drv: expansion-site ref clamped to register_driver name
                {"main.c", "my_drv", 73, 88},
                // my_drv: spelling ref in the macro argument
                {"main.c", "my_drv", 89, 95},
            },
            {
                {"main.c", "__exit"},
                {"main.c", "__init"},
                {"main.c", "main.c"},
                {"main.c", "my_drv"},
                // register_driver: #define instance + expansion site instance
                {"main.c", "register_driver"},
                {"register.h", "REGISTER_H"},
                {"register.h", "driver_info"},
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
            },
            {
                {"main.c", "counter", 305, 312},
                {"main.c", "add", 315, 324},
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
                {"api.h", "do_work", 10},      // documentation — from declaration visit
                {"api.h", "do_work", 10},      // documentation — from definition visit (comment is in header)
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
                {"errors.h", GLOBAL, FILETYPE},
                {"global_cfg", GLOBAL, DATA},
                {"global_opts", GLOBAL, DATA},
                {"main", GLOBAL, FUNCTION},
                {"main.c", GLOBAL, FILETYPE},
                {"options", GLOBAL, TYPE},
                {"setup", GLOBAL, FUNCTION},
            },
            {
                {"errors.h", "config", 434, 440},
                {"errors.h", "config", 570, 576},
                {"main.c", "config", 28, 34},
                {"main.c", "config", 94, 100},
                {"main.c", "global_cfg", 182, 192},
                {"main.c", "global_opts", 199, 210},
                {"main.c", "options", 54, 61},
                {"main.c", "setup", 175, 193},
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
                // Spelling refs in macro body (macros.h)
                {"macros.h", "do_something", 86, 98},
                {"macros.h", "GLOBAL_VARIABLE", 105, 120},
                // Expansion refs clamped to some_macro name [56, 66)
                {"main.c", "GLOBAL_VARIABLE", 56, 66},
                {"main.c", "bar", 56, 66},
                {"main.c", "do_something", 56, 66},
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
                {"main.c", "some_macro", 3, 56, 66},
            }
        }
    ),
    [](const testing::TestParamInfo<FixtureSpec>& info) {
        return info.param.name;
    }
);

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
