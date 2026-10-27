#include "smartdns_verifier.h"
#include "encoding_utils.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>

#include <algorithm>
#include <chrono>
#include <future>
#include <iostream>
#include <mutex>
#include <random>
#include <regex>
#include <set>
#include <sstream>

#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")

using QuickDns::Encoding::utf8_to_wide;

namespace {

void ensure_winsock() {
    static std::once_flag init_flag;
    std::call_once(init_flag, []() {
        WSADATA wsaData;
        WSAStartup(MAKEWORD(2, 2), &wsaData);
    });
}

// Build standard DNS wireformat query packet (RFC 1035)
std::vector<uint8_t> build_dns_query_packet(uint16_t txid, const std::string& domain) {
    std::vector<uint8_t> packet;
    packet.reserve(64 + domain.size());

    packet.push_back(static_cast<uint8_t>(txid >> 8));
    packet.push_back(static_cast<uint8_t>(txid & 0xFF));
    packet.push_back(0x01); packet.push_back(0x00); // RD = 1 (recursion desired)
    packet.push_back(0x00); packet.push_back(0x01); // 1 Question
    packet.push_back(0x00); packet.push_back(0x00); // 0 Answers
    packet.push_back(0x00); packet.push_back(0x00); // 0 Authority
    packet.push_back(0x00); packet.push_back(0x00); // 0 Additional

    size_t start = 0;
    while (start < domain.size()) {
        size_t dot = domain.find('.', start);
        if (dot == std::string::npos) dot = domain.size();
        size_t len = dot - start;
        if (len > 0) {
            packet.push_back(static_cast<uint8_t>(len));
            for (size_t i = start; i < dot; ++i) {
                packet.push_back(static_cast<uint8_t>(tolower(static_cast<unsigned char>(domain[i]))));
            }
        }
        start = dot + 1;
    }
    packet.push_back(0x00); // Root null label
    packet.push_back(0x00); packet.push_back(0x01); // Type: A (IPv4)
    packet.push_back(0x00); packet.push_back(0x01); // Class: IN
    return packet;
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
    if (ip_str.empty()) return false;
    unsigned int a = 0, b = 0, c = 0, d = 0;
    char tail = 0;
    if (sscanf_s(ip_str.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail, 1) != 4) {
        return false;
    }
    if (a > 255 || b > 255 || c > 255 || d > 255) return false;

    // Filter non-routable, private, multicast, loopback addresses
    if (a == 0) return false;                           // 0.0.0.0/8
    if (a == 10) return false;                          // 10.0.0.0/8 Private
    if (a == 127) return false;                         // 127.0.0.0/8 Loopback
    if (a == 169 && b == 254) return false;             // 169.254.0.0/16 Link-local
    if (a == 172 && (b >= 16 && b <= 31)) return false; // 172.16.0.0/12 Private
    if (a == 192 && b == 168) return false;             // 192.168.0.0/16 Private
    if (a >= 224 && a <= 239) return false;             // Multicast
    if (a >= 240) return false;                         // Reserved / 255.255.255.255

    return true;
}

std::string SmartDnsVerifier::extract_host_from_url(const std::string& url) {
    std::string host = url;
    if (host.rfind("https://", 0) == 0) host = host.substr(8);
    else if (host.rfind("http://", 0) == 0) host = host.substr(7);

    size_t slash = host.find('/');
    if (slash != std::string::npos) host = host.substr(0, slash);
    size_t qmark = host.find('?');
    if (qmark != std::string::npos) host = host.substr(0, qmark);
    size_t colon = host.find(':');
    if (colon != std::string::npos) host = host.substr(0, colon);

    return host;
}

std::vector<std::string> SmartDnsVerifier::resolve_hostname_ips(const std::string& host) {
    std::vector<std::string> ips;
    if (host.empty()) return ips;
    ensure_winsock();

    addrinfo hints{};
    hints.ai_family = AF_INET; // IPv4
    hints.ai_socktype = SOCK_STREAM;

    addrinfo* result = nullptr;
    if (getaddrinfo(host.c_str(), nullptr, &hints, &result) == 0 && result) {
        for (addrinfo* ptr = result; ptr != nullptr; ptr = ptr->ai_next) {
            if (ptr->ai_family == AF_INET) {
                auto* sa4 = reinterpret_cast<sockaddr_in*>(ptr->ai_addr);
                char ip_buf[INET_ADDRSTRLEN] = {};
                if (inet_ntop(AF_INET, &sa4->sin_addr, ip_buf, sizeof(ip_buf))) {
                    std::string ip(ip_buf);
                    if (is_valid_public_ipv4(ip)) {
                        ips.push_back(ip);
                    }
                }
            }
        }
        freeaddrinfo(result);
    }
    return ips;
}

std::vector<std::string> SmartDnsVerifier::scrape_ips_from_webpage(const std::string& url, int timeout_ms) {
    std::vector<std::string> found_ips;
    if (url.empty()) return found_ips;

    std::wstring wurl = utf8_to_wide(url);
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = ARRAYSIZE(host);
    urlComp.lpszUrlPath = path;
    urlComp.dwUrlPathLength = ARRAYSIZE(path);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.length()), 0, &urlComp)) {
        return found_ips;
    }

    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) QuickDNS/1.7.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0
    );
    if (!hSession) return found_ips;

    DWORD to = static_cast<DWORD>(timeout_ms);
    WinHttpSetTimeouts(hSession, to, to, to, to);

    HINTERNET hConnect = WinHttpConnect(hSession, host, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return found_ips;
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return found_ips;
    }

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));

    BOOL sent = WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0);
    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        std::string html_body;
        DWORD bytesRead = 0;
        char buffer[8192];
        while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
            html_body.append(buffer, bytesRead);
            if (html_body.size() > 512 * 1024) break; // Limit to 512 KB
        }

        // Regex for IPv4 pattern
        static const std::regex ip_regex(R"(\b(?:(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\.){3}(?:25[0-5]|2[0-4][0-9]|[01]?[0-9][0-9]?)\b)");
        auto words_begin = std::sregex_iterator(html_body.begin(), html_body.end(), ip_regex);
        auto words_end = std::sregex_iterator();

        for (auto it = words_begin; it != words_end; ++it) {
            std::string candidate = it->str();
            if (is_valid_public_ipv4(candidate)) {
                found_ips.push_back(candidate);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return found_ips;
}

std::vector<std::string> SmartDnsVerifier::discover_candidate_ips(const DnsProfile& profile) {
    std::vector<std::string> candidates;
    std::set<std::string> seen;

    auto add_candidate = [&](const std::string& ip) {
        if (!ip.empty() && is_valid_public_ipv4(ip) && seen.insert(ip).second) {
            candidates.push_back(ip);
        }
    };

    // 1. Initial configured IPs
    add_candidate(profile.primary_ipv4);
    add_candidate(profile.secondary_ipv4);

    // 2. Resolve Hostname from doh_template
    std::string dohHost = extract_host_from_url(profile.doh_template);
    if (!dohHost.empty()) {
        auto resolved = resolve_hostname_ips(dohHost);
        for (const auto& ip : resolved) add_candidate(ip);
    }

    // 3. Resolve Hostname from website_url
    std::string webHost = extract_host_from_url(profile.website_url);
    if (!webHost.empty() && webHost != dohHost) {
        auto resolved = resolve_hostname_ips(webHost);
        for (const auto& ip : resolved) add_candidate(ip);
    }

    // 4. Known SmartDNS server pools and aliases
    if (profile.id.find("comss") != std::string::npos) {
        for (const auto& ip : resolve_hostname_ips("dns.comss.one")) add_candidate(ip);
    } else if (profile.id.find("geohide") != std::string::npos) {
        for (const auto& ip : resolve_hostname_ips("dns.geohide.ru")) add_candidate(ip);
        for (const auto& ip : resolve_hostname_ips("eu.geohide.ru")) add_candidate(ip);
        for (const auto& ip : resolve_hostname_ips("us.geohide.ru")) add_candidate(ip);
    } else if (profile.id.find("bezmezhau") != std::string::npos) {
        for (const auto& ip : resolve_hostname_ips("dns.bezmezhau.com")) add_candidate(ip);
    } else if (profile.id.find("zapret") != std::string::npos) {
        for (const auto& ip : resolve_hostname_ips("dns.zapret.moe")) add_candidate(ip);
    } else if (profile.id.find("malw") != std::string::npos) {
        for (const auto& ip : resolve_hostname_ips("dns.malw.link")) add_candidate(ip);
    } else if (profile.id.find("astracat") != std::string::npos) {
        for (const auto& ip : resolve_hostname_ips("dns.astracat.ru")) add_candidate(ip);
    } else if (profile.id.find("mafioznik") != std::string::npos) {
        for (const auto& ip : resolve_hostname_ips("dns.mafioznik.xyz")) add_candidate(ip);
    } else if (profile.id.find("shecan") != std::string::npos) {
        for (const auto& ip : resolve_hostname_ips("free.shecan.ir")) add_candidate(ip);
    }

    // 5. Web scraping from provider webpage if available
    if (!profile.website_url.empty()) {
        auto scraped = scrape_ips_from_webpage(profile.website_url, 2500);
        for (const auto& ip : scraped) add_candidate(ip);
    }

    return candidates;
}

DnsIpCandidate SmartDnsVerifier::verify_ip(const std::string& ip, const std::string& target_domain, int timeout_ms) {
    DnsIpCandidate candidate;
    candidate.ip = ip;
    candidate.is_alive = false;
    candidate.latency_ms = -1;

    if (!is_valid_public_ipv4(ip)) return candidate;
    ensure_winsock();

    sockaddr_in sa4{};
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons(53);
    if (inet_pton(AF_INET, ip.c_str(), &sa4.sin_addr) != 1) {
        return candidate;
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) return candidate;

    DWORD to = static_cast<DWORD>(timeout_ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint16_t> dist(1, 0xFFFE);
    uint16_t txid = dist(rng);

    auto query = build_dns_query_packet(txid, target_domain);

    auto t0 = std::chrono::steady_clock::now();
    int sent = sendto(sock, reinterpret_cast<const char*>(query.data()), static_cast<int>(query.size()), 0,
                      reinterpret_cast<const sockaddr*>(&sa4), sizeof(sa4));

    if (sent > 0) {
        char recv_buf[512] = {};
        sockaddr_in from{};
        int from_len = sizeof(from);
        int received = recvfrom(sock, recv_buf, sizeof(recv_buf), 0,
                                reinterpret_cast<sockaddr*>(&from), &from_len);
        if (received >= 12) {
            uint16_t r_txid = (static_cast<uint8_t>(recv_buf[0]) << 8) | static_cast<uint8_t>(recv_buf[1]);
            uint8_t flags_hi = static_cast<uint8_t>(recv_buf[2]);
            bool is_response = (flags_hi & 0x80) != 0;

            if (r_txid == txid && is_response) {
                auto t1 = std::chrono::steady_clock::now();
                int lat = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count());
                candidate.latency_ms = std::max(1, lat);
                candidate.is_alive = true;
            }
        }
    }

    closesocket(sock);
    return candidate;
}

DohVerificationResult SmartDnsVerifier::verify_doh(const std::string& doh_url, int timeout_ms) {
    DohVerificationResult result;
    result.doh_url = doh_url;
    result.is_working = false;
    result.latency_ms = -1;
    result.http_status = 0;
    result.status_text = L"DoH не настроен";

    if (doh_url.empty()) return result;

    std::wstring wurl = utf8_to_wide(doh_url);
    URL_COMPONENTS urlComp{};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {};
    wchar_t path[1024] = {};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = ARRAYSIZE(host);
    urlComp.lpszUrlPath = path;
    urlComp.dwUrlPathLength = ARRAYSIZE(path);

    if (!WinHttpCrackUrl(wurl.c_str(), static_cast<DWORD>(wurl.length()), 0, &urlComp)) {
        result.status_text = L"Неверный URL DoH";
        return result;
    }

    HINTERNET hSession = WinHttpOpen(
        L"QuickDNS/1.7.0 (Windows NT 10.0; Win64; x64)",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0
    );
    if (!hSession) {
        result.status_text = L"Ошибка инициализации сети";
        return result;
    }

    DWORD to = static_cast<DWORD>(timeout_ms);
    WinHttpSetTimeouts(hSession, to, to, to, to);

    // Enable HTTP/2 if available
    DWORD protocols = WINHTTP_PROTOCOL_FLAG_HTTP2;
    WinHttpSetOption(hSession, WINHTTP_OPTION_ENABLE_HTTP_PROTOCOL, &protocols, sizeof(protocols));

    HINTERNET hConnect = WinHttpConnect(hSession, host, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        result.status_text = L"Не удалось подключиться к DoH";
        return result;
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    auto query = build_dns_query_packet(0x3456, "google.com");

    DWORD secFlags = SECURITY_FLAG_IGNORE_UNKNOWN_CA |
                     SECURITY_FLAG_IGNORE_CERT_DATE_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_CN_INVALID |
                     SECURITY_FLAG_IGNORE_CERT_WRONG_USAGE;

    // 1. Try HTTP POST (standard RFC 8484)
    HINTERNET hReqPost = WinHttpOpenRequest(
        hConnect, L"POST", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags
    );

    if (hReqPost) {
        WinHttpSetOption(hReqPost, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
        std::wstring headers = L"Content-Type: application/dns-message\r\nAccept: application/dns-message\r\n";

        auto t0 = std::chrono::steady_clock::now();
        BOOL sent = WinHttpSendRequest(
            hReqPost, headers.c_str(), static_cast<DWORD>(headers.length()),
            query.data(), static_cast<DWORD>(query.size()),
            static_cast<DWORD>(query.size()), 0
        );

        if (sent && WinHttpReceiveResponse(hReqPost, nullptr)) {
            auto t1 = std::chrono::steady_clock::now();
            DWORD statusCode = 0;
            DWORD sz = sizeof(statusCode);
            WinHttpQueryHeaders(hReqPost, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
            result.http_status = static_cast<int>(statusCode);

            if (statusCode == 200) {
                result.latency_ms = std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()));
                result.is_working = true;
                result.status_text = L"DoH OK (" + std::to_wstring(result.latency_ms) + L" ms)";
                WinHttpCloseHandle(hReqPost);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return result;
            }
        }
        WinHttpCloseHandle(hReqPost);
    }

    // 2. Fallback: Try HTTP GET with base64url wireformat (for servers that reject POST)
    std::string b64q = base64url_encode(query);
    std::wstring getPath = path;
    if (getPath.find(L'?') == std::wstring::npos) {
        getPath += L"?dns=" + utf8_to_wide(b64q);
    } else {
        getPath += L"&dns=" + utf8_to_wide(b64q);
    }

    HINTERNET hReqGet = WinHttpOpenRequest(
        hConnect, L"GET", getPath.c_str(), nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags
    );

    if (hReqGet) {
        WinHttpSetOption(hReqGet, WINHTTP_OPTION_SECURITY_FLAGS, &secFlags, sizeof(secFlags));
        std::wstring headers = L"Accept: application/dns-message\r\n";

        auto t0 = std::chrono::steady_clock::now();
        BOOL sent = WinHttpSendRequest(
            hReqGet, headers.c_str(), static_cast<DWORD>(headers.length()),
            nullptr, 0, 0, 0
        );

        if (sent && WinHttpReceiveResponse(hReqGet, nullptr)) {
            auto t1 = std::chrono::steady_clock::now();
            DWORD statusCode = 0;
            DWORD sz = sizeof(statusCode);
            WinHttpQueryHeaders(hReqGet, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);
            result.http_status = static_cast<int>(statusCode);

            if (statusCode == 200) {
                result.latency_ms = std::max(1, static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t1 - t0).count()));
                result.is_working = true;
                result.status_text = L"DoH OK (GET: " + std::to_wstring(result.latency_ms) + L" ms)";
                WinHttpCloseHandle(hReqGet);
                WinHttpCloseHandle(hConnect);
                WinHttpCloseHandle(hSession);
                return result;
            }
        }
        WinHttpCloseHandle(hReqGet);
    }

    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (result.http_status > 0) {
        result.status_text = L"DoH Ошибка (HTTP " + std::to_wstring(result.http_status) + L")";
    } else {
        result.status_text = L"DoH Недоступен (тайм-аут/сбой)";
    }

    return result;
}

