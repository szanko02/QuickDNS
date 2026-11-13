#pragma once
namespace quickdns {
enum class PacketOwner { Protected, Other, Unknown };
enum class PacketAction { RouteSelected, Pass, Drop };
PacketAction decide_packet(PacketOwner owner, bool adapter_ready, bool backend_ready);
}
