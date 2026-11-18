#include "dns_manager.h"

#include <cstring>
#include <fcntl.h>
#include <iomanip>
#include <io.h>
#include <iostream>
#include <string>
#include <string_view>
#include <vector>
#include <windns.h>

#ifdef _MSC_VER
#pragma comment(lib, "dnsapi.lib")
#endif

namespace {

void print_separator() {
    std::wcout << L"----------------------------------------------------------------------\n";
}

void print_double_separator() {
    std::wcout << L"======================================================================\n";
}

bool is_null_guid(const GUID& g) {
    static const GUID zero_guid = {};
    return std::memcmp(&g, &zero_guid, sizeof(GUID)) == 0;
}

bool guids_equal(const GUID& a, const GUID& b) {
    return std::memcmp(&a, &b, sizeof(GUID)) == 0;
}

bool test_ip_and_guid_validation() {
    print_double_separator();
    std::wcout << L"[TEST SUITE 1] Wide/Narrow IP Validation & GUID Round-Trip\n";
    print_separator();

    struct WideTestCase {
        std::wstring ip;
        bool expected_ipv4;
        bool expected_ipv6;
    };

    std::vector<WideTestCase> cases = {
        // Valid IPv4
        { L"8.8.8.8", true, false },
        { L"1.1.1.1", true, false },
        { L"192.168.1.254", true, false },
        { L"10.0.0.1", true, false },
        { L"127.0.0.1", true, false },
        { L"255.255.255.255", true, false },
        { L"0.0.0.0", true, false },

        // Invalid IPv4
        { L"256.0.0.1", false, false },
        { L"1.2.3", false, false },
        { L"1.2.3.4.5", false, false },
        { L"8.8.8.8.com", false, false },
        { L"abc.def.ghi.jkl", false, false },
        { L"", false, false },

        // Valid IPv6
        { L"2001:4860:4860::8888", false, true },
        { L"2606:4700:4700::1111", false, true },
        { L"::1", false, true },
        { L"fe80::1ff:fe23:4567:890a", false, true },

        // Invalid IPv6
        { L"2001:::1", false, false },
        { L"2001:xyz::1", false, false },
        { L"12345::1", false, false }
    };

    bool all_passed = true;
    for (const auto& tc : cases) {
        bool v4 = quickdns::DnsManager::is_valid_ipv4(tc.ip);
        bool v6 = quickdns::DnsManager::is_valid_ipv6(tc.ip);
        bool any = quickdns::DnsManager::is_valid_ip(tc.ip);
        bool expected_any = tc.expected_ipv4 || tc.expected_ipv6;

        bool pass = (v4 == tc.expected_ipv4) && (v6 == tc.expected_ipv6) && (any == expected_any);
        if (!pass) {
            all_passed = false;
        }

        std::wcout << L"  IP: " << std::left << std::setw(28) << (tc.ip.empty() ? L"\"\" (empty)" : tc.ip)
                   << L" | IPv4: " << (v4 ? L"YES" : L"NO ")
                   << L" | IPv6: " << (v6 ? L"YES" : L"NO ")
                   << L" | Status: " << (pass ? L"PASS" : L"FAIL") << L"\n";
    }

    // Narrow std::string_view validation tests
    struct NarrowTestCase {
        std::string_view ip;
        bool expected_ipv4;
        bool expected_ipv6;
    };
    const std::vector<NarrowTestCase> narrow_cases = {
        { "1.1.1.1", true, false },
        { "83.220.169.155", true, false },
        { "999.1.1.1", false, false },
        { "", false, false },
        { "2001:4860:4860::8888", false, true },
        { "::1", false, true },
        { "2001:::1", false, false }
    };
    for (const auto& nc : narrow_cases) {
        bool v4 = quickdns::DnsManager::is_valid_ipv4(nc.ip);
        bool v6 = quickdns::DnsManager::is_valid_ipv6(nc.ip);
        if (v4 != nc.expected_ipv4 || v6 != nc.expected_ipv6) {
            std::wcout << L"  [FAIL] Narrow string_view IP check failed\n";
            all_passed = false;
        }
    }

    // GUID round-trip and malformed rejection
    const GUID sample_guid = { 0x4D36E972u, 0xE325u, 0x11CEu, { 0xBF, 0xC1, 0x08, 0x00, 0x2B, 0xE1, 0x03, 0x18 } };
    std::wstring guid_str = quickdns::DnsManager::guid_to_string(sample_guid);
    GUID parsed_guid = quickdns::DnsManager::string_to_guid(guid_str);
    bool guid_ok = (guid_str.size() == 38) &&
                   (guid_str.front() == L'{') &&
                   (guid_str.back() == L'}') &&
                   guids_equal(sample_guid, parsed_guid) &&
                   is_null_guid(quickdns::DnsManager::string_to_guid(L"")) &&
                   is_null_guid(quickdns::DnsManager::string_to_guid(L"not-a-valid-guid")) &&
                   is_null_guid(quickdns::DnsManager::string_to_guid(L"{1234-invalid}"));
    std::wcout << L"  GUID round-trip (" << guid_str << L") & invalid rejection: "
               << (guid_ok ? L"PASS" : L"FAIL") << L"\n";
    if (!guid_ok) {
        all_passed = false;
    }

    std::wcout << L"\nResult: " << (all_passed ? L"ALL IP & GUID VALIDATION TESTS PASSED" : L"SOME TESTS FAILED") << L"\n\n";
    return all_passed;
}

bool test_doh_validation() {
    print_double_separator();
    std::wcout << L"[TEST SUITE 1.5] DoH Template URL Validation & Boundary Tests\n";
    print_separator();

    struct DohTestCase {
        std::wstring t;
        bool expected;
    };

    // Exact length boundaries: min 12 ("https://a.bc"), max 2048
    std::wstring exact_min = L"https://a.bc"; // 12 chars
    std::wstring below_min = L"https://a.b";  // 11 chars
    std::wstring prefix_max = L"https://example.com/dns-query?";
    std::wstring exact_max = prefix_max + std::wstring(2048 - prefix_max.size(), L'a'); // 2048 chars
    std::wstring above_max = exact_max + L"a";                                          // 2049 chars

    std::vector<DohTestCase> cases = {
        // Valid
        { L"https://dns.google/dns-query", true },
        { L"HTTPS://DNS.GOOGLE/dns-query", true },
        { L"https://cloudflare-dns.com/dns-query{?dns}", true },
        { L"https://example.com:8443/q", true },
        { exact_min, true },
        { exact_max, true },

        // Invalid
        { L"", false },
        { below_min, false },
        { above_max, false },
        { L"http://a.b/q", false },
        { L"https://", false },
        { L"https://:8443/q", false },
        { L"https:///dns-query", false },
        { L"https://?dns=AAAA", false },
        { L"https://{?dns}", false },
        { L"https://a.b/q' ; calc", false },
        { L"https://a.b/q\"x", false },
        { L"https://a.b/$(x)", false },
        { L"https://a.b/q`x", false },
        { L"https://dns.google/dns query", false },
        { L"https://dns.google/q\r\nx", false },
        { L"https://dns.google/\x0442\x0435\x0441\x0442", false }
    };

    bool all_passed = true;
    for (const auto& tc : cases) {
        bool res = quickdns::DnsManager::is_valid_doh_template(tc.t);
        bool pass = (res == tc.expected);
        if (!pass) all_passed = false;
        std::wstring display = tc.t;
        for (wchar_t& ch : display) {
            if (ch < 32 || ch > 126) ch = L'?';
        }
        if (display.length() > 40) display = display.substr(0, 37) + L"...";
        std::wcout << L"  DoH: " << std::left << std::setw(42) << (display.empty() ? L"\"\" (empty)" : display)
                   << L" | Valid: " << (res ? L"YES" : L"NO ")
                   << L" | Status: " << (pass ? L"PASS" : L"FAIL") << L"\n";
    }

    std::wcout << L"\nResult: " << (all_passed ? L"ALL DOH VALIDATION TESTS PASSED" : L"SOME DOH TESTS FAILED") << L"\n\n";
    return all_passed;
}

bool test_mixed_address_validation() {
    print_double_separator();
    std::wcout << L"[TEST SUITE 1.7] Mixed IPv4/IPv6 & Orphan Secondary Validation\n";
    print_separator();

    struct TestCase {
        std::wstring v4_pri;
        std::wstring v4_sec;
        std::wstring v6_pri;
        std::wstring v6_sec;
        bool expected;
        std::wstring desc;
    };

    std::vector<TestCase> cases = {
        { L"8.8.8.8", L"8.8.4.4", L"", L"", true, L"Valid IPv4 primary and secondary" },
        { L"1.1.1.1", L"", L"", L"", true, L"Valid IPv4 primary only" },
        { L"", L"", L"2001:4860:4860::8888", L"2001:4860:4860::8844", true, L"Valid IPv6 primary and secondary" },
        { L"", L"", L"2606:4700:4700::1111", L"", true, L"Valid IPv6 primary only" },
        { L"8.8.8.8", L"8.8.4.4", L"2001:4860:4860::8888", L"2001:4860:4860::8844", true, L"Valid mixed full dual-stack" },
        { L"1.1.1.1", L"", L"2606:4700:4700::1111", L"", true, L"Valid mixed single dual-stack" },
        { L"", L"", L"", L"", false, L"Reject all empty addresses" },
        { L"", L"8.8.4.4", L"", L"", false, L"Reject orphan IPv4 secondary only" },
        { L"", L"8.8.4.4", L"2001:4860:4860::8888", L"", false, L"Reject orphan IPv4 secondary + IPv6" },
        { L"", L"", L"", L"2001:4860:4860::8844", false, L"Reject orphan IPv6 secondary only" },
        { L"8.8.8.8", L"", L"", L"2001:4860:4860::8844", false, L"Reject IPv4 + orphan IPv6 secondary" },
        { L"999.1.1.1", L"", L"", L"", false, L"Reject out-of-range IPv4 primary" },
        { L"1.1.1.1", L"not.an.ip", L"", L"", false, L"Reject invalid IPv4 secondary" },
        { L"", L"", L"2001:xyz::1", L"", false, L"Reject invalid IPv6 primary" },
        { L"", L"", L"2001:4860:4860::8888", L"2001:::1", false, L"Reject invalid IPv6 secondary" },
        { L"2001:4860:4860::8888", L"", L"", L"", false, L"Reject IPv6 in IPv4 primary slot" },
        { L"", L"", L"8.8.8.8", L"", false, L"Reject IPv4 in IPv6 primary slot" },
        { L"8.8.8.8", L"8.8.4.4", L"bad_ipv6", L"", false, L"Reject valid IPv4 with bad IPv6" },
        { L"bad_ipv4", L"", L"2001:4860:4860::8888", L"", false, L"Reject valid IPv6 with bad IPv4" }
    };

    bool all_passed = true;
    for (const auto& tc : cases) {
        std::wstring err;
        bool res = quickdns::DnsManager::validate_dns_addresses(tc.v4_pri, tc.v4_sec, tc.v6_pri, tc.v6_sec, &err);
        bool err_ok = tc.expected ? err.empty() : !err.empty();
        bool null_err_res = quickdns::DnsManager::validate_dns_addresses(tc.v4_pri, tc.v4_sec, tc.v6_pri, tc.v6_sec, nullptr);
        bool pass = (res == tc.expected) && err_ok && (null_err_res == tc.expected);
        if (!pass) all_passed = false;
        std::wcout << L"  " << std::left << std::setw(40) << tc.desc
                   << L" | Expected: " << (tc.expected ? L"PASS" : L"FAIL")
                   << L" | Got: " << (res ? L"PASS" : L"FAIL")
                   << L" | Status: " << (pass ? L"PASS" : L"FAIL") << L"\n";
    }

    std::wcout << L"\nResult: " << (all_passed ? L"ALL MIXED ADDRESS VALIDATION TESTS PASSED" : L"SOME TESTS FAILED") << L"\n\n";
    return all_passed;
}

void print_adapter(size_t index, const quickdns::NetworkAdapterInfo& adapter, bool is_primary = false) {
    print_separator();
    std::wcout << L"[" << index << L"] " << adapter.name
               << (is_primary ? L"  *** (PRIMARY ACTIVE ADAPTER) ***" : L"") << L"\n";
    std::wcout << L"    ID (GUID):    " << adapter.id << L"\n";
    std::wcout << L"    Index (IfIdx):" << adapter.if_index << L"\n";
    std::wcout << L"    Description:  " << adapter.description << L"\n";
    std::wcout << L"    Status:       " << (adapter.is_connected ? L"CONNECTED (Up)" : L"DISCONNECTED (Down)") << L"\n";
    std::wcout << L"    DNS Mode:     " << (adapter.is_dhcp_dns ? L"DHCP (Dynamic)" : L"Static (Manual)") << L"\n";

    std::wcout << L"    IPv4 DNS:     ";
    if (adapter.dns_ipv4.empty()) {
        std::wcout << L"[None]\n";
    } else {
        for (size_t i = 0; i < adapter.dns_ipv4.size(); ++i) {
            if (i > 0) std::wcout << L", ";
            std::wcout << adapter.dns_ipv4[i];
        }
        std::wcout << L"\n";
    }

    std::wcout << L"    IPv6 DNS:     ";
    if (adapter.dns_ipv6.empty()) {
        std::wcout << L"[None]\n";
    } else {
        for (size_t i = 0; i < adapter.dns_ipv6.size(); ++i) {
            if (i > 0) std::wcout << L", ";
            std::wcout << adapter.dns_ipv6[i];
        }
        std::wcout << L"\n";
    }
}

} // anonymous namespace

