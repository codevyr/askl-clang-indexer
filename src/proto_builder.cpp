#include "proto_builder.h"
#include "stage1.h" // for FileData
#include "symbol_table.h"
#include "symbol_types.h"
#include "proto/index.pb.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

std::string ProtoBuilder::build(
    const std::string& project_name,
    const std::string& root_path,
    const SymbolTable& symbols,
    const std::vector<FileData>& files,
    int64_t next_object_id) {

    askl::index::Project project;
    project.set_project_name(project_name);
    project.set_root_path(root_path);

    // Add all symbols and collect directory symbol IDs in a single pass
    std::unordered_map<std::string, int64_t> dirSymbolID;
    for (auto& sym : symbols.allSymbols()) {
        auto* pb_sym = project.add_symbols();
        pb_sym->set_local_id(sym.local_id);
        pb_sym->set_name(sym.name);
        pb_sym->set_scope(static_cast<askl::index::SymbolScope>(sym.scope));
        pb_sym->set_type(static_cast<askl::index::SymbolType>(sym.type));
        if (sym.type == SYMTYPE_DIRECTORY) {
            dirSymbolID[sym.name] = sym.local_id;
        }
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
                pb_inst->set_instance_type(static_cast<askl::index::InstanceType>(inst.instance_type));
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

    // Snapshot file object count before adding directory sentinels
    const int file_object_count = project.objects_size();

    // Create sentinel objects for each directory (self-instance [0,0))
    // Store indices (not pointers) since add_objects() can invalidate pointers.
    std::unordered_map<std::string, int> dirObjectIndex;
    for (auto& [dirPath, symID] : dirSymbolID) {
        int idx = project.objects_size();
        auto* sentinel = project.add_objects();
        sentinel->set_local_id(next_object_id++);
        sentinel->set_module_path(dirPath);
        sentinel->set_filesystem_path(dirPath);
        sentinel->set_filetype("directory");

        auto* inst = sentinel->add_symbol_instances();
        inst->set_symbol_local_id(symID);
        inst->set_instance_type(askl::index::InstanceType::SENTINEL);
        inst->set_start_offset(0);
        inst->set_end_offset(0);

        dirObjectIndex[dirPath] = idx;
    }

    // Add directory instances on child file objects for containment
    for (int i = 0; i < file_object_count; ++i) {
        auto* obj = project.mutable_objects(i);
        const std::string& path = obj->filesystem_path();
        auto last_slash = path.rfind('/');
        if (last_slash == std::string::npos) continue;
        std::string parentDir = path.substr(0, last_slash);
        auto it = dirSymbolID.find(parentDir);
        if (it == dirSymbolID.end()) continue;
        auto* inst = obj->add_symbol_instances();
        inst->set_symbol_local_id(it->second);
        inst->set_instance_type(askl::index::InstanceType::CONTAINMENT);
        inst->set_start_offset(0);
        inst->set_end_offset(static_cast<int32_t>(obj->content().size()));
    }

    // Add parent→child refs on directory sentinels
    for (auto& [dirPath, symID] : dirSymbolID) {
        auto last_slash = dirPath.rfind('/');
        if (last_slash == std::string::npos || last_slash == 0) continue;
        std::string parentPath = dirPath.substr(0, last_slash);
        auto parentIt = dirObjectIndex.find(parentPath);
        if (parentIt == dirObjectIndex.end()) continue;
        auto* parentObj = project.mutable_objects(parentIt->second);
        auto* ref = parentObj->add_refs();
        ref->set_to_symbol_local_id(symID);
        ref->set_from_offset_start(0);
        ref->set_from_offset_end(0);
    }

    std::string output;
    project.SerializeToString(&output);
    return output;
}
