#include "dns_benchmark.h"
#include "config_manager.h"
#include <iostream>
#include <cassert>
#include <io.h>
#include <fcntl.h>

using namespace quickdns;

int main() {
    _setmode(_fileno(stdout), _O_U16TEXT);
    std::wcout << L"=== QuickDNS: Test DNS Benchmark Engine ===" << std::endl;

    // Test pinging public DNS servers
    std::wcout << L"1. Testing single ping to Cloudflare (1.1.1.1)..." << std::endl;
    int cf_ping = DnsBenchmark::ping_dns_server("1.1.1.1");
    std::wcout << L"Cloudflare latency: " << cf_ping << L" ms" << std::endl;
    assert(cf_ping > 0);

    std::wcout << L"2. Testing single ping to Google (8.8.8.8)..." << std::endl;
    int g_ping = DnsBenchmark::ping_dns_server("8.8.8.8");
    std::wcout << L"Google latency: " << g_ping << L" ms" << std::endl;
    assert(g_ping > 0);

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
                   << L" | IP: " << (r.primary_ip.empty() ? L"(DHCP)" : r.primary_ip) << std::endl;
    }

    std::wcout << L"\n=== All DNS Benchmark tests passed! ===" << std::endl;
    return 0;
}
