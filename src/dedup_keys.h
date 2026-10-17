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

/// WHAT IDENTIFIES AN INSTANCE: a declaration in the text.
///
/// Not a parse of that text.  A header without an include guard is parsed once
/// per inclusion -- glibc's `libc-header-start.h` is built that way on purpose,
/// so every public header can re-run its `__USE_*` setup -- and each parse used
/// to record the same `#define` again.  Measured on criu: 146 keys carrying 366
/// instances beyond the first, 31 of them whole duplicated subtrees.
///
/// The type belongs in the key.  A forward declaration and a definition of the
/// same symbol are two declarations and may even share an extent; collapsing
/// them would lose the distinction `DECLARATION` vs `DEFINITION` exists for.
///
/// The SYMBOL is what keeps the genuinely distinct case apart, and it is the
/// reason this key is safe where `(symbol, extent)` was not: one `DECL(alpha)`
/// line included twice under different macros declares `alpha_f` and
/// `alpha_d` -- two symbols at one extent, two keys, both kept
/// (`test/reincluded_header`).
struct InstKey {
    int64_t symbol;
    int32_t type;
    int32_t start;
    int32_t end;
    bool operator==(const InstKey& o) const {
        return symbol == o.symbol && type == o.type && start == o.start && end == o.end;
    }
};

struct InstKeyHash {
    size_t operator()(const InstKey& k) const {
        size_t h = std::hash<int64_t>{}(k.symbol);
        for (int32_t v : {k.type, k.start, k.end})
            h ^= std::hash<int32_t>{}(v) + 0x9e3779b9 + (h << 6) + (h >> 2);
        return h;
    }
};
