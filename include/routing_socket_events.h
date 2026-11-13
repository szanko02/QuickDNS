#pragma once
#include <windows.h>
#include <memory>
#include "windivert/windivert.h"
namespace quickdns {
struct DivertApi {
    decltype(&WinDivertOpen) open{};
    decltype(&WinDivertRecv) recv{};
    decltype(&WinDivertShutdown) shutdown{};
    decltype(&WinDivertClose) close{};
    decltype(&WinDivertSend) send{};
    std::shared_ptr<void> library;
};
class SocketEvents {
public:
    explicit SocketEvents(DivertApi api);
    ~SocketEvents();
    SocketEvents(const SocketEvents&) = delete;
    SocketEvents& operator=(const SocketEvents&) = delete;
    bool start();
    bool read(WINDIVERT_ADDRESS& event);
    void stop();
private:
    DivertApi api_;
    HANDLE handle_ = INVALID_HANDLE_VALUE;
};
}


