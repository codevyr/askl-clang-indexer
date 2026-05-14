#include <CLI/CLI.hpp>
#include "indexer.h"

#include <cstdio>
#include <filesystem>
#include <stdexcept>
#include <thread>

int main(int argc, char* argv[]) {
    CLI::App app{"askl-clang-indexer — C language indexer for askl"};

    std::string project_dir = ".";
    std::string output_path = "index";
    std::string project_name = "main";
    int threads = std::max(1u, std::thread::hardware_concurrency());
    bool include_git_files = true;
    std::string git_root;
    bool show_progress = false;
    bool show_warnings = false;
    std::string modules_method;

    app.add_option("project_dir", project_dir,
        "Project directory (contains compile_commands.json, used as root)");
    app.add_option("--output,-o", output_path, "Output directory for index files");
    app.add_option("--project", project_name, "Project name");
    app.add_option("--threads,-j", threads, "Number of parallel threads");
    app.add_flag("--include-git-files,!--no-include-git-files", include_git_files,
        "Include all git-tracked files at HEAD in the project files list (default: true)");
    app.add_option("--git-root", git_root,
        "Git root directory for --include-git-files (default: project_dir)");
    app.add_flag("--progress", show_progress, "Show progress during indexing");
    app.add_flag("--warn", show_warnings, "Print clang diagnostic errors to stderr");
    app.add_option("--modules", modules_method, "Create MODULE symbols (method: kbuild)");

    CLI11_PARSE(app, argc, argv);

    project_dir = std::filesystem::canonical(project_dir).string();

    if (include_git_files) {
        git_root = git_root.empty() ? project_dir
                                    : std::filesystem::canonical(git_root).string();
    }

    try {
        Indexer indexer(project_name, project_dir, project_dir, threads, git_root, show_progress, show_warnings, modules_method);
        indexer.run();
        indexer.write(output_path);
    } catch (const std::runtime_error& e) {
        fprintf(stderr, "Error: %s\n", e.what());
        return 1;
    }
    return 0;
}
