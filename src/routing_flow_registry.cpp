#include "routing_flow_registry.h"
namespace quickdns {
void FlowRegistry::opened(const FlowKey& key, uint32_t pid, PacketOwner owner) {
    opened(key, pid, owner, 0);
}
void FlowRegistry::opened(const FlowKey& key, uint32_t pid, PacketOwner owner, std::uint64_t creation_time) {
    std::lock_guard lock(mutex_);
    flows_[key].insert_or_assign(pid, Record{owner, creation_time});
}
void FlowRegistry::closed(const FlowKey& key, uint32_t pid) {
    closed(key, pid, 0);
}
void FlowRegistry::closed(const FlowKey& key, uint32_t pid, std::uint64_t creation_time) {
    std::lock_guard lock(mutex_);
    auto it = flows_.find(key);
    if (it == flows_.end()) return;
    auto entry = it->second.find(pid);
    if (entry == it->second.end() || entry->second.process_creation_time != creation_time) return;
    it->second.erase(entry);
    if (it->second.empty()) flows_.erase(it);
}
PacketOwner FlowRegistry::lookup(const FlowKey& key) const {
    std::lock_guard lock(mutex_);
    auto it = flows_.find(key);
    if (it == flows_.end() || it->second.size() != 1) return PacketOwner::Unknown;
    return it->second.begin()->second.owner;
}
}
