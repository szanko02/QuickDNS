#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif

#include "winsock_init.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <bcrypt.h>

#include "smartdns_verifier.h"
#include "ai_checker.h"
#include "encoding_utils.h"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <mutex>
#include <set>
#include <thread>

using QuickDns::Encoding::utf8_to_wide;
using QuickDns::Encoding::wide_to_utf8;

namespace {

using quickdns::ensure_winsock;

constexpr size_t kMaxScrapedIps       = 16;
constexpr size_t kMaxScrapedHtmlBytes = 512 * 1024;
constexpr size_t kMaxDohResponseBytes = 4096;
constexpr int    kCancelPollSliceMs   = 50;

char ascii_lower(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

bool is_ascii_alnum(char c) noexcept {
    return (c >= '0' && c <= '9') ||
           (c >= 'a' && c <= 'z') ||
           (c >= 'A' && c <= 'Z');
}

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        ch = ascii_lower(ch);
    }
    return s;
}

std::string trim_ascii(const std::string& s) {
    size_t first = 0;
    while (first < s.size() && (s[first] == ' ' || s[first] == '\t' || s[first] == '\r' || s[first] == '\n')) {
        ++first;
    }
    size_t last = s.size();
    while (last > first && (s[last - 1] == ' ' || s[last - 1] == '\t' || s[last - 1] == '\r' || s[last - 1] == '\n')) {
        --last;
    }
    return s.substr(first, last - first);
}

bool is_cancelled(const std::atomic<bool>* cancel_flag) noexcept {
    return cancel_flag != nullptr && cancel_flag->load(std::memory_order_relaxed);
}

int remaining_ms(std::chrono::steady_clock::time_point deadline) noexcept {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (ms > 600000) return 600000;
    return static_cast<int>(ms);
}

uint16_t generate_dns_txid() noexcept {
    uint16_t txid = 0;
    if (BCryptGenRandom(
            nullptr,
            reinterpret_cast<PUCHAR>(&txid),
            sizeof(txid),
            BCRYPT_USE_SYSTEM_PREFERRED_RNG) != 0 ||
        txid == 0) {
        uint64_t ticks = static_cast<uint64_t>(
            std::chrono::steady_clock::now().time_since_epoch().count());
        txid = static_cast<uint16_t>((ticks ^ (ticks >> 16)) | 1u);
    }
    return txid;
}

bool is_standard_public_resolver(const std::string& ip) {
    static const char* const kPublicResolvers[] = {
        "1.1.1.1",
        "1.0.0.1",
        "8.8.8.8",
        "8.8.4.4",
        "9.9.9.9",
        "149.112.112.112",
        "77.88.8.8",
        "77.88.8.1",
        "94.140.14.14",
        "94.140.15.15",
        "208.67.222.222",
        "208.67.220.220"
    };
    for (const char* r : kPublicResolvers) {
        if (ip == r) {
            return true;
        }
    }
    return false;
}

bool ends_with_ascii(const std::string& str, const char* suffix) {
    size_t slen = std::strlen(suffix);
    if (str.size() < slen) return false;
    return str.compare(str.size() - slen, slen, suffix) == 0;
}

bool is_safe_public_hostname_syntax(const std::string& host_raw) {
    if (host_raw.empty() || host_raw.size() > 253) {
        return false;
    }
    if (host_raw.front() == '[' || host_raw.back() == ']' || host_raw.find(':') != std::string::npos) {
        return false; // Reject IPv6 literals for IPv4 scraping/resolution
    }

    std::string host = to_lower_ascii(host_raw);
    if (host.back() == '.') {
        host.pop_back();
    }
    if (host.empty() || host == "localhost" || host.find('.') == std::string::npos) {
        return false; // Reject single-label/internal hostnames and integer IPs
    }
    if (ends_with_ascii(host, ".localhost") ||
        ends_with_ascii(host, ".local") ||
        ends_with_ascii(host, ".internal") ||
        ends_with_ascii(host, ".lan") ||
        ends_with_ascii(host, ".home") ||
        ends_with_ascii(host, ".arpa")) {
        return false;
    }

    bool all_digits_or_dots = true;
    for (char c : host) {
        if (!((c >= '0' && c <= '9') || c == '.')) {
            all_digits_or_dots = false;
            break;
        }
    }
    if (all_digits_or_dots) {
        return quickdns::AiChecker::is_valid_public_ipv4(host);
    }

    // Ensure valid DNS hostname syntax
    return !quickdns::AiChecker::build_dns_a_query(1, host).empty();
}

// Non-blocking UDP receive helper:
//  > 0 : bytes received
//   -1 : timeout
//   -2 : ICMP port unreachable / socket error
//   -3 : cancelled
int udp_recv_with_deadline(
    SOCKET sock,
    uint8_t* buf,
    int max_len,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel_flag)
{
    while (true) {
        if (is_cancelled(cancel_flag)) {
            return -3;
        }

        int got = recv(sock, reinterpret_cast<char*>(buf), max_len, 0);
        if (got > 0) {
            return got;
        }
        if (got == 0) {
            return -2;
        }

        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK) {
            return -2;
        }

        int rem = remaining_ms(deadline);
        if (rem <= 0) {
            return -1;
        }
        int slice_ms = cancel_flag ? std::min(rem, kCancelPollSliceMs) : rem;

        fd_set read_fds;
        FD_ZERO(&read_fds);
        FD_SET(sock, &read_fds);
        fd_set err_fds;
        FD_ZERO(&err_fds);
        FD_SET(sock, &err_fds);

        timeval tv{};
        tv.tv_sec = slice_ms / 1000;
        tv.tv_usec = (slice_ms % 1000) * 1000;

        int rc = select(0, &read_fds, nullptr, &err_fds, &tv);
        if (rc == SOCKET_ERROR) {
            return -2;
        }
        if (rc == 0) {
            continue;
        }
        if (FD_ISSET(sock, &err_fds) && !FD_ISSET(sock, &read_fds)) {
            return -2;
        }
    }
}

