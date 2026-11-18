#include "winsock_init.h"
#include <winsock2.h>
#include <ws2tcpip.h>

#include "dns_benchmark.h"
#include "encoding_utils.h"

#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <random>
#include <thread>
#include <tuple>
#include <vector>

using QuickDns::Encoding::utf8_to_wide;

namespace {

using quickdns::ensure_winsock;

const std::vector<std::string> BENCHMARK_DOMAINS = {
    "chatgpt.com",
    "claude.ai",
    "google.com"
};

bool parse_ipv6_with_optional_scope(const std::string& ip_str, sockaddr_in6& sa6) {
    if (inet_pton(AF_INET6, ip_str.c_str(), &sa6.sin6_addr) == 1) {
        return true;
    }
    size_t pct = ip_str.find('%');
    if (pct == std::string::npos || pct == 0 || pct + 1 >= ip_str.size()) {
        return false;
    }
    std::string addr_part = ip_str.substr(0, pct);
    std::string scope_part = ip_str.substr(pct + 1);
    if (inet_pton(AF_INET6, addr_part.c_str(), &sa6.sin6_addr) != 1) {
        return false;
    }
    unsigned long long scope_val = 0;
    for (char ch : scope_part) {
        if (ch < '0' || ch > '9') {
            return false;
        }
        scope_val = scope_val * 10ULL + static_cast<unsigned long long>(ch - '0');
        if (scope_val > 0xFFFFFFFFULL) {
            return false;
        }
    }
    sa6.sin6_scope_id = static_cast<ULONG>(scope_val);
    return true;
}

} // anonymous namespace