SmartDnsVerificationResult SmartDnsVerifier::verify_profile(const DnsProfile& profile) {
    SmartDnsVerificationResult res;
    res.profile_id = profile.id;
    res.profile_name = profile.name;
    res.original_primary_ip = profile.primary_ipv4;
    res.original_secondary_ip = profile.secondary_ipv4;
    res.best_primary_ip = profile.primary_ipv4;
    res.best_secondary_ip = profile.secondary_ipv4;

    // 1. Discover all candidate IPs
    auto candidateIps = discover_candidate_ips(profile);

    // 2. Verify all candidate IPs concurrently
    std::vector<std::future<DnsIpCandidate>> futures;
    futures.reserve(candidateIps.size());
    for (const auto& ip : candidateIps) {
        futures.push_back(std::async(std::launch::async, [ip]() {
            return verify_ip(ip);
        }));
    }

    // 3. Verify DoH endpoint in parallel
    auto dohFuture = std::async(std::launch::async, [&profile]() {
        return verify_doh(profile.doh_template);
    });

    for (auto& f : futures) {
        res.candidate_ips.push_back(f.get());
    }
    res.doh_result = dohFuture.get();

    // 4. Filter and rank alive IPs
    std::vector<DnsIpCandidate> alive;
    for (const auto& c : res.candidate_ips) {
        if (c.is_alive && c.latency_ms > 0) {
            alive.push_back(c);
        }
    }

    std::sort(alive.begin(), alive.end(), [](const DnsIpCandidate& a, const DnsIpCandidate& b) {
        return a.latency_ms < b.latency_ms;
    });

    if (!alive.empty()) {
        res.best_primary_ip = alive[0].ip;
        res.best_primary_latency = alive[0].latency_ms;

        if (alive.size() > 1) {
            res.best_secondary_ip = alive[1].ip;
            res.best_secondary_latency = alive[1].latency_ms;
        } else {
            res.best_secondary_ip = "";
            res.best_secondary_latency = -1;
        }

        // Check if IPs changed or improved
        if (res.best_primary_ip != res.original_primary_ip ||
            res.best_secondary_ip != res.original_secondary_ip) {
            res.ip_updated = true;
        }
    } else {
        // No responsive IP found
        res.best_primary_latency = -1;
        res.best_secondary_latency = -1;
    }

    return res;
}