bool read_doh_response_body(
    HINTERNET hRequest,
    std::vector<uint8_t>& out_body,
    const std::atomic<bool>* cancel_flag)
{
    out_body.clear();
    out_body.reserve(512);

    uint8_t chunk[1024];
    DWORD bytes_read = 0;
    while (true) {
        if (is_cancelled(cancel_flag)) {
            return false;
        }
        bytes_read = 0;
        if (!WinHttpReadData(hRequest, chunk, sizeof(chunk), &bytes_read)) {
            return false;
        }
        if (bytes_read == 0) {
            break;
        }
        if (out_body.size() + bytes_read > kMaxDohResponseBytes) {
            return false; // Reject oversized non-DNS body
        }
        out_body.insert(out_body.end(), chunk, chunk + bytes_read);
    }
    return !out_body.empty();
}

struct ThreadPoolGuard {
    std::vector<std::thread>& threads;
    explicit ThreadPoolGuard(std::vector<std::thread>& t) noexcept : threads(t) {}
    ~ThreadPoolGuard() {
        for (auto& th : threads) {
            if (th.joinable()) {
                th.join();
            }
        }
    }
};

quickdns::SmartDnsVerificationResult verify_profile_impl(
    const DnsProfile& profile,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag,
    bool allow_subpool)
{
    using namespace quickdns;

    SmartDnsVerificationResult res;
    res.profile_id = profile.id;
    res.profile_name = profile.name;
    res.original_primary_ip = profile.primary_ipv4;
    res.original_secondary_ip = profile.secondary_ipv4;
    res.best_primary_ip = profile.primary_ipv4;
    res.best_secondary_ip = profile.secondary_ipv4;

    if (is_cancelled(cancel_flag)) {
        return res;
    }

    int safe_timeout = std::max(200, timeout_ms);
    int ip_timeout = std::min(1200, std::max(300, (safe_timeout * 2) / 5));

    // 1. Discover candidate IPs (deduplicated, with source populated)
    auto discovered = SmartDnsVerifier::discover_candidate_ips(profile, cancel_flag);
    if (is_cancelled(cancel_flag)) {
        return res;
    }

    res.candidate_ips.resize(discovered.size());
    for (size_t i = 0; i < discovered.size(); ++i) {
        res.candidate_ips[i] = discovered[i];
    }

    if (allow_subpool && discovered.size() > 1) {
        // Standalone verify_profile call: use a bounded 4-thread pool
        std::atomic<size_t> next_task{0};
        const size_t total_tasks = discovered.size() + 1; // +1 for verify_doh
        const size_t worker_count = std::min<size_t>(4, total_tasks);

        std::vector<std::thread> workers;
        workers.reserve(worker_count);
        ThreadPoolGuard guard(workers);

        auto worker_fn = [&]() {
            while (!is_cancelled(cancel_flag)) {
                size_t task_idx = next_task.fetch_add(1, std::memory_order_relaxed);
                if (task_idx >= total_tasks) {
                    break;
                }
                try {
                    if (task_idx == 0) {
                        res.doh_result = SmartDnsVerifier::verify_doh(
                            profile.doh_template, safe_timeout, cancel_flag);
                    } else {
                        size_t cand_idx = task_idx - 1;
                        auto probed = SmartDnsVerifier::verify_ip(
                            discovered[cand_idx].ip, "google.com", ip_timeout, cancel_flag);
                        probed.source = discovered[cand_idx].source;
                        res.candidate_ips[cand_idx] = std::move(probed);
                    }
                } catch (...) {
                }
            }
        };

        try {
            for (size_t w = 0; w < worker_count; ++w) {
                workers.emplace_back(worker_fn);
            }
        } catch (...) {
            if (workers.empty()) {
                worker_fn();
            }
        }
    } else {
        // Inside verify_profiles's 4-thread worker pool: run directly on this worker without nested threads
        for (size_t i = 0; i < discovered.size(); ++i) {
            if (is_cancelled(cancel_flag)) {
                return res;
            }
            auto probed = SmartDnsVerifier::verify_ip(
                discovered[i].ip, "google.com", ip_timeout, cancel_flag);
            probed.source = discovered[i].source;
            res.candidate_ips[i] = std::move(probed);
        }
        if (!is_cancelled(cancel_flag)) {
            res.doh_result = SmartDnsVerifier::verify_doh(
                profile.doh_template, safe_timeout, cancel_flag);
        }
    }

    if (is_cancelled(cancel_flag)) {
        return res;
    }

    // Filter and rank alive IPs (stable_sort preserves discovery priority on ties)
    std::vector<DnsIpCandidate> alive;
    alive.reserve(res.candidate_ips.size());
    for (const auto& c : res.candidate_ips) {
        if (c.is_alive && c.latency_ms > 0) {
            alive.push_back(c);
        }
    }

    std::stable_sort(alive.begin(), alive.end(), [](const DnsIpCandidate& a, const DnsIpCandidate& b) {
        return a.latency_ms < b.latency_ms;
    });

    if (!alive.empty()) {
        res.best_primary_ip = alive[0].ip;
        res.best_primary_latency = alive[0].latency_ms;

        if (alive.size() > 1) {
            res.best_secondary_ip = alive[1].ip;
            res.best_secondary_latency = alive[1].latency_ms;
        } else {
            // Preserve existing secondary_ipv4 on single-survivor transient UDP loss when distinct from primary
            if (!profile.secondary_ipv4.empty() &&
                profile.secondary_ipv4 != res.best_primary_ip &&
                SmartDnsVerifier::is_valid_public_ipv4(profile.secondary_ipv4)) {
                res.best_secondary_ip = profile.secondary_ipv4;
                res.best_secondary_latency = -1;
            } else {
                res.best_secondary_ip.clear();
                res.best_secondary_latency = -1;
            }
        }

        if (res.best_primary_ip != res.original_primary_ip ||
            res.best_secondary_ip != res.original_secondary_ip) {
            res.ip_updated = true;
        }
    } else {
        res.best_primary_latency = -1;
        res.best_secondary_latency = -1;
    }

    return res;
}

} // anonymous namespace

