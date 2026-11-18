#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include <winsock2.h>
#include <atomic>
#include <mutex>

namespace quickdns {

inline bool ensure_winsock() {
    static std::atomic<bool> initialized{false};
    if (initialized.load(std::memory_order_acquire)) {
        return true;
    }

    static std::mutex init_mutex;
    std::lock_guard<std::mutex> lock(init_mutex);
    if (initialized.load(std::memory_order_relaxed)) {
        return true;
    }

    WSADATA wsaData{};
    if (WSAStartup(MAKEWORD(2, 2), &wsaData) == 0) {
        initialized.store(true, std::memory_order_release);
        return true;
    }
    return false;
}

} // namespace quickdns
