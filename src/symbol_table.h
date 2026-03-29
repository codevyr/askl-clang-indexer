#pragma once

#include <cstdint>
#include <mutex>
#include <string>
#include <unordered_map>
#include <vector>

struct SymbolKey {
    std::string name;
    int type;
    std::string file_path; // empty for GLOBAL, file path for LOCAL

    bool operator==(const SymbolKey& other) const {
        return name == other.name && type == other.type && file_path == other.file_path;
    }
};

struct SymbolKeyHash {
    size_t operator()(const SymbolKey& k) const {
        size_t h = std::hash<std::string>{}(k.name);
        h ^= std::hash<int>{}(k.type) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<std::string>{}(k.file_path) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};

struct SymbolInfo {
    int64_t local_id;
    std::string name;
    int scope;
    int type;
};

class SymbolTable {
public:
    // Returns (symbol_local_id, was_new). Thread-safe.
    std::pair<int64_t, bool> getOrCreate(
        const std::string& name, int scope, int type,
        const std::string& file_path = "");

    std::vector<SymbolInfo> allSymbols() const;

private:
    mutable std::mutex mutex_;
    std::unordered_map<SymbolKey, int64_t, SymbolKeyHash> symbols_;
    std::vector<SymbolInfo> symbol_list_;
    int64_t next_id_ = 1;
};