namespace quickdns {

std::string SmartDnsVerifier::base64url_encode(const std::vector<uint8_t>& data) {
    static const char tbl[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    std::string res;
    res.reserve((data.size() * 4 + 2) / 3);
    size_t i = 0;
    while (i + 2 < data.size()) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8) |
                      static_cast<uint32_t>(data[i + 2]);
        res.push_back(tbl[(n >> 18) & 63]);
        res.push_back(tbl[(n >> 12) & 63]);
        res.push_back(tbl[(n >> 6) & 63]);
        res.push_back(tbl[n & 63]);
        i += 3;
    }
    if (i + 1 < data.size()) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16) |
                     (static_cast<uint32_t>(data[i + 1]) << 8);
        res.push_back(tbl[(n >> 18) & 63]);
        res.push_back(tbl[(n >> 12) & 63]);
        res.push_back(tbl[(n >> 6) & 63]);
    } else if (i < data.size()) {
        uint32_t n = (static_cast<uint32_t>(data[i]) << 16);
        res.push_back(tbl[(n >> 18) & 63]);
        res.push_back(tbl[(n >> 12) & 63]);
    }
    return res;
}

bool SmartDnsVerifier::is_valid_public_ipv4(const std::string& ip_str) {
    return AiChecker::is_valid_public_ipv4(ip_str);
}

std::string SmartDnsVerifier::extract_host_from_url(const std::string& url) {
    std::string trimmed = trim_ascii(url);
    if (trimmed.empty()) {
        return "";
    }

    std::wstring wurl = utf8_to_wide(trimmed);
    if (wurl.empty()) {
        return "";
    }

    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = ARRAYSIZE(host);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.length()), 0, &urlComp)) {
        return "";
    }
    if (urlComp.nScheme != INTERNET_SCHEME_HTTPS && urlComp.nScheme != INTERNET_SCHEME_HTTP) {
        return "";
    }
    if (urlComp.dwHostNameLength == 0) {
        return "";
    }

    std::string host_utf8 = wide_to_utf8(std::wstring_view(host, urlComp.dwHostNameLength));
    if (host_utf8.size() >= 2 && host_utf8.front() == '[' && host_utf8.back() == ']') {
        host_utf8 = host_utf8.substr(1, host_utf8.size() - 2);
    }
    return to_lower_ascii(trim_ascii(host_utf8));
}

std::vector<std::string> SmartDnsVerifier::resolve_hostname_ips(
    const std::string& host,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag)
{
    std::vector<std::string> ips;
    std::string clean_host = to_lower_ascii(trim_ascii(host));
    if (clean_host.empty() || timeout_ms <= 0 || is_cancelled(cancel_flag)) {
        return ips;
    }

    if (is_valid_public_ipv4(clean_host)) {
        ips.push_back(clean_host);
        return ips;
    }

    if (!is_safe_public_hostname_syntax(clean_host)) {
        return ips;
    }

    int safe_timeout = std::max(250, timeout_ms);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(safe_timeout);

    auto append_unique_public = [&](const std::vector<std::string>& resolved) {
        for (const auto& ip : resolved) {
            if (is_valid_public_ipv4(ip) &&
                std::find(ips.begin(), ips.end(), ip) == ips.end()) {
                ips.push_back(ip);
            }
        }
    };

    // Resolve via Cloudflare 1.1.1.1 with explicit timeout, falling back to Google 8.8.8.8
    int t1 = std::max(200, (safe_timeout * 3) / 5);
    auto res1 = AiChecker::resolve_domain_all_via_dns("1.1.1.1", clean_host, t1, cancel_flag);
    append_unique_public(res1);

    if (ips.empty() && !is_cancelled(cancel_flag)) {
        int t2 = std::max(200, remaining_ms(deadline));
        auto res2 = AiChecker::resolve_domain_all_via_dns("8.8.8.8", clean_host, t2, cancel_flag);
        append_unique_public(res2);
    }

    return ips;
}