int main(int argc, char* argv[]) {
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    bool run_network = false;
    for (int i = 1; i < argc; ++i) {
        if (std::strcmp(argv[i], "--network") == 0) {
            run_network = true;
        } else if (std::strcmp(argv[i], "--offline") == 0) {
            run_network = false;
        }
    }

    print_double_separator();
    std::wcout << L"       QuickDNS Native Windows Network Engine Test Suite\n";
    std::wcout << L"                      C++20 Win32 / IPHlpApi\n";
    print_double_separator();
    std::wcout << L"\n";

    quickdns::DnsManager dns_mgr;

    std::wcout << L"Process Elevation (Administrator): "
               << (quickdns::DnsManager::is_elevated() ? L"YES (Elevated)" : L"NO (Standard User)") << L"\n\n";

    bool suite_ok = true;

    if (!test_ip_and_guid_validation()) {
        suite_ok = false;
    }

    if (!test_doh_validation()) {
        suite_ok = false;
    }

    if (!test_mixed_address_validation()) {
        suite_ok = false;
    }

    // Test 2: Network Adapter Enumeration & Invariant Verification
    print_double_separator();
    std::wcout << L"[TEST SUITE 2] Network Adapter Enumeration & Invariants\n";
    print_separator();

    auto adapters = dns_mgr.get_network_adapters();
    std::wcout << L"Discovered " << adapters.size() << L" eligible network adapter(s).\n";

    auto primary_opt = dns_mgr.get_primary_adapter();
    std::wstring primary_id = primary_opt ? primary_opt->id : L"";

    bool adapters_invariants_ok = true;
    for (size_t i = 0; i < adapters.size(); ++i) {
        const auto& adp = adapters[i];
        bool is_pri = (!primary_id.empty() && adp.id == primary_id);
        print_adapter(i + 1, adp, is_pri);

        if (adp.id.empty() || adp.name.empty() || adp.if_index == 0 ||
            is_null_guid(quickdns::DnsManager::string_to_guid(adp.id))) {
            std::wcout << L"  [FAIL] Adapter invariant failed for index " << (i + 1) << L"\n";
            adapters_invariants_ok = false;
        }
        for (const auto& ip4 : adp.dns_ipv4) {
            if (!quickdns::DnsManager::is_valid_ipv4(ip4)) {
                std::wcout << L"  [FAIL] Adapter reported invalid IPv4 DNS: " << ip4 << L"\n";
                adapters_invariants_ok = false;
            }
        }
        for (const auto& ip6 : adp.dns_ipv6) {
            if (!quickdns::DnsManager::is_valid_ipv6(ip6)) {
                std::wcout << L"  [FAIL] Adapter reported invalid IPv6 DNS: " << ip6 << L"\n";
                adapters_invariants_ok = false;
            }
        }
    }
    if (!adapters_invariants_ok) {
        suite_ok = false;
    }
    std::wcout << L"\nAdapter invariants check: " << (adapters_invariants_ok ? L"PASS" : L"FAIL") << L"\n\n";

    // Test 3: Primary Adapter Query & Consistency
    print_double_separator();
    std::wcout << L"[TEST SUITE 3] Active Internet Connection Adapter Query\n";
    print_separator();

    if (primary_opt) {
        std::wcout << L"Primary adapter detected: " << primary_opt->name
                   << L" (" << primary_opt->id << L", IfIdx=" << primary_opt->if_index << L")\n";
        bool found_in_list = false;
        for (const auto& adp : adapters) {
            if (adp.id == primary_opt->id) {
                found_in_list = true;
                break;
            }
        }
        bool primary_ok = !primary_opt->id.empty() &&
                          !primary_opt->name.empty() &&
                          primary_opt->if_index != 0 &&
                          !is_null_guid(quickdns::DnsManager::string_to_guid(primary_opt->id)) &&
                          (adapters.empty() || found_in_list);
        std::wcout << L"Primary adapter consistency: " << (primary_ok ? L"PASS" : L"FAIL") << L"\n";
        if (!primary_ok) {
            suite_ok = false;
        }
    } else {
        std::wcout << L"Info: No primary active adapter identified (offline or no active default route).\n";
    }
    std::wcout << L"\n";

    // Test 4: Flush DNS Resolver Cache (gated behind --network so offline CI never mutates host DNS cache)
    print_double_separator();
    std::wcout << L"[TEST SUITE 4] Windows DNS Resolver Cache Flush Verification\n";
    print_separator();

    if (!run_network) {
        std::wcout << L"[SKIP] Live DnsQuery_W & host DNS cache flush skipped (pass --network to run).\n\n";
    } else {
        PDNS_RECORD pDnsRecord = nullptr;
        DNS_STATUS dnsStatus = DnsQuery_W(L"example.com", DNS_TYPE_A, DNS_QUERY_STANDARD, nullptr, &pDnsRecord, nullptr);
        if (dnsStatus == ERROR_SUCCESS && pDnsRecord) {
            std::wcout << L"1. Queried example.com (cached in Windows DNS resolver): SUCCESS\n";
            DnsRecordListFree(pDnsRecord, DnsFreeRecordList);
        } else {
            std::wcout << L"1. Queried example.com: status code " << dnsStatus << L"\n";
        }

        bool flush_ok = dns_mgr.flush_dns_cache();
        std::wcout << L"2. Invoked flush_dns_cache(): " << (flush_ok ? L"SUCCESS" : L"FAILED") << L"\n";
        if (!flush_ok) {
            suite_ok = false;
        }

        PDNS_RECORD pCachedRecord = nullptr;
        DNS_STATUS cacheQueryStatus = DnsQuery_W(
            L"example.com",
            DNS_TYPE_A,
            DNS_QUERY_NO_WIRE_QUERY | DNS_QUERY_CACHE_ONLY,
            nullptr,
            &pCachedRecord,
            nullptr
        );
        if (cacheQueryStatus != ERROR_SUCCESS) {
            std::wcout << L"3. Verified DNS Cache-Only query: RECORD CLEARED - FLUSHDNS VERIFIED!\n";
        } else {
            std::wcout << L"3. [FAIL] Cache-only query still returned cached record after flush!\n";
            if (pCachedRecord) {
                DnsRecordListFree(pCachedRecord, DnsFreeRecordList);
            }
            suite_ok = false;
        }
        std::wcout << L"\n";
    }

    print_double_separator();
    std::wcout << L"Overall Suite Status: " << (suite_ok ? L"PASSED" : L"FAILED") << L"\n";
    print_double_separator();

    return suite_ok ? 0 : 1;
}
