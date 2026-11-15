#include "routing_packet_processor.h"
#include <array>
#include <vector>
using namespace quickdns;
int main() {
 FlowRegistry flows; FlowKey key{}; key.family=4; key.protocol=17; key.local[0]=10; key.local[3]=1; key.remote[0]=8; key.remote[1]=8; key.remote[2]=8; key.remote[3]=8; key.local_port=12345; key.remote_port=53;
 std::array<uint8_t,4> adapter={192,0,2,1}; std::vector<uint8_t> packet={0x45,0,0,28,0,1,0,0,64,17,0,0,10,0,0,1,8,8,8,8,0x30,0x39,0,53,0,8,0,0};
 auto original=packet;
 if(process_packet(flows,key,packet,adapter,true,true,true)!=PacketAction::Drop || packet!=original) return 1;
 flows.opened(key,10,PacketOwner::Protected);
 if(process_packet(flows,key,packet,adapter,true,true,true)!=PacketAction::RouteSelected || packet[12]!=192) return 2;
 packet=original; if(process_packet(flows,key,packet,adapter,true,false,true)!=PacketAction::Drop || packet!=original) return 3;
 flows.opened(key,11,PacketOwner::Other); if(process_packet(flows,key,packet,adapter,true,true,true)!=PacketAction::Drop) return 4;
 flows.closed(key,10); if(process_packet(flows,key,packet,adapter,true,true,true)!=PacketAction::Pass || packet!=original) return 5;
 if(process_packet(flows,key,packet,{},true,false,false)!=PacketAction::Pass || packet!=original) return 9;
 flows.closed(key,11); flows.opened(key,10,PacketOwner::Protected); packet[25]=9; auto bad=packet;
  if(process_packet(flows,key,packet,adapter,true,true,true)!=PacketAction::Drop || packet!=bad) return 6;
 std::vector<uint8_t> reply={0x45,0,0,28,0,1,0,0,64,17,0,0,8,8,8,8,192,0,2,1,0,53,0x30,0x39,0,8,0,0};
 if(process_packet(flows,key,reply,adapter,false,true,true)!=PacketAction::RouteSelected || reply[16]!=10 || reply[19]!=1) return 7;
 FlowKey k6{}; k6.family=6; k6.protocol=6; k6.local[0]=0x20; k6.local[15]=1; k6.remote[0]=0x20; k6.remote[15]=2; k6.local_port=12345; k6.remote_port=443;
 std::array<uint8_t,16> a6{}; a6[0]=0x20; a6[15]=3; std::vector<uint8_t> p6(60); p6[0]=0x60; p6[5]=20; p6[6]=6; p6[8]=0x20; p6[23]=1; p6[24]=0x20; p6[39]=2; p6[40]=0x30; p6[41]=0x39; p6[42]=1; p6[43]=187; p6[52]=0x50;
 flows.opened(k6,12,PacketOwner::Protected); if(process_packet(flows,k6,p6,a6,true,true,true)!=PacketAction::RouteSelected || p6[23]!=3) return 8;
}


