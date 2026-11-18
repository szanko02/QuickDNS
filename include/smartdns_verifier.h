#pragma once

#include <atomic>
#include <chrono>
#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>
#include "config_manager.h"

namespace quickdns {

struct DnsIpCandidate {
    std::string ip;
    int latency_ms = -1;  // -1 if unreachable/timed out
    bool is_alive = false;
    std::string source;   // "preset", "resolved_host", "scraped_web"

    operator const std::string&() const noexcept { return ip; }

    bool operator==(const DnsIpCandidate& other) const noexcept {
        return ip == other.ip &&
               latency_ms == other.latency_ms &&
               is_alive == other.is_alive &&
               source == other.source;
    }
    bool operator==(std::string_view rhs) const noexcept { return ip == rhs; }
    bool operator==(const std::string& rhs) const noexcept { return ip == rhs; }
    bool operator==(const char* rhs) const noexcept { return rhs != nullptr && ip == rhs; }

    friend std::string operator+(const char* lhs, const DnsIpCandidate& rhs) {
        return std::string(lhs ? lhs : "") + rhs.ip;
    }
    friend std::string operator+(const std::string& lhs, const DnsIpCandidate& rhs) {
        return lhs + rhs.ip;
    }
    friend std::string operator+(const DnsIpCandidate& lhs, const char* rhs) {
        return lhs.ip + (rhs ? rhs : "");
    }
    friend std::string operator+(const DnsIpCandidate& lhs, const std::string& rhs) {
        return lhs.ip + rhs;
    }
};

struct DohVerificationResult {
    std::string doh_url;
    bool is_working = false;
    int latency_ms = -1;
    int http_status = 0;
    std::wstring status_text; // e.g. L"DoH OK (42 ms)", L"DoH Ошибка (HTTP 502)", L"DoH Недоступен (тайм-аут/сбой)"
};

struct SmartDnsVerificationResult {
    std::string profile_id;
    std::string profile_name;
    std::vector<DnsIpCandidate> candidate_ips;
    std::string original_primary_ip;
    std::string original_secondary_ip;
    std::string best_primary_ip;
    std::string best_secondary_ip;
    int best_primary_latency = -1;
    int best_secondary_latency = -1;
    DohVerificationResult doh_result;
    bool ip_updated = false; // true if best IPs differ from originals or replaced dead ones
};

class SmartDnsVerifier {
public:
    /**
     * @brief Discovers candidate IPv4 addresses for a profile:
     * - Configured preset primary/secondary IPv4 and ip_pool ("preset")
     * - Timed DNS resolution of hostnames from doh_template, website_url, and known mirrors ("resolved_host")
     * - Safe HTTPS web scraping of public IPv4 addresses from provider pages ("scraped_web")
     */
    static std::vector<DnsIpCandidate> discover_candidate_ips(
        const DnsProfile& profile,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    /**
     * @brief Measures UDP DNS query latency against an IPv4 address.
     * @param ip Target IPv4 string.
     * @param target_domain Domain to query (default "google.com").
     * @param timeout_ms Timeout in ms (default 900 ms).
     * @param cancel_flag Optional cancellation flag.
     */
    static DnsIpCandidate verify_ip(
        const std::string& ip,
        const std::string& target_domain = "google.com",
        int timeout_ms = 900,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    /**
     * @brief Verifies a DoH (DNS-over-HTTPS) endpoint via RFC 8484 (POST and GET fallback).
     * @param doh_url Target DoH endpoint URL (must be https://).
     * @param timeout_ms Timeout in ms (default 3000 ms).
     * @param cancel_flag Optional cancellation flag.
     */
    static DohVerificationResult verify_doh(
        const std::string& doh_url,
        int timeout_ms = 3000,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    /**
     * @brief Performs full discovery, verification and latency sorting for a single profile.
     */
    static SmartDnsVerificationResult verify_profile(
        const DnsProfile& profile,
        int timeout_ms = 2500,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    /**
     * @brief Performs full verification across multiple DNS profiles using a bounded 4-thread pool.
     * @param profiles List of DNS profiles.
     * @param on_progress Optional callback (completed, total, latest_result).
     * @param cancel_flag Optional cancellation flag.
     */
    static std::vector<SmartDnsVerificationResult> verify_profiles(
        const std::vector<DnsProfile>& profiles,
        std::function<void(size_t completed, size_t total, const SmartDnsVerificationResult& latest)> on_progress = nullptr,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    /**
     * @brief Applies verified best IPs to ConfigManager profiles (re-reading each profile by ID).
     * @param config ConfigManager instance.
     * @param results Verification results.
     * @return Number of profiles updated.
     */
    static int apply_best_ips(ConfigManager& config, const std::vector<SmartDnsVerificationResult>& results);

    /**
     * @brief Scrapes HTTPS webpage text and extracts public IPv4 addresses (capped at 16).
     * @param url Webpage URL to scrape (must be https:// with public host).
     * @param timeout_ms Timeout in ms.
     * @param cancel_flag Optional cancellation flag.
     */
    static std::vector<std::string> scrape_ips_from_webpage(
        const std::string& url,
        int timeout_ms = 3000,
        const std::atomic<bool>* cancel_flag = nullptr
    );

    /**
     * @brief Validates if string is a strict dotted-quad valid public unicast IPv4 address.
     */
    static bool is_valid_public_ipv4(const std::string& ip_str);

    /**
     * @brief Encodes binary data to RFC 4648 URL-safe Base64 without padding.
     */
    static std::string base64url_encode(const std::vector<uint8_t>& data);

private:
    static std::string extract_host_from_url(const std::string& url);
    static std::vector<std::string> resolve_hostname_ips(
        const std::string& host,
        int timeout_ms = 1200,
        const std::atomic<bool>* cancel_flag = nullptr
    );
};

} // namespace quickdns
