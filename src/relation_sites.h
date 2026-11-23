#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>
#include <unordered_set>
#include <vector>

struct FileData;

/// WHERE A RELATION HAPPENED: one place that made it so, beside its row.
///
/// A relation is not something a token says.  Its row sits on the declaration
/// of its source -- the field's declaration for `HOLDS`, the macro's name in
/// its `#define` for `DECLARES` -- so the row says WHO is related.  The code
/// that made it so is somewhere else entirely:
///
///     .read = bt_bmc_read             stores the function in the slot
///     ops->read = bt_bmc_read         the same, by assignment
///     DEFINE_MUTEX(module_mutex)      the invocation that declared it
///
/// Each of those is a PLACE: a file and a byte range in it.  A `RelationSite`
/// is one place of one row, recorded where the row is made (`Stage2`,
/// `Stage1::linkDeclared`).
///
/// The row is identified by the file it is in, its target and its range, which
/// is what identifies a relation row in a file whatever declaration the walk
/// read it inside.  Files are named by index: into the unit's files while a
/// translation unit is being read, into the project's once it is merged.
struct RelationSite {
    size_t row_file;
    int64_t to;
    int32_t row_start, row_end;

    size_t place_file;
    int32_t place_start, place_end;

    bool operator==(const RelationSite& o) const {
        return row_file == o.row_file && to == o.to && row_start == o.row_start &&
               row_end == o.row_end && place_file == o.place_file &&
               place_start == o.place_start && place_end == o.place_end;
    }
};

struct RelationSiteHash {
    size_t operator()(const RelationSite& s) const {
        size_t h = std::hash<int64_t>{}(s.to);
        auto mix = [&h](size_t v) { h ^= v + 0x9e3779b9 + (h << 6) + (h >> 2); };
        mix(s.row_file);
        mix(static_cast<size_t>(static_cast<uint32_t>(s.row_start)));
        mix(static_cast<size_t>(static_cast<uint32_t>(s.row_end)));
        mix(s.place_file);
        mix(static_cast<size_t>(static_cast<uint32_t>(s.place_start)));
        mix(static_cast<size_t>(static_cast<uint32_t>(s.place_end)));
        return h;
    }
};

/// A PLACE READ BY MANY TRANSLATION UNITS IS ONE PLACE.
///
/// A header's initialiser is read by every unit that includes it, and one
/// file can be compiled twice.  The places of the project are a SET, so
/// "how many places" counts text and not readings, and neither it nor "which
/// is first" depends on how many units there were or the order they came in.
using RelationSites = std::unordered_set<RelationSite, RelationSiteHash>;

/// Give every relation row its witness (`FileData::witnesses`): THE FIRST
/// PLACE, AND HOW MANY THERE ARE.
///
/// First is by the path of the file the place is in, then by where it starts,
/// then by where it ends: an order of the text alone.  The indices in `sites`
/// are into `files`.
void resolveWitnesses(std::vector<FileData>& files, const RelationSites& sites);
