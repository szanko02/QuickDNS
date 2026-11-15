#include "routing_packet_policy.h"
#include <iostream>
using namespace quickdns;
int main() {
    if (decide_packet(PacketOwner::Protected, true, true) != PacketAction::RouteSelected) return 1;
    if (decide_packet(PacketOwner::Protected, false, true) != PacketAction::Drop) return 2;
    if (decide_packet(PacketOwner::Protected, true, false) != PacketAction::Drop) return 3;
    if (decide_packet(PacketOwner::Unknown, true, true) != PacketAction::Drop) return 4;
    if (decide_packet(PacketOwner::Other, false, false) != PacketAction::Pass) return 5;
    std::cout << "routing decisions passed\n";
}
