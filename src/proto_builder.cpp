#include "proto_builder.h"
#include "dedup_keys.h"
#include "stage1.h" // for FileData
#include "symbol_table.h"
#include "symbol_types.h"
#include "proto/index.pb.h"
#include "../third_party/picosha2.h"

#include <algorithm>
#include <unordered_map>
#include <unordered_set>

BuildResult ProtoBuilder::build(
    const std::string& project_name,
    const std::string& root_path,
    const SymbolTable& symbols,
    const std::vector<FileData>& files,
    int64_t next_object_id) {

    askl::index::Project project;
    project.set_project_name(project_name);
    project.set_root_path(root_path);

    // Add all symbols and collect directory/module symbol IDs in a single pass
    std::unordered_map<std::string, int64_t> dirSymbolID;
    std::unordered_map<std::string, int64_t> modSymbolID;
    for (auto& sym : symbols.allSymbols()) {
        auto* pb_sym = project.add_symbols();
        pb_sym->set_local_id(sym.local_id);
        pb_sym->set_name(sym.name);
        pb_sym->set_scope(static_cast<askl::index::SymbolScope>(sym.scope));
        pb_sym->set_type(static_cast<askl::index::SymbolType>(sym.type));
        if (sym.type == SYMTYPE_DIRECTORY) {
            dirSymbolID[sym.name] = sym.local_id;
        } else if (sym.type == SYMTYPE_MODULE) {
            modSymbolID[sym.name] = sym.local_id;
        }
    }

    // Collect content entries for ContentBatch messages (pointers to avoid copying)
    std::vector<std::pair<std::string, const std::vector<uint8_t>*>> content_entries; // (hash, content ptr)

    // Add all files as Objects
    for (auto& file : files) {
        auto* obj = project.add_objects();
        obj->set_local_id(file.object_local_id);
        obj->set_module_path(file.module_path);
        obj->set_filesystem_path(file.filesystem_path);
        obj->set_filetype(file.filetype);

        // Compute SHA256 and set content_hash instead of inline content
        std::string hash_hex = picosha2::hash256_hex_string(file.content.begin(), file.content.end());
        obj->set_content_hash(hash_hex);
        // Do NOT set content on the Object — it goes into ContentBatch

        content_entries.emplace_back(hash_hex, &file.content);

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
        // Directory sentinels: no content, no content_hash

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
        // Use file.content.size() via content_entries instead of obj->content().size()
        inst->set_end_offset(static_cast<int32_t>(content_entries[i].second->size()));
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

    // Add module containment instances on file objects
    for (int i = 0; i < file_object_count; ++i) {
        const auto& file = files[i];
        if (file.modname.empty()) continue;
        auto it = modSymbolID.find(file.modname);
        if (it == modSymbolID.end()) continue;
        auto* obj = project.mutable_objects(i);
        auto* inst = obj->add_symbol_instances();
        inst->set_symbol_local_id(it->second);
        inst->set_instance_type(askl::index::InstanceType::CONTAINMENT);
        inst->set_start_offset(0);
        inst->set_end_offset(static_cast<int32_t>(content_entries[i].second->size()));
    }

    // Build result
    BuildResult result;
    project.SerializeToString(&result.project_data);

    // Build ContentBatch messages, splitting when batch exceeds threshold
    askl::index::ContentBatch current_batch;
    size_t current_batch_size = 0;

    for (auto& [hash, content_ptr] : content_entries) {
        auto* oc = current_batch.add_contents();
        oc->set_content_hash(hash);
        oc->set_content(content_ptr->data(), content_ptr->size());
        current_batch_size += content_ptr->size();

        if (current_batch_size >= CONTENT_BATCH_MAX_BYTES) {
            std::string batch_data;
            current_batch.SerializeToString(&batch_data);
            result.content_batches.push_back(std::move(batch_data));
            current_batch.Clear();
            current_batch_size = 0;
        }
    }

    // Flush remaining entries
    if (current_batch.contents_size() > 0) {
        std::string batch_data;
        current_batch.SerializeToString(&batch_data);
        result.content_batches.push_back(std::move(batch_data));
    }

    return result;
}
