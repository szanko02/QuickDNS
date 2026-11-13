#include "routing_flow_registry.h"
using namespace quickdns;
int main() {
 FlowRegistry r; FlowKey a{}; a.family=4; a.protocol=6; a.local[0]=10; a.local_port=40000; a.remote[0]=8; a.remote_port=443;
 FlowKey b=a; b.remote[0]=1; FlowKey c=a; c.family=6;
 if(r.lookup(a)!=PacketOwner::Unknown) return 1;
 r.opened(a,10,PacketOwner::Protected);
 if(r.lookup(a)!=PacketOwner::Protected) return 2;
 if(r.lookup(b)!=PacketOwner::Unknown || r.lookup(c)!=PacketOwner::Unknown) return 3;
 r.opened(b,11,PacketOwner::Other); r.closed(a,11);
 if(r.lookup(a)!=PacketOwner::Protected || r.lookup(b)!=PacketOwner::Other) return 4;
 r.opened(a,12,PacketOwner::Other); if(r.lookup(a)!=PacketOwner::Unknown) return 7; r.closed(a,10);
 if(r.lookup(a)!=PacketOwner::Other) return 5;
 r.closed(a,12); if(r.lookup(a)!=PacketOwner::Unknown) return 6;
}

