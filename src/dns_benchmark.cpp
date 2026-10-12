#include "dns_benchmark.h"
#include "encoding_utils.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <chrono>
#include <thread>
#include <future>
#include <mutex>
#include <random>

#pragma comment(lib, "ws2_32.lib")

using QuickDns::Encoding::utf8_to_wide;

namespace {

void ensure_winsock() {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    });
}

const std::vector<std::string> BENCHMARK_DOMAINS = {
    "chatgpt.com",
    "claude.ai",
    "google.com"
};

} // anonymous namespace

namespace quickdns {

int DnsBenchmark::ping_dns_server(const std::string& ip_str, int timeout_ms, bool single_domain) {
    if (ip_str.empty()) return -1;
    ensure_winsock();

    sockaddr_in sa4{};
    sockaddr_in6 sa6{};
    bool is_v4 = (inet_pton(AF_INET, ip_str.c_str(), &sa4.sin_addr) == 1);
    bool is_v6 = (!is_v4 && inet_pton(AF_INET6, ip_str.c_str(), &sa6.sin6_addr) == 1);

    if (!is_v4 && !is_v6) {
        return -1;
    }

    SOCKET sock = socket(is_v4 ? AF_INET : AF_INET6, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return -1;
    }

    DWORD to = static_cast<DWORD>(timeout_ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));

    if (is_v4) {
        sa4.sin_family = AF_INET;
        sa4.sin_port = htons(53);
    } else {
        sa6.sin6_family = AF_INET6;
        sa6.sin6_port = htons(53);
    }

    std::vector<int> measured_latencies;
    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint16_t> dist(1, 0xFFFE);

    for (const auto& domain : BENCHMARK_DOMAINS) {
        std::vector<uint8_t> packet;
        packet.reserve(64);

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
            if (len > 0) {
                packet.push_back(static_cast<uint8_t>(len));
                for (size_t i = start; i < dot; ++i) {
                    packet.push_back(static_cast<uint8_t>(domain[i]));
                }
            }
            start = dot + 1;
        }
        packet.push_back(0x00); // End of labels

        packet.push_back(0x00); packet.push_back(0x01); // Type: A
        packet.push_back(0x00); packet.push_back(0x01); // Class: IN

        auto t0 = std::chrono::steady_clock::now();
        int sent = 0;
        if (is_v4) {
            sent = sendto(sock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                          reinterpret_cast<const sockaddr*>(&sa4), sizeof(sa4));
        } else {
            sent = sendto(sock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0,
                          reinterpret_cast<const sockaddr*>(&sa6), sizeof(sa6));
        }

        if (sent > 0) {
            char recv_buf[512] = {};
            sockaddr_storage from{};
            int from_len = sizeof(from);
            int received = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                    reinterpret_cast<sockaddr*>(&from), &from_len);
            if (received >= 12) {
                uint16_t r_txid = (static_cast<uint8_t>(recv_buf[0]) << 8) | static_cast<uint8_t>(recv_buf[1]);
                if (r_txid == txid) {
                    auto t1 = std::chrono::steady_clock::now();
                    int ms = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
                    measured_latencies.push_back(ms);
                    if (single_domain) break;
                }
            }
        }
        if (single_domain && !measured_latencies.empty()) break;
    }

    closesocket(sock);

    if (measured_latencies.empty()) {
        return -1;
    }

    // Return the minimum (best round-trip response time) measured
    int min_lat = *std::min_element(measured_latencies.begin(), measured_latencies.end());
    return min_lat;
}

std::vector<DnsBenchmarkResult> DnsBenchmark::run_benchmark(
    const std::vector<DnsProfile>& profiles,
    std::function<void(size_t completed, size_t total, const DnsBenchmarkResult& result)> on_progress)
{
    ensure_winsock();

    std::vector<DnsBenchmarkResult> results;
    results.reserve(profiles.size());

    std::mutex mtx;
    std::atomic<size_t> completed_count{0};
    const size_t total_count = profiles.size();

    std::vector<std::future<DnsBenchmarkResult>> futures;
    futures.reserve(profiles.size());

    for (const auto& p : profiles) {
        futures.push_back(std::async(std::launch::async, [&p, &completed_count, total_count, on_progress]() {
            DnsBenchmarkResult r;
            r.profile_id = p.id;
            r.profile_name = utf8_to_wide(p.name);
            r.primary_ip = utf8_to_wide(p.primary_ipv4);
            r.doh_template = utf8_to_wide(p.doh_template);

            if (p.is_dhcp) {
                r.latency_ms = 0;
                r.quality = DnsQuality::Fast;
                r.status_text = L"DHCP провайдера";
                r.quality_label = L"Автоматический";
            } else {
                int lat = ping_dns_server(p.primary_ipv4);
                // If primary IPv4 failed or timed out and secondary exists, try secondary IPv4
                if (lat < 0 && !p.secondary_ipv4.empty()) {
                    lat = ping_dns_server(p.secondary_ipv4);
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

            size_t c = ++completed_count;
            if (on_progress) {
                on_progress(c, total_count, r);
            }
            return r;
        }));
    }

    for (auto& f : futures) {
        results.push_back(f.get());
    }

    // Sort results:
    // 1. Fast (< 150ms) sorted by latency ascending
    // 2. Moderate (150-350ms) sorted by latency ascending
    // 3. DHCP
    // 4. Slow (> 350ms)
    // 5. Down / Timeout (-1)
    std::sort(results.begin(), results.end(), [](const DnsBenchmarkResult& a, const DnsBenchmarkResult& b) {
        auto score = [](const DnsBenchmarkResult& r) -> int {
            if (r.latency_ms > 0 && r.latency_ms < 150) return 1000 + r.latency_ms;
            if (r.latency_ms >= 150 && r.latency_ms <= 350) return 2000 + r.latency_ms;
            if (r.latency_ms == 0) return 3000; // DHCP
            if (r.latency_ms > 350) return 4000 + r.latency_ms;
            return 99999; // Down / Timeout
        };
        return score(a) < score(b);
    });

    return results;
}

} // namespace quickdns
