#include "routing_socket_events.h"
namespace quickdns {
SocketEvents::SocketEvents(DivertApi a):api_(a) {}
SocketEvents::~SocketEvents() { stop(); }
bool SocketEvents::start() {
    if (handle_ != INVALID_HANDLE_VALUE || !api_.open || !api_.recv || !api_.shutdown || !api_.close) return false;
    handle_ = api_.open("event == CONNECT or event == CLOSE", WINDIVERT_LAYER_SOCKET, 1000,
                        WINDIVERT_FLAG_SNIFF | WINDIVERT_FLAG_RECV_ONLY);
    return handle_ != INVALID_HANDLE_VALUE && handle_ != nullptr;
}
bool SocketEvents::read(WINDIVERT_ADDRESS& event) {
    if (!handle_ || handle_ == INVALID_HANDLE_VALUE) return false;
    UINT len=0; WINDIVERT_ADDRESS next{};
    if (!api_.recv(handle_, nullptr, 0, &len, &next)) return false;
    if (next.Layer != WINDIVERT_LAYER_SOCKET || (next.Event != WINDIVERT_EVENT_SOCKET_CONNECT && next.Event != WINDIVERT_EVENT_SOCKET_CLOSE)) return false;
    event=next; return true;
}
void SocketEvents::stop() {
    if (!handle_ || handle_ == INVALID_HANDLE_VALUE) { handle_=INVALID_HANDLE_VALUE; return; }
    api_.shutdown(handle_, WINDIVERT_SHUTDOWN_RECV);
    api_.close(handle_); handle_=INVALID_HANDLE_VALUE;
}
}
