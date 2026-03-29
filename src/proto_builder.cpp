#include "proto_builder.h"
#include "stage1.h" // for FileData
#include "symbol_table.h"
#include "proto/index.pb.h"

#include <algorithm>
#include <unordered_set>

std::string ProtoBuilder::build(
    const std::string& project_name,
    const std::string& root_path,
    const SymbolTable& symbols,
    const std::vector<FileData>& files) {

    askl::index::Project project;
    project.set_project_name(project_name);
    project.set_root_path(root_path);

    // Add all symbols
    for (auto& sym : symbols.allSymbols()) {
        auto* pb_sym = project.add_symbols();
        pb_sym->set_local_id(sym.local_id);
        pb_sym->set_name(sym.name);
        pb_sym->set_scope(static_cast<askl::index::SymbolScope>(sym.scope));
        pb_sym->set_type(static_cast<askl::index::SymbolType>(sym.type));
    }

    // Dedup key: (symbol_id, start_offset, end_offset)
    struct EntryKey {
        int64_t sym_id;
        int32_t start;
        int32_t end;
        bool operator==(const EntryKey& o) const {
            return sym_id == o.sym_id && start == o.start && end == o.end;
        }
    };
    struct EntryKeyHash {
        size_t operator()(const EntryKey& k) const {
            size_t h = std::hash<int64_t>{}(k.sym_id);
            h ^= std::hash<int32_t>{}(k.start) + 0x9e3779b9 + (h << 6) + (h >> 2);
            h ^= std::hash<int32_t>{}(k.end) + 0x9e3779b9 + (h << 6) + (h >> 2);
            return h;
        }
    };

    // Add all files as Objects
    for (auto& file : files) {
        auto* obj = project.add_objects();
        obj->set_local_id(file.object_local_id);
        obj->set_module_path(file.module_path);
        obj->set_filesystem_path(file.filesystem_path);
        obj->set_filetype(file.filetype);
        obj->set_content(file.content.data(), file.content.size());

        // Deduplicate instances by (symbol_local_id, start_offset, end_offset)
        std::unordered_set<EntryKey, EntryKeyHash> seen_instances;
        for (auto& inst : file.instances) {
            EntryKey key{inst.symbol_local_id, inst.start_offset, inst.end_offset};
            if (seen_instances.insert(key).second) {
                auto* pb_inst = obj->add_symbol_instances();
                pb_inst->set_symbol_local_id(inst.symbol_local_id);
                pb_inst->set_start_offset(inst.start_offset);
                pb_inst->set_end_offset(inst.end_offset);
            }
        }

        // Deduplicate refs by (to_symbol_local_id, from_offset_start, from_offset_end)
        std::unordered_set<EntryKey, EntryKeyHash> seen_refs;
        for (auto& ref : file.refs) {
            EntryKey key{ref.to_symbol_local_id, ref.from_offset_start, ref.from_offset_end};
            if (seen_refs.insert(key).second) {
                auto* pb_ref = obj->add_refs();
                pb_ref->set_to_symbol_local_id(ref.to_symbol_local_id);
                pb_ref->set_from_offset_start(ref.from_offset_start);
                pb_ref->set_from_offset_end(ref.from_offset_end);
            }
        }
    }

    std::string output;
    project.SerializeToString(&output);
    return output;
}
