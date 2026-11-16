#pragma once

#include "routing_adapters.h"
#include <filesystem>

namespace quickdns {
bool has_embedded_routing_engine();
std::string make_split_routing_config(const RoutingAdapter& primary, const RoutingAdapter& secondary,
                                      const std::filesystem::path& app, bool package_directory,
                                      const std::filesystem::path& log, const std::wstring& package_family = {});

class SplitRoutingSession {
public:
    SplitRoutingSession() = default;
    ~SplitRoutingSession();
    SplitRoutingSession(const SplitRoutingSession&) = delete;
    SplitRoutingSession& operator=(const SplitRoutingSession&) = delete;

    std::wstring start(const std::filesystem::path& engine, const RoutingAdapter& primary,
                       const RoutingAdapter& secondary, const std::filesystem::path& app,
                       bool package_directory, const std::wstring& package_family = {});
    void stop();
    bool running() const;
    std::wstring status() const;
    std::filesystem::path engine;
    std::wstring primary_id;

private:
    HANDLE process_ = nullptr;
    HANDLE job_ = nullptr;
    std::filesystem::path config_;
    std::filesystem::path log_;
    std::filesystem::path embedded_directory_;
};
}
