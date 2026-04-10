#pragma once

#include <string>
#include <vector>

class SymbolTable;
struct FileData;

struct BuildResult {
    bool content_inlined;                      // true = content inlined in Objects, no ContentBatch files
    std::string project_data;                  // Serialized Project
    std::vector<std::string> content_batches;  // Empty when content_inlined
};

class ProtoBuilder {
public:
    static constexpr size_t CONTENT_BATCH_MAX_BYTES = 1ULL * 1024 * 1024 * 1024; // 1 GB

    static BuildResult build(
        const std::string& project_name,
        const std::string& root_path,
        const SymbolTable& symbols,
        const std::vector<FileData>& files,
        int64_t next_object_id);
};
