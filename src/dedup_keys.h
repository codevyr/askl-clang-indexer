#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

struct EntryKey {
    int64_t id;
    int32_t start;
    int32_t end;
    bool operator==(const EntryKey& o) const {
        return id == o.id && start == o.start && end == o.end;
    }
};

struct EntryKeyHash {
    size_t operator()(const EntryKey& k) const {
        size_t h = std::hash<int64_t>{}(k.id);
        h ^= std::hash<int32_t>{}(k.start) + 0x9e3779b9 + (h << 6) + (h >> 2);
        h ^= std::hash<int32_t>{}(k.end) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