std::vector<std::string> SmartDnsVerifier::scrape_ips_from_webpage(
    const std::string& url,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag)
{
    std::vector<std::string> found_ips;
    std::string trimmed = trim_ascii(url);
    if (trimmed.empty() || timeout_ms <= 0 || is_cancelled(cancel_flag)) {
        return found_ips;
    }

    std::wstring wurl = utf8_to_wide(trimmed);
    if (wurl.empty()) {
        return found_ips;
    }

    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    wchar_t extra[512] = {};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = ARRAYSIZE(host);
    urlComp.lpszUrlPath = path;
    urlComp.dwUrlPathLength = ARRAYSIZE(path);
    urlComp.lpszExtraInfo = extra;
    urlComp.dwExtraInfoLength = ARRAYSIZE(extra);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.length()), 0, &urlComp)) {
        return found_ips;
    }

    // Strictly require HTTPS
    if (urlComp.nScheme != INTERNET_SCHEME_HTTPS || urlComp.dwHostNameLength == 0) {
        return found_ips;
    }

    std::wstring host_wide(host, urlComp.dwHostNameLength);
    std::string host_utf8 = to_lower_ascii(wide_to_utf8(host_wide));
    if (!is_safe_public_hostname_syntax(host_utf8)) {
        return found_ips;
    }

    // Verify target host resolves only to public IPv4 (blocks RFC1918/loopback/link-local SSRF)
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);
    int dns_budget = std::min(timeout_ms / 2, 1000);
    if (dns_budget >= 200) {
        auto host_ips = resolve_hostname_ips(host_utf8, dns_budget, cancel_flag);
        if (host_ips.empty() || is_cancelled(cancel_flag)) {
            return found_ips;
        }
    }

    int http_budget = remaining_ms(deadline);
    if (http_budget <= 0 || is_cancelled(cancel_flag)) {
        return found_ips;
    }

    std::wstring req_path(path, urlComp.dwUrlPathLength);
    if (req_path.empty()) {
        req_path = L"/";
    }
    if (urlComp.dwExtraInfoLength > 0) {
        req_path.append(extra, urlComp.dwExtraInfoLength);
    }

    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) QuickDNS/2.3.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) {
        return found_ips;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    DWORD to = static_cast<DWORD>(std::max(150, http_budget));
    WinHttpSetTimeouts(hSession, to, to, to, to);

    HINTERNET hConnect = WinHttpConnect(hSession, host_wide.c_str(), urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return found_ips;
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        req_path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return found_ips;
    }

    BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0);
    if (sent && !is_cancelled(cancel_flag) && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusSz = sizeof(statusCode);
        bool statusOk = WinHttpQueryHeaders(
            hRequest,
            WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
            WINHTTP_HEADER_NAME_BY_INDEX,
            &statusCode,
            &statusSz,
            WINHTTP_NO_HEADER_INDEX
        ) != FALSE;

        // Verify post-redirect URL remains HTTPS on a safe public host
        bool finalUrlSafe = true;
        DWORD finalUrlBytes = 0;
        WinHttpQueryOption(hRequest, WINHTTP_OPTION_URL, nullptr, &finalUrlBytes);
        if (GetLastError() == ERROR_INSUFFICIENT_BUFFER && finalUrlBytes > 0 && finalUrlBytes <= 8192) {
            std::wstring finalUrl(finalUrlBytes / sizeof(wchar_t), L'\0');
            if (WinHttpQueryOption(hRequest, WINHTTP_OPTION_URL, finalUrl.data(), &finalUrlBytes)) {
                size_t nul_pos = finalUrl.find(L'\0');
                if (nul_pos != std::wstring::npos) {
                    finalUrl.resize(nul_pos);
                }
                URL_COMPONENTS finalComp{};
                finalComp.dwStructSize = sizeof(finalComp);
                wchar_t finalHost[256] = {};
                finalComp.lpszHostName = finalHost;
                finalComp.dwHostNameLength = ARRAYSIZE(finalHost);
                if (!WinHttpCrackUrl(finalUrl.c_str(), static_cast<DWORD>(finalUrl.length()), 0, &finalComp) ||
                    finalComp.nScheme != INTERNET_SCHEME_HTTPS ||
                    !is_safe_public_hostname_syntax(wide_to_utf8(std::wstring_view(finalHost, finalComp.dwHostNameLength)))) {
                    finalUrlSafe = false;
                }
            }
        }

        if (statusOk && statusCode == 200 && finalUrlSafe) {
            std::string html_body;
            html_body.reserve(16384);
            DWORD bytesRead = 0;
            char buffer[8192];
            while (!is_cancelled(cancel_flag) &&
                   WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) &&
                   bytesRead > 0) {
                html_body.append(buffer, bytesRead);
                if (html_body.size() >= kMaxScrapedHtmlBytes) {
                    break;
                }
            }

            // Linear O(N) scan for standalone dotted-quad IPv4 tokens, deduplicated and capped at 16
            std::set<std::string> seen;
            size_t n = html_body.size();
            size_t i = 0;
            while (i < n && found_ips.size() < kMaxScrapedIps) {
                char ch = html_body[i];
                if (ch >= '0' && ch <= '9') {
                    bool prev_invalid = (i > 0) && (is_ascii_alnum(html_body[i - 1]) || html_body[i - 1] == '.');
                    size_t start = i;
                    while (i < n && ((html_body[i] >= '0' && html_body[i] <= '9') || html_body[i] == '.')) {
                        ++i;
                    }
                    bool next_invalid = (i < n) && is_ascii_alnum(html_body[i]);
                    if (!prev_invalid && !next_invalid) {
                        size_t end = i;
                        if (end > start && html_body[end - 1] == '.') {
                            --end; // Strip sentence-ending trailing dot if not followed by digit/alnum
                        }
                        size_t tok_len = end - start;
                        if (tok_len >= 7 && tok_len <= 15) {
                            std::string token = html_body.substr(start, tok_len);
                            if (is_valid_public_ipv4(token) && seen.insert(token).second) {
                                found_ips.push_back(std::move(token));
                            }
                        }
                    }
                } else {
                    ++i;
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return found_ips;
}

std::vector<DnsIpCandidate> SmartDnsVerifier::discover_candidate_ips(
    const DnsProfile& profile,
    const std::atomic<bool>* cancel_flag)
{
    std::vector<DnsIpCandidate> candidates;
    std::set<std::string> seen;

    auto add_candidate = [&](const std::string& ip, const char* source, bool always_exclude_std_resolvers) {
        if (ip.empty() || !is_valid_public_ipv4(ip)) {
            return;
        }
        if ((profile.is_smart_dns || always_exclude_std_resolvers) &&
            is_standard_public_resolver(ip)) {
            return;
        }
        if (seen.insert(ip).second) {
            DnsIpCandidate cand;
            cand.ip = ip;
            cand.latency_ms = -1;
            cand.is_alive = false;
            cand.source = source;
            candidates.push_back(std::move(cand));
        }
    };

    // 1. Initial configured IPs and IP pool ("preset")
    add_candidate(profile.primary_ipv4, "preset", false);
    add_candidate(profile.secondary_ipv4, "preset", false);
    for (const auto& ip : profile.ip_pool) {
        add_candidate(ip, "preset", false);
    }

    if (is_cancelled(cancel_flag)) {
        return candidates;
    }

    // 2. Resolve hostname from doh_template ("resolved_host")
    std::string dohHost = extract_host_from_url(profile.doh_template);
    if (!dohHost.empty() && !is_cancelled(cancel_flag)) {
        auto resolved = resolve_hostname_ips(dohHost, 1000, cancel_flag);
        for (const auto& ip : resolved) {
            add_candidate(ip, "resolved_host", false);
        }
    }

    // 3. Resolve hostname from website_url ("resolved_host")
    std::string webHost = extract_host_from_url(profile.website_url);
    if (!webHost.empty() && webHost != dohHost && !is_cancelled(cancel_flag)) {
        // Avoid resolving shared marketing domain geohide.ru into regional EU/US profiles
        if (profile.id != "geohide_eu" && profile.id != "geohide_us") {
            auto resolved = resolve_hostname_ips(webHost, 1000, cancel_flag);
            for (const auto& ip : resolved) {
                add_candidate(ip, "resolved_host", false);
            }
        }
    }

    if (is_cancelled(cancel_flag)) {
        return candidates;
    }

    // 4. Known SmartDNS server pools and exact regional aliases
    auto add_resolved_host = [&](const char* host) {
        if (is_cancelled(cancel_flag)) return;
        for (const auto& ip : resolve_hostname_ips(host, 1000, cancel_flag)) {
            add_candidate(ip, "resolved_host", true);
        }
    };

    if (profile.id.find("comss") != std::string::npos) {
        add_resolved_host("dns.comss.one");
    } else if (profile.id == "geohide_eu") {
        add_resolved_host("eu.geohide.ru");
    } else if (profile.id == "geohide_us") {
        add_resolved_host("us.geohide.ru");
    } else if (profile.id == "geohide" || profile.id.find("geohide") != std::string::npos) {
        add_resolved_host("dns.geohide.ru");
    } else if (profile.id.find("bezmezhau") != std::string::npos) {
        add_resolved_host("dns.bezmezhau.com");
        add_resolved_host("sync.bezmezhau.com");
        add_resolved_host("bezmezhau.com");
        add_candidate("185.236.10.189", "preset", true);
        add_candidate("143.20.64.55", "preset", true);
        add_candidate("31.58.216.201", "preset", true);
        add_candidate("181.215.40.49", "preset", true);
    } else if (profile.id.find("zapret") != std::string::npos) {
        add_resolved_host("dns.zapret.moe");
    } else if (profile.id.find("malw") != std::string::npos) {
        add_resolved_host("dns.malw.link");
    } else if (profile.id.find("astracat") != std::string::npos) {
        add_resolved_host("dns.astracat.ru");
    } else if (profile.id.find("mafioznik") != std::string::npos) {
        add_resolved_host("dns.mafioznik.xyz");
    } else if (profile.id.find("shecan") != std::string::npos) {
        add_resolved_host("free.shecan.ir");
    }

    // 5. Safe HTTPS web scraping from provider webpage (defer UDP probing to verify_profile)
    if (!profile.website_url.empty() &&
        !is_cancelled(cancel_flag) &&
        profile.id != "geohide_eu" &&
        profile.id != "geohide_us") {
        auto scraped = scrape_ips_from_webpage(profile.website_url, 2200, cancel_flag);
        size_t added_scraped = 0;
        for (const auto& ip : scraped) {
            if (added_scraped >= kMaxScrapedIps) {
                break;
            }
            size_t before = candidates.size();
            add_candidate(ip, "scraped_web", true);
            if (candidates.size() > before) {
                ++added_scraped;
            }
        }
    }

    return candidates;
}

DnsIpCandidate SmartDnsVerifier::verify_ip(
    const std::string& ip,
    const std::string& target_domain,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag)
{
    DnsIpCandidate candidate;
    candidate.ip = ip;
    candidate.is_alive = false;
    candidate.latency_ms = -1;

    if (timeout_ms <= 0 || is_cancelled(cancel_flag) || !is_valid_public_ipv4(ip)) {
        return candidate;
    }
    if (AiChecker::build_dns_a_query(1, target_domain).empty()) {
        return candidate;
    }

    ensure_winsock();

    sockaddr_in sa4{};
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons(53);
    if (inet_pton(AF_INET, ip.c_str(), &sa4.sin_addr) != 1) {
        return candidate;
    }

    auto total_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    // Up to 2 UDP attempts (1 automatic retry on packet loss/timeout within timeout_ms)
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (is_cancelled(cancel_flag)) {
            return candidate;
        }
        int rem_total = remaining_ms(total_deadline);
        if (rem_total <= 0) {
            break;
        }

        int attempt_budget = (attempt == 0)
            ? std::max(120, std::min(rem_total, (timeout_ms * 3) / 5))
            : rem_total;
        auto attempt_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(attempt_budget);

        uint16_t txid = generate_dns_txid();
        auto query = AiChecker::build_dns_a_query(txid, target_domain);
        if (query.empty()) {
            return candidate;
        }

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            return candidate;
        }

        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);

        // Connect UDP socket so Winsock enforces exact remote IP and port 53
        if (connect(sock, reinterpret_cast<const sockaddr*>(&sa4), sizeof(sa4)) == SOCKET_ERROR) {
            closesocket(sock);
            return candidate;
        }

        auto t0 = std::chrono::steady_clock::now();
        int sent = send(
            sock,
            reinterpret_cast<const char*>(query.data()),
            static_cast<int>(query.size()),
            0
        );
        if (sent != static_cast<int>(query.size())) {
            closesocket(sock);
            continue;
        }

        bool definitive_negative = false;
        bool need_tcp_fallback = false;

        // Receive loop ignoring stray datagrams until attempt_deadline
        while (true) {
            uint8_t recv_buf[1500] = {};
            int received = udp_recv_with_deadline(
                sock,
                recv_buf,
                sizeof(recv_buf),
                attempt_deadline,
                cancel_flag
            );
            if (received == -3) {
                closesocket(sock);
                return candidate;
            }
            if (received <= 0) {
                break; // Timeout (-1) or ICMP unreachable (-2)
            }
            if (received < 12) {
                continue;
            }

            uint16_t rx_txid = static_cast<uint16_t>((static_cast<uint16_t>(recv_buf[0]) << 8) | recv_buf[1]);
            if (rx_txid != txid) {
                continue; // Stray packet with different TXID
            }
            if ((recv_buf[2] & 0x80) == 0) {
                continue; // Not a DNS response
            }

            uint8_t rcode = recv_buf[3] & 0x0F;
            bool tc_bit = (recv_buf[2] & 0x02) != 0;
            if (tc_bit && rcode == 0) {
                need_tcp_fallback = true;
                break;
            }

            std::string resolved_ip = AiChecker::parse_dns_a_response(
                recv_buf, static_cast<size_t>(received), txid);
            if (!resolved_ip.empty()) {
                auto t1 = std::chrono::steady_clock::now();
                int lat = static_cast<int>(
                    std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
                candidate.latency_ms = std::max(1, lat);
                candidate.is_alive = true;
                closesocket(sock);
                return candidate;
            }

            // Sinkhole IP, ANCOUNT=0 referral, or non-zero RCODE (SERVFAIL/REFUSED/NXDOMAIN)
            definitive_negative = true;
            break;
        }

        closesocket(sock);

        if (need_tcp_fallback) {
            int tcp_rem = remaining_ms(total_deadline);
            if (tcp_rem > 0) {
                auto t_tcp0 = std::chrono::steady_clock::now();
                std::string tcp_ip = AiChecker::resolve_domain_via_dns(
                    ip, target_domain, tcp_rem, cancel_flag);
                if (!tcp_ip.empty()) {
                    auto t_tcp1 = std::chrono::steady_clock::now();
                    int lat = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(t_tcp1 - t_tcp0).count());
                    candidate.latency_ms = std::max(1, lat);
                    candidate.is_alive = true;
                }
            }
            return candidate;
        }

        if (definitive_negative) {
            return candidate;
        }
    }

    return candidate;
}

