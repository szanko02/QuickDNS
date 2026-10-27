#pragma once

#include <string>
#include <vector>
#include <functional>
#include <optional>
#include <chrono>
#include "config_manager.h"

namespace quickdns {

struct DnsIpCandidate {
    std::string ip;
    int latency_ms = -1;  // -1 if unreachable/timed out
    bool is_alive = false;
    std::string source;   // "preset", "resolved_host", "scraped_web"
};

struct DohVerificationResult {
    std::string doh_url;
    bool is_working = false;
    int latency_ms = -1;
    int http_status = 0;
    std::wstring status_text; // e.g. L"DoH OK (42 ms)", L"Ошибка HTTP 505", L"Недоступен"
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
     * - Configured preset primary/secondary IPv4
     * - DNS resolution of hostnames from doh_template and website_url
     * - Known provider subdomains and mirrors
     * - Web scraping of public IPv4 addresses from provider pages
     */
    static std::vector<std::string> discover_candidate_ips(const DnsProfile& profile);

    /**
     * @brief Measures UDP DNS query latency against an IPv4 address.
     * @param ip Target IPv4 string.
     * @param target_domain Domain to query (default "google.com").
     * @param timeout_ms Timeout in ms (default 900 ms).
     */
    static DnsIpCandidate verify_ip(const std::string& ip, const std::string& target_domain = "google.com", int timeout_ms = 900);

    /**
     * @brief Verifies a DoH (DNS-over-HTTPS) endpoint via RFC 8484 (POST and GET fallback).
     * @param doh_url Target DoH endpoint URL.
     * @param timeout_ms Timeout in ms (default 3000 ms).
     */
    static DohVerificationResult verify_doh(const std::string& doh_url, int timeout_ms = 3000);

    /**
     * @brief Performs full discovery, verification and latency sorting for a single profile.
     */
    static SmartDnsVerificationResult verify_profile(const DnsProfile& profile);

    /**
     * @brief Performs full verification across multiple DNS profiles concurrently.
     * @param profiles List of DNS profiles.
     * @param on_progress Optional callback (completed, total, latest_result).
     */
    static std::vector<SmartDnsVerificationResult> verify_profiles(
        const std::vector<DnsProfile>& profiles,
        std::function<void(size_t completed, size_t total, const SmartDnsVerificationResult& result)> on_progress = nullptr
    );

    /**
     * @brief Applies verified best IPs to ConfigManager profiles.
     * @param config ConfigManager instance.
     * @param results Verification results.
     * @return Number of profiles updated.
     */
    static int apply_best_ips(ConfigManager& config, const std::vector<SmartDnsVerificationResult>& results);

    /**
     * @brief Scrapes webpage text and extracts public IPv4 addresses using regex.
     * @param url Webpage URL to scrape.
     * @param timeout_ms Timeout in ms.
     */
    static std::vector<std::string> scrape_ips_from_webpage(const std::string& url, int timeout_ms = 3000);

    /**
     * @brief Validates if string is a valid public unicast IPv4 address.
     */
    static bool is_valid_public_ipv4(const std::string& ip_str);

    /**
     * @brief Encodes binary data to RFC 4648 URL-safe Base64 without padding.
     */
    static std::string base64url_encode(const std::vector<uint8_t>& data);

private:
    static std::string extract_host_from_url(const std::string& url);
    static std::vector<std::string> resolve_hostname_ips(const std::string& host);
};

} // namespace quickdns
