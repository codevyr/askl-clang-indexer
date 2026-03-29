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
    std::string from_file;  // module_path of the file containing the reference
    std::string to_name;    // name of the referenced symbol

    bool operator==(const ExpectedRef& o) const {
        return from_file == o.from_file && to_name == o.to_name;
    }
    bool operator<(const ExpectedRef& o) const {
        if (from_file != o.from_file) return from_file < o.from_file;
        return to_name < o.to_name;
    }
};

void PrintTo(const ExpectedRef& r, std::ostream* os) {
    *os << "{from=" << r.from_file << ", to=" << r.to_name << "}";
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

    // Extract refs
    for (auto& file : indexer.allFiles()) {
        for (auto& ref : file.refs) {
            auto it = id_to_name.find(ref.to_symbol_local_id);
            std::string to_name = (it != id_to_name.end()) ? it->second : "UNKNOWN";
            result.refs.push_back({file.module_path, to_name});
        }
    }

    std::sort(result.symbols.begin(), result.symbols.end());
    std::sort(result.refs.begin(), result.refs.end());

    return result;
}

// --- Fixture spec ---

struct FixtureSpec {
    std::string name;
    std::vector<std::string> sources;
    std::vector<ExpectedSymbol> symbols;
    std::vector<ExpectedRef> refs;
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
            // refs
            {
                {"main.c", "file_ops"},
                {"main.c", "file_ops"},
                {"main.c", "setup"},
                {"ops.c", "container"},
                {"ops.c", "file_ops"},
                {"ops.c", "file_ops"},
                {"ops.c", "my_read"},
                {"ops.c", "my_read"},
                {"ops.c", "my_read"},
                {"ops.c", "my_write"},
                {"ops.c", "my_write"},
                {"ops.h", "file_ops"},
                {"ops.h", "file_ops"},
            }
        }
    ),
    [](const testing::TestParamInfo<FixtureSpec>& info) {
        return info.param.name;
    }
);
