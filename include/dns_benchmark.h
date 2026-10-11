#pragma once

#include <string>
#include <vector>
#include <functional>
#include <chrono>
#include "config_manager.h"

namespace quickdns {

enum class DnsQuality {
    Fast,        // 🟢 Зеленый: < 150 ms
    Moderate,    // 🟡 Оранжевый: 150 - 350 ms
    SlowOrDown   // 🔴 Красный: > 350 ms или ошибка/тайм-аут
};

struct DnsBenchmarkResult {
    std::string profile_id;
    std::wstring profile_name;
    std::wstring primary_ip;
    std::wstring doh_template;
    int latency_ms = -1; // -1 if failed/timed out
    DnsQuality quality = DnsQuality::SlowOrDown;
    std::wstring status_text;
    std::wstring quality_label;
};

class DnsBenchmark {
public:
    /**
     * @brief Measures DNS query latency against a target DNS IP address in milliseconds.
     * Queries a pool of test domains (chatgpt.com, claude.ai, google.com).
     * @param ip_str IPv4 or IPv6 address of the DNS server.
     * @param timeout_ms Per-query timeout in milliseconds (default 1200ms).
     * @return Latency in milliseconds, or -1 if unreachable/timed out.
     */
    static int ping_dns_server(const std::string& ip_str, int timeout_ms = 1200);

    /**
     * @brief Benchmarks a list of DNS profiles concurrently using multi-threading.
     * @param profiles Profiles to benchmark.
     * @param on_progress Optional callback invoked when each profile test completes.
     * @return Results sorted by latency (fastest first, followed by timeouts).
     */
    static std::vector<DnsBenchmarkResult> run_benchmark(
        const std::vector<DnsProfile>& profiles,
        std::function<void(size_t completed, size_t total, const DnsBenchmarkResult& result)> on_progress = nullptr
    );
};

} // namespace quickdns
