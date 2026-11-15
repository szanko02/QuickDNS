#pragma once
#include <array>
#include <cstdint>
#include <map>
#include <mutex>
#include <vector>
#include "routing_packet_policy.h"
namespace quickdns {
struct OwnerObservation;
struct FlowKey {
    uint8_t family{}; uint8_t protocol{};
    std::array<uint8_t,16> local{}; std::array<uint8_t,16> remote{};
    uint16_t local_port{}; uint16_t remote_port{};
    auto operator<=>(const FlowKey&) const = default;
};
class FlowRegistry {
public:
    void opened(const FlowKey&, uint32_t pid, PacketOwner);
    void opened(const FlowKey&, uint32_t pid, PacketOwner, std::uint64_t process_creation_time);
    void closed(const FlowKey&, uint32_t pid);
    void closed(const FlowKey&, uint32_t pid, std::uint64_t process_creation_time);
    PacketOwner lookup(const FlowKey&) const;
    PacketOwner lookup_consistent(const FlowKey&, const std::vector<OwnerObservation>& first,
                                  const std::vector<OwnerObservation>& second) const;
private:
    struct Record { PacketOwner owner; std::uint64_t process_creation_time; };
    using Entry = std::map<uint32_t, Record>;
    mutable std::mutex mutex_;
    std::map<FlowKey,Entry> flows_;
};
}

