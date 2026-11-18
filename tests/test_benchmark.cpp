#include "dns_benchmark.h"
#include "config_manager.h"
#include "encoding_utils.h"

#include <atomic>
#include <cstring>
#include <fcntl.h>
#include <io.h>
#include <iostream>
#include <mutex>
#include <string>
#include <vector>

using namespace quickdns;

namespace {

int g_pass = 0;
int g_fail = 0;

#define TEST_CHECK(cond, name) \
    do { \
        if (cond) { \
            std::wcout << L"[PASS] " << (name) << L"\n"; \
            ++g_pass; \
        } else { \
            std::wcout << L"[FAIL] " << (name) << L" (line " << __LINE__ << L")\n"; \
            ++g_fail; \
        } \
    } while (0)

void test_sanitize_domain() {
    std::wcout << L"--- Suite 1: sanitize_domain rules ---\n";

    TEST_CHECK(DnsBenchmark::sanitize_domain("") == "google.com", L"Empty domain defaults to google.com");
    TEST_CHECK(DnsBenchmark::sanitize_domain("   ") == "google.com", L"Whitespace domain defaults to google.com");
    TEST_CHECK(DnsBenchmark::sanitize_domain("  google.com  ") == "google.com", L"Trimmed google.com");
    TEST_CHECK(DnsBenchmark::sanitize_domain("https://example.com/path?arg=1#frag") == "example.com",
               L"Strip lowercase https://, path, query, and fragment");
    TEST_CHECK(DnsBenchmark::sanitize_domain("http://example.com:8080/path") == "example.com",
               L"Strip lowercase http://, port, and path");
    TEST_CHECK(DnsBenchmark::sanitize_domain("HTTPS://EXAMPLE.COM:443/path") == "example.com",
               L"Strip uppercase HTTPS://, port 443, and path");
    TEST_CHECK(DnsBenchmark::sanitize_domain("HTTP://Sub.Example.COM:8080/test?q=1") == "sub.example.com",
               L"Strip mixed-case HTTP://, subdomain, port, and query");
    TEST_CHECK(DnsBenchmark::sanitize_domain("https://user:pass@Example.COM:8443/login") == "example.com",
               L"Strip userinfo@, port, and path");
    TEST_CHECK(DnsBenchmark::sanitize_domain("GOOGLE.COM") == "google.com", L"Lowercase GOOGLE.COM");
    TEST_CHECK(DnsBenchmark::sanitize_domain("example.com.") == "example.com", L"Strip trailing dot");

    std::string long_label(70, 'a');
    std::string sanitized = DnsBenchmark::sanitize_domain(long_label + ".com");
    TEST_CHECK(sanitized.find('.') == 63 && sanitized == std::string(63, 'a') + ".com",
               L"Truncate >63 char label to RFC 1035 63-octet limit");

    std::string ultra_domain;
    for (int i = 0; i < 10; ++i) {
        if (!ultra_domain.empty()) ultra_domain += ".";
        ultra_domain += std::string(30, 'b');
    }
    std::string sanitized_ultra = DnsBenchmark::sanitize_domain(ultra_domain);
    TEST_CHECK(sanitized_ultra.size() <= 253, L"Cap total domain length to <= 253 chars");
}

void test_validate_dns_response() {
    std::wcout << L"--- Suite 2: validate_dns_response (QR bit, TXID, RCODE) ---\n";

    uint8_t short_buf[10] = {};
    TEST_CHECK(!DnsBenchmark::validate_dns_response(short_buf, sizeof(short_buf), 0x1234),
               L"Reject DNS packet shorter than 12 bytes");
    TEST_CHECK(!DnsBenchmark::validate_dns_response(nullptr, 0, 0x1234),
               L"Reject nullptr DNS packet");

    uint8_t valid_resp[12] = {
        0x12, 0x34,
        0x81, 0x80,
        0x00, 0x01,
        0x00, 0x01,
        0x00, 0x00,
        0x00, 0x00
    };
    TEST_CHECK(DnsBenchmark::validate_dns_response(valid_resp, sizeof(valid_resp), 0x1234),
               L"Accept valid QR=1 RCODE=0 NOERROR response");
    TEST_CHECK(!DnsBenchmark::validate_dns_response(valid_resp, sizeof(valid_resp), 0x5678),
               L"Reject mismatched TXID");

    uint8_t query_packet[12] = {
        0x12, 0x34,
        0x01, 0x00,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    TEST_CHECK(!DnsBenchmark::validate_dns_response(query_packet, sizeof(query_packet), 0x1234),
               L"Reject query packet with QR=0");

    uint8_t nxdomain_resp[12] = {
        0x12, 0x34,
        0x81, 0x83,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    TEST_CHECK(DnsBenchmark::validate_dns_response(nxdomain_resp, sizeof(nxdomain_resp), 0x1234),
               L"Accept authoritative NXDOMAIN (RCODE=3) response");

    uint8_t servfail_resp[12] = {
        0x12, 0x34,
        0x81, 0x82,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    TEST_CHECK(!DnsBenchmark::validate_dns_response(servfail_resp, sizeof(servfail_resp), 0x1234),
               L"Reject SERVFAIL (RCODE=2) response");

    uint8_t refused_resp[12] = {
        0x12, 0x34,
        0x81, 0x85,
        0x00, 0x01, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    TEST_CHECK(!DnsBenchmark::validate_dns_response(refused_resp, sizeof(refused_resp), 0x1234),
               L"Reject REFUSED (RCODE=5) response");
}

void test_offline_ping_and_synthetic_benchmark() {
    std::wcout << L"--- Suite 3: Deterministic offline ping & synthetic run_benchmark ---\n";

    int empty_ping = DnsBenchmark::ping_dns_server("", 100, true);
    int bad_ipv4_ping = DnsBenchmark::ping_dns_server("not.an.ip.address", 100, true);
    int out_of_range_ping = DnsBenchmark::ping_dns_server_domain("999.999.999.999", "google.com", 100);
    int bad_ipv6_ping = DnsBenchmark::ping_dns_server_domain("2001:::bad", "google.com", 100);

    TEST_CHECK(empty_ping == -1, L"ping_dns_server('') returns -1 immediately");
    TEST_CHECK(bad_ipv4_ping == -1, L"ping_dns_server('not.an.ip.address') returns -1 immediately");
    TEST_CHECK(out_of_range_ping == -1, L"ping_dns_server_domain('999.999.999.999') returns -1 immediately");
    TEST_CHECK(bad_ipv6_ping == -1, L"ping_dns_server_domain('2001:::bad') returns -1 immediately");

    DnsProfile pInvalid;
    pInvalid.id = "invalid_ip_prof";
    pInvalid.name = "Invalid IP Profile";
    pInvalid.primary_ipv4 = "invalid.ip.literal";
    pInvalid.is_dhcp = false;

    DnsProfile pDhcp;
    pDhcp.id = "dhcp";
    pDhcp.name = "Automatic (DHCP)";
    pDhcp.is_dhcp = true;

    DnsProfile pEmpty;
    pEmpty.id = "empty_ip_prof";
    pEmpty.name = "Empty IP Profile";
    pEmpty.is_dhcp = false;

    std::vector<DnsProfile> synthetic = {pInvalid, pDhcp, pEmpty};
    std::atomic<size_t> callback_count{0};
    std::atomic<bool> progress_bounds_ok{true};

    auto results = DnsBenchmark::run_benchmark(
        synthetic,
        [&](size_t done, size_t total, const DnsBenchmarkResult&) {
            ++callback_count;
            if (done == 0 || done > total || total != synthetic.size()) {
                progress_bounds_ok.store(false);
            }
        }
    );

    TEST_CHECK(results.size() == synthetic.size(), L"Synthetic run_benchmark returned result for every profile");
    TEST_CHECK(callback_count.load() == synthetic.size(), L"Progress callback invoked exactly once per profile");
    TEST_CHECK(progress_bounds_ok.load(), L"Progress callback (completed, total) bounds valid");

    if (results.size() == 3) {
        TEST_CHECK(results[0].is_dhcp && results[0].latency_ms == 0,
                   L"DHCP profile has latency_ms == 0 and sorts ahead of failed/invalid profiles");
        TEST_CHECK(!results[1].is_dhcp && results[1].latency_ms == -1 && results[1].quality == DnsQuality::SlowOrDown,
                   L"Invalid/empty profile #2 marked SlowOrDown with latency_ms == -1");
        TEST_CHECK(!results[2].is_dhcp && results[2].latency_ms == -1 && results[2].quality == DnsQuality::SlowOrDown,
                   L"Invalid/empty profile #3 marked SlowOrDown with latency_ms == -1");
    }
}

void test_live_network_benchmark() {
    std::wcout << L"--- Suite 4: Live network DNS ping & preset benchmark ---\n";

    int cf_ping = DnsBenchmark::ping_dns_server("1.1.1.1", 1500, true);
    int g_ping = DnsBenchmark::ping_dns_server("8.8.8.8", 1500, true);
    std::wcout << L"[INFO] Cloudflare (1.1.1.1) latency: " << cf_ping
               << L" ms, Google (8.8.8.8) latency: " << g_ping << L" ms\n";
    TEST_CHECK(cf_ping >= 0 || g_ping >= 0, L"At least one global public DNS (1.1.1.1 or 8.8.8.8) responded");

    auto presets = ConfigManager::get_default_presets();
    std::atomic<size_t> cb_count{0};
    std::mutex io_mtx;

    auto results = DnsBenchmark::run_benchmark(
        presets,
        [&](size_t done, size_t total, const DnsBenchmarkResult& r) {
            ++cb_count;
            std::lock_guard<std::mutex> lock(io_mtx);
            std::wcout << L"  [Progress] (" << done << L"/" << total << L") " << r.profile_name
                       << L" -> " << r.status_text << L" (" << r.quality_label << L")\n";
        }
    );

    TEST_CHECK(results.size() == presets.size(), L"Live benchmark result count matches preset count");
    TEST_CHECK(cb_count.load() == presets.size(), L"Live benchmark callback count matches preset count");

    // Verify timeout entries (-1) are placed strictly at the end after reachable and DHCP entries
    bool seen_timeout = false;
    bool order_ok = true;
    for (const auto& r : results) {
        if (r.latency_ms < 0 && !r.is_dhcp) {
            seen_timeout = true;
        } else if (seen_timeout) {
            order_ok = false;
            break;
        }
    }
    TEST_CHECK(order_ok, L"Reachable and DHCP profiles sort strictly before timed-out profiles");
}

} // namespace

int main(int argc, char* argv[]) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    bool offline_only = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--offline") == 0) {
            offline_only = true;
        } else if (std::strcmp(argv[i], "--network") == 0) {
            offline_only = false;
        }
    }

    std::wcout << L"=== QuickDNS: Test DNS Benchmark Engine ("
               << (offline_only ? L"OFFLINE" : L"NETWORK") << L") ===\n";

    test_sanitize_domain();
    test_validate_dns_response();
    test_offline_ping_and_synthetic_benchmark();

    if (!offline_only) {
        test_live_network_benchmark();
    } else {
        std::wcout << L"[SKIP] Live network preset benchmark skipped in --offline mode.\n";
    }

    std::wcout << L"========================================\n";
    std::wcout << L"Results: " << g_pass << L" PASSED, " << g_fail << L" FAILED\n";
    std::wcout << L"========================================\n";

    return (g_fail == 0) ? 0 : 1;
}
