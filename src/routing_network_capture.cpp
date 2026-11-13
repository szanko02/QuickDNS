#include "routing_network_capture.h"
#include <condition_variable>
#include <limits>
#include <mutex>
namespace quickdns {
struct NetworkCapture::State {
 explicit State(DivertApi a):api(a) {}
 ~State() { if(handle!=INVALID_HANDLE_VALUE && handle && api.close) api.close(handle); }
 DivertApi api; HANDLE handle=INVALID_HANDLE_VALUE; std::mutex mutex; std::condition_variable cv;
 unsigned active=0; bool stopping=false, abandoned=false;
};
NetworkCapture::NetworkCapture(DivertApi a):state_(std::make_shared<State>(a)) {}
NetworkCapture::~NetworkCapture() {
 if(!stop()) { auto s=state_; std::lock_guard lock(s->mutex); s->abandoned=true; }
}
bool NetworkCapture::start() {
 auto s=state_; std::lock_guard lock(s->mutex);
 if(s->handle!=INVALID_HANDLE_VALUE || s->stopping || s->abandoned || !s->api.open || !s->api.recv || !s->api.shutdown || !s->api.close || !s->api.send) return false;
 auto h=s->api.open("ip or ipv6",WINDIVERT_LAYER_NETWORK,0,0);
 if(!h || h==INVALID_HANDLE_VALUE) return false;
 s->handle=h; return true;
}
bool NetworkCapture::read(std::span<unsigned char> buffer,UINT& length,WINDIVERT_ADDRESS& address) {
 auto s=state_; HANDLE h;
 {std::lock_guard lock(s->mutex); if(s->stopping || s->abandoned || s->handle==INVALID_HANDLE_VALUE || buffer.size()>(std::numeric_limits<UINT>::max)()) return false; ++s->active; h=s->handle;}
 UINT next=0; WINDIVERT_ADDRESS result{};
 const bool ok=s->api.recv(h,buffer.data(),static_cast<UINT>(buffer.size()),&next,&result)!=FALSE;
 {std::lock_guard lock(s->mutex); --s->active; s->cv.notify_all(); if(s->stopping || s->abandoned) return false;}
 if(!ok || next>buffer.size() || result.Layer!=WINDIVERT_LAYER_NETWORK) return false;
 length=next; address=result; return true;
}
bool NetworkCapture::send(std::span<const unsigned char> packet, const WINDIVERT_ADDRESS& address) {
 auto s=state_; HANDLE h;
 {std::lock_guard lock(s->mutex); if(s->stopping || s->abandoned || s->handle==INVALID_HANDLE_VALUE || !s->api.send || packet.empty() || packet.size()>(std::numeric_limits<UINT>::max)() || address.Layer!=WINDIVERT_LAYER_NETWORK) return false; ++s->active; h=s->handle;}
 UINT written=0; const bool ok=s->api.send(h,packet.data(),static_cast<UINT>(packet.size()),&written,&address)!=FALSE;
 {std::lock_guard lock(s->mutex); --s->active; s->cv.notify_all(); if(s->stopping || s->abandoned) return false;}
 return ok && written==packet.size();
}
bool NetworkCapture::stop() {
 auto s=state_; std::unique_lock lock(s->mutex);
 if(s->stopping) {s->cv.wait(lock,[&]{return !s->stopping;}); return s->handle==INVALID_HANDLE_VALUE;}
 if(s->handle==INVALID_HANDLE_VALUE) return true;
 s->stopping=true; HANDLE h=s->handle;
 if(!s->api.shutdown(h,WINDIVERT_SHUTDOWN_RECV)) { s->stopping=false; s->cv.notify_all(); return false; }
 s->cv.wait(lock,[&]{return s->active==0;});
 const bool ok=s->api.close(h)!=FALSE;
 if(ok) s->handle=INVALID_HANDLE_VALUE;
 s->stopping=false; s->cv.notify_all(); return ok;
}
}



