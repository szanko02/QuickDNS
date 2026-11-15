#include "routing_owner_table.h"
#include <stdexcept>
#include <iostream>
using namespace quickdns;
static void require(bool ok, const char* why) { if (!ok) throw std::runtime_error(why); }
static PacketOwner classify_current(HANDLE process, void*) {
 return GetProcessId(process)==GetCurrentProcessId() ? PacketOwner::Other : PacketOwner::Unknown;
}
int main() {
 try {
  FlowKey key{}; key.family=4; key.protocol=6; key.local[0]=10; key.local_port=12345; key.remote[0]=8; key.remote_port=443;
  OwnerObservation a{key, 42, 1234, PacketOwner::Other};
  require(classify_owner_tables(key,{a},{a})==PacketOwner::Other,"stable TCP must identify owner");
  auto b=a; b.creation_time++;
  require(classify_owner_tables(key,{a},{b})==PacketOwner::Unknown,"PID reuse must remain unknown");
  b=a; b.pid++; b.owner=PacketOwner::Protected;
  require(classify_owner_tables(key,{a,b},{a,b})==PacketOwner::Unknown,"ambiguous owners must remain unknown");
  b=a; b.owner=PacketOwner::Unknown;
  require(classify_owner_tables(key,{b},{b})==PacketOwner::Unknown,"inaccessible identity must remain unknown");
  b=a; b.creation_time=0;
  require(classify_owner_tables(key,{b},{b})==PacketOwner::Unknown,"missing creation time must remain unknown");
  b=a; b.flow.remote[0]=1;
  require(classify_owner_tables(key,{a},{b})==PacketOwner::Unknown,"disappearing tuple must remain unknown");
  auto udp=key; udp.protocol=17; OwnerObservation u{udp,42,1234,PacketOwner::Other}; u.flow.remote={}; u.flow.remote_port=0; u.wildcard_local=true;
  require(classify_owner_tables(udp,{u},{u})==PacketOwner::Other,"single UDP wildcard owner must match");
  auto u2=u; u2.pid=99; u2.owner=PacketOwner::Protected;
  require(classify_owner_tables(udp,{u,u2},{u,u2})==PacketOwner::Unknown,"shared UDP port must remain unknown");
  auto v6=key; v6.family=6; v6.local={}; v6.local[0]=0xfe; v6.local[1]=0x80;
  OwnerObservation v{v6,42,1234,PacketOwner::Other};
  require(classify_owner_tables(v6,{v},{v})==PacketOwner::Unknown,"link-local IPv6 without scope must remain unknown");
  auto dual=u; dual.flow.family=6; dual.flow.local={}; dual.scope_ambiguous=true;
  require(classify_owner_tables(udp,{u,dual},{u,dual})==PacketOwner::Unknown,"dual-stack wildcard must remain unknown");
  auto mapped=a; mapped.flow.family=6; mapped.flow.local={}; mapped.flow.local[10]=0xff; mapped.flow.local[11]=0xff; mapped.flow.local[12]=10;
  mapped.flow.remote={}; mapped.flow.remote[10]=0xff; mapped.flow.remote[11]=0xff; mapped.flow.remote[12]=8;
  require(classify_owner_tables(key,{a,mapped},{a,mapped})==PacketOwner::Unknown,"IPv4-mapped IPv6 tuple must remain ambiguous");
  WSADATA wsa{}; require(WSAStartup(MAKEWORD(2,2),&wsa)==0,"WSA startup failed");
  const SOCKET socket_handle=socket(AF_INET,SOCK_DGRAM,IPPROTO_UDP);
  require(socket_handle!=INVALID_SOCKET,"UDP socket failed");
  sockaddr_in address{}; address.sin_family=AF_INET; address.sin_addr.s_addr=htonl(INADDR_LOOPBACK);
  require(bind(socket_handle,reinterpret_cast<sockaddr*>(&address),sizeof(address))==0,"UDP bind failed");
  int length=sizeof(address); require(getsockname(socket_handle,reinterpret_cast<sockaddr*>(&address),&length)==0,"UDP local address failed");
  FlowKey own{}; own.family=4; own.protocol=17; own.local={127,0,0,1}; own.local_port=ntohs(address.sin_port);
  std::vector<OwnerObservation> first,second;
  require(capture_owner_table_snapshot(own,classify_current,nullptr,first) && capture_owner_table_snapshot(own,classify_current,nullptr,second),"OS owner table read failed");
  require(classify_owner_tables(own,first,second)==PacketOwner::Other,"live UDP socket owner not found");
  require(!capture_owner_table_snapshot(own,nullptr,nullptr,first) && first.empty(),"failed read must not expose stale observations");
  closesocket(socket_handle); WSACleanup();
  return 0;
 } catch(const std::exception& e) { std::cerr << e.what() << '\n'; return 1; }
}