DohVerificationResult SmartDnsVerifier::verify_doh(
    const std::string& doh_url,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag)
{
    DohVerificationResult result;
    result.doh_url = doh_url;
    result.is_working = false;
    result.latency_ms = -1;
    result.http_status = 0;
    result.status_text = L"DoH не настроен";

    std::string trimmed = trim_ascii(doh_url);
    if (trimmed.empty()) {
        return result;
    }
    if (timeout_ms <= 0 || is_cancelled(cancel_flag)) {
        result.status_text = L"Отменено";
        return result;
    }

    // Strip RFC 8484 URI template suffix "{?dns}" if present
    size_t tmpl_pos = trimmed.find("{?dns}");
    if (tmpl_pos != std::string::npos) {
        trimmed.erase(tmpl_pos, 6);
    }

    std::wstring wurl = utf8_to_wide(trimmed);
    if (wurl.empty()) {
        result.status_text = L"Неверный URL DoH";
        return result;
    }

    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    wchar_t extra[512] = {};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = ARRAYSIZE(host);
    urlComp.lpszUrlPath = path;
    urlComp.dwUrlPathLength = ARRAYSIZE(path);
    urlComp.lpszExtraInfo = extra;
    urlComp.dwExtraInfoLength = ARRAYSIZE(extra);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.length()), 0, &urlComp)) {
        result.status_text = L"Неверный URL DoH";
        return result;
    }

    if (urlComp.nScheme != INTERNET_SCHEME_HTTPS || urlComp.dwHostNameLength == 0) {
        result.status_text = L"Требуется HTTPS для DoH";
        return result;
    }

    std::wstring host_wide(host, urlComp.dwHostNameLength);
    std::wstring norm_path(path, urlComp.dwUrlPathLength);
    if (norm_path.empty()) {
        norm_path = L"/";
    }
    if (urlComp.dwExtraInfoLength > 0) {
        norm_path.append(extra, urlComp.dwExtraInfoLength);
    }

    HINTERNET hSession = WinHttpOpen(
        L"QuickDNS/2.3.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) {
        result.status_text = L"Ошибка инициализации сети";
        return result;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    DWORD to = static_cast<DWORD>(std::max(150, timeout_ms));
    WinHttpSetTimeouts(hSession, to, to, to, to);

    DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));

    HINTERNET hConnect = WinHttpConnect(hSession, host_wide.c_str(), urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        result.status_text = L"Не удалось подключиться к DoH";
        return result;
    }

    const uint16_t txid = generate_dns_txid();
    auto query = AiChecker::build_dns_a_query(txid, "google.com");
    if (query.empty()) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        result.status_text = L"Ошибка формирования запроса";
        return result;
    }

    DWORD lastErr = 0;
    DWORD postStatus = 0;
    bool invalidDnsBodyOn200 = false;

    // 1. Try HTTP POST (RFC 8484)
    HINTERNET hReqPost = WinHttpOpenRequest(
        hConnect,
        L"POST",
        norm_path.c_str(),
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );

    if (hReqPost) {
        std::wstring headers = L"Content-Type: application/dns-message\r\nAccept: application/dns-message\r\n";

        auto t0 = std::chrono::steady_clock::now();
        BOOL sent = WinHttpSendRequest(
            hReqPost,
            headers.c_str(),
            static_cast<DWORD>(headers.length()),
            query.data(),
            static_cast<DWORD>(query.size()),
            static_cast<DWORD>(query.size()),
            0
        );

        if (sent && !is_cancelled(cancel_flag) && WinHttpReceiveResponse(hReqPost, nullptr)) {
            DWORD statusCode = 0;
            DWORD sz = sizeof(statusCode);
            WinHttpQueryHeaders(
                hReqPost,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &sz,
                WINHTTP_NO_HEADER_INDEX
            );
            postStatus = statusCode;
            result.http_status = static_cast<int>(statusCode);

            if (statusCode == 200) {
                std::vector<uint8_t> body;
                bool readOk = read_doh_response_body(hReqPost, body, cancel_flag);
                auto t1 = std::chrono::steady_clock::now();
                if (readOk && !AiChecker::parse_dns_a_response(body.data(), body.size(), txid).empty()) {
                    result.latency_ms = std::max(
                        1,
                        static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()));
                    result.is_working = true;
                    result.status_text = L"DoH OK (" + std::to_wstring(result.latency_ms) + L" ms)";
                    WinHttpCloseHandle(hReqPost);
                    WinHttpCloseHandle(hConnect);
                    WinHttpCloseHandle(hSession);
                    return result;
                }
                invalidDnsBodyOn200 = true;
            }
        } else {
            lastErr = GetLastError();
        }
        WinHttpCloseHandle(hReqPost);
    }

    // 2. Fallback: Try HTTP GET ONLY on method/media-type status rejection (400 / 405 / 415 / 501)
    bool shouldTryGet = !is_cancelled(cancel_flag) &&
                        (postStatus == 400 || postStatus == 405 || postStatus == 415 || postStatus == 501);

    if (shouldTryGet) {
        std::string b64q = base64url_encode(query);
        std::wstring getPath = norm_path;
        if (getPath.find(L'?') == std::wstring::npos) {
            getPath += L"?dns=" + utf8_to_wide(b64q);
        } else {
            getPath += L"&dns=" + utf8_to_wide(b64q);
        }

        HINTERNET hReqGet = WinHttpOpenRequest(
            hConnect,
            L"GET",
            getPath.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE
        );

        if (hReqGet) {
            std::wstring headers = L"Accept: application/dns-message\r\n";

            auto t0 = std::chrono::steady_clock::now();
            BOOL sent = WinHttpSendRequest(
                hReqGet,
                headers.c_str(),
                static_cast<DWORD>(headers.length()),
                nullptr,
                0,
                0,
                0
            );

            if (sent && !is_cancelled(cancel_flag) && WinHttpReceiveResponse(hReqGet, nullptr)) {
                DWORD statusCode = 0;
                DWORD sz = sizeof(statusCode);
                WinHttpQueryHeaders(
                    hReqGet,
                    WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                    WINHTTP_HEADER_NAME_BY_INDEX,
                    &statusCode,
                    &sz,
                    WINHTTP_NO_HEADER_INDEX
                );
                result.http_status = static_cast<int>(statusCode);

                if (statusCode == 200) {
                    std::vector<uint8_t> body;
                    bool readOk = read_doh_response_body(hReqGet, body, cancel_flag);
                    auto t1 = std::chrono::steady_clock::now();
                    if (readOk && !AiChecker::parse_dns_a_response(body.data(), body.size(), txid).empty()) {
                        result.latency_ms = std::max(
                            1,
                            static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()));
                        result.is_working = true;
                        result.status_text = L"DoH OK (GET: " + std::to_wstring(result.latency_ms) + L" ms)";
                        WinHttpCloseHandle(hReqGet);
                        WinHttpCloseHandle(hConnect);
                        WinHttpCloseHandle(hSession);
                        return result;
                    }
                    invalidDnsBodyOn200 = true;
                }
            } else {
                lastErr = GetLastError();
            }
            WinHttpCloseHandle(hReqGet);
        }
    }

    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (is_cancelled(cancel_flag)) {
        result.status_text = L"Отменено";
    } else if (invalidDnsBodyOn200) {
        result.status_text = L"DoH Ошибка (некорректный DNS-ответ)";
    } else if (lastErr == ERROR_WINHTTP_SECURE_FAILURE || lastErr == ERROR_WINHTTP_CLIENT_AUTH_CERT_NEEDED) {
        result.status_text = L"Ошибка TLS-сертификата";
    } else if (result.http_status > 0) {
        result.status_text = L"DoH Ошибка (HTTP " + std::to_wstring(result.http_status) + L")";
    } else {
        result.status_text = L"DoH Недоступен (тайм-аут/сбой)";
    }

    return result;
}

