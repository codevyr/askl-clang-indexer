#pragma once

#include <string>
#include <vector>

class SymbolTable;
struct FileData;

class ProtoBuilder {
public:
    static std::string build(
        const std::string& project_name,
        const std::string& root_path,
        const SymbolTable& symbols,
        const std::vector<FileData>& files);
};
