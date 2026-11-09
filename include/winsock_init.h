#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <winsock2.h>
#include <mutex>

namespace quickdns {

inline void ensure_winsock() {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    });
}

} // namespace quickdns
