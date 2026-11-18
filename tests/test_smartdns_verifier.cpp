#include "smartdns_verifier.h"
#include "config_manager.h"

#include <windows.h>
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <iostream>
#include <string>
#include <vector>

using namespace quickdns;

namespace {

int g_pass = 0;
int g_fail = 0;

#define TEST_CHECK(cond, name) \
    do { \
        if (cond) { \
            std::cout << "[PASS] " << (name) << std::endl; \
            ++g_pass; \
        } else { \
            std::cout << "[FAIL] " << (name) << " at line " << __LINE__ << std::endl; \
            ++g_fail; \
        } \
    } while (0)

void test_public_ipv4_validation() {
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("8.8.8.8"), "Valid public IPv4 8.8.8.8");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("1.1.1.1"), "Valid public IPv4 1.1.1.1");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("83.220.169.155"), "Valid public IPv4 Comss IP");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("193.233.112.67"), "Valid public IPv4 GeoHide IP");

    // Link-local 169.254.0.0/16 & RFC1918 172.16.0.0/12 boundaries
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("169.253.255.255"), "Allow public 169.253.255.255");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("169.254.0.1"), "Reject link-local 169.254.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("169.254.1.1"), "Reject link-local 169.254.1.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("169.254.255.254"), "Reject link-local 169.254.255.254");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("169.255.0.1"), "Allow public 169.255.0.1");

    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("172.15.255.254"), "Allow public 172.15.255.254");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("172.16.0.1"), "Reject RFC1918 172.16.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("172.16.5.1"), "Reject RFC1918 172.16.5.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("172.31.255.254"), "Reject RFC1918 172.31.255.254");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("172.32.0.1"), "Allow public 172.32.0.1");

    // Loopback, private, CGNAT, documentation, benchmark, multicast, reserved
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("127.0.0.1"), "Reject loopback 127.0.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("10.0.0.1"), "Reject private 10.0.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("192.168.1.1"), "Reject private 192.168.1.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("0.0.0.0"), "Reject 0.0.0.0/8");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("0.1.2.3"), "Reject 0.1.2.3 in 0.0.0.0/8");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("100.64.0.1"), "Reject CGNAT 100.64.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("100.127.255.254"), "Reject CGNAT 100.127.255.254");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("100.63.255.255"), "Allow public 100.63.255.255");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("100.128.0.1"), "Allow public 100.128.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("192.0.0.1"), "Reject 192.0.0.1 (192.0.0.0/24)");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("192.0.2.1"), "Reject TEST-NET-1 192.0.2.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("198.18.0.1"), "Reject benchmark 198.18.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("198.19.255.254"), "Reject benchmark 198.19.255.254");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("198.20.0.1"), "Allow public 198.20.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("198.51.100.1"), "Reject TEST-NET-2 198.51.100.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("203.0.113.1"), "Reject TEST-NET-3 203.0.113.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("224.0.0.1"), "Reject multicast 224.0.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("239.255.255.254"), "Reject multicast 239.255.255.254");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("240.0.0.1"), "Reject reserved 240.0.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("255.255.255.255"), "Reject broadcast 255.255.255.255");

    // Strict syntax: whitespace, signs, leading zeros, malformed octets
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4(" 8.8.8.8"), "Reject leading whitespace ' 8.8.8.8'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("8.8.8.8 "), "Reject trailing whitespace '8.8.8.8 '");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("+8.8.8.8"), "Reject plus sign '+8.8.8.8'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("8. +8. 8. +8"), "Reject internal whitespace/sign '8. +8. 8. +8'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("-1.1.1.1"), "Reject negative sign '-1.1.1.1'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("08.08.08.08"), "Reject leading zeros '08.08.08.08'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("8.8.8.8."), "Reject trailing dot '8.8.8.8.'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4(".8.8.8.8"), "Reject leading dot '.8.8.8.8'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("8..8.8.8"), "Reject empty octet '8..8.8.8'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("8.8.8"), "Reject 3-octet IPv4 '8.8.8'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("8.8.8.8.8"), "Reject 5-octet IPv4 '8.8.8.8.8'");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("999.999.999.999"), "Reject out-of-range 999.999.999.999");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4(""), "Reject empty string");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("abc.def.ghi.jkl"), "Reject non-numeric string");
}

