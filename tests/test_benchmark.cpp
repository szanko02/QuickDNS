#include "dns_benchmark.h"
#include "config_manager.h"
#include <iostream>
#include <cassert>
#include <vector>
#include <string>
#include <io.h>
#include <fcntl.h>

using namespace quickdns;

void test_sanitize_domain() {
    std::wcout << L"4. Testing sanitize_domain rules..." << std::endl;

    assert(DnsBenchmark::sanitize_domain("") == "google.com");
    assert(DnsBenchmark::sanitize_domain("   ") == "google.com");
    assert(DnsBenchmark::sanitize_domain("  google.com  ") == "google.com");
    assert(DnsBenchmark::sanitize_domain("https://example.com/path?arg=1#frag") == "example.com");
    assert(DnsBenchmark::sanitize_domain("http://example.com:8080/path") == "example.com");
    assert(DnsBenchmark::sanitize_domain("GOOGLE.COM") == "google.com");
    assert(DnsBenchmark::sanitize_domain("example.com.") == "example.com");

    // Test label longer than 63 characters (RFC 1035 limit)
    std::string long_label(70, 'a');
    std::string test_domain = long_label + ".com";
    std::string sanitized = DnsBenchmark::sanitize_domain(test_domain);
    assert(sanitized.find('.') == 63); // label must be truncated to 63 characters
    assert(sanitized == std::string(63, 'a') + ".com");

    // Test total domain longer than 253 characters
    std::string ultra_domain;
    for (int i = 0; i < 10; ++i) {
        if (!ultra_domain.empty()) ultra_domain += ".";
        ultra_domain += std::string(30, 'b');
    }
    // ultra_domain is 309 characters
    std::string sanitized_ultra = DnsBenchmark::sanitize_domain(ultra_domain);
    assert(sanitized_ultra.size() <= 253);

    std::wcout << L"   sanitize_domain tests passed!" << std::endl;
}

void test_validate_dns_response() {
    std::wcout << L"5. Testing validate_dns_response (QR bit, RCODE)..." << std::endl;

    // Buffer smaller than 12 bytes
    uint8_t short_buf[10] = {};
    assert(!DnsBenchmark::validate_dns_response(short_buf, sizeof(short_buf), 0x1234));
    assert(!DnsBenchmark::validate_dns_response(nullptr, 0, 0x1234));

    // Valid response packet (ID=0x1234, QR=1 (0x80), RCODE=0 (NOERROR))
    uint8_t valid_resp[12] = {
        0x12, 0x34, // ID
        0x81, 0x80, // Flags: QR=1, RD=1, RA=1, RCODE=0
        0x00, 0x01, // QDCOUNT = 1
        0x00, 0x01, // ANCOUNT = 1
        0x00, 0x00, // NSCOUNT = 0
        0x00, 0x00  // ARCOUNT = 0
    };
    assert(DnsBenchmark::validate_dns_response(valid_resp, sizeof(valid_resp), 0x1234));

    // Mismatched ID
    assert(!DnsBenchmark::validate_dns_response(valid_resp, sizeof(valid_resp), 0x5678));

    // Query packet (QR=0) instead of response
    uint8_t query_packet[12] = {
        0x12, 0x34,
        0x01, 0x00, // QR=0
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(!DnsBenchmark::validate_dns_response(query_packet, sizeof(query_packet), 0x1234));

    // Response with NXDOMAIN (RCODE=3) -> Valid answer according to P1-6 requirement
    uint8_t nxdomain_resp[12] = {
        0x12, 0x34,
        0x81, 0x83, // Flags: QR=1, RCODE=3 (NXDOMAIN)
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(DnsBenchmark::validate_dns_response(nxdomain_resp, sizeof(nxdomain_resp), 0x1234));

    // Response with SERVFAIL (RCODE=2) -> Invalid / error
    uint8_t servfail_resp[12] = {
        0x12, 0x34,
        0x81, 0x82, // Flags: QR=1, RCODE=2 (SERVFAIL)
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(!DnsBenchmark::validate_dns_response(servfail_resp, sizeof(servfail_resp), 0x1234));

    // Response with REFUSED (RCODE=5) -> Invalid / error
    uint8_t refused_resp[12] = {
        0x12, 0x34,
        0x81, 0x85, // Flags: QR=1, RCODE=5 (REFUSED)
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    assert(!DnsBenchmark::validate_dns_response(refused_resp, sizeof(refused_resp), 0x1234));

    std::wcout << L"   validate_dns_response tests passed!" << std::endl;
}

int main() {
    _setmode(_fileno(stdout), _O_U16TEXT);
    std::wcout << L"=== QuickDNS: Test DNS Benchmark Engine ===" << std::endl;

    // Run unit tests first
    test_sanitize_domain();
    test_validate_dns_response();

    // Test pinging public DNS servers
    std::wcout << L"1. Testing single ping to Cloudflare (1.1.1.1)..." << std::endl;
    int cf_ping = DnsBenchmark::ping_dns_server("1.1.1.1");
    std::wcout << L"Cloudflare latency: " << cf_ping << L" ms" << std::endl;
    if (cf_ping > 0) {
        std::wcout << L"Cloudflare server responded successfully." << std::endl;
    } else {
        std::wcout << L"Note: UDP port 53 outbound is filtered or unreachable in this test environment." << std::endl;
    }

    std::wcout << L"2. Testing single ping to Google (8.8.8.8)..." << std::endl;
    int g_ping = DnsBenchmark::ping_dns_server("8.8.8.8");
    std::wcout << L"Google latency: " << g_ping << L" ms" << std::endl;
    if (g_ping > 0) {
        std::wcout << L"Google server responded successfully." << std::endl;
    } else {
        std::wcout << L"Note: UDP port 53 outbound is filtered or unreachable in this test environment." << std::endl;
    }

    // Test full benchmark run
    std::wcout << L"3. Running full concurrent benchmark across all default presets..." << std::endl;
    auto presets = ConfigManager::get_default_presets();
    auto results = DnsBenchmark::run_benchmark(presets, [](size_t done, size_t total, const DnsBenchmarkResult& r) {
        std::wcout << L"  [Progress] (" << done << L"/" << total << L") " << r.profile_name 
                   << L" -> " << r.status_text << L" (" << r.quality_label << L")" << std::endl;
    });

    assert(!results.empty());
    std::wcout << L"Total benchmarked results: " << results.size() << std::endl;

    std::wcout << L"\n=== Benchmark Ranking ===" << std::endl;
    for (size_t i = 0; i < results.size(); ++i) {
        const auto& r = results[i];
        std::wcout << L"#" << (i + 1) << L": " << r.profile_name 
                   << L" | " << r.status_text 
                   << L" | " << r.quality_label 
                   << L" | IP: " << (r.primary_ip.empty() ? L"(DHCP)" : r.primary_ip)
                   << L" | is_dhcp: " << (r.is_dhcp ? L"true" : L"false") << std::endl;
    }

    std::wcout << L"\n=== All DNS Benchmark tests passed! ===" << std::endl;
    return 0;
}
