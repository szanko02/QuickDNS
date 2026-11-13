#pragma once
#include "routing_flow_registry.h"
#include <vector>
#include <optional>
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>

namespace quickdns {
struct OwnerObservation {
    FlowKey flow;
    std::uint32_t pid = 0;
    std::uint64_t creation_time = 0;
    PacketOwner owner = PacketOwner::Unknown;
    bool wildcard_local = false;
    bool scope_ambiguous = false;
};

PacketOwner classify_owner_tables(const FlowKey& packet, const std::vector<OwnerObservation>& first,
                                  const std::vector<OwnerObservation>& second);
std::optional<OwnerObservation> resolve_owner_tables(const FlowKey& packet,
    const std::vector<OwnerObservation>& first, const std::vector<OwnerObservation>& second);
using ProcessOwnerClassifier = PacketOwner (*)(HANDLE process, void* context);
// Snapshots and process identities are advisory. The caller must not claim a
// leak-proof kill switch solely from this result: PID/flow state can change.
bool capture_owner_table_snapshot(const FlowKey& packet, ProcessOwnerClassifier classifier,
                                  void* context, std::vector<OwnerObservation>& result);
}
