#pragma once

#include <string>
#include <vector>
#include <functional>
#include <atomic>
#include <cstdint>
#include "config_manager.h"

namespace quickdns {

enum class AiService {
    ChatGPT = 0,
    Claude  = 1,
    Gemini  = 2,
    Copilot = 3
};

enum class AiAccessStatus {
    Pending = 0,
    Checking,
    Available,    // Service is reachable and not geo-blocked via this DNS
    GeoBlocked,   // Reached server, but geo-blocked (e.g. 403 / unsupported_country / loc=RU/BY without proxy / Gemini /faq redirect)
    DnsFailed,    // DNS server did not resolve the domain
    Unreachable   // TCP/TLS connection failed or timed out
};

struct AiServiceCheckResult {
    AiService service = AiService::ChatGPT;
    AiAccessStatus status = AiAccessStatus::Pending;
    std::string resolved_ip;
    int latency_ms = -1;
    std::wstring detail; // e.g. L"Доступен (45 мс)", L"Гео-блок (RU)", L"Нет ответа DNS", L"Тайм-аут TLS"
};

struct ProfileAiCheckResult {
    std::string profile_id;
    std::string profile_name;
    std::string dns_ip_used;
    AiServiceCheckResult chatgpt;
    AiServiceCheckResult claude;
    AiServiceCheckResult gemini;
    AiServiceCheckResult copilot;
    int available_count = 0; // 0..4
    int avg_latency_ms = -1;
};

struct ClientIpInfo {
    std::string ip;
    std::string country_code; // e.g. "RU", "BY", "NL"
    bool valid = false;
};

class AiChecker {
public:
    // Fetches current public IP and country code from https://1.1.1.1/cdn-cgi/trace (fallback https://cloudflare.com/cdn-cgi/trace)
    static ClientIpInfo get_client_ip_info(int timeout_ms = 2500);

    // Resolves IPv4 A-record for `domain` by sending a raw UDP DNS query to `dns_server_ip:53`
    static std::string resolve_domain_via_dns(const std::string& dns_server_ip, const std::string& domain, int timeout_ms = 1200);

    // Checks a single AI service via a specific DNS server IP
    static AiServiceCheckResult check_single_service(
        const std::string& dns_server_ip,
        AiService service,
        const std::string& client_country_code = "",
        int timeout_ms = 3000
    );

    // Checks all 4 AI services (ChatGPT, Claude, Gemini, Copilot) concurrently for a single DnsProfile
    static ProfileAiCheckResult check_profile(
        const DnsProfile& profile,
        const std::string& client_country_code = "",
        int timeout_ms = 3000
    );

    // Checks a list of profiles and invokes `on_profile_done(index, result)` as each profile completes
    static std::vector<ProfileAiCheckResult> check_profiles(
        const std::vector<DnsProfile>& profiles,
        const std::string& client_country_code,
        std::atomic<bool>* cancel_flag = nullptr,
        std::function<void(size_t index, const ProfileAiCheckResult& result)> on_profile_done = nullptr
    );

    // Packet & aggregation helpers (also exposed for deterministic offline unit tests)
    static std::vector<uint8_t> build_dns_a_query(uint16_t txid, const std::string& domain);
    static std::string parse_dns_a_response(const uint8_t* packet, size_t packet_len, uint16_t expected_txid);
    static void aggregate_profile_result(ProfileAiCheckResult& result);
};

} // namespace quickdns
