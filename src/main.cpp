#include <CLI/CLI.hpp>
#include "indexer.h"

#include <thread>

int main(int argc, char* argv[]) {
    CLI::App app{"askl-clang-indexer — C language indexer for askl"};

    std::string compile_commands_dir = ".";
    std::string output_path = "index.pb";
    std::string project_name = "main";
    std::string root_path;
    int threads = std::max(1u, std::thread::hardware_concurrency());
    bool include_git_files = false;
    bool show_progress = false;

    app.add_option("--compile-commands", compile_commands_dir,
        "Directory containing compile_commands.json");
    app.add_option("--output,-o", output_path, "Output protobuf file path");
    app.add_option("--project", project_name, "Project name");
    app.add_option("--root", root_path, "Project root directory (default: auto-detect)");
    app.add_option("--threads,-j", threads, "Number of parallel threads");
    app.add_flag("--include-git-files", include_git_files,
        "Include all git-tracked files at HEAD in the project files list");
    app.add_flag("--progress", show_progress, "Show progress during indexing");

    CLI11_PARSE(app, argc, argv);

    Indexer indexer(project_name, compile_commands_dir, root_path, threads, include_git_files, show_progress);
    indexer.run();
    indexer.write(output_path);
    return 0;
}
