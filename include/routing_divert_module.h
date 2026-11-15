#pragma once
#include "routing_socket_events.h"
#include <filesystem>
namespace quickdns {
class DivertModule {
public:
    ~DivertModule();
    DivertModule() = default;
    DivertModule(const DivertModule&) = delete;
    DivertModule& operator=(const DivertModule&) = delete;
    bool load(const std::filesystem::path& path);
    DivertApi api() const { return api_; }
private:
    HMODULE module_{};
    DivertApi api_{};
};
}
