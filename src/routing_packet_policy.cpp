#include "routing_packet_policy.h"
namespace quickdns {
PacketAction decide_packet(PacketOwner owner, bool adapter_ready, bool backend_ready) {
    if (owner == PacketOwner::Other) return PacketAction::Pass;
    if (owner == PacketOwner::Protected && adapter_ready && backend_ready) return PacketAction::RouteSelected;
    return PacketAction::Drop;
}
}