namespace quickdns {

std::string DnsBenchmark::sanitize_domain(const std::string& input_domain) {
    std::string domain = input_domain;
    while (!domain.empty() && std::isspace(static_cast<unsigned char>(domain.front()))) {
        domain.erase(domain.begin());
    }
    while (!domain.empty() && std::isspace(static_cast<unsigned char>(domain.back()))) {
        domain.pop_back();
    }

    for (char& c : domain) {
        c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    }

    if (domain.rfind("https://", 0) == 0) {
        domain.erase(0, 8);
    } else if (domain.rfind("http://", 0) == 0) {
        domain.erase(0, 7);
    } else if (size_t scheme_pos = domain.find("://"); scheme_pos != std::string::npos) {
        domain.erase(0, scheme_pos + 3);
    } else if (domain.rfind("//", 0) == 0) {
        domain.erase(0, 2);
    }

    size_t path_pos = domain.find_first_of("/?#\\");
    if (path_pos != std::string::npos) {
        domain.erase(path_pos);
    }

    size_t at_pos = domain.rfind('@');
    if (at_pos != std::string::npos) {
        domain.erase(0, at_pos + 1);
    }

    size_t colon = domain.find(':');
    if (colon != std::string::npos) {
        domain.erase(colon);
    }

    while (!domain.empty() && domain.back() == '.') {
        domain.pop_back();
    }

    std::string result;
    size_t start = 0;
    while (start < domain.size()) {
        size_t dot = domain.find('.', start);
        if (dot == std::string::npos) {
            dot = domain.size();
        }
        if (dot > start) {
            std::string clean_label;
            clean_label.reserve(dot - start);
            for (size_t i = start; i < dot; ++i) {
                char ch = domain[i];
                if ((ch >= 'a' && ch <= 'z') || (ch >= '0' && ch <= '9') || ch == '-') {
                    clean_label.push_back(ch);
                }
            }
            while (!clean_label.empty() && clean_label.front() == '-') {
                clean_label.erase(clean_label.begin());
            }
            if (clean_label.size() > 63) {
                clean_label.resize(63);
            }
            while (!clean_label.empty() && clean_label.back() == '-') {
                clean_label.pop_back();
            }
            if (!clean_label.empty()) {
                if (!result.empty()) {
                    result.push_back('.');
                }
                result.append(clean_label);
            }
        }
        start = dot + 1;
    }

    if (result.size() > 253) {
        result.resize(253);
        while (!result.empty() && (result.back() == '.' || result.back() == '-')) {
            result.pop_back();
        }
    }

    if (result.empty()) {
        result = "google.com";
    }
    return result;
}

bool DnsBenchmark::validate_dns_response(const uint8_t* buf, size_t len, uint16_t expected_txid) {
    if (!buf || len < 12) {
        return false;
    }

    uint16_t txid = (static_cast<uint16_t>(buf[0]) << 8) | static_cast<uint16_t>(buf[1]);
    if (txid != expected_txid) {
        return false;
    }

    // QR bit is bit 7 of byte 2: must be 1 for DNS response
    bool qr_bit = (buf[2] & 0x80) != 0;
    if (!qr_bit) {
        return false;
    }

    // RCODE is low 4 bits of byte 3: must be 0 (NOERROR) or 3 (NXDOMAIN)
    uint8_t rcode = buf[3] & 0x0F;
    if (rcode != 0 && rcode != 3) {
        return false;
    }

    return true;
}

int DnsBenchmark::ping_dns_server_domain(const std::string& ip_str, const std::string& target_domain, int timeout_ms) {
    if (ip_str.empty()) return -1;
    if (!ensure_winsock()) return -1;

    const int clamped_timeout = std::max(1, timeout_ms);
    std::string domain = sanitize_domain(target_domain);

    sockaddr_in sa4{};
    sockaddr_in6 sa6{};
    bool is_v4 = (inet_pton(AF_INET, ip_str.c_str(), &sa4.sin_addr) == 1);
    bool is_v6 = (!is_v4 && parse_ipv6_with_optional_scope(ip_str, sa6));

    if (!is_v4 && !is_v6) {
        return -1;
    }

    SOCKET sock = socket(is_v4 ? AF_INET : AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return -1;
    }

    DWORD to = static_cast<DWORD>(clamped_timeout);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));

    const sockaddr* p_sa = nullptr;
    int sa_len = 0;
    if (is_v4) {
        sa4.sin_family = AF_INET;
        sa4.sin_port = htons(53);
        p_sa = reinterpret_cast<const sockaddr*>(&sa4);
        sa_len = sizeof(sa4);
    } else {
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(53);
        p_sa = reinterpret_cast<const sockaddr*>(&sa6);
        sa_len = sizeof(sa6);
    }

    if (connect(sock, p_sa, sa_len) == SOCKET_ERROR) {
        closesocket(sock);
        return -1;
    }

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint16_t> dist(1, 0xFFFE);

    std::vector<uint8_t> packet;
    packet.reserve(64 + domain.size());

    uint16_t txid = dist(rng);
    packet.push_back(static_cast<uint8_t>(txid >> 8));
    packet.push_back(static_cast<uint8_t>(txid & 0xFF));
    packet.push_back(0x01); packet.push_back(0x00); // Standard query with RD=1
    packet.push_back(0x00); packet.push_back(0x01); // 1 Question
    packet.push_back(0x00); packet.push_back(0x00); // 0 Answers
    packet.push_back(0x00); packet.push_back(0x00); // 0 Authority
    packet.push_back(0x00); packet.push_back(0x00); // 0 Additional

    // QNAME encoding
    size_t start = 0;
    while (start < domain.size()) {
        size_t dot = domain.find('.', start);
        if (dot == std::string::npos) dot = domain.size();
        size_t len = dot - start;
        if (len > 0 && len <= 63) {
            packet.push_back(static_cast<uint8_t>(len));
            for (size_t i = start; i < start + len; ++i) {
                packet.push_back(static_cast<uint8_t>(domain[i]));
            }
        }
        start = dot + 1;
    }
    packet.push_back(0x00); // End of labels

    packet.push_back(0x00); packet.push_back(0x01); // Type: A
    packet.push_back(0x00); packet.push_back(0x01); // Class: IN

    auto t0 = std::chrono::steady_clock::now();
    auto deadline = t0 + std::chrono::milliseconds(clamped_timeout);
    int sent = send(sock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0);

    int measured_lat = -1;
    if (sent > 0) {
        while (true) {
            char recv_buf[512] = {};
            int received = recv(sock, recv_buf, sizeof(recv_buf), 0);
            if (received == SOCKET_ERROR || received <= 0) {
                break;
            }
            if (received >= 12 && validate_dns_response(reinterpret_cast<const uint8_t*>(recv_buf), static_cast<size_t>(received), txid)) {
                auto t1 = std::chrono::steady_clock::now();
                auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
                int lat_ms = static_cast<int>((elapsed_us + 999) / 1000);
                measured_lat = std::max(1, lat_ms);
                break;
            }

            auto now = std::chrono::steady_clock::now();
            if (now >= deadline) {
                break;
            }
            auto rem_ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
            DWORD rem_to = static_cast<DWORD>(std::max<long long>(1, rem_ms));
            setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&rem_to), sizeof(rem_to));
        }
    }

    closesocket(sock);
    return measured_lat;
}

int DnsBenchmark::ping_dns_server(const std::string& ip_str, int timeout_ms, bool single_domain) {
    const int clamped_timeout = std::max(1, timeout_ms);
    if (single_domain) {
        return ping_dns_server_domain(ip_str, "google.com", clamped_timeout);
    }

    std::vector<int> measured_latencies;
    measured_latencies.reserve(BENCHMARK_DOMAINS.size());
    for (const auto& domain : BENCHMARK_DOMAINS) {
        int lat = ping_dns_server_domain(ip_str, domain, clamped_timeout);
        if (lat >= 0) {
            measured_latencies.push_back(lat);
        }
    }

    if (measured_latencies.empty()) {
        return -1;
    }

    return *std::min_element(measured_latencies.begin(), measured_latencies.end());
}