void test_base64url_encoding() {
    std::vector<uint8_t> empty;
    TEST_CHECK(SmartDnsVerifier::base64url_encode(empty).empty(), "Base64url empty vector");

    std::vector<uint8_t> f = {'f'};
    TEST_CHECK(SmartDnsVerifier::base64url_encode(f) == "Zg", "Base64url 'f' -> 'Zg'");

    std::vector<uint8_t> fo = {'f', 'o'};
    TEST_CHECK(SmartDnsVerifier::base64url_encode(fo) == "Zm8", "Base64url 'fo' -> 'Zm8'");

    std::vector<uint8_t> foo = {'f', 'o', 'o'};
    TEST_CHECK(SmartDnsVerifier::base64url_encode(foo) == "Zm9v", "Base64url 'foo' -> 'Zm9v'");

    std::vector<uint8_t> binary = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA};
    std::string enc = SmartDnsVerifier::base64url_encode(binary);
    TEST_CHECK(enc.find('+') == std::string::npos &&
               enc.find('/') == std::string::npos &&
               enc.find('=') == std::string::npos &&
               (enc.find('-') != std::string::npos || enc.find('_') != std::string::npos),
               "Base64url uses URL-safe '-' / '_' without '+', '/', or '='");
}

void test_apply_best_ips_offline() {
    _putenv_s("TESTING", "1");
    std::error_code ec;
    const std::filesystem::path tempDir =
        std::filesystem::temp_directory_path(ec) /
        (L"quickdns_smartdns_test_" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::remove_all(tempDir, ec);
    std::filesystem::create_directories(tempDir, ec);

    const std::filesystem::path cfgPath = tempDir / L"profiles_apply_test.json";
    {
        ConfigManager cfg(cfgPath);

        DnsProfile customSmart;
        customSmart.id = "custom_smart_test";
        customSmart.name = "Custom SmartDNS Test";
        customSmart.primary_ipv4 = "83.220.169.155";
        customSmart.secondary_ipv4 = "212.109.195.93";
        customSmart.is_smart_dns = true;
        customSmart.ip_pool = {"83.220.169.155", "212.109.195.93"};

        bool added = cfg.add_profile(customSmart);
        bool saved = cfg.save_to_file();
        TEST_CHECK(added && saved, "Prepared temp ConfigManager with custom SmartDNS profile");

        SmartDnsVerificationResult vRes;
        vRes.profile_id = "custom_smart_test";
        vRes.profile_name = customSmart.name;
        vRes.original_primary_ip = "83.220.169.155";
        vRes.original_secondary_ip = "212.109.195.93";
        vRes.best_primary_ip = "195.133.25.16";
        vRes.best_secondary_ip = "83.220.169.155";
        vRes.best_primary_latency = 18;
        vRes.best_secondary_latency = 42;
        vRes.ip_updated = true;
        vRes.candidate_ips = {
            {"83.220.169.155", 42, true, "preset"},
            {"195.133.25.16", 18, true, "scraped_web"},
            {"195.133.25.16", 18, true, "resolved_host"}, // duplicate
            {"212.109.195.93", -1, false, "preset"},      // dead existing pool member
            {"10.0.0.1", 3, true, "scraped_web"}          // private bogon must be rejected
        };

        int updatedCount = SmartDnsVerifier::apply_best_ips(cfg, {vRes});
        TEST_CHECK(updatedCount == 1, "SmartDnsVerifier::apply_best_ips updated 1 profile");

        auto p = cfg.get_profile_by_id("custom_smart_test");
        TEST_CHECK(p.has_value(), "Updated profile present in ConfigManager");
        if (p) {
            TEST_CHECK(p->primary_ipv4 == "195.133.25.16", "apply_best_ips promoted fastest primary IPv4");
            TEST_CHECK(p->secondary_ipv4 == "83.220.169.155", "apply_best_ips promoted second-fastest IPv4");
            TEST_CHECK(p->ip_pool.size() == 3 &&
                       p->ip_pool[0] == "195.133.25.16" &&
                       p->ip_pool[1] == "83.220.169.155" &&
                       p->ip_pool[2] == "212.109.195.93",
                       "apply_best_ips sorted alive candidates, preserved pool, deduplicated, and rejected 10.0.0.1");
        }

        // Idempotent re-apply when already up to date
        int secondCount = SmartDnsVerifier::apply_best_ips(cfg, {vRes});
        TEST_CHECK(secondCount == 0, "Re-applying identical best IPs is idempotent (returns 0)");

        // DoH-only profile (UDP port 53 disabled, e.g. dns_ai) must preserve existing primary/secondary IPs
        SmartDnsVerificationResult dohOnlyRes;
        dohOnlyRes.profile_id = "dns_ai";
        dohOnlyRes.profile_name = "DNS-AI (DoH)";
        dohOnlyRes.original_primary_ip = "194.87.196.13";
        dohOnlyRes.original_secondary_ip = "45.151.102.219";
        dohOnlyRes.best_primary_ip = "194.87.196.13";
        dohOnlyRes.best_secondary_ip = "45.151.102.219";
        dohOnlyRes.best_primary_latency = -1;
        dohOnlyRes.best_secondary_latency = -1;
        dohOnlyRes.best_latency_ms = 38;
        dohOnlyRes.doh_result.doh_url = "https://dns.dns-ai.ru/dns-query";
        dohOnlyRes.doh_result.is_working = true;
        dohOnlyRes.doh_result.latency_ms = 38;
        dohOnlyRes.doh_result.http_status = 200;
        dohOnlyRes.is_doh_only = true;
        dohOnlyRes.ip_updated = false;
        dohOnlyRes.candidate_ips = {
            {"194.87.196.13", -1, false, "preset"},
            {"45.151.102.219", -1, false, "preset"}
        };

        int dohUpdated = SmartDnsVerifier::apply_best_ips(cfg, {dohOnlyRes});
        auto dnsAiAfter = cfg.get_profile_by_id("dns_ai");
        TEST_CHECK(dohUpdated == 0 && dnsAiAfter.has_value() &&
                   dnsAiAfter->primary_ipv4 == "194.87.196.13" &&
                   dnsAiAfter->secondary_ipv4 == "45.151.102.219" &&
                   dnsAiAfter->ip_pool.size() == 2,
                   "apply_best_ips preserves DoH-only dns_ai primary/secondary IPs and ip_pool");
    }

    // Verify persistence on reload from disk
    {
        ConfigManager cfgReloaded(cfgPath);
        auto p = cfgReloaded.get_profile_by_id("custom_smart_test");
        TEST_CHECK(p.has_value() && p->primary_ipv4 == "195.133.25.16" && p->secondary_ipv4 == "83.220.169.155",
                   "apply_best_ips persisted updated IPs to disk");
    }

    std::filesystem::remove_all(tempDir, ec);
}

void test_candidate_discovery() {
    DnsProfile testProfile;
    testProfile.id = "comss";
    testProfile.name = "Comss.one DNS";
    testProfile.primary_ipv4 = "83.220.169.155";
    testProfile.secondary_ipv4 = "212.109.195.93";
    testProfile.doh_template = "https://dns.comss.one/dns-query";
    testProfile.website_url = "https://dns.comss.one/";
    testProfile.is_smart_dns = true;

    auto candidates = SmartDnsVerifier::discover_candidate_ips(testProfile);
    TEST_CHECK(!candidates.empty(), "Discovered candidates list is not empty");

    bool hasPrimary = false;
    bool hasSecondary = false;
    bool allPublicValid = true;
    bool allSourcesPopulated = true;
    for (const auto& item : candidates) {
        const std::string& ip = item.ip;
        if (ip == "83.220.169.155") hasPrimary = true;
        if (ip == "212.109.195.93") hasSecondary = true;
        if (!SmartDnsVerifier::is_valid_public_ipv4(ip)) {
            allPublicValid = false;
        }
        if (item.source.empty()) {
            allSourcesPopulated = false;
        }
    }
    TEST_CHECK(allPublicValid, "Every discovered candidate IP is a valid public IPv4");
    TEST_CHECK(allSourcesPopulated, "Every discovered candidate has non-empty source metadata");
    TEST_CHECK(hasPrimary, "Candidates include configured primary IP");
    TEST_CHECK(hasSecondary, "Candidates include configured secondary IP");

    DnsProfile bezRuProfile;
    bezRuProfile.id = "bezmezhau_ru";
    bezRuProfile.name = "Bezmezhau DNS (RF)";
    bezRuProfile.primary_ipv4 = "185.236.10.189";
    bezRuProfile.secondary_ipv4 = "31.58.216.201";
    bezRuProfile.doh_template = "https://sync.bezmezhau.com/dns-query";
    bezRuProfile.is_smart_dns = true;
    bezRuProfile.ip_pool = {"185.236.10.189", "31.58.216.201", "143.20.64.55", "181.215.40.49"};

    auto bezRuCandidates = SmartDnsVerifier::discover_candidate_ips(bezRuProfile);
    bool hasRfInBezRu = false;
    bool hasSecInBezRu = false;
    for (const auto& item : bezRuCandidates) {
        if (item.ip == "185.236.10.189") hasRfInBezRu = true;
        if (item.ip == "31.58.216.201") hasSecInBezRu = true;
    }
    TEST_CHECK(hasRfInBezRu && hasSecInBezRu, "Bezmezhau RU candidates include RF primary and secondary IPs");

    DnsProfile dnsAiProfile;
    dnsAiProfile.id = "dns_ai";
    dnsAiProfile.name = "DNS-AI (DoH)";
    dnsAiProfile.primary_ipv4 = "194.87.196.13";
    dnsAiProfile.secondary_ipv4 = "45.151.102.219";
    dnsAiProfile.doh_template = "https://dns.dns-ai.ru/dns-query";
    dnsAiProfile.website_url = "https://dns-ai.ru/";
    dnsAiProfile.is_smart_dns = true;
    dnsAiProfile.ip_pool = {"194.87.196.13", "45.151.102.219"};

    auto dnsAiCandidates = SmartDnsVerifier::discover_candidate_ips(dnsAiProfile);
    bool hasDnsAiPri = false;
    bool hasDnsAiSec = false;
    bool hasDnsAiWebProxyIp = false;
    for (const auto& item : dnsAiCandidates) {
        if (item.ip == "194.87.196.13") hasDnsAiPri = true;
        if (item.ip == "45.151.102.219") hasDnsAiSec = true;
        if (item.ip == "152.228.98.30") hasDnsAiWebProxyIp = true;
    }
    TEST_CHECK(hasDnsAiPri && hasDnsAiSec && !hasDnsAiWebProxyIp,
               "DNS-AI discovery includes resolver nodes and excludes website SNI proxy IP 152.228.98.30");
}

void test_live_udp_dns_verification() {
    auto resGoogle = SmartDnsVerifier::verify_ip("8.8.8.8", "google.com", 1500);
    auto resCloudflare = SmartDnsVerifier::verify_ip("1.1.1.1", "google.com", 1500);
    TEST_CHECK(resGoogle.is_alive || resCloudflare.is_alive,
               "Live UDP DNS verification succeeded against 8.8.8.8 or 1.1.1.1");
    if (resGoogle.is_alive) {
        TEST_CHECK(resGoogle.latency_ms >= 0, "Latency from 8.8.8.8 is non-negative");
    }
    if (resCloudflare.is_alive) {
        TEST_CHECK(resCloudflare.latency_ms >= 0, "Latency from 1.1.1.1 is non-negative");
    }

    auto resDummy = SmartDnsVerifier::verify_ip("192.0.2.1", "google.com", 300);
    TEST_CHECK(!resDummy.is_alive && resDummy.latency_ms == -1,
               "Non-public TEST-NET-1 IP 192.0.2.1 reports not alive with latency -1");
}

void test_live_doh_verification() {
    auto resCf = SmartDnsVerifier::verify_doh("https://cloudflare-dns.com/dns-query", 3500);
    auto resGoogle = SmartDnsVerifier::verify_doh("https://dns.google/dns-query", 3500);
    TEST_CHECK(resCf.is_working || resGoogle.is_working,
               "Live RFC 8484 DoH verification succeeded against Cloudflare or Google");
    if (resCf.is_working) {
        TEST_CHECK(resCf.http_status == 200 && resCf.latency_ms >= 0, "Cloudflare DoH returned HTTP 200");
    }

    // Community server dns.comss.one may be geo-filtered or transiently unreachable; log without hard-failing
    auto resComss = SmartDnsVerifier::verify_doh("https://dns.comss.one/dns-query", 3500);
    std::cout << "[INFO] Community DoH https://dns.comss.one/dns-query working="
              << (resComss.is_working ? "true" : "false")
              << " status=" << resComss.http_status << std::endl;

    auto resCleartext = SmartDnsVerifier::verify_doh("http://cloudflare-dns.com/dns-query", 1000);
    TEST_CHECK(!resCleartext.is_working, "Cleartext http:// DoH URL is rejected");

    auto resInvalid = SmartDnsVerifier::verify_doh("https://invalid-non-existent-domain-123456.com/dns-query", 1000);
    TEST_CHECK(!resInvalid.is_working, "Unresolvable DoH URL fails cleanly");
}

void test_profile_verification() {
    DnsProfile cfProfile;
    cfProfile.id = "cloudflare_test";
    cfProfile.name = "Cloudflare Test";
    cfProfile.primary_ipv4 = "1.1.1.1";
    cfProfile.secondary_ipv4 = "1.0.0.1";
    cfProfile.doh_template = "https://cloudflare-dns.com/dns-query";

    auto vRes = SmartDnsVerifier::verify_profile(cfProfile);
    TEST_CHECK(!vRes.best_primary_ip.empty() && vRes.best_primary_latency >= 0,
               "verify_profile identified reachable primary IP for Cloudflare");

    DnsProfile dnsAiProfile;
    dnsAiProfile.id = "dns_ai";
    dnsAiProfile.name = "DNS-AI (DoH)";
    dnsAiProfile.primary_ipv4 = "194.87.196.13";
    dnsAiProfile.secondary_ipv4 = "45.151.102.219";
    dnsAiProfile.doh_template = "https://dns.dns-ai.ru/dns-query";
    dnsAiProfile.website_url = "https://dns-ai.ru/";
    dnsAiProfile.is_smart_dns = true;
    dnsAiProfile.ip_pool = {"194.87.196.13", "45.151.102.219"};

    auto aiRes = SmartDnsVerifier::verify_profile(dnsAiProfile);
    TEST_CHECK(aiRes.best_primary_ip == "194.87.196.13" &&
               aiRes.best_secondary_ip == "45.151.102.219" &&
               !aiRes.ip_updated,
               "verify_profile preserves primary/secondary IPs and ip_updated==false for DoH-only dns_ai");
    if (aiRes.doh_result.is_working) {
        TEST_CHECK(aiRes.is_doh_only && aiRes.best_latency_ms == aiRes.doh_result.latency_ms && aiRes.best_latency_ms > 0,
                   "verify_profile records DoH latency for working DoH-only dns_ai");
    }
}

} // namespace

int main(int argc, char* argv[]) {
    bool offline_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--offline") == 0) {
            offline_only = true;
        } else if (std::strcmp(argv[i], "--network") == 0) {
            offline_only = false;
        }
    }

    std::cout << "========================================" << std::endl;
    std::cout << "  QuickDNS SmartDNS & DoH Verifier Tests ("
              << (offline_only ? "OFFLINE" : "NETWORK") << ")" << std::endl;
    std::cout << "========================================" << std::endl;

    test_public_ipv4_validation();
    test_base64url_encoding();
    test_apply_best_ips_offline();

    if (!offline_only) {
        test_candidate_discovery();
        test_live_udp_dns_verification();
        test_live_doh_verification();
        test_profile_verification();
    } else {
        std::cout << "[SKIP] Live network discovery, UDP, and DoH probes skipped in --offline mode." << std::endl;
    }

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << g_pass << " PASSED, " << g_fail << " FAILED" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_fail == 0) ? 0 : 1;
}
