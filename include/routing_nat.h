#pragma once
#include <cstdint>
#include <span>
namespace quickdns {
// Rewrites source (outbound) or destination (inbound). Rejects fragmented or unsupported packets without modification.
bool rewrite_address(std::span<uint8_t> packet, std::span<const uint8_t> address, bool source);
}
