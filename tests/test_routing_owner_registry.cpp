#include "routing_owner_table.h"
#include <iostream>
#include <stdexcept>
using namespace quickdns;
static void require(bool v,const char* m){if(!v)throw std::runtime_error(m);}
int main() {
 try {
  FlowRegistry registry; FlowKey key{}; key.family=4; key.protocol=6; key.local[0]=10; key.local_port=5000; key.remote[0]=8; key.remote_port=443;
  OwnerObservation obs{key,10,111,PacketOwner::Other};
  require(registry.lookup_consistent(key,{obs},{obs})==PacketOwner::Other,"unambiguous snapshots must resolve missed event");
  registry.opened(key,10,PacketOwner::Other,111);
  require(registry.lookup_consistent(key,{obs},{obs})==PacketOwner::Other,"matching event and snapshot must agree");
  auto reused=obs; reused.creation_time=222;
  require(registry.lookup_consistent(key,{reused},{reused})==PacketOwner::Unknown,"reused PID must not match old event");
  registry.opened(key,10,PacketOwner::Other);
  require(registry.lookup_consistent(key,{obs},{obs})==PacketOwner::Unknown,"event without generation must not confirm snapshot");
  registry.opened(key,10,PacketOwner::Protected,111);
  require(registry.lookup_consistent(key,{obs},{obs})==PacketOwner::Unknown,"conflicting owner must stay unknown");
  registry.opened(key,11,PacketOwner::Other,111);
  require(registry.lookup_consistent(key,{obs},{obs})==PacketOwner::Unknown,"ambiguous events must stay unknown");
  registry.closed(key,11,111);
  registry.opened(key,10,PacketOwner::Other,222);
  registry.closed(key,10,111);
  require(registry.lookup_consistent(key,{reused},{reused})==PacketOwner::Other,"old close must not remove reused PID event");
  require(registry.lookup_consistent(key,{obs},{obs})==PacketOwner::Unknown,"old close must not erase new PID generation");
  registry.closed(key,10);
  require(registry.lookup_consistent(key,{obs},{obs})==PacketOwner::Unknown,"close without generation must not erase identified event");
  registry.closed(key,10,222);
  require(registry.lookup_consistent(key,{reused},{reused})==PacketOwner::Other,"snapshots must work after matching close");
  return 0;
 } catch(const std::exception& e){std::cerr<<e.what()<<'\n';return 1;}
}
