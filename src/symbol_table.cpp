#include "symbol_table.h"

std::pair<int64_t, bool> SymbolTable::getOrCreate(
    const std::string& name, int scope, int type,
    const std::string& file_path) {

    // For GLOBAL symbols (scope == 2), dedup by (name, type)
    // For LOCAL symbols (scope == 1), dedup by (name, type, file_path)
    SymbolKey key{name, type, (scope == 1) ? file_path : ""};

    std::lock_guard<std::mutex> lock(mutex_);
    auto it = symbols_.find(key);
    if (it != symbols_.end()) {
        return {it->second, false};
    }

    int64_t id = next_id_++;
    symbols_[key] = id;
    symbol_list_.push_back({id, name, scope, type});
    return {id, true};
}

std::vector<SymbolInfo> SymbolTable::allSymbols() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return symbol_list_;
}
