#pragma once

#include "routing_adapters.h"
#include <filesystem>
#include <span>

namespace quickdns {
bool has_embedded_routing_engine();
std::wstring new_split_routing_tun_alias();
struct SplitRoutingTarget {
    std::filesystem::path path;
    std::wstring package_family;
};

std::string make_split_routing_config(const RoutingAdapter& primary, const RoutingAdapter& secondary,
                                      std::span<const SplitRoutingTarget> targets, const std::filesystem::path& log,
                                      const std::wstring& tun_alias = L"QuickDNS TUN", bool split_dns = false,
                                      std::span<const std::string> domains = {}, bool public_web = false);

class SplitRoutingSession {
public:
    SplitRoutingSession() = default;
    ~SplitRoutingSession();
    SplitRoutingSession(const SplitRoutingSession&) = delete;
    SplitRoutingSession& operator=(const SplitRoutingSession&) = delete;

    std::wstring start(const std::filesystem::path& engine, const RoutingAdapter& primary,
                       const RoutingAdapter& secondary, std::span<const SplitRoutingTarget> targets,
                       bool split_dns = false, std::span<const std::string> domains = {}, bool public_web = false);
    void stop();
    bool running() const;
    std::wstring status() const;
    std::filesystem::path engine;
    std::wstring primary_id;
    std::wstring secondary_id;

private:
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    std::filesystem::path config_;
    std::filesystem::path log_;
    std::filesystem::path embedded_directory_;
    std::wstring tun_alias_;
};
}
