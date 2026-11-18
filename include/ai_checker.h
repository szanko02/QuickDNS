#pragma once

#include <atomic>
#include <concepts>
#include <cstdint>
#include <functional>
#include <string>
#include <utility>
#include <vector>
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

using AiDnsProfileResult = ProfileAiCheckResult;

struct ClientIpInfo {
    std::string ip;
    std::string country_code; // e.g. "RU", "BY", "NL"
    bool valid = false;
};

class AiChecker {
public:
    // Fetches current public IP and country code from https://1.1.1.1/cdn-cgi/trace (fallback https://cloudflare.com/cdn-cgi/trace)
    static ClientIpInfo get_client_ip_info(
        int timeout_ms = 2500,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    // Resolves IPv4 A-record for `domain` by sending a raw UDP DNS query to `dns_server_ip:53`
    static std::string resolve_domain_via_dns(
        const std::string& dns_server_ip,
        const std::string& domain,
        int timeout_ms = 1200,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    // Resolves all valid public IPv4 A-records returned in a single DNS response from `dns_server_ip:53`
    static std::vector<std::string> resolve_domain_all_via_dns(
        const std::string& dns_server_ip,
        const std::string& domain,
        int timeout_ms = 1200,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    // Checks a single AI service via a specific DNS server IP
    static AiServiceCheckResult check_single_service(
        const std::string& dns_server_ip,
        AiService service,
        const std::string& client_country_code = "",
        int timeout_ms = 3500,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    // Checks all 4 AI services (ChatGPT, Claude, Gemini, Copilot) concurrently for a single DnsProfile
    static AiDnsProfileResult check_profile(
        const DnsProfile& profile,
        const std::string& client_loc = "",
        int timeout_ms = 3500,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    // Checks a list of profiles and invokes `on_profile_done(index, result)` as each profile completes
    static std::vector<ProfileAiCheckResult> check_profiles(
        const std::vector<DnsProfile>& profiles,
        const std::string& client_country_code = "",
        const std::atomic<bool>* cancel_flag = nullptr,
        std::function<void(size_t index, const ProfileAiCheckResult& result)> on_profile_done = nullptr,
        int timeout_ms = 3500
    );

    // Overload accepting callback before cancel_flag for caller convenience
    template <typename Callback>
        requires std::invocable<Callback, size_t, const ProfileAiCheckResult&>
    static std::vector<ProfileAiCheckResult> check_profiles(
        const std::vector<DnsProfile>& profiles,
        const std::string& client_country_code,
        Callback&& on_profile_done,
        const std::atomic<bool>* cancel_flag = nullptr,
        int timeout_ms = 3500
    ) {
        return check_profiles(
            profiles,
            client_country_code,
            cancel_flag,
            std::function<void(size_t, const ProfileAiCheckResult&)>(std::forward<Callback>(on_profile_done)),
            timeout_ms
        );
    }

    // Packet & aggregation helpers (also exposed for deterministic offline unit tests and SmartDnsVerifier)
    static std::vector<uint8_t> build_dns_a_query(uint16_t txid, const std::string& domain);
    static std::string parse_dns_a_response(const uint8_t* packet, size_t packet_len, uint16_t expected_txid);
    static std::vector<std::string> parse_dns_all_a_responses(const uint8_t* packet, size_t packet_len, uint16_t expected_txid);
    static void aggregate_profile_result(ProfileAiCheckResult& result);

    // Strict digit-only dotted-quad IPv4 parsing and public unicast validation
    static bool parse_ipv4_octets(const std::string& ip_str, uint8_t& a, uint8_t& b, uint8_t& c, uint8_t& d);
    static bool is_valid_public_ipv4_octets(uint8_t a, uint8_t b, uint8_t c, uint8_t d);
    static bool is_valid_public_ipv4(const std::string& ip_str);
};

} // namespace quickdns
