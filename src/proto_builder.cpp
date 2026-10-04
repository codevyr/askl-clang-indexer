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
#include <vector>

namespace {
/// Give an object's first instance a root interval when nothing numbered it.
///
/// Stage1::finalizeFileRoots() numbers instances[0] of every file it PARSED.
/// Two kinds of object never go through that walk:
///
///  - a file with no compile command -- everything --include-git-files pulls
///    in -- whose file instance comes from Indexer::addFile;
///  - a directory sentinel, created above out of the symbol table alone.
///
/// Left at [0,0) such a root can contain nothing, because containment is
/// `parent.enter < child.enter < parent.exit` and a zero-width interval
/// satisfies it in neither direction.  For a git-only file that is visible:
/// `file(...) { search(...) }` returns nothing and a match inside the file
/// attaches to the directory instead.
///
/// A parsed file always ends at `last + stride >= stride`, so `dfs_exit == 0`
/// is an exact test for "never numbered".  Such an object has no declarations,
/// so one empty stride is the whole interval it needs.
///
/// The same holds for refs: Stage1 places every ref at its scope's dfs_enter
/// plus one, so a parsed ref is never at 0.  The parent->child refs added to
/// directory sentinels above are, and a ref outside every interval is as
/// invisible as an instance outside one.
///
/// Doing it here, at the single point every producer's objects pass through,
/// is what keeps a future path that appends objects from silently reopening
/// the hole.
void ensureObjectRoot(askl::index::Object* obj, int64_t stride) {
    if (obj->symbol_instances_size() == 0) return;
    auto* root = obj->mutable_symbol_instances(0);
    if (root->dfs_exit() == 0) {
        root->set_dfs_enter(0);
        root->set_dfs_exit(stride);
        root->set_nesting_level(0);
    }
    for (int i = 0; i < obj->refs_size(); ++i) {
        auto* ref = obj->mutable_refs(i);
        if (ref->dfs_enter() != 0) continue;
        ref->set_dfs_enter(root->dfs_enter() + 1);
        ref->set_nesting_level(root->nesting_level() + 1);
        ref->set_fold_depth(root->fold_depth() + (root->folded() ? 1 : 0));
    }
}

/// Slot the enclosing containment instances above the file's own numbering.
///
/// Stage1 numbers each file locally: the file instance at 0 and its
/// declarations above it.  The module and directory instances are only added
/// here, and they are genuine ANCESTORS of the file, not peers -- a module
/// contains directories, a directory contains files.  So make room by shifting
/// the whole file up by one stride per enclosing level and giving the chain the
/// vacated numbers, outermost first.
///
/// Renumbering here rather than reserving a fixed prefix in Stage1 is what
/// handles the chain length varying: a file outside any module has only a
/// directory above it, and a fixed reservation would leave a hole in the depths
/// that breaks "a direct child is at depth + 1".
void nestUnderContainers(askl::index::Object* obj,
                         const std::unordered_set<int64_t>& modIDs,
                         const std::unordered_set<int64_t>& dirIDs,
                         int64_t stride) {
    // Outermost first: a module may span several directories.
    std::vector<askl::index::SymbolInstance*> chain;
    for (int i = 0; i < obj->symbol_instances_size(); ++i)
        if (modIDs.count(obj->symbol_instances(i).symbol_local_id()))
            chain.push_back(obj->mutable_symbol_instances(i));
    for (int i = 0; i < obj->symbol_instances_size(); ++i)
        if (dirIDs.count(obj->symbol_instances(i).symbol_local_id()))
            chain.push_back(obj->mutable_symbol_instances(i));
    if (chain.empty()) return;

    const int64_t k = static_cast<int64_t>(chain.size());
    const int64_t shift = k * stride;

    int64_t max_exit = 0;
    for (int i = 0; i < obj->symbol_instances_size(); ++i) {
        auto* inst = obj->mutable_symbol_instances(i);
        bool is_chain = false;
        for (auto* c : chain) is_chain = is_chain || (c == inst);
        if (is_chain) continue;
        inst->set_dfs_enter(inst->dfs_enter() + shift);
        inst->set_dfs_exit(inst->dfs_exit() + shift);
        inst->set_nesting_level(inst->nesting_level() + static_cast<int32_t>(k));
        max_exit = std::max(max_exit, inst->dfs_exit());
    }
    for (int i = 0; i < obj->refs_size(); ++i) {
        auto* ref = obj->mutable_refs(i);
        ref->set_dfs_enter(ref->dfs_enter() + shift);
        ref->set_nesting_level(ref->nesting_level() + static_cast<int32_t>(k));
    }

    for (int64_t d = 0; d < k; ++d) {
        chain[d]->set_dfs_enter(d * stride);
        chain[d]->set_dfs_exit(max_exit + (k - d) * stride);
        chain[d]->set_nesting_level(static_cast<int32_t>(d));
    }
}
} // namespace

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
                pb_inst->set_nesting_level(inst.nesting_level);
                pb_inst->set_dfs_enter(inst.dfs_enter);
                pb_inst->set_dfs_exit(inst.dfs_exit);
                pb_inst->set_folded(inst.folded);
                pb_inst->set_fold_depth(inst.fold_depth);
                // -1 means the indexer could not determine the span; leave the
                // field unset so "unknown" stays distinct from "empty".
                if (inst.name_start >= 0) {
                    auto* sp = pb_inst->mutable_name();
                    sp->set_start(inst.name_start);
                    sp->set_end(inst.name_end);
                }
                if (inst.sig_start >= 0) {
                    auto* sp = pb_inst->mutable_signature();
                    sp->set_start(inst.sig_start);
                    sp->set_end(inst.sig_end);
                }
                if (inst.content_start >= 0) {
                    auto* sp = pb_inst->mutable_content();
                    sp->set_start(inst.content_start);
                    sp->set_end(inst.content_end);
                }
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
                pb_ref->set_nesting_level(ref.nesting_level);
                pb_ref->set_dfs_enter(ref.dfs_enter);
                pb_ref->set_folded(ref.folded);
                pb_ref->set_fold_depth(ref.fold_depth);
                if (ref.full_start >= 0) {
                    auto* sp = pb_ref->mutable_full();
                    sp->set_start(ref.full_start);
                    sp->set_end(ref.full_end);
                }
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

    // Both containment loops have run, so each file object now knows its full
    // enclosing chain.  Slot it above the file's own numbering.
    {
        std::unordered_set<int64_t> modIDs, dirIDs;
        for (auto& [name, id] : modSymbolID) modIDs.insert(id);
        for (auto& [name, id] : dirSymbolID) dirIDs.insert(id);
        // Every object first, so nestUnderContainers shifts a real interval
        // and the directory sentinels -- which it never visits -- are numbered
        // too.  Already-numbered objects are left alone.
        for (int i = 0; i < project.objects_size(); ++i)
            ensureObjectRoot(project.mutable_objects(i), 1024);
        for (int i = 0; i < file_object_count; ++i)
            nestUnderContainers(project.mutable_objects(i), modIDs, dirIDs, 1024);
    }

    // Decide whether to inline content into Objects or use separate ContentBatch files
    size_t total_content_size = 0;
    for (auto& [hash, content_ptr] : content_entries) {
        total_content_size += content_ptr->size();
    }

    BuildResult result;

    if (total_content_size < CONTENT_BATCH_MAX_BYTES) {
        // Content-inlined mode: set content on each file Object, keep content_hash
        for (int i = 0; i < file_object_count; ++i) {
            auto* obj = project.mutable_objects(i);
            obj->set_content(content_entries[i].second->data(), content_entries[i].second->size());
        }
        result.content_inlined = true;
        project.SerializeToString(&result.project_data);
    } else {
        // Multi-file mode: keep content_hash set, build ContentBatch messages
        result.content_inlined = false;
        project.SerializeToString(&result.project_data);

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
    }

    return result;
}