std::vector<DnsBenchmarkResult> DnsBenchmark::run_benchmark(
    const std::vector<DnsProfile>& profiles,
    std::function<void(size_t completed, size_t total, const DnsBenchmarkResult& result)> on_progress)
{
    ensure_winsock();

    if (profiles.empty()) {
        return {};
    }

    const size_t total_count = profiles.size();
    std::vector<DnsBenchmarkResult> results(total_count);

    std::atomic<size_t> next_index{0};
    std::atomic<size_t> completed_count{0};
    std::mutex progress_mutex;

    constexpr size_t MAX_WORKERS = 8;
    const size_t worker_count = std::min(total_count, MAX_WORKERS);

    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    struct WorkerJoinGuard {
        std::vector<std::thread>& pool;
        ~WorkerJoinGuard() {
            for (auto& t : pool) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
    } join_guard{workers};

    for (size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&profiles, &results, &next_index, &completed_count, &progress_mutex, total_count, &on_progress]() {
            try {
                while (true) {
                    const size_t idx = next_index.fetch_add(1, std::memory_order_relaxed);
                    if (idx >= total_count) {
                        break;
                    }

                    const DnsProfile& p = profiles[idx];
                    DnsBenchmarkResult r;
                    r.profile_id = p.id;
                    r.profile_name = utf8_to_wide(p.name);
                    const std::string& display_ip = !p.primary_ipv4.empty() ? p.primary_ipv4 :
                        (!p.secondary_ipv4.empty() ? p.secondary_ipv4 :
                        (!p.primary_ipv6.empty() ? p.primary_ipv6 : p.secondary_ipv6));
                    r.primary_ip = utf8_to_wide(display_ip);
                    r.doh_template = utf8_to_wide(p.doh_template);
                    r.is_dhcp = p.is_dhcp;

                    if (p.is_dhcp) {
                        r.latency_ms = 0;
                        r.quality = DnsQuality::Fast;
                        r.status_text = L"DHCP провайдера";
                        r.quality_label = L"Автоматический";
                    } else {
                        int lat = -1;
                        if (!p.primary_ipv4.empty()) {
                            lat = ping_dns_server(p.primary_ipv4);
                        }
                        if (lat < 0 && !p.secondary_ipv4.empty()) {
                            lat = ping_dns_server(p.secondary_ipv4);
                        }
                        if (lat < 0 && !p.primary_ipv6.empty()) {
                            lat = ping_dns_server(p.primary_ipv6);
                        }
                        if (lat < 0 && !p.secondary_ipv6.empty()) {
                            lat = ping_dns_server(p.secondary_ipv6);
                        }

                        r.latency_ms = lat;
                        if (lat < 0) {
                            r.quality = DnsQuality::SlowOrDown;
                            r.status_text = L"Тайм-аут";
                            r.quality_label = L"Недоступен";
                        } else if (lat < 150) {
                            r.quality = DnsQuality::Fast;
                            r.status_text = std::to_wstring(lat) + L" мс";
                            r.quality_label = L"Отличный";
                        } else if (lat <= 350) {
                            r.quality = DnsQuality::Moderate;
                            r.status_text = std::to_wstring(lat) + L" мс";
                            r.quality_label = L"Средний";
                        } else {
                            r.quality = DnsQuality::SlowOrDown;
                            r.status_text = std::to_wstring(lat) + L" мс";
                            r.quality_label = L"Медленный";
                        }
                    }

                    if (on_progress) {
                        std::lock_guard<std::mutex> lock(progress_mutex);
                        const size_t c = ++completed_count;
                        try {
                            on_progress(c, total_count, r);
                        } catch (...) {
                        }
                    } else {
                        completed_count.fetch_add(1, std::memory_order_relaxed);
                    }

                    results[idx] = std::move(r);
                }
            } catch (...) {
            }
        });
    }

    for (auto& t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }

    // Sort results:
    // 0. Fast (< 150ms) sorted by latency ascending
    // 1. Moderate (150-350ms) sorted by latency ascending
    // 2. DHCP
    // 3. Slow (> 350ms) sorted by latency ascending
    // 4. Down / Timeout (-1)
    std::sort(results.begin(), results.end(), [](const DnsBenchmarkResult& a, const DnsBenchmarkResult& b) {
        auto rank_key = [](const DnsBenchmarkResult& r) -> std::tuple<int, int> {
            if (r.is_dhcp) return std::tuple<int, int>{2, 0};
            if (r.latency_ms >= 0 && r.latency_ms < 150) return std::tuple<int, int>{0, r.latency_ms};
            if (r.latency_ms >= 150 && r.latency_ms <= 350) return std::tuple<int, int>{1, r.latency_ms};
            if (r.latency_ms > 350) return std::tuple<int, int>{3, r.latency_ms};
            return std::tuple<int, int>{4, 0};
        };
        return rank_key(a) < rank_key(b);
    });

    return results;
}

} // namespace quickdns
