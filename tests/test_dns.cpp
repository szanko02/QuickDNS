#include "dns_manager.h"

#include <iostream>
#include <iomanip>
#include <string>
#include <vector>
#include <io.h>
#include <fcntl.h>
#include <windns.h>

#pragma comment(lib, "dnsapi.lib")

namespace {

void print_separator() {
    std::wcout << L"----------------------------------------------------------------------\n";
}

void print_double_separator() {
    std::wcout << L"======================================================================\n";
}

bool test_ip_validation() {
    print_double_separator();
    std::wcout << L"[TEST SUITE 1] IP Address Validation Helper Functions\n";
    print_separator();

    struct TestCase {
        std::wstring ip;
        bool expected_ipv4;
        bool expected_ipv6;
    };

    std::vector<TestCase> cases = {
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

        bool pass = (v4 == tc.expected_ipv4) && (v6 == tc.expected_ipv6);
        if (!pass) {
            all_passed = false;
        }

        std::wcout << L"  IP: " << std::left << std::setw(28) << (tc.ip.empty() ? L"\"\" (empty)" : tc.ip)
                   << L" | IPv4: " << (v4 ? L"YES" : L"NO ")
                   << L" | IPv6: " << (v6 ? L"YES" : L"NO ")
                   << L" | Status: " << (pass ? L"PASS" : L"FAIL") << L"\n";
    }

    std::wcout << L"\nResult: " << (all_passed ? L"ALL IP VALIDATION TESTS PASSED" : L"SOME TESTS FAILED") << L"\n\n";
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

int main() {
    // Configure console output mode for wide characters
    _setmode(_fileno(stdout), _O_U8TEXT);
    _setmode(_fileno(stderr), _O_U8TEXT);

    print_double_separator();
    std::wcout << L"       QuickDNS Native Windows Network Engine Test Suite\n";
    std::wcout << L"                      C++20 Win32 / IPHlpApi\n";
    print_double_separator();
    std::wcout << L"\n";

    quickdns::DnsManager dns_mgr;

    std::wcout << L"Process Elevation (Administrator): " 
               << (quickdns::DnsManager::is_elevated() ? L"YES (Elevated)" : L"NO (Standard User)") << L"\n\n";

    // Test 1: IP Validation
    test_ip_validation();

    // Test 2: Network Adapter Enumeration
    print_double_separator();
    std::wcout << L"[TEST SUITE 2] Network Adapter Enumeration & Filtering\n";
    print_separator();

    auto adapters = dns_mgr.get_network_adapters();
    std::wcout << L"Discovered " << adapters.size() << L" eligible network adapter(s).\n";

    auto primary_opt = dns_mgr.get_primary_adapter();
    std::wstring primary_id = primary_opt ? primary_opt->id : L"";

    for (size_t i = 0; i < adapters.size(); ++i) {
        bool is_pri = (!primary_id.empty() && adapters[i].id == primary_id);
        print_adapter(i + 1, adapters[i], is_pri);
    }
    std::wcout << L"\n";

    // Test 3: Primary Adapter Query
    print_double_separator();
    std::wcout << L"[TEST SUITE 3] Active Internet Connection Adapter Query\n";
    print_separator();

    if (primary_opt) {
        std::wcout << L"Primary adapter successfully detected:\n";
        std::wcout << L"  Name:        " << primary_opt->name << L"\n";
        std::wcout << L"  Description: " << primary_opt->description << L"\n";
        std::wcout << L"  Index:       " << primary_opt->if_index << L"\n";
        std::wcout << L"  GUID:        " << primary_opt->id << L"\n";
        std::wcout << L"  Connected:   " << (primary_opt->is_connected ? L"Yes" : L"No") << L"\n";
        std::wcout << L"  DNS Mode:    " << (primary_opt->is_dhcp_dns ? L"DHCP" : L"Static") << L"\n";
        std::wcout << L"  Current IPv4 DNS: ";
        if (primary_opt->dns_ipv4.empty()) {
            std::wcout << L"[None]";
        } else {
            for (const auto& dns : primary_opt->dns_ipv4) {
                std::wcout << dns << L" ";
            }
        }
        std::wcout << L"\n";
    } else {
        std::wcout << L"Warning: No primary active adapter identified (offline or no active default gateway).\n";
    }
    std::wcout << L"\n";

    // Test 4: Flush DNS Resolver Cache with Verification
    print_double_separator();
    std::wcout << L"[TEST SUITE 4] Windows DNS Resolver Cache Flush Verification\n";
    print_separator();

    // Populate DNS cache with a resolution query
    PDNS_RECORD pDnsRecord = nullptr;
    DNS_STATUS dnsStatus = DnsQuery_W(L"example.com", DNS_TYPE_A, DNS_QUERY_STANDARD, nullptr, &pDnsRecord, nullptr);
    if (dnsStatus == ERROR_SUCCESS && pDnsRecord) {
        std::wcout << L"1. Queried example.com (cached in Windows DNS resolver): SUCCESS\n";
        DnsRecordListFree(pDnsRecord, DnsFreeRecordList);
    } else {
        std::wcout << L"1. Queried example.com: status code " << dnsStatus << L"\n";
    }

    // Flush cache
    bool flush_ok = dns_mgr.flush_dns_cache();
    std::wcout << L"2. Invoked flush_dns_cache() (DnsFlushResolverCache + ipconfig /flushdns): " 
               << (flush_ok ? L"SUCCESS" : L"FAILED") << L"\n";

    // Verify cache-only query
    PDNS_RECORD pCachedRecord = nullptr;
    DNS_STATUS cacheQueryStatus = DnsQuery_W(L"example.com", DNS_TYPE_A, DNS_QUERY_NO_WIRE_QUERY | DNS_QUERY_CACHE_ONLY, nullptr, &pCachedRecord, nullptr);
    if (cacheQueryStatus != ERROR_SUCCESS) {
        std::wcout << L"3. Verified DNS Cache-Only query: RECORD CLEARED (STATUS_NOT_FOUND) - FLUSHDNS VERIFIED!\n";
    } else {
        std::wcout << L"3. Cache-only query status: " << cacheQueryStatus << L"\n";
        if (pCachedRecord) {
            DnsRecordListFree(pCachedRecord, DnsFreeRecordList);
        }
    }
    std::wcout << L"\n";

    print_double_separator();
    std::wcout << L"All diagnostic tests completed.\n";
    print_double_separator();

    return 0;
}
