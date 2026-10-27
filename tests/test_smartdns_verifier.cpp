#include "smartdns_verifier.h"
#include "config_manager.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>

using namespace quickdns;

static int g_pass = 0;
static int g_fail = 0;

#define TEST_CHECK(cond, name) \
    do { \
        if (cond) { \
            std::cout << "[PASS] " << (name) << std::endl; \
            g_pass++; \
        } else { \
            std::cout << "[FAIL] " << (name) << " at line " << __LINE__ << std::endl; \
            g_fail++; \
        } \
    } while(0)

void test_public_ipv4_validation() {
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("8.8.8.8"), "Valid public IPv4 8.8.8.8");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("1.1.1.1"), "Valid public IPv4 1.1.1.1");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("83.220.169.155"), "Valid public IPv4 Comss IP");
    TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4("193.233.112.67"), "Valid public IPv4 GeoHide IP");

    // Invalid / Non-public IP cases
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("127.0.0.1"), "Reject loopback 127.0.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("10.0.0.1"), "Reject private 10.0.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("192.168.1.1"), "Reject private 192.168.1.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("172.16.5.1"), "Reject private 172.16.5.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("0.0.0.0"), "Reject 0.0.0.0");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("255.255.255.255"), "Reject broadcast 255.255.255.255");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("224.0.0.1"), "Reject multicast 224.0.0.1");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("999.999.999.999"), "Reject out-of-range 999.999.999.999");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4(""), "Reject empty string");
    TEST_CHECK(!SmartDnsVerifier::is_valid_public_ipv4("abc.def.ghi.jkl"), "Reject non-numeric string");
}

void test_base64url_encoding() {
    std::vector<uint8_t> empty;
    TEST_CHECK(SmartDnsVerifier::base64url_encode(empty) == "", "Base64url empty vector");

    std::vector<uint8_t> f = {'f'};
    TEST_CHECK(SmartDnsVerifier::base64url_encode(f) == "Zg", "Base64url 'f' -> 'Zg'");

    std::vector<uint8_t> fo = {'f', 'o'};
    TEST_CHECK(SmartDnsVerifier::base64url_encode(fo) == "Zm8", "Base64url 'fo' -> 'Zm8'");

    std::vector<uint8_t> foo = {'f', 'o', 'o'};
    TEST_CHECK(SmartDnsVerifier::base64url_encode(foo) == "Zm9v", "Base64url 'foo' -> 'Zm9v'");

    // Verify URL safe characters (no + or / or =)
    std::vector<uint8_t> binary = {0xFF, 0xFE, 0xFD, 0xFC, 0xFB, 0xFA};
    std::string enc = SmartDnsVerifier::base64url_encode(binary);
    TEST_CHECK(enc.find('+') == std::string::npos &&
               enc.find('/') == std::string::npos &&
               enc.find('=') == std::string::npos,
               "Base64url does not contain +, /, or = characters");
}

void test_candidate_discovery() {
    DnsProfile testProfile;
    testProfile.id = "comss";
    testProfile.name = "Comss.one DNS";
    testProfile.primary_ipv4 = "83.220.169.155";
    testProfile.secondary_ipv4 = "212.109.195.93";
    testProfile.doh_template = "https://dns.comss.one/dns-query";
    testProfile.website_url = "https://dns.comss.one/";

    auto candidates = SmartDnsVerifier::discover_candidate_ips(testProfile);
    TEST_CHECK(!candidates.empty(), "Discovered candidates list is not empty");

    bool hasPrimary = false;
    bool hasSecondary = false;
    for (const auto& ip : candidates) {
        if (ip == "83.220.169.155") hasPrimary = true;
        if (ip == "212.109.195.93") hasSecondary = true;
        TEST_CHECK(SmartDnsVerifier::is_valid_public_ipv4(ip), "Candidate IP is valid public IPv4: " + ip);
    }
    TEST_CHECK(hasPrimary, "Candidates include configured primary IP");
    TEST_CHECK(hasSecondary, "Candidates include configured secondary IP");
}

void test_live_udp_dns_verification() {
    auto resGoogle = SmartDnsVerifier::verify_ip("8.8.8.8", "google.com", 1500);
    TEST_CHECK(resGoogle.is_alive, "Live UDP DNS response from 8.8.8.8");
    TEST_CHECK(resGoogle.latency_ms > 0, "Latency from 8.8.8.8 is greater than 0 ms");

    auto resCloudflare = SmartDnsVerifier::verify_ip("1.1.1.1", "google.com", 1500);
    TEST_CHECK(resCloudflare.is_alive, "Live UDP DNS response from 1.1.1.1");
    TEST_CHECK(resCloudflare.latency_ms > 0, "Latency from 1.1.1.1 is greater than 0 ms");

    // Test unreachable/dummy address
    auto resDummy = SmartDnsVerifier::verify_ip("192.0.2.1", "google.com", 300);
    TEST_CHECK(!resDummy.is_alive, "Unreachable IP 192.0.2.1 correctly reports not alive");
    TEST_CHECK(resDummy.latency_ms == -1, "Unreachable IP latency is -1");
}

void test_live_doh_verification() {
    auto resCf = SmartDnsVerifier::verify_doh("https://cloudflare-dns.com/dns-query", 3500);
    TEST_CHECK(resCf.is_working, "Cloudflare DoH is working");
    TEST_CHECK(resCf.http_status == 200, "Cloudflare DoH returned HTTP 200");
    TEST_CHECK(resCf.latency_ms > 0, "Cloudflare DoH latency is greater than 0 ms");

    auto resComss = SmartDnsVerifier::verify_doh("https://dns.comss.one/dns-query", 3500);
    TEST_CHECK(resComss.is_working, "Comss.one DoH is working");
    TEST_CHECK(resComss.http_status == 200, "Comss.one DoH returned HTTP 200");

    auto resInvalid = SmartDnsVerifier::verify_doh("https://invalid-non-existent-domain-123456.com/dns-query", 1000);
    TEST_CHECK(!resInvalid.is_working, "Invalid DoH URL correctly fails");
}

void test_profile_verification_and_apply() {
    DnsProfile cfProfile;
    cfProfile.id = "cloudflare_test";
    cfProfile.name = "Cloudflare Test";
    cfProfile.primary_ipv4 = "1.1.1.1";
    cfProfile.secondary_ipv4 = "1.0.0.1";
    cfProfile.doh_template = "https://cloudflare-dns.com/dns-query";

    auto vRes = SmartDnsVerifier::verify_profile(cfProfile);
    TEST_CHECK(!vRes.best_primary_ip.empty(), "Profile verification found best primary IP");
    TEST_CHECK(vRes.best_primary_latency > 0, "Best primary IP has positive latency");
    TEST_CHECK(vRes.doh_result.is_working, "Profile DoH verification succeeded");
}

int main() {
    std::cout << "========================================" << std::endl;
    std::cout << "  QuickDNS SmartDNS & DoH Verifier Tests" << std::endl;
    std::cout << "========================================" << std::endl;

    test_public_ipv4_validation();
    test_base64url_encoding();
    test_candidate_discovery();
    test_live_udp_dns_verification();
    test_live_doh_verification();
    test_profile_verification_and_apply();

    std::cout << "========================================" << std::endl;
    std::cout << "Results: " << g_pass << " PASSED, " << g_fail << " FAILED" << std::endl;
    std::cout << "========================================" << std::endl;

    return (g_fail == 0) ? 0 : 1;
}
