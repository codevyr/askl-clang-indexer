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
            case 5: return "TYPE"; case 6: return "DATA"; default: return "?";
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

    bool operator==(const ExpectedInstance& o) const {
        return file == o.file && symbol == o.symbol;
    }
    bool operator<(const ExpectedInstance& o) const {
        if (file != o.file) return file < o.file;
        return symbol < o.symbol;
    }
};

void PrintTo(const ExpectedInstance& i, std::ostream* os) {
    *os << "{file=" << i.file << ", sym=" << i.symbol << "}";
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

    RunResult result;

    // Extract symbols and build id -> name map in one pass
    std::unordered_map<int64_t, std::string> id_to_name;
    for (auto& sym : indexer.symbolTable().allSymbols()) {
        result.symbols.push_back({sym.name, sym.scope, sym.type});
        id_to_name[sym.local_id] = sym.name;
    }

    // Extract instances, refs, and validate offset ranges
    for (auto& file : indexer.allFiles()) {
        for (auto& inst : file.instances) {
            EXPECT_LE(inst.start_offset, inst.end_offset)
                << "Bad instance range in " << file.module_path
                << ": start=" << inst.start_offset << " > end=" << inst.end_offset;
            auto it = id_to_name.find(inst.symbol_local_id);
            std::string sym_name = (it != id_to_name.end()) ? it->second : "UNKNOWN";
            result.instances.push_back({file.module_path, sym_name});
        }
        for (auto& ref : file.refs) {
            EXPECT_LE(ref.from_offset_start, ref.from_offset_end)
                << "Bad ref range in " << file.module_path
                << ": start=" << ref.from_offset_start << " > end=" << ref.from_offset_end;
            auto it = id_to_name.find(ref.to_symbol_local_id);
            std::string to_name = (it != id_to_name.end()) ? it->second : "UNKNOWN";
            result.refs.push_back({file.module_path, to_name, ref.from_offset_start, ref.from_offset_end});
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

INSTANTIATE_TEST_SUITE_P(
    Fixtures,
    IndexerFixtureTest,
    testing::Values(
        FixtureSpec{
            "small_project",
            {"ops.c", "main.c"},
            // symbols
            {
                {"container", GLOBAL, TYPE},
                {"default_ops", LOCAL, DATA},
                {"file_ops", GLOBAL, TYPE},
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
                {"ops.c", "container", 442, 451},
                {"ops.c", "file_ops", 207, 215},
                {"ops.c", "file_ops", 330, 338},
                {"ops.c", "my_read", 236, 251},
                {"ops.c", "my_read", 351, 370},
                {"ops.c", "my_read", 482, 497},
                {"ops.c", "my_write", 257, 274},
                {"ops.c", "my_write", 376, 397},
                {"ops.h", "file_ops", 180, 188},
                {"ops.h", "file_ops", 180, 188},
            }
        },
        FixtureSpec{
            "enum_basic",
            {"main.c"},
            {
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
                {"handle_status", GLOBAL, FUNCTION},
                {"handler.c", GLOBAL, FILETYPE},
                {"last_error", GLOBAL, DATA},
                {"status", GLOBAL, TYPE},
                {"status.h", GLOBAL, FILETYPE},
            },
            {
                {"handler.c", "status", 43, 49},
                {"handler.c", "OK", 81, 83},
                {"handler.c", "OK", 81, 83},
                {"handler.c", "ERR_IO", 109, 115},
                {"handler.c", "ERR_IO", 109, 115},
                {"handler.c", "ERR_MEM", 137, 144},
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
                {"main.c", "result", 272, 278},
                {"main.c", "result", 302, 308},
                {"main.c", "S_OK", 325, 339},
            }
        },
        FixtureSpec{
            "macro_struct_in_union",
            {"main.c"},
            {
                {"any_type", GLOBAL, TYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"type_a", GLOBAL, TYPE},
                {"type_b", GLOBAL, TYPE},
                {"type_c", GLOBAL, TYPE},
                {"types.h", GLOBAL, FILETYPE},
                {"use_any", GLOBAL, FUNCTION},
            },
            // refs — macro-expanded type_a/b/c refs clamped to [start, start+1)
            // Clang visits each TypeRef twice (through TypedefDecl + anonymous UnionDecl);
            // ProtoBuilder deduplicates via hash-set at serialization time.
            {
                {"main.c", "any_type", 33, 41},
                {"types.h", "type_a", 398, 399},
                {"types.h", "type_a", 398, 399},
                {"types.h", "type_b", 398, 399},
                {"types.h", "type_b", 398, 399},
                {"types.h", "type_c", 398, 399},
                {"types.h", "type_c", 398, 399},
            },
            // instances — macro-expanded struct instances have zero-width ranges, filtered out
            {
                {"main.c", "main.c"},
                {"main.c", "use_any"},
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
                // Macro-expanded TypeRef: zero-width clamped to [start, start+1)
                // Points to ALL_STRUCTS expansion site — preserves the reference.
                // Clang visits this TypeRef twice (through TypedefDecl + anonymous UnionDecl);
                // ProtoBuilder deduplicates at serialization time.
                {"types.h", "alpha", 477, 478},
                {"types.h", "alpha", 477, 478},
            },
            {
                {"main.c", "main.c"},
                {"main.c", "use_types"},
                {"main.c", "x"},
                // beta instance present — source-level struct, proper range
                {"types.h", "beta"},
                {"types.h", "types.h"},
                {"types.h", "wrapper"},
                {"types.h", "wrapper"},
                {"types.h", "wrapper"},
                // NOTE: alpha instance ABSENT — nested macro expansion
                // produces zero-width range (start == end), correctly filtered
            }
        },
        FixtureSpec{
            "shared_header_typedef",
            {"a.c", "b.c"},
            {
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
        FixtureSpec{
            "macro_only_header",
            {"main.c"},
            {
                {"buf", GLOBAL, DATA},
                {"macros.h", GLOBAL, FILETYPE},
                {"main.c", GLOBAL, FILETYPE},
                {"smallest", GLOBAL, FUNCTION},
            },
            {},
            {
                {"macros.h", "macros.h"},
                {"main.c", "buf"},
                {"main.c", "main.c"},
                {"main.c", "smallest"},
            }
        }
    ),
    [](const testing::TestParamInfo<FixtureSpec>& info) {
        return info.param.name;
    }
);