SmartDnsVerificationResult SmartDnsVerifier::verify_profile(
    const DnsProfile& profile,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag)
{
    return verify_profile_impl(profile, timeout_ms, cancel_flag, true);
}

std::vector<SmartDnsVerificationResult> SmartDnsVerifier::verify_profiles(
    const std::vector<DnsProfile>& profiles,
    std::function<void(size_t completed, size_t total, const SmartDnsVerificationResult& latest)> on_progress,
    const std::atomic<bool>* cancel_flag)
{
    std::vector<SmartDnsVerificationResult> results(profiles.size());
    for (size_t i = 0; i < profiles.size(); ++i) {
        results[i].profile_id = profiles[i].id;
        results[i].profile_name = profiles[i].name;
        results[i].original_primary_ip = profiles[i].primary_ipv4;
        results[i].original_secondary_ip = profiles[i].secondary_ipv4;
        results[i].best_primary_ip = profiles[i].primary_ipv4;
        results[i].best_secondary_ip = profiles[i].secondary_ipv4;
    }

    if (profiles.empty() || is_cancelled(cancel_flag)) {
        return results;
    }

    std::atomic<size_t> next_index{0};
    std::mutex progress_mutex;
    size_t completed_count = 0;
    const size_t total_count = profiles.size();
    const size_t worker_count = std::min<size_t>(4, total_count);

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    ThreadPoolGuard join_guard(workers);

    auto worker_fn = [&]() {
        while (!is_cancelled(cancel_flag)) {
            size_t idx = next_index.fetch_add(1, std::memory_order_relaxed);
            if (idx >= total_count) {
                break;
            }
            try {
                auto res = verify_profile_impl(profiles[idx], 2500, cancel_flag, false);
                results[idx] = std::move(res);
                if (on_progress && !is_cancelled(cancel_flag)) {
                    std::lock_guard<std::mutex> lock(progress_mutex);
                    if (!is_cancelled(cancel_flag)) {
                        ++completed_count;
                        on_progress(completed_count, total_count, results[idx]);
                    }
                }
            } catch (...) {
            }
        }
    };

    try {
        for (size_t w = 0; w < worker_count; ++w) {
            workers.emplace_back(worker_fn);
        }
    } catch (...) {
        if (workers.empty()) {
            worker_fn();
        }
    }

    return results;
}