std::vector<SmartDnsVerificationResult> SmartDnsVerifier::verify_profiles(
    const std::vector<DnsProfile>& profiles,
    std::function<void(size_t completed, size_t total, const SmartDnsVerificationResult& result)> on_progress)
{
    std::vector<SmartDnsVerificationResult> results;
    results.reserve(profiles.size());

    std::mutex progress_mutex;
    size_t completed_count = 0;
    const size_t total_count = profiles.size();

    std::vector<std::future<SmartDnsVerificationResult>> futures;
    futures.reserve(profiles.size());

    for (const auto& p : profiles) {
        futures.push_back(std::async(std::launch::async, [&p, &progress_mutex, &completed_count, total_count, on_progress]() {
            auto res = verify_profile(p);
            if (on_progress) {
                std::lock_guard<std::mutex> lock(progress_mutex);
                completed_count++;
                on_progress(completed_count, total_count, res);
            }
            return res;
        }));
    }

    for (auto& f : futures) {
        results.push_back(f.get());
    }

    return results;
}

int SmartDnsVerifier::apply_best_ips(ConfigManager& config, const std::vector<SmartDnsVerificationResult>& results) {
    int updated_count = 0;
    auto profiles = config.get_all_profiles();

    for (const auto& res : results) {
        if (!res.ip_updated || res.best_primary_ip.empty()) continue;

        for (auto& p : profiles) {
            if (p.id == res.profile_id) {
                if (p.primary_ipv4 != res.best_primary_ip || p.secondary_ipv4 != res.best_secondary_ip) {
                    p.primary_ipv4 = res.best_primary_ip;
                    p.secondary_ipv4 = res.best_secondary_ip;
                    config.update_profile(p);
                    updated_count++;
                }
                break;
            }
        }
    }

    if (updated_count > 0) {
        config.save_to_file();
    }

    return updated_count;
}

} // namespace quickdns
