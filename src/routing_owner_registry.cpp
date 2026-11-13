#include "routing_owner_table.h"
namespace quickdns {
PacketOwner FlowRegistry::lookup_consistent(const FlowKey& key, const std::vector<OwnerObservation>& first,
                                            const std::vector<OwnerObservation>& second) const {
    const auto observation = resolve_owner_tables(key, first, second);
    if (!observation) return PacketOwner::Unknown;
    std::lock_guard lock(mutex_);
    const auto it = flows_.find(key);
    if (it == flows_.end()) return observation->owner;
    if (it->second.size() != 1) return PacketOwner::Unknown;
    const auto& [pid, record] = *it->second.begin();
    if (pid != observation->pid || !record.process_creation_time ||
        record.process_creation_time != observation->creation_time || record.owner != observation->owner)
        return PacketOwner::Unknown;
    return record.owner;
}
}
