#include "routing_packet_processor.h"
#include "routing_nat.h"
#include <algorithm>
namespace quickdns {
PacketAction process_packet(const FlowRegistry& flows,const FlowKey& flow,std::span<uint8_t> p,std::span<const uint8_t> adapter,bool outbound,bool adapter_ready,bool backend_ready) {
 if(p.empty()) return PacketAction::Drop;
 const bool v4=(p[0]>>4)==4; const bool v6=(p[0]>>4)==6;
 const size_t h=v4?20:40, n=v4?4:16, local=outbound?(v4?12:8):(v4?16:24), remote=outbound?(v4?16:24):(v4?12:8);
 if((!v4&&!v6) || p.size()<h+8 || flow.family!=(v4?4:6)) return PacketAction::Drop;
 if(v4 && ((p[0]&15)!=5 || (p[6]&0x3f) || p[7])) return PacketAction::Drop;
 const uint8_t proto=p[v4?9:6]; if(proto!=flow.protocol || (proto!=6 && proto!=17)) return PacketAction::Drop;
 const auto& expected_local=flow.local; const auto& expected_remote=flow.remote;
 if(!std::equal(p.begin()+remote,p.begin()+remote+n,expected_remote.begin())) return PacketAction::Drop;
 const auto owner=flows.lookup(flow);
 if(owner==PacketOwner::Unknown) return PacketAction::Drop;
 if(outbound || owner==PacketOwner::Other) {
  if(!std::equal(p.begin()+local,p.begin()+local+n,expected_local.begin())) return PacketAction::Drop;
 } else {
  if(adapter.size()!=n || !std::equal(p.begin()+local,p.begin()+local+n,adapter.begin())) return PacketAction::Drop;
 }
 auto port=[&](size_t at){return static_cast<uint16_t>((p[at]<<8)|p[at+1]);};
 if(port(h+(outbound?0:2))!=flow.local_port || port(h+(outbound?2:0))!=flow.remote_port) return PacketAction::Drop;
 const auto action=decide_packet(owner,adapter_ready,backend_ready);
 if(action!=PacketAction::RouteSelected) return action;
 if(adapter.size()!=n) return PacketAction::Drop;
 const std::span<const uint8_t> addr=outbound?adapter:std::span<const uint8_t>(flow.local.data(),n);
 return rewrite_address(p,addr,outbound)?PacketAction::RouteSelected:PacketAction::Drop;
}
}
