#include "dns_benchmark.h"
#include "encoding_utils.h"
#include "winsock_init.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <thread>
#include <future>
#include <mutex>
#include <random>

using QuickDns::Encoding::utf8_to_wide;

namespace {

using quickdns::ensure_winsock;

const std::vector<std::string> BENCHMARK_DOMAINS = {
    "chatgpt.com",
    "claude.ai",
    "google.com"
};

} // anonymous namespace

namespace quickdns {

std::string DnsBenchmark::sanitize_domain(const std::string& input_domain) {
    std::string domain = input_domain;
    while (!domain.empty() && isspace(static_cast<unsigned char>(domain.front()))) domain.erase(domain.begin());
    while (!domain.empty() && isspace(static_cast<unsigned char>(domain.back()))) domain.pop_back();

    if (domain.rfind("http://", 0) == 0) domain = domain.substr(7);
    if (domain.rfind("https://", 0) == 0) domain = domain.substr(8);

    size_t slash = domain.find('/');
    if (slash != std::string::npos) domain = domain.substr(0, slash);
    size_t qmark = domain.find('?');
    if (qmark != std::string::npos) domain = domain.substr(0, qmark);
    size_t hash = domain.find('#');
    if (hash != std::string::npos) domain = domain.substr(0, hash);
    size_t colon = domain.find(':');
    if (colon != std::string::npos) domain = domain.substr(0, colon);

    for (char& c : domain) c = static_cast<char>(tolower(static_cast<unsigned char>(c)));

    while (!domain.empty() && domain.back() == '.') {
        domain.pop_back();
    }

    std::string result;
    size_t start = 0;
    while (start < domain.size()) {
        size_t dot = domain.find('.', start);
        if (dot == std::string::npos) dot = domain.size();
        size_t len = dot - start;
        if (len > 0) {
            if (len > 63) {
                len = 63;
            }
            if (!result.empty()) {
                result.push_back('.');
            }
            result.append(domain, start, len);
        }
        start = dot + 1;
    }

    if (result.size() > 253) {
        result.resize(253);
        while (!result.empty() && result.back() == '.') {
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
    ensure_winsock();

    std::string domain = sanitize_domain(target_domain);

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
    int sent = send(sock, reinterpret_cast<const char*>(packet.data()), static_cast<int>(packet.size()), 0);

    int measured_lat = -1;
    if (sent > 0) {
        char recv_buf[512] = {};
        int received = recv(sock, recv_buf, sizeof(recv_buf), 0);
        if (received >= 12 && validate_dns_response(reinterpret_cast<const uint8_t*>(recv_buf), static_cast<size_t>(received), txid)) {
            auto t1 = std::chrono::steady_clock::now();
            auto elapsed_us = std::chrono::duration_cast<std::chrono::microseconds>(t1 - t0).count();
            int lat_ms = static_cast<int>((elapsed_us + 999) / 1000);
            measured_lat = std::max(1, lat_ms);
        }
    }

    closesocket(sock);
    return measured_lat;
}

int DnsBenchmark::ping_dns_server(const std::string& ip_str, int timeout_ms, bool single_domain) {
    if (single_domain) {
        return ping_dns_server_domain(ip_str, "google.com", timeout_ms);
    }

    std::vector<int> measured_latencies;
    for (const auto& domain : BENCHMARK_DOMAINS) {
        int lat = ping_dns_server_domain(ip_str, domain, timeout_ms);
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

    std::vector<DnsBenchmarkResult> results;
    results.reserve(profiles.size());

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
            r.is_dhcp = p.is_dhcp;

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
            if (r.is_dhcp) return 3000; // DHCP
            if (r.latency_ms >= 0 && r.latency_ms < 150) return 1000 + r.latency_ms;
            if (r.latency_ms >= 150 && r.latency_ms <= 350) return 2000 + r.latency_ms;
            if (r.latency_ms > 350) return 4000 + r.latency_ms;
            return 99999; // Down / Timeout
        };
        return score(a) < score(b);
    });

    return results;
}

} // namespace quickdns