int SmartDnsVerifier::apply_best_ips(
    ConfigManager& config,
    const std::vector<SmartDnsVerificationResult>& results)
{
    int updated_count = 0;
    bool any_saved = false;

    for (const auto& res : results) {
        if (res.profile_id.empty()) {
            continue;
        }

        // Re-read current profile state by ID to avoid stale snapshot TOCTOU overwrites
        auto opt_p = config.get_profile_by_id(res.profile_id);
        if (!opt_p) {
            continue;
        }

        DnsProfile p = *opt_p;
        bool profile_modified = false;

        std::vector<DnsIpCandidate> alive;
        for (const auto& c : res.candidate_ips) {
            if (c.is_alive && c.latency_ms > 0 && is_valid_public_ipv4(c.ip)) {
                alive.push_back(c);
            }
        }
        std::stable_sort(alive.begin(), alive.end(), [](const DnsIpCandidate& a, const DnsIpCandidate& b) {
            return a.latency_ms < b.latency_ms;
        });

        std::vector<std::string> merged_pool;
        auto add_to_pool = [&](const std::string& ip) {
            if (!ip.empty() &&
                is_valid_public_ipv4(ip) &&
                (!p.is_smart_dns || !is_standard_public_resolver(ip)) &&
                std::find(merged_pool.begin(), merged_pool.end(), ip) == merged_pool.end()) {
                merged_pool.push_back(ip);
            }
        };

        for (const auto& c : alive) {
            add_to_pool(c.ip);
        }
        if (!res.best_primary_ip.empty()) {
            add_to_pool(res.best_primary_ip);
        }
        if (!res.best_secondary_ip.empty()) {
            add_to_pool(res.best_secondary_ip);
        }
        for (const auto& ip : p.ip_pool) {
            add_to_pool(ip);
        }

        if (p.ip_pool != merged_pool) {
            p.ip_pool = std::move(merged_pool);
            profile_modified = true;
        }

        if (res.ip_updated && is_valid_public_ipv4(res.best_primary_ip) &&
            (!p.is_smart_dns || !is_standard_public_resolver(res.best_primary_ip))) {
            std::string target_secondary;
            if (!res.best_secondary_ip.empty() &&
                res.best_secondary_ip != res.best_primary_ip &&
                is_valid_public_ipv4(res.best_secondary_ip) &&
                (!p.is_smart_dns || !is_standard_public_resolver(res.best_secondary_ip))) {
                target_secondary = res.best_secondary_ip;
            } else if (alive.size() <= 1 &&
                       !p.secondary_ipv4.empty() &&
                       p.secondary_ipv4 != res.best_primary_ip &&
                       is_valid_public_ipv4(p.secondary_ipv4)) {
                // Preserve existing valid secondary IPv4 when only 1 alive candidate responded
                target_secondary = p.secondary_ipv4;
            }

            if (p.primary_ipv4 != res.best_primary_ip || p.secondary_ipv4 != target_secondary) {
                p.primary_ipv4 = res.best_primary_ip;
                p.secondary_ipv4 = target_secondary;
                profile_modified = true;
                ++updated_count;
            }
        }

        if (profile_modified) {
            if (config.update_profile(p)) {
                any_saved = true;
            }
        }
    }

    if (any_saved) {
        config.save_to_file();
    }

    return updated_count;
}

} // namespace quickdns
