#include "routing_socket_events.h"
#include <cstring>
using namespace quickdns;
static int opened, closed, shut;
static HANDLE open_fake(const char* filter,WINDIVERT_LAYER layer,INT16,UINT64 flags) {
 if(std::strcmp(filter,"event == CONNECT or event == CLOSE") || layer!=WINDIVERT_LAYER_SOCKET || flags!=(WINDIVERT_FLAG_SNIFF|WINDIVERT_FLAG_RECV_ONLY)) return INVALID_HANDLE_VALUE;
 ++opened; return reinterpret_cast<HANDLE>(1);
}
static BOOL recv_fake(HANDLE,VOID*,UINT,UINT* len,WINDIVERT_ADDRESS* a) { *len=0; a->Layer=WINDIVERT_LAYER_SOCKET; a->Event=WINDIVERT_EVENT_SOCKET_CONNECT; a->Socket.ProcessId=42; return TRUE; }
static BOOL shutdown_fake(HANDLE,WINDIVERT_SHUTDOWN) { ++shut; return TRUE; }
static BOOL close_fake(HANDLE) { ++closed; return TRUE; }
int main() {
 DivertApi api{open_fake,recv_fake,shutdown_fake,close_fake};
 { SocketEvents s(api); WINDIVERT_ADDRESS a{};
   if(!s.start() || !s.read(a) || a.Socket.ProcessId!=42) return 1;
   s.stop(); s.stop(); if(closed!=1 || shut!=1) return 2;
   if(s.read(a)) return 3;
 }
 if(opened!=1 || closed!=1) return 4;
 SocketEvents bad({}); if(bad.start()) return 5;
}

