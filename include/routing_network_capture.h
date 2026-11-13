#pragma once
#include "routing_socket_events.h"
#include <memory>
#include <span>
namespace quickdns {
class NetworkCapture {
public:
 explicit NetworkCapture(DivertApi api);
 ~NetworkCapture();
 NetworkCapture(const NetworkCapture&) = delete;
 NetworkCapture& operator=(const NetworkCapture&) = delete;
 bool start();
 bool read(std::span<unsigned char> buffer, UINT& length, WINDIVERT_ADDRESS& address);
 bool send(std::span<const unsigned char> packet, const WINDIVERT_ADDRESS& address);
 bool stop();
private:
 struct State;
 std::shared_ptr<State> state_;
};
}

