#pragma once
#include "routing_flow_registry.h"
#include <span>
namespace quickdns {
PacketAction process_packet(const FlowRegistry& flows, const FlowKey& flow, std::span<uint8_t> packet,
                            std::span<const uint8_t> adapter_address, bool outbound, bool adapter_ready, bool backend_ready);
}
