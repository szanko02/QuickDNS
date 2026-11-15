#include "routing_network_capture.h"
#include <atomic>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <chrono>
#include <cstring>
#include <memory>
using namespace quickdns;
static std::mutex m; static std::condition_variable cv; static bool receiving=false, done=false; static int closes=0, shutdowns=0; static bool fail_shutdown=false;
static HANDLE openfake(const char* f,WINDIVERT_LAYER layer,INT16,UINT64 flags) { return !std::strcmp(f,"ip or ipv6") && layer==WINDIVERT_LAYER_NETWORK && flags==0 ? reinterpret_cast<HANDLE>(1) : INVALID_HANDLE_VALUE; }
static BOOL recvfake(HANDLE,void*,UINT,UINT*,WINDIVERT_ADDRESS*) { std::unique_lock lock(m); receiving=true; cv.notify_all(); cv.wait(lock,[]{return done;}); return FALSE; }
static BOOL shut(HANDLE,WINDIVERT_SHUTDOWN) { std::lock_guard lock(m); ++shutdowns; if(fail_shutdown) return FALSE; done=true; cv.notify_all(); return TRUE; }
static BOOL sendfake(HANDLE,const void* p,UINT n,UINT* sent,const WINDIVERT_ADDRESS* a) { if(n!=3 || static_cast<const unsigned char*>(p)[0]!=42 || a->Layer!=WINDIVERT_LAYER_NETWORK) return FALSE; *sent=n; return TRUE; }
static BOOL closefake(HANDLE) { ++closes; return TRUE; }
int main() {
 DivertApi api{openfake,recvfake,shut,closefake,sendfake}; NetworkCapture cap(api); if(!cap.start() || cap.start()) return 1; unsigned char pkt[3]={42,2,3}; WINDIVERT_ADDRESS out{}; out.Layer=WINDIVERT_LAYER_NETWORK; if(!cap.send(pkt,out)) return 13;
 bool got=true; std::thread t([&]{unsigned char buf[1500]; UINT n=1; WINDIVERT_ADDRESS a{}; got=cap.read(buf,n,a);});
 {std::unique_lock lock(m); if(!cv.wait_for(lock,std::chrono::seconds(2),[]{return receiving;})) {t.detach(); return 2;} }
 cap.stop(); t.join(); cap.stop(); if(got||closes!=1||shutdowns!=1) return 3;
 unsigned char b[8]; UINT n=0; WINDIVERT_ADDRESS a{}; if(cap.read(b,n,a)) return 4;
  NetworkCapture bad({}); if(bad.start()) return 5; DivertApi missing_send=api; missing_send.send=nullptr; NetworkCapture no_send(missing_send); if(no_send.start()) return 14;
 receiving=false; done=false; fail_shutdown=true;
 NetworkCapture retry(api); if(!retry.start()) return 6;
 std::thread t2([&]{unsigned char b2[16]; UINT n2=0; WINDIVERT_ADDRESS a2{}; retry.read(b2,n2,a2);});
 {std::unique_lock lock(m); if(!cv.wait_for(lock,std::chrono::seconds(2),[]{return receiving;})) {t2.detach(); return 7;} }
 if(retry.stop()) {t2.detach(); return 8;}
  fail_shutdown=false; if(!retry.stop()) {t2.detach(); return 9;} t2.join();
 receiving=false; done=false; fail_shutdown=true; const int prior=closes;
 auto dying=std::make_unique<NetworkCapture>(api); if(!dying->start()) return 10;
 auto raw=dying.get(); std::thread t3([raw]{unsigned char bb[16]; UINT nn=0; WINDIVERT_ADDRESS aa{}; raw->read(bb,nn,aa);});
 {std::unique_lock lock(m); if(!cv.wait_for(lock,std::chrono::seconds(2),[]{return receiving;})) {t3.detach(); return 11;} }
 dying.reset(); {std::lock_guard lock(m); done=true; cv.notify_all();} t3.join();
 if(closes!=prior+1) return 12;
}




