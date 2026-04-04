#include <CLI/CLI.hpp>
#include "indexer.h"

#include <filesystem>
#include <thread>

int main(int argc, char* argv[]) {
    CLI::App app{"askl-clang-indexer — C language indexer for askl"};

    std::string project_dir = ".";
    std::string output_path = "index";
    std::string project_name = "main";
    int threads = std::max(1u, std::thread::hardware_concurrency());
    bool include_git_files = false;
    bool show_progress = false;

    app.add_option("project_dir", project_dir,
        "Project directory (contains compile_commands.json, used as root)");
    app.add_option("--output,-o", output_path, "Output directory for index files");
    app.add_option("--project", project_name, "Project name");
    app.add_option("--threads,-j", threads, "Number of parallel threads");
    app.add_flag("--include-git-files", include_git_files,
        "Include all git-tracked files at HEAD in the project files list");
    app.add_flag("--progress", show_progress, "Show progress during indexing");

    CLI11_PARSE(app, argc, argv);

    project_dir = std::filesystem::canonical(project_dir).string();

    Indexer indexer(project_name, project_dir, project_dir, threads, include_git_files, show_progress);
    indexer.run();
    indexer.write(output_path);
    return 0;
}
