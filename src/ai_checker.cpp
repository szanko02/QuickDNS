#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include "winsock_init.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <security.h>
#include <schannel.h>
#include <bcrypt.h>

#include "ai_checker.h"
#include "encoding_utils.h"

#ifdef _MSC_VER
#pragma comment(lib, "ws2_32.lib")
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#endif

#ifndef UNISP_NAME_W
#define UNISP_NAME_W L"Microsoft Unified Security Protocol Provider"
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <sstream>
#include <thread>

using QuickDns::Encoding::utf8_to_wide;

namespace {

constexpr size_t kInitialTlsBufferSize = 32768;
constexpr size_t kMaxTlsBufferSize     = 131072; // 128 KB cap per probe
constexpr size_t kMaxHttpPlaintextSize = 16384;
constexpr int    kCancelPollSliceMs    = 50;

char ascii_lower(char c) noexcept {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

char ascii_upper(char c) noexcept {
    if (c >= 'a' && c <= 'z') {
        return static_cast<char>(c - 'a' + 'A');
    }
    return c;
}

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        ch = ascii_lower(ch);
    }
    return s;
}

std::string to_upper_ascii(std::string s) {
    for (char& ch : s) {
        ch = ascii_upper(ch);
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

bool decode_dns_name(
    const uint8_t* packet,
    size_t packet_len,
    size_t inline_end,
    size_t& io_offset,
    std::string& out_name)
{
    out_name.clear();
    if (!packet || inline_end > packet_len || io_offset >= inline_end) {
        return false;
    }

    size_t cur = io_offset;
    bool jumped = false;
    size_t next_offset = 0;
    size_t jumps = 0;
    size_t wire_len = 0;

    while (true) {
        const size_t bound = jumped ? packet_len : inline_end;
        if (cur >= bound) {
            return false;
        }

        uint8_t len = packet[cur];
        if (len == 0) {
            wire_len += 1;
            if (wire_len > 255) {
                return false;
            }
            io_offset = jumped ? next_offset : (cur + 1);
            return true;
        }

        if ((len & 0xC0) == 0xC0) {
            if (cur + 1 >= bound) {
                return false;
            }
            uint16_t ptr = static_cast<uint16_t>(
                (static_cast<uint16_t>(len & 0x3F) << 8) | packet[cur + 1]);
            if (ptr < 12 || ptr >= packet_len || ptr == cur) {
                return false;
            }
            if (++jumps > 64) {
                return false;
            }
            if (!jumped) {
                next_offset = cur + 2;
                jumped = true;
            }
            cur = ptr;
            continue;
        }

        if ((len & 0xC0) != 0) {
            return false;
        }

        if (cur + 1 + len > bound) {
            return false;
        }
        wire_len += 1 + len;
        if (wire_len >= 255) {
            return false; // Needs at least 1 byte left for root terminator 0x00
        }

        if (!out_name.empty()) {
            out_name.push_back('.');
        }
        for (size_t i = 0; i < len; ++i) {
            char ch = static_cast<char>(packet[cur + 1 + i]);
            out_name.push_back(ascii_lower(ch));
        }
        cur += 1 + len;
    }
}

bool is_geo_blocked_country(const std::string& code) {
    std::string cc = to_upper_ascii(trim_ascii(code));
    return (cc == "RU" || cc == "BY" || cc == "IR" || cc == "CN" ||
            cc == "KP" || cc == "SY" || cc == "CU");
}

bool is_direct_google_ip(const std::string& ip) {
    uint8_t a = 0, b = 0, c = 0, d = 0;
    if (!quickdns::AiChecker::parse_ipv4_octets(ip, a, b, c, d)) return false;
    (void)c;
    (void)d;
    if (a == 142 && (b == 250 || b == 251)) return true; // 142.250.0.0/15
    if (a == 172 && (b == 217 || b == 253)) return true; // 172.217.0.0/16, 172.253.0.0/16
    if (a == 173 && b == 194) return true;               // 173.194.0.0/16
    if (a == 192 && (b == 178 || b == 179)) return true; // 192.178.0.0/15
    if (a == 216 && (b == 58 || b == 239)) return true;  // 216.58.0.0/16, 216.239.0.0/16
    if (a == 74 && b == 125) return true;                // 74.125.0.0/16
    if (a == 64 && b == 233) return true;                // 64.233.0.0/16
    if (a == 108 && b == 177) return true;               // 108.177.0.0/16
    if (a == 209 && b == 85) return true;                // 209.85.0.0/16
    return false;
}

bool is_direct_microsoft_akamai_ip(const std::string& ip) {
    uint8_t a = 0, b = 0, c = 0, d = 0;
    if (!quickdns::AiChecker::parse_ipv4_octets(ip, a, b, c, d)) return false;
    (void)d;

    // Microsoft Edge / Azure Front Door / Bing Front End
    if (a == 13 && b == 107) return true;                // 13.107.0.0/16
    if (a == 150 && b == 171) return true;               // 150.171.0.0/16
    if (a == 204 && b == 79 && c == 197) return true;    // 204.79.197.0/24

    // Akamai Edge ranges serving copilot.microsoft.com (avoiding broad /8s)
    if (a == 23 && (b <= 15 ||                           // 23.0.0.0/12
                    (b >= 32 && b <= 63) ||              // 23.32.0.0/11
                    (b >= 64 && b <= 67) ||              // 23.64.0.0/14
                    (b >= 72 && b <= 79) ||              // 23.72.0.0/13
                    (b >= 192 && b <= 223))) {           // 23.192.0.0/11
        return true;
    }
    if (a == 104 && (b >= 64 && b <= 127)) return true;  // 104.64.0.0/10
    if (a == 96 && (b == 6 || b == 7)) return true;      // 96.6.0.0/15
    if (a == 96 && (b == 16 || b == 17)) return true;    // 96.16.0.0/15
    if (a == 173 && (b == 222 || b == 223)) return true; // 173.222.0.0/15
    if (a == 72 && (b == 246 || b == 247)) return true;  // 72.246.0.0/15
    if (a == 2 && (b >= 16 && b <= 23)) return true;     // 2.16.0.0/13
    if (a == 184 && ((b >= 24 && b <= 31) ||
                     (b == 50 || b == 51) ||
                     (b >= 84 && b <= 87))) {            // 184.24/13, 184.50/15, 184.84/14
        return true;
    }
    if (a == 95 && (b == 100 || b == 101)) return true;  // 95.100.0.0/15
    if (a == 92 && (b == 122 || b == 123)) return true;  // 92.122.0.0/15
    if (a == 88 && b == 221) return true;                // 88.221.0.0/16
    return false;
}

struct TraceParseResult {
    std::string ip;
    std::string loc;
    bool has_loc = false;
};

TraceParseResult parse_cdn_cgi_trace(const std::string& body) {
    TraceParseResult res;
    std::istringstream iss(body);
    std::string line;
    while (std::getline(iss, line)) {
        line = trim_ascii(line);
        if (line.rfind("ip=", 0) == 0) {
            res.ip = trim_ascii(line.substr(3));
        } else if (line.rfind("loc=", 0) == 0) {
            res.loc = to_upper_ascii(trim_ascii(line.substr(4)));
            if (!res.loc.empty() && res.loc != "XX") {
                res.has_loc = true;
            }
        }
    }
    return res;
}

bool is_valid_ip_literal(const std::string& ip) {
    if (ip.empty()) return false;
    quickdns::ensure_winsock();
    in_addr v4{};
    if (inet_pton(AF_INET, ip.c_str(), &v4) == 1) return true;
    in6_addr v6{};
    if (inet_pton(AF_INET6, ip.c_str(), &v6) == 1) return true;
    return false;
}

struct HttpsProbeResult {
    bool tcp_connected = false;
    bool tls_ok = false;
    int http_status = 0;
    int latency_ms = -1;
    std::string headers;
    std::string location;
    std::string body;
    std::wstring error_detail;
};

struct HttpStreamInspection {
    bool complete_headers = false;
    int http_status = 0;
    int64_t content_length = -1;
    size_t header_start = 0;
    size_t header_end = 0;
    size_t body_start = 0;
    size_t received_body_len = 0;
    std::string location;
};

int parse_status_code_from_header_block(const std::string& header_block) {
    size_t line_end = header_block.find("\r\n");
    std::string status_line = (line_end != std::string::npos)
        ? header_block.substr(0, line_end)
        : header_block;
    if (status_line.rfind("HTTP/", 0) != 0) {
        return 0;
    }
    size_t sp1 = status_line.find(' ');
    if (sp1 == std::string::npos) {
        return 0;
    }
    while (sp1 < status_line.size() && status_line[sp1] == ' ') {
        ++sp1;
    }
    if (sp1 + 3 > status_line.size()) {
        return 0;
    }
    int code = 0;
    for (size_t i = 0; i < 3; ++i) {
        char c = status_line[sp1 + i];
        if (c < '0' || c > '9') {
            return 0;
        }
        code = code * 10 + (c - '0');
    }
    if (sp1 + 3 < status_line.size()) {
        char next_ch = status_line[sp1 + 3];
        if (next_ch != ' ' && next_ch != '\r' && next_ch != '\t') {
            return 0;
        }
    }
    return code;
}

int64_t parse_strict_content_length(const std::string& val) {
    std::string s = trim_ascii(val);
    if (s.empty() || s.size() > 16) {
        return -1;
    }
    int64_t result = 0;
    for (char c : s) {
        if (c < '0' || c > '9') {
            return -1;
        }
        result = result * 10 + (c - '0');
    }
    return result;
}

HttpStreamInspection inspect_http_stream(const std::string& raw) {
    HttpStreamInspection info;
    size_t pos = 0;
    while (pos < raw.size()) {
        size_t hdr_end = raw.find("\r\n\r\n", pos);
        if (hdr_end == std::string::npos) {
            return info; // Complete "\r\n\r\n" header terminator has not arrived yet
        }

        std::string block = raw.substr(pos, hdr_end - pos);
        int code = parse_status_code_from_header_block(block);

        // Skip 1xx interim header blocks (e.g. 100 Continue, 102 Processing, 103 Early Hints), except 101
        if (code >= 100 && code < 200 && code != 101) {
            pos = hdr_end + 4;
            continue;
        }

        info.complete_headers = true;
        info.http_status = code;
        info.header_start = pos;
        info.header_end = hdr_end;
        info.body_start = hdr_end + 4;
        info.received_body_len = raw.size() - info.body_start;

        std::istringstream iss(block);
        std::string line;
        std::getline(iss, line); // Skip status line
        while (std::getline(iss, line)) {
            std::string trimmed = trim_ascii(line);
            std::string lower = to_lower_ascii(trimmed);
            if (lower.rfind("location:", 0) == 0) {
                info.location = trim_ascii(trimmed.substr(9));
            } else if (lower.rfind("content-length:", 0) == 0) {
                info.content_length = parse_strict_content_length(trimmed.substr(15));
            }
        }
        return info;
    }
    return info;
}

void parse_http_response(const std::string& raw, HttpsProbeResult& out) {
    HttpStreamInspection info = inspect_http_stream(raw);
    if (!info.complete_headers) {
        out.http_status = 0;
        out.headers.clear();
        out.location.clear();
        out.body.clear();
        return;
    }

    out.http_status = info.http_status;
    out.headers = raw.substr(info.header_start, info.header_end - info.header_start);
    out.location = info.location;
    out.body = raw.substr(info.body_start);
}

int remaining_ms(std::chrono::steady_clock::time_point deadline) noexcept {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count();
    if (ms > 600000) return 600000;
    return static_cast<int>(ms);
}

bool wait_socket(
    SOCKET sock,
    bool for_write,
    int wait_ms,
    const std::atomic<bool>* cancel_flag = nullptr)
{
    if (wait_ms <= 0) return false;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);

    while (true) {
        if (is_cancelled(cancel_flag)) {
            return false;
        }
        int rem = remaining_ms(deadline);
        if (rem <= 0) {
            return false;
        }
        int slice_ms = cancel_flag ? std::min(rem, kCancelPollSliceMs) : rem;

        fd_set fds;
        FD_ZERO(&fds);
        FD_SET(sock, &fds);
        fd_set err_fds;
        FD_ZERO(&err_fds);
        FD_SET(sock, &err_fds);

        timeval tv{};
        tv.tv_sec = slice_ms / 1000;
        tv.tv_usec = (slice_ms % 1000) * 1000;

        int rc = select(0, for_write ? nullptr : &fds, for_write ? &fds : nullptr, &err_fds, &tv);
        if (rc == SOCKET_ERROR) {
            return false;
        }
        if (rc == 0) {
            continue;
        }
        if (FD_ISSET(sock, &err_fds)) {
            return false;
        }
        if (FD_ISSET(sock, &fds)) {
            return true;
        }
    }
}

enum class ConnectWaitResult {
    Connected,
    Error,
    Timeout,
    Cancelled
};

ConnectWaitResult wait_tcp_connect(
    SOCKET sock,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel_flag,
    int& out_so_error)
{
    out_so_error = 0;
    while (true) {
        if (is_cancelled(cancel_flag)) {
            return ConnectWaitResult::Cancelled;
        }
        int rem = remaining_ms(deadline);
        if (rem <= 0) {
            return ConnectWaitResult::Timeout;
        }
        int slice_ms = cancel_flag ? std::min(rem, kCancelPollSliceMs) : rem;

        fd_set write_fds;
        FD_ZERO(&write_fds);
        FD_SET(sock, &write_fds);
        fd_set err_fds;
        FD_ZERO(&err_fds);
        FD_SET(sock, &err_fds);

        timeval tv{};
        tv.tv_sec = slice_ms / 1000;
        tv.tv_usec = (slice_ms % 1000) * 1000;

        int rc = select(0, nullptr, &write_fds, &err_fds, &tv);
        if (rc == SOCKET_ERROR) {
            out_so_error = WSAGetLastError();
            return ConnectWaitResult::Error;
        }
        if (rc == 0) {
            continue;
        }

        if (FD_ISSET(sock, &write_fds) || FD_ISSET(sock, &err_fds)) {
            int so_err = 0;
            int so_len = sizeof(so_err);
            if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &so_len) != 0) {
                out_so_error = WSAGetLastError();
                return ConnectWaitResult::Error;
            }
            if (so_err != 0) {
                out_so_error = so_err;
                return ConnectWaitResult::Error;
            }
            if (FD_ISSET(sock, &write_fds)) {
                return ConnectWaitResult::Connected;
            }
            return ConnectWaitResult::Error;
        }
    }
}

bool socket_send_all(
    SOCKET sock,
    const char* data,
    int len,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel_flag = nullptr)
{
    int total_sent = 0;
    while (total_sent < len) {
        if (is_cancelled(cancel_flag)) {
            return false;
        }
        int rem = remaining_ms(deadline);
        if (rem <= 0) {
            return false;
        }
        int sent = send(sock, data + total_sent, len - total_sent, 0);
        if (sent > 0) {
            total_sent += sent;
            continue;
        }
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            if (!wait_socket(sock, true, rem, cancel_flag)) {
                return false;
            }
            continue;
        }
        return false;
    }
    return true;
}

// Returns:
//  > 0 : number of bytes read
//    0 : graceful EOF
//   -1 : timeout
//   -2 : connection reset / aborted (e.g. DPI TCP RST)
//   -3 : cancelled
//   -4 : other socket error
int socket_recv_some(
    SOCKET sock,
    char* buf,
    int max_len,
    int wait_ms,
    const std::atomic<bool>* cancel_flag = nullptr)
{
    if (is_cancelled(cancel_flag)) return -3;
    if (wait_ms <= 0 || max_len <= 0) return -1;

    auto classify_recv_err = [](int wsa_err) -> int {
        if (wsa_err == WSAECONNRESET || wsa_err == WSAECONNABORTED || wsa_err == WSAENETRESET) {
            return -2;
        }
        return -4;
    };

    int got = recv(sock, buf, max_len, 0);
    if (got > 0) return got;
    if (got == 0) return 0;

    int err = WSAGetLastError();
    if (err != WSAEWOULDBLOCK) {
        return classify_recv_err(err);
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(wait_ms);
    while (true) {
        if (is_cancelled(cancel_flag)) {
            return -3;
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
            return classify_recv_err(WSAGetLastError());
        }
        if (rc == 0) {
            continue;
        }

        got = recv(sock, buf, max_len, 0);
        if (got > 0) return got;
        if (got == 0) return 0;

        err = WSAGetLastError();
        if (err == WSAEWOULDBLOCK) {
            if (FD_ISSET(sock, &err_fds)) {
                int so_err = 0;
                int so_len = sizeof(so_err);
                if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &so_len) == 0 && so_err != 0) {
                    return classify_recv_err(so_err);
                }
            }
            continue;
        }
        return classify_recv_err(err);
    }
}

std::wstring classify_recv_failure_detail(int recv_rc) {
    if (recv_rc == 0 || recv_rc == -2) {
        return L"Сброс соединения TLS";
    }
    if (recv_rc == -3) {
        return L"Отменено";
    }
    if (recv_rc == -1) {
        return L"Тайм-аут TLS";
    }
    return L"Ошибка соединения TLS";
}

std::wstring classify_tls_handshake_error(SECURITY_STATUS ss) {
    if (ss == SEC_E_UNTRUSTED_ROOT ||
        ss == SEC_E_CERT_EXPIRED ||
        ss == SEC_E_WRONG_PRINCIPAL ||
        ss == SEC_E_CERT_UNKNOWN ||
        ss == CRYPT_E_REVOKED ||
        ss == CRYPT_E_NO_REVOCATION_CHECK ||
        ss == CRYPT_E_REVOCATION_OFFLINE ||
        ss == TRUST_E_CERT_SIGNATURE ||
        ss == CERT_E_EXPIRED ||
        ss == CERT_E_UNTRUSTEDROOT ||
        ss == CERT_E_CN_NO_MATCH ||
        ss == CERT_E_REVOKED) {
        return L"Ошибка TLS сертификата";
    }
    if (ss == SEC_E_ILLEGAL_MESSAGE ||
        ss == SEC_E_DECRYPT_FAILURE ||
        ss == SEC_E_MESSAGE_ALTERED) {
        return L"Сбой рукопожатия TLS";
    }
    if (ss == SEC_E_ALGORITHM_MISMATCH ||
        ss == SEC_E_UNSUPPORTED_FUNCTION) {
        return L"Несовместимая версия TLS";
    }
    return L"Ошибка рукопожатия TLS";
}

bool has_unsafe_header_chars(const std::string& s) {
    for (char c : s) {
        if (c == '\r' || c == '\n' || c == '\0') {
            return true;
        }
    }
    return false;
}

HttpsProbeResult https_probe_ip(
    const std::string& resolved_ip,
    const std::string& host_sni,
    const std::string& path,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag = nullptr)
{
    HttpsProbeResult result;
    if (is_cancelled(cancel_flag)) {
        result.error_detail = L"Отменено";
        return result;
    }
    if (resolved_ip.empty() || host_sni.empty() || path.empty() || path[0] != '/' ||
        has_unsafe_header_chars(host_sni) || has_unsafe_header_chars(path)) {
        result.error_detail = L"Неверный адрес";
        return result;
    }
    if (timeout_ms <= 0) {
        result.error_detail = L"Тайм-аут TCP";
        return result;
    }

    uint8_t oa = 0, ob = 0, oc = 0, od = 0;
    if (!quickdns::AiChecker::parse_ipv4_octets(resolved_ip, oa, ob, oc, od)) {
        result.error_detail = L"Неверный IP";
        return result;
    }

    quickdns::ensure_winsock();

    sockaddr_in sa4{};
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons(443);
    if (inet_pton(AF_INET, resolved_ip.c_str(), &sa4.sin_addr) != 1) {
        result.error_detail = L"Неверный IP";
        return result;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        result.error_detail = L"Ошибка сокета";
        return result;
    }

    u_long non_blocking = 1;
    ioctlsocket(sock, FIONBIO, &non_blocking);

    auto start_time = std::chrono::steady_clock::now();
    auto deadline = start_time + std::chrono::milliseconds(timeout_ms);

    int rc = connect(sock, reinterpret_cast<const sockaddr*>(&sa4), sizeof(sa4));
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            closesocket(sock);
            result.error_detail = (err == WSAECONNREFUSED) ? L"Отказ соединения TCP" : L"Нет соединения TCP";
            return result;
        }
        int so_err = 0;
        ConnectWaitResult cw = wait_tcp_connect(sock, deadline, cancel_flag, so_err);
        if (cw != ConnectWaitResult::Connected) {
            closesocket(sock);
            if (cw == ConnectWaitResult::Cancelled) {
                result.error_detail = L"Отменено";
            } else if (cw == ConnectWaitResult::Timeout) {
                result.error_detail = L"Тайм-аут TCP";
            } else if (so_err == WSAECONNRESET) {
                result.error_detail = L"Сброс соединения TCP";
            } else {
                result.error_detail = L"Отказ соединения TCP";
            }
            return result;
        }
    }
    result.tcp_connected = true;

    SCHANNEL_CRED cred{};
    cred.dwVersion = SCHANNEL_CRED_VERSION;
    cred.dwFlags = SCH_CRED_NO_DEFAULT_CREDS | SCH_CRED_AUTO_CRED_VALIDATION | SCH_USE_STRONG_CRYPTO;

    CredHandle hCred{};
    TimeStamp tsExpiry{};
    SECURITY_STATUS ss = AcquireCredentialsHandleW(
        nullptr,
        const_cast<LPWSTR>(UNISP_NAME_W),
        SECPKG_CRED_OUTBOUND,
        nullptr,
        &cred,
        nullptr,
        nullptr,
        &hCred,
        &tsExpiry
    );

    if (ss != SEC_E_OK) {
        closesocket(sock);
        result.error_detail = L"Ошибка инициализации TLS";
        return result;
    }

    CtxtHandle hCtx{};
    bool has_ctx = false;
    std::wstring w_host = utf8_to_wide(host_sni);

    DWORD isc_flags = ISC_REQ_SEQUENCE_DETECT |
                      ISC_REQ_REPLAY_DETECT |
                      ISC_REQ_CONFIDENTIALITY |
                      ISC_RET_EXTENDED_ERROR |
                      ISC_REQ_ALLOCATE_MEMORY |
                      ISC_REQ_STREAM;

    std::vector<char> in_buf(kInitialTlsBufferSize);
    size_t in_used = 0;
    bool need_more_network_data = false;

    while (true) {
        if (is_cancelled(cancel_flag)) {
            result.error_detail = L"Отменено";
            break;
        }
        if (remaining_ms(deadline) <= 0) {
            result.error_detail = L"Тайм-аут TLS";
            break;
        }

        if (has_ctx && (in_used == 0 || need_more_network_data)) {
            if (in_used >= in_buf.size()) {
                if (in_buf.size() >= kMaxTlsBufferSize) {
                    result.error_detail = L"Переполнение буфера TLS";
                    break;
                }
                in_buf.resize(std::min<size_t>(in_buf.size() * 2, kMaxTlsBufferSize));
            }
            int got = socket_recv_some(
                sock,
                in_buf.data() + in_used,
                static_cast<int>(in_buf.size() - in_used),
                remaining_ms(deadline),
                cancel_flag
            );
            if (got <= 0) {
                result.error_detail = classify_recv_failure_detail(got);
                break;
            }
            in_used += static_cast<size_t>(got);
            need_more_network_data = false;
        }

        SecBuffer in_sec[2]{};
        in_sec[0].pvBuffer = in_buf.data();
        in_sec[0].cbBuffer = static_cast<unsigned long>(in_used);
        in_sec[0].BufferType = SECBUFFER_TOKEN;
        in_sec[1].pvBuffer = nullptr;
        in_sec[1].cbBuffer = 0;
        in_sec[1].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc in_desc{};
        in_desc.ulVersion = SECBUFFER_VERSION;
        in_desc.cBuffers = 2;
        in_desc.pBuffers = in_sec;

        SecBuffer out_sec[1]{};
        out_sec[0].pvBuffer = nullptr;
        out_sec[0].cbBuffer = 0;
        out_sec[0].BufferType = SECBUFFER_TOKEN;

        SecBufferDesc out_desc{};
        out_desc.ulVersion = SECBUFFER_VERSION;
        out_desc.cBuffers = 1;
        out_desc.pBuffers = out_sec;

        DWORD out_attrs = 0;
        ss = InitializeSecurityContextW(
            &hCred,
            has_ctx ? &hCtx : nullptr,
            const_cast<SEC_WCHAR*>(w_host.c_str()),
            isc_flags,
            0,
            SECURITY_NATIVE_DREP,
            has_ctx ? &in_desc : nullptr,
            0,
            &hCtx,
            &out_desc,
            &out_attrs,
            &tsExpiry
        );

        if (!has_ctx && (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED)) {
            has_ctx = true;
        }

        bool send_failed = false;
        if (out_sec[0].pvBuffer != nullptr) {
            if (out_sec[0].cbBuffer > 0 &&
                (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED ||
                 (FAILED(ss) && (out_attrs & ISC_RET_EXTENDED_ERROR)))) {
                if (!socket_send_all(
                        sock,
                        static_cast<const char*>(out_sec[0].pvBuffer),
                        static_cast<int>(out_sec[0].cbBuffer),
                        deadline,
                        cancel_flag)) {
                    send_failed = true;
                }
            }
            FreeContextBuffer(out_sec[0].pvBuffer);
            out_sec[0].pvBuffer = nullptr;
        }

        if (send_failed) {
            result.error_detail = is_cancelled(cancel_flag) ? L"Отменено" : L"Ошибка отправки TLS";
            break;
        }

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            need_more_network_data = true;
            continue;
        }

        if (has_ctx && in_used > 0) {
            if (in_sec[1].BufferType == SECBUFFER_EXTRA && in_sec[1].cbBuffer > 0 && in_sec[1].cbBuffer <= in_used) {
                size_t extra_len = in_sec[1].cbBuffer;
                const char* src = (in_sec[1].pvBuffer != nullptr)
                    ? static_cast<const char*>(in_sec[1].pvBuffer)
                    : (in_buf.data() + (in_used - extra_len));
                std::memmove(in_buf.data(), src, extra_len);
                in_used = extra_len;
            } else {
                in_used = 0;
            }
        }

        if (ss == SEC_E_OK) {
            result.tls_ok = true;
            break;
        }

        if (ss == SEC_I_CONTINUE_NEEDED) {
            continue;
        }

        result.error_detail = classify_tls_handshake_error(ss);
        break;
    }

    if (!result.tls_ok) {
        if (has_ctx) DeleteSecurityContext(&hCtx);
        FreeCredentialsHandle(&hCred);
        closesocket(sock);
        return result;
    }

    SecPkgContext_StreamSizes sizes{};
    if (QueryContextAttributesW(&hCtx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK || sizes.cbMaximumMessage == 0) {
        DeleteSecurityContext(&hCtx);
        FreeCredentialsHandle(&hCred);
        closesocket(sock);
        result.tls_ok = false;
        result.error_detail = L"Ошибка параметров TLS";
        return result;
    }

    std::string http_req =
        "GET " + path + " HTTP/1.1\r\n"
        "Host: " + host_sni + "\r\n"
        "User-Agent: Mozilla/5.0 (Windows NT 10.0; Win64; x64) AppleWebKit/537.36 (KHTML, like Gecko) Chrome/131.0.0.0 Safari/537.36\r\n"
        "Accept: text/html,application/xhtml+xml,*/*\r\n"
        "Accept-Language: en-US,en;q=0.9\r\n"
        "Connection: close\r\n\r\n";

    size_t req_offset = 0;
    while (req_offset < http_req.size()) {
        size_t chunk_len = std::min(static_cast<size_t>(sizes.cbMaximumMessage), http_req.size() - req_offset);
        std::vector<char> io_buf(sizes.cbHeader + chunk_len + sizes.cbTrailer);
        std::memcpy(io_buf.data() + sizes.cbHeader, http_req.data() + req_offset, chunk_len);

        SecBuffer bufs[4]{};
        bufs[0].pvBuffer = io_buf.data();
        bufs[0].cbBuffer = sizes.cbHeader;
        bufs[0].BufferType = SECBUFFER_STREAM_HEADER;

        bufs[1].pvBuffer = io_buf.data() + sizes.cbHeader;
        bufs[1].cbBuffer = static_cast<unsigned long>(chunk_len);
        bufs[1].BufferType = SECBUFFER_DATA;

        bufs[2].pvBuffer = io_buf.data() + sizes.cbHeader + chunk_len;
        bufs[2].cbBuffer = sizes.cbTrailer;
        bufs[2].BufferType = SECBUFFER_STREAM_TRAILER;

        bufs[3].pvBuffer = nullptr;
        bufs[3].cbBuffer = 0;
        bufs[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc msg_desc{};
        msg_desc.ulVersion = SECBUFFER_VERSION;
        msg_desc.cBuffers = 4;
        msg_desc.pBuffers = bufs;

        if (EncryptMessage(&hCtx, 0, &msg_desc, 0) != SEC_E_OK) {
            DeleteSecurityContext(&hCtx);
            FreeCredentialsHandle(&hCred);
            closesocket(sock);
            result.tls_ok = false;
            result.error_detail = L"Ошибка шифрования TLS";
            return result;
        }

        for (int bi = 0; bi < 3; ++bi) {
            if (bufs[bi].cbBuffer > 0 && bufs[bi].pvBuffer != nullptr) {
                if (!socket_send_all(
                        sock,
                        static_cast<const char*>(bufs[bi].pvBuffer),
                        static_cast<int>(bufs[bi].cbBuffer),
                        deadline,
                        cancel_flag)) {
                    DeleteSecurityContext(&hCtx);
                    FreeCredentialsHandle(&hCred);
                    closesocket(sock);
                    result.tls_ok = false;
                    result.error_detail = is_cancelled(cancel_flag) ? L"Отменено" : L"Ошибка отправки HTTP";
                    return result;
                }
            }
        }
        req_offset += chunk_len;
    }

    std::string plaintext;
    plaintext.reserve(kMaxHttpPlaintextSize);
    bool need_more_cipher = (in_used == 0);
    bool latency_recorded = false;

    while (plaintext.size() < kMaxHttpPlaintextSize) {
        if (is_cancelled(cancel_flag)) {
            if (plaintext.empty()) {
                result.tls_ok = false;
                result.error_detail = L"Отменено";
            }
            break;
        }
        int rem = remaining_ms(deadline);
        if (rem <= 0) {
            break;
        }

        if (need_more_cipher) {
            if (in_used >= in_buf.size()) {
                if (in_buf.size() >= kMaxTlsBufferSize) {
                    if (plaintext.empty()) {
                        result.error_detail = L"Переполнение буфера TLS";
                    }
                    break;
                }
                in_buf.resize(std::min<size_t>(in_buf.size() * 2, kMaxTlsBufferSize));
            }

            int read_wait = rem;
            HttpStreamInspection pre_check = inspect_http_stream(plaintext);
            if (pre_check.complete_headers && plaintext.size() >= pre_check.body_start) {
                read_wait = std::min(rem, 200);
            }

            int got = socket_recv_some(
                sock,
                in_buf.data() + in_used,
                static_cast<int>(in_buf.size() - in_used),
                read_wait,
                cancel_flag
            );
            if (got <= 0) {
                if (plaintext.empty() && got != 0) {
                    result.error_detail = classify_recv_failure_detail(got);
                }
                break;
            }
            in_used += static_cast<size_t>(got);
            need_more_cipher = false;
        }

        SecBuffer dec_bufs[4]{};
        dec_bufs[0].pvBuffer = in_buf.data();
        dec_bufs[0].cbBuffer = static_cast<unsigned long>(in_used);
        dec_bufs[0].BufferType = SECBUFFER_DATA;
        dec_bufs[1].BufferType = SECBUFFER_EMPTY;
        dec_bufs[2].BufferType = SECBUFFER_EMPTY;
        dec_bufs[3].BufferType = SECBUFFER_EMPTY;

        SecBufferDesc dec_desc{};
        dec_desc.ulVersion = SECBUFFER_VERSION;
        dec_desc.cBuffers = 4;
        dec_desc.pBuffers = dec_bufs;

        ss = DecryptMessage(&hCtx, &dec_desc, 0, nullptr);
        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            need_more_cipher = true;
            continue;
        }

        if (ss == SEC_I_CONTEXT_EXPIRED) {
            break;
        }

        if (ss != SEC_E_OK && ss != SEC_I_RENEGOTIATE) {
            if (plaintext.empty()) {
                result.error_detail = L"Ошибка расшифровки TLS";
            }
            break;
        }

        SecBuffer* extra_buf = nullptr;
        for (int i = 0; i < 4; ++i) {
            if (dec_bufs[i].BufferType == SECBUFFER_DATA &&
                dec_bufs[i].cbBuffer > 0 &&
                dec_bufs[i].pvBuffer != nullptr) {
                if (!latency_recorded) {
                    auto t_now = std::chrono::steady_clock::now();
                    int lat = static_cast<int>(
                        std::chrono::duration_cast<std::chrono::milliseconds>(t_now - start_time).count());
                    result.latency_ms = std::max(1, lat);
                    latency_recorded = true;
                }
                size_t to_append = dec_bufs[i].cbBuffer;
                if (plaintext.size() + to_append > kMaxHttpPlaintextSize) {
                    to_append = kMaxHttpPlaintextSize - plaintext.size();
                }
                plaintext.append(static_cast<const char*>(dec_bufs[i].pvBuffer), to_append);
            } else if (dec_bufs[i].BufferType == SECBUFFER_EXTRA && dec_bufs[i].cbBuffer > 0) {
                extra_buf = &dec_bufs[i];
            }
        }

        if (extra_buf != nullptr && extra_buf->cbBuffer <= in_used) {
            size_t extra_len = extra_buf->cbBuffer;
            const char* src = (extra_buf->pvBuffer != nullptr)
                ? static_cast<const char*>(extra_buf->pvBuffer)
                : (in_buf.data() + (in_used - extra_len));
            std::memmove(in_buf.data(), src, extra_len);
            in_used = extra_len;
            need_more_cipher = false;
        } else {
            in_used = 0;
            need_more_cipher = true;
        }

        // Handle TLS 1.3 post-handshake messages (NewSessionTicket) and TLS 1.2 renegotiation
        if (ss == SEC_I_RENEGOTIATE) {
            bool reneg_ok = false;
            bool need_reneg_recv = false;

            while (true) {
                if (is_cancelled(cancel_flag)) {
                    result.error_detail = L"Отменено";
                    break;
                }
                int rrem = remaining_ms(deadline);
                if (rrem <= 0) {
                    if (plaintext.empty()) {
                        result.error_detail = L"Тайм-аут TLS";
                    }
                    break;
                }

                if (need_reneg_recv) {
                    if (in_used >= in_buf.size()) {
                        if (in_buf.size() >= kMaxTlsBufferSize) {
                            if (plaintext.empty()) {
                                result.error_detail = L"Переполнение буфера TLS";
                            }
                            break;
                        }
                        in_buf.resize(std::min<size_t>(in_buf.size() * 2, kMaxTlsBufferSize));
                    }
                    int rgot = socket_recv_some(
                        sock,
                        in_buf.data() + in_used,
                        static_cast<int>(in_buf.size() - in_used),
                        rrem,
                        cancel_flag
                    );
                    if (rgot <= 0) {
                        if (plaintext.empty()) {
                            result.error_detail = classify_recv_failure_detail(rgot);
                        }
                        break;
                    }
                    in_used += static_cast<size_t>(rgot);
                    need_reneg_recv = false;
                }

                SecBuffer rin_sec[2]{};
                rin_sec[0].pvBuffer = in_buf.data();
                rin_sec[0].cbBuffer = static_cast<unsigned long>(in_used);
                rin_sec[0].BufferType = SECBUFFER_TOKEN;
                rin_sec[1].pvBuffer = nullptr;
                rin_sec[1].cbBuffer = 0;
                rin_sec[1].BufferType = SECBUFFER_EMPTY;

                SecBufferDesc rin_desc{};
                rin_desc.ulVersion = SECBUFFER_VERSION;
                rin_desc.cBuffers = 2;
                rin_desc.pBuffers = rin_sec;

                SecBuffer rout_sec[1]{};
                rout_sec[0].pvBuffer = nullptr;
                rout_sec[0].cbBuffer = 0;
                rout_sec[0].BufferType = SECBUFFER_TOKEN;

                SecBufferDesc rout_desc{};
                rout_desc.ulVersion = SECBUFFER_VERSION;
                rout_desc.cBuffers = 1;
                rout_desc.pBuffers = rout_sec;

                DWORD rout_attrs = 0;
                SECURITY_STATUS rss = InitializeSecurityContextW(
                    &hCred,
                    &hCtx,
                    const_cast<SEC_WCHAR*>(w_host.c_str()),
                    isc_flags,
                    0,
                    SECURITY_NATIVE_DREP,
                    &rin_desc,
                    0,
                    &hCtx,
                    &rout_desc,
                    &rout_attrs,
                    &tsExpiry
                );

                bool rsend_failed = false;
                if (rout_sec[0].pvBuffer != nullptr) {
                    if (rout_sec[0].cbBuffer > 0 &&
                        (rss == SEC_E_OK || rss == SEC_I_CONTINUE_NEEDED ||
                         (FAILED(rss) && (rout_attrs & ISC_RET_EXTENDED_ERROR)))) {
                        if (!socket_send_all(
                                sock,
                                static_cast<const char*>(rout_sec[0].pvBuffer),
                                static_cast<int>(rout_sec[0].cbBuffer),
                                deadline,
                                cancel_flag)) {
                            rsend_failed = true;
                        }
                    }
                    FreeContextBuffer(rout_sec[0].pvBuffer);
                    rout_sec[0].pvBuffer = nullptr;
                }

                if (rsend_failed) {
                    if (plaintext.empty()) {
                        result.error_detail = is_cancelled(cancel_flag) ? L"Отменено" : L"Ошибка отправки TLS";
                    }
                    break;
                }

                if (rss == SEC_E_INCOMPLETE_MESSAGE) {
                    need_reneg_recv = true;
                    continue;
                }

                if (in_used > 0) {
                    if (rin_sec[1].BufferType == SECBUFFER_EXTRA &&
                        rin_sec[1].cbBuffer > 0 &&
                        rin_sec[1].cbBuffer <= in_used) {
                        size_t rextra = rin_sec[1].cbBuffer;
                        const char* rsrc = (rin_sec[1].pvBuffer != nullptr)
                            ? static_cast<const char*>(rin_sec[1].pvBuffer)
                            : (in_buf.data() + (in_used - rextra));
                        std::memmove(in_buf.data(), rsrc, rextra);
                        in_used = rextra;
                    } else {
                        in_used = 0;
                    }
                }

                if (rss == SEC_E_OK) {
                    reneg_ok = true;
                    need_more_cipher = (in_used == 0);
                    break;
                }

                if (rss == SEC_I_CONTINUE_NEEDED) {
                    need_reneg_recv = (in_used == 0);
                    continue;
                }

                if (plaintext.empty()) {
                    result.error_detail = classify_tls_handshake_error(rss);
                }
                break;
            }

            if (!reneg_ok) {
                break;
            }
        }

        // Fast-exit checks once final HTTP headers and needed body are present
        HttpStreamInspection post_check = inspect_http_stream(plaintext);
        if (post_check.complete_headers) {
            if (path == "/cdn-cgi/trace") {
                if (post_check.http_status != 200) {
                    break;
                }
                size_t loc_pos = plaintext.find("loc=", post_check.body_start);
                if (loc_pos != std::string::npos && plaintext.find('\n', loc_pos) != std::string::npos) {
                    break;
                }
            }
            if (post_check.http_status >= 300 && post_check.http_status < 400 && !post_check.location.empty()) {
                break;
            }
            if (post_check.content_length >= 0 &&
                static_cast<int64_t>(post_check.received_body_len) >= post_check.content_length) {
                break;
            }
        }
    }

    DeleteSecurityContext(&hCtx);
    FreeCredentialsHandle(&hCred);
    closesocket(sock);

    if (!plaintext.empty()) {
        parse_http_response(plaintext, result);
        if (result.http_status == 0 && result.error_detail.empty()) {
            result.error_detail = L"Неполный ответ HTTP";
        }
    } else if (result.error_detail.empty()) {
        result.error_detail = L"Пустой ответ HTTP";
    }

    return result;
}

std::string winhttp_fetch_text(
    const wchar_t* host,
    const wchar_t* path,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag = nullptr)
{
    if (timeout_ms <= 0 || is_cancelled(cancel_flag)) {
        return "";
    }

    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) QuickDNS/2.3.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) return "";

    DWORD to = static_cast<DWORD>(std::max(100, timeout_ms));
    WinHttpSetTimeouts(hSession, to, to, to, to);

    HINTERNET hConnect = WinHttpConnect(hSession, host, INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect,
        L"GET",
        path,
        nullptr,
        WINHTTP_NO_REFERER,
        WINHTTP_DEFAULT_ACCEPT_TYPES,
        WINHTTP_FLAG_SECURE
    );
    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD disableRedirects = WINHTTP_DISABLE_REDIRECTS;
    WinHttpSetOption(hRequest, WINHTTP_OPTION_DISABLE_FEATURE, &disableRedirects, sizeof(disableRedirects));

    std::string body;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0;
        DWORD statusSize = sizeof(statusCode);
        if (WinHttpQueryHeaders(
                hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusSize,
                WINHTTP_NO_HEADER_INDEX) &&
            statusCode == 200) {
            DWORD bytes_read = 0;
            char buf[4096];
            while (!is_cancelled(cancel_flag) &&
                   WinHttpReadData(hRequest, buf, sizeof(buf), &bytes_read) &&
                   bytes_read > 0) {
                body.append(buf, bytes_read);
                if (body.size() >= 8192) break;
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return body;
}

std::vector<uint8_t> attach_edns0_opt(const std::vector<uint8_t>& base_query) {
    if (base_query.size() < 12) {
        return base_query;
    }
    std::vector<uint8_t> pkt = base_query;
    pkt.reserve(base_query.size() + 11);
    // Set ARCOUNT = 1
    pkt[10] = 0x00;
    pkt[11] = 0x01;
    // Root name (0x00), TYPE=OPT (41 = 0x0029), UDP payload size = 1232 (0x04D0),
    // EXT-RCODE=0, EDNS-VER=0, Z=0x0000, RDLENGTH=0x0000
    static const uint8_t opt_rr[11] = {
        0x00, 0x00, 0x29, 0x04, 0xD0, 0x00, 0x00, 0x00, 0x00, 0x00, 0x00
    };
    pkt.insert(pkt.end(), std::begin(opt_rr), std::end(opt_rr));
    return pkt;
}

std::vector<std::string> query_dns_over_tcp(
    const sockaddr_in& sa4,
    const std::string& domain,
    std::chrono::steady_clock::time_point deadline,
    const std::atomic<bool>* cancel_flag)
{
    std::vector<std::string> empty_res;
    if (is_cancelled(cancel_flag) || remaining_ms(deadline) <= 0) {
        return empty_res;
    }

    uint16_t txid = generate_dns_txid();
    auto query = quickdns::AiChecker::build_dns_a_query(txid, domain);
    if (query.empty() || query.size() > 65535) {
        return empty_res;
    }

    SOCKET sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (sock == INVALID_SOCKET) {
        return empty_res;
    }

    u_long nb = 1;
    ioctlsocket(sock, FIONBIO, &nb);

    int rc = connect(sock, reinterpret_cast<const sockaddr*>(&sa4), sizeof(sa4));
    if (rc == SOCKET_ERROR) {
        int err = WSAGetLastError();
        if (err != WSAEWOULDBLOCK && err != WSAEINPROGRESS) {
            closesocket(sock);
            return empty_res;
        }
        int so_err = 0;
        if (wait_tcp_connect(sock, deadline, cancel_flag, so_err) != ConnectWaitResult::Connected) {
            closesocket(sock);
            return empty_res;
        }
    }

    std::vector<char> tcp_msg(2 + query.size());
    tcp_msg[0] = static_cast<char>((query.size() >> 8) & 0xFF);
    tcp_msg[1] = static_cast<char>(query.size() & 0xFF);
    std::memcpy(tcp_msg.data() + 2, query.data(), query.size());

    if (!socket_send_all(sock, tcp_msg.data(), static_cast<int>(tcp_msg.size()), deadline, cancel_flag)) {
        closesocket(sock);
        return empty_res;
    }

    uint8_t len_hdr[2] = {};
    int hdr_read = 0;
    while (hdr_read < 2) {
        int got = socket_recv_some(
            sock,
            reinterpret_cast<char*>(len_hdr + hdr_read),
            2 - hdr_read,
            remaining_ms(deadline),
            cancel_flag
        );
        if (got <= 0) {
            closesocket(sock);
            return empty_res;
        }
        hdr_read += got;
    }

    uint16_t resp_len = static_cast<uint16_t>((static_cast<uint16_t>(len_hdr[0]) << 8) | len_hdr[1]);
    if (resp_len < 12 || resp_len > 16384) {
        closesocket(sock);
        return empty_res;
    }

    std::vector<uint8_t> resp_buf(resp_len);
    int body_read = 0;
    while (body_read < static_cast<int>(resp_len)) {
        int got = socket_recv_some(
            sock,
            reinterpret_cast<char*>(resp_buf.data() + body_read),
            static_cast<int>(resp_len) - body_read,
            remaining_ms(deadline),
            cancel_flag
        );
        if (got <= 0) {
            closesocket(sock);
            return empty_res;
        }
        body_read += got;
    }

    closesocket(sock);
    return quickdns::AiChecker::parse_dns_all_a_responses(resp_buf.data(), resp_buf.size(), txid);
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

} // anonymous namespace

namespace quickdns {

bool AiChecker::parse_ipv4_octets(const std::string& ip_str, uint8_t& a, uint8_t& b, uint8_t& c, uint8_t& d) {
    a = b = c = d = 0;
    if (ip_str.size() < 7 || ip_str.size() > 15) {
        return false;
    }

    uint8_t octets[4] = {};
    int part = 0;
    size_t part_len = 0;
    unsigned int acc = 0;

    for (size_t i = 0; i < ip_str.size(); ++i) {
        char ch = ip_str[i];
        if (ch == '.') {
            if (part_len == 0 || part >= 3) {
                return false;
            }
            octets[part++] = static_cast<uint8_t>(acc);
            part_len = 0;
            acc = 0;
        } else if (ch >= '0' && ch <= '9') {
            if (part_len == 1 && acc == 0) {
                return false; // Reject leading zeros like "01" or "00"
            }
            if (part_len >= 3) {
                return false;
            }
            acc = acc * 10u + static_cast<unsigned int>(ch - '0');
            if (acc > 255u) {
                return false;
            }
            ++part_len;
        } else {
            return false; // Reject whitespace, '+', '-', hex, etc.
        }
    }

    if (part != 3 || part_len == 0) {
        return false;
    }
    octets[3] = static_cast<uint8_t>(acc);

    a = octets[0];
    b = octets[1];
    c = octets[2];
    d = octets[3];
    return true;
}

bool AiChecker::is_valid_public_ipv4_octets(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    (void)d;
    if (a == 0) return false;                                // 0.0.0.0/8 Current network / sinkhole
    if (a == 10) return false;                               // 10.0.0.0/8 Private
    if (a == 100 && (b >= 64 && b <= 127)) return false;     // 100.64.0.0/10 Shared Address Space (CGNAT)
    if (a == 127) return false;                              // 127.0.0.0/8 Loopback
    if (a == 169 && b == 254) return false;                  // 169.254.0.0/16 Link-local
    if (a == 172 && (b >= 16 && b <= 31)) return false;      // 172.16.0.0/12 Private
    if (a == 192 && b == 0 && c == 0) return false;          // 192.0.0.0/24 IETF Protocol Assignments
    if (a == 192 && b == 0 && c == 2) return false;          // 192.0.2.0/24 TEST-NET-1
    if (a == 192 && b == 168) return false;                  // 192.168.0.0/16 Private
    if (a == 198 && (b == 18 || b == 19)) return false;      // 198.18.0.0/15 Benchmark tests
    if (a == 198 && b == 51 && c == 100) return false;       // 198.51.100.0/24 TEST-NET-2
    if (a == 203 && b == 0 && c == 113) return false;        // 203.0.113.0/24 TEST-NET-3
    if (a >= 224) return false;                              // 224.0.0.0/4 Multicast & 240.0.0.0/4 Reserved/Broadcast
    return true;
}

bool AiChecker::is_valid_public_ipv4(const std::string& ip_str) {
    uint8_t a = 0, b = 0, c = 0, d = 0;
    if (!parse_ipv4_octets(ip_str, a, b, c, d)) {
        return false;
    }
    return is_valid_public_ipv4_octets(a, b, c, d);
}

std::vector<uint8_t> AiChecker::build_dns_a_query(uint16_t txid, const std::string& domain) {
    if (domain.empty()) {
        return {};
    }

    std::string effective = domain;
    if (effective.back() == '.') {
        effective.pop_back();
        if (effective.empty() || effective.back() == '.') {
            return {};
        }
    }
    if (effective.front() == '.') {
        return {};
    }

    std::vector<uint8_t> qname;
    qname.reserve(effective.size() + 2);

    size_t start = 0;
    while (start < effective.size()) {
        size_t dot = effective.find('.', start);
        size_t end = (dot == std::string::npos) ? effective.size() : dot;
        size_t len = end - start;
        if (len == 0 || len > 63) {
            return {};
        }

        qname.push_back(static_cast<uint8_t>(len));
        for (size_t i = start; i < end; ++i) {
            unsigned char uch = static_cast<unsigned char>(effective[i]);
            if (uch <= 0x20 || uch >= 0x7F) {
                return {};
            }
            qname.push_back(static_cast<uint8_t>(ascii_lower(static_cast<char>(uch))));
        }

        if (dot == std::string::npos) {
            break;
        }
        if (dot + 1 >= effective.size()) {
            return {};
        }
        start = dot + 1;
    }

    qname.push_back(0x00); // Root label
    if (qname.size() > 255) {
        return {};
    }

    std::vector<uint8_t> packet;
    packet.reserve(12 + qname.size() + 4);

    packet.push_back(static_cast<uint8_t>(txid >> 8));
    packet.push_back(static_cast<uint8_t>(txid & 0xFF));
    packet.push_back(0x01);
    packet.push_back(0x00); // RD = 1
    packet.push_back(0x00);
    packet.push_back(0x01); // QDCOUNT = 1
    packet.push_back(0x00);
    packet.push_back(0x00); // ANCOUNT = 0
    packet.push_back(0x00);
    packet.push_back(0x00); // NSCOUNT = 0
    packet.push_back(0x00);
    packet.push_back(0x00); // ARCOUNT = 0

    packet.insert(packet.end(), qname.begin(), qname.end());

    packet.push_back(0x00);
    packet.push_back(0x01); // QTYPE = A (1)
    packet.push_back(0x00);
    packet.push_back(0x01); // QCLASS = IN (1)
    return packet;
}

std::vector<std::string> AiChecker::parse_dns_all_a_responses(
    const uint8_t* packet,
    size_t packet_len,
    uint16_t expected_txid)
{
    std::vector<std::string> valid_ips;
    if (!packet || packet_len < 12) {
        return valid_ips;
    }

    uint16_t rx_txid = static_cast<uint16_t>((static_cast<uint16_t>(packet[0]) << 8) | packet[1]);
    if (rx_txid != expected_txid) {
        return valid_ips;
    }

    uint8_t flags_hi = packet[2];
    uint8_t flags_lo = packet[3];
    bool is_response = (flags_hi & 0x80) != 0;
    uint8_t opcode = (flags_hi >> 3) & 0x0F;
    bool is_truncated = (flags_hi & 0x02) != 0;
    uint8_t rcode = flags_lo & 0x0F;
    if (!is_response || opcode != 0 || is_truncated || rcode != 0) {
        return valid_ips;
    }

    uint16_t qdcount = static_cast<uint16_t>((static_cast<uint16_t>(packet[4]) << 8) | packet[5]);
    uint16_t ancount = static_cast<uint16_t>((static_cast<uint16_t>(packet[6]) << 8) | packet[7]);
    if (qdcount == 0 || ancount == 0) {
        return valid_ips;
    }

    size_t offset = 12;
    std::string qname;
    for (uint16_t q = 0; q < qdcount; ++q) {
        std::string cur_qname;
        if (!decode_dns_name(packet, packet_len, packet_len, offset, cur_qname)) {
            return {};
        }
        if (offset + 4 > packet_len) {
            return {};
        }
        uint16_t qtype = static_cast<uint16_t>((static_cast<uint16_t>(packet[offset]) << 8) | packet[offset + 1]);
        uint16_t qclass = static_cast<uint16_t>((static_cast<uint16_t>(packet[offset + 2]) << 8) | packet[offset + 3]);
        offset += 4;
        if (q == 0) {
            if (qtype != 1 || qclass != 1 || cur_qname.empty()) {
                return {};
            }
            qname = std::move(cur_qname);
        }
    }

    struct ParsedCname {
        std::string owner;
        std::string target;
    };
    struct ParsedARecord {
        std::string owner;
        std::string ipv4;
    };

    std::vector<ParsedCname> cnames;
    std::vector<ParsedARecord> a_records;
    cnames.reserve(std::min<size_t>(ancount, 16));
    a_records.reserve(std::min<size_t>(ancount, 16));

    for (uint16_t a = 0; a < ancount; ++a) {
        std::string owner;
        if (!decode_dns_name(packet, packet_len, packet_len, offset, owner)) {
            return {};
        }
        if (offset + 10 > packet_len) {
            return {};
        }

        uint16_t rtype = static_cast<uint16_t>((static_cast<uint16_t>(packet[offset]) << 8) | packet[offset + 1]);
        uint16_t rclass = static_cast<uint16_t>((static_cast<uint16_t>(packet[offset + 2]) << 8) | packet[offset + 3]);
        uint16_t rdlength = static_cast<uint16_t>((static_cast<uint16_t>(packet[offset + 8]) << 8) | packet[offset + 9]);
        offset += 10;

        if (offset + rdlength > packet_len) {
            return {};
        }

        size_t rdata_end = offset + rdlength;
        if (rtype == 5 && rclass == 1) {
            if (rdlength == 0) {
                return {};
            }
            size_t rdata_cursor = offset;
            std::string cname_target;
            if (!decode_dns_name(packet, packet_len, rdata_end, rdata_cursor, cname_target) ||
                rdata_cursor != rdata_end) {
                return {};
            }
            if (!cname_target.empty()) {
                cnames.push_back({std::move(owner), std::move(cname_target)});
            }
        } else if (rtype == 1 && rclass == 1) {
            if (rdlength != 4) {
                return {};
            }
            uint8_t o1 = packet[offset];
            uint8_t o2 = packet[offset + 1];
            uint8_t o3 = packet[offset + 2];
            uint8_t o4 = packet[offset + 3];
            if (is_valid_public_ipv4_octets(o1, o2, o3, o4)) {
                std::string ip_str = std::to_string(o1) + "." +
                                     std::to_string(o2) + "." +
                                     std::to_string(o3) + "." +
                                     std::to_string(o4);
                a_records.push_back({std::move(owner), std::move(ip_str)});
            }
        }

        offset = rdata_end;
    }

    // Track allowed owner names: {qname} plus any CNAME chain targets reachable from qname
    std::vector<std::string> allowed_names;
    allowed_names.push_back(qname);

    auto is_allowed = [&](const std::string& name) -> bool {
        return std::find(allowed_names.begin(), allowed_names.end(), name) != allowed_names.end();
    };

    bool expanded = true;
    size_t max_passes = cnames.size();
    for (size_t pass = 0; pass < max_passes && expanded && allowed_names.size() < 32; ++pass) {
        expanded = false;
        for (const auto& cn : cnames) {
            if (is_allowed(cn.owner) && !is_allowed(cn.target)) {
                allowed_names.push_back(cn.target);
                expanded = true;
                if (allowed_names.size() >= 32) {
                    break;
                }
            }
        }
    }

    for (const auto& ar : a_records) {
        if (is_allowed(ar.owner) &&
            std::find(valid_ips.begin(), valid_ips.end(), ar.ipv4) == valid_ips.end()) {
            valid_ips.push_back(ar.ipv4);
        }
    }

    return valid_ips;
}

std::string AiChecker::parse_dns_a_response(const uint8_t* packet, size_t packet_len, uint16_t expected_txid) {
    auto all = parse_dns_all_a_responses(packet, packet_len, expected_txid);
    if (all.empty()) {
        return "";
    }
    return all.front();
}

void AiChecker::aggregate_profile_result(ProfileAiCheckResult& result) {
    const AiServiceCheckResult* services[4] = {
        &result.chatgpt,
        &result.claude,
        &result.gemini,
        &result.copilot
    };

    int avail = 0;
    int sum_avail_latency = 0;
    int count_avail_latency = 0;

    for (const auto* s : services) {
        if (s->status == AiAccessStatus::Available) {
            ++avail;
            if (s->latency_ms > 0) {
                sum_avail_latency += s->latency_ms;
                ++count_avail_latency;
            }
        }
    }

    result.available_count = avail;
    if (count_avail_latency > 0) {
        result.avg_latency_ms = sum_avail_latency / count_avail_latency;
    } else {
        result.avg_latency_ms = -1;
    }
}

ClientIpInfo AiChecker::get_client_ip_info(int timeout_ms, const std::atomic<bool>* cancel_flag) {
    ClientIpInfo info;
    if (timeout_ms <= 0 || is_cancelled(cancel_flag)) {
        return info;
    }

    int total_budget = std::max(300, timeout_ms);
    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(total_budget);

    // 1. Direct TLS probe to https://1.1.1.1/cdn-cgi/trace
    int step1_ms = std::min(remaining_ms(deadline), std::max(250, (total_budget * 2) / 5));
    if (step1_ms > 0 && !is_cancelled(cancel_flag)) {
        auto probe = https_probe_ip("1.1.1.1", "one.one.one.one", "/cdn-cgi/trace", step1_ms, cancel_flag);
        if (probe.tls_ok && probe.http_status == 200 && !probe.body.empty()) {
            auto parsed = parse_cdn_cgi_trace(probe.body);
            if (is_valid_ip_literal(parsed.ip) && parsed.has_loc) {
                info.ip = parsed.ip;
                info.country_code = parsed.loc;
                info.valid = true;
                return info;
            }
        }
    }

    // 2. Fallback via WinHTTP (honors system proxy) to https://1.1.1.1/cdn-cgi/trace
    int step2_ms = std::min(remaining_ms(deadline), std::max(200, total_budget / 3));
    if (step2_ms > 0 && !is_cancelled(cancel_flag)) {
        std::string text = winhttp_fetch_text(L"1.1.1.1", L"/cdn-cgi/trace", step2_ms, cancel_flag);
        if (!text.empty()) {
            auto parsed = parse_cdn_cgi_trace(text);
            if (is_valid_ip_literal(parsed.ip) && parsed.has_loc) {
                info.ip = parsed.ip;
                info.country_code = parsed.loc;
                info.valid = true;
                return info;
            }
        }
    }

    // 3. Fallback via WinHTTP to https://cloudflare.com/cdn-cgi/trace within remaining deadline
    int step3_ms = remaining_ms(deadline);
    if (step3_ms > 0 && !is_cancelled(cancel_flag)) {
        std::string text = winhttp_fetch_text(L"cloudflare.com", L"/cdn-cgi/trace", step3_ms, cancel_flag);
        if (!text.empty()) {
            auto parsed = parse_cdn_cgi_trace(text);
            if (is_valid_ip_literal(parsed.ip) && parsed.has_loc) {
                info.ip = parsed.ip;
                info.country_code = parsed.loc;
                info.valid = true;
                return info;
            }
        }
    }

    return info;
}

std::vector<std::string> AiChecker::resolve_domain_all_via_dns(
    const std::string& dns_server_ip,
    const std::string& domain,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag)
{
    if (dns_server_ip.empty() || domain.empty() || timeout_ms <= 0 || is_cancelled(cancel_flag)) {
        return {};
    }

    uint8_t oa = 0, ob = 0, oc = 0, od = 0;
    if (!parse_ipv4_octets(dns_server_ip, oa, ob, oc, od)) {
        return {};
    }

    // Validate domain syntax up front
    if (build_dns_a_query(1, domain).empty()) {
        return {};
    }

    ensure_winsock();

    sockaddr_in sa4{};
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons(53);
    if (inet_pton(AF_INET, dns_server_ip.c_str(), &sa4.sin_addr) != 1) {
        return {};
    }

    auto total_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(timeout_ms);

    // Up to 2 UDP attempts (Attempt 0 with EDNS0 1232B, Attempt 1 plain RFC 1035 on timeout or FORMERR)
    for (int attempt = 0; attempt < 2; ++attempt) {
        if (is_cancelled(cancel_flag)) {
            return {};
        }
        int rem_total = remaining_ms(total_deadline);
        if (rem_total <= 0) {
            break;
        }

        int attempt_budget = (attempt == 0)
            ? std::max(150, std::min(rem_total, (timeout_ms * 3) / 5))
            : rem_total;
        auto attempt_deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(attempt_budget);

        uint16_t txid = generate_dns_txid();
        auto base_query = build_dns_a_query(txid, domain);
        if (base_query.empty()) {
            return {};
        }
        std::vector<uint8_t> wire_query = (attempt == 0) ? attach_edns0_opt(base_query) : base_query;

        SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
        if (sock == INVALID_SOCKET) {
            return {};
        }

        u_long nb = 1;
        ioctlsocket(sock, FIONBIO, &nb);

        // Connect UDP socket to enforce remote IP + port 53 kernel filtering
        if (connect(sock, reinterpret_cast<const sockaddr*>(&sa4), sizeof(sa4)) == SOCKET_ERROR) {
            closesocket(sock);
            return {};
        }

        int sent = send(
            sock,
            reinterpret_cast<const char*>(wire_query.data()),
            static_cast<int>(wire_query.size()),
            0
        );
        if (sent != static_cast<int>(wire_query.size())) {
            closesocket(sock);
            continue;
        }

        bool need_tcp_fallback = false;
        bool definitive_negative = false;

        while (true) {
            if (is_cancelled(cancel_flag)) {
                closesocket(sock);
                return {};
            }
            int wait_rem = remaining_ms(attempt_deadline);
            if (wait_rem <= 0) {
                break;
            }

            uint8_t recv_buf[1500] = {};
            int received = socket_recv_some(
                sock,
                reinterpret_cast<char*>(recv_buf),
                sizeof(recv_buf),
                wait_rem,
                cancel_flag
            );

            if (received == -3) { // Cancelled
                closesocket(sock);
                return {};
            }
            if (received == -1) { // Attempt timeout
                break;
            }
            if (received <= 0) {  // ICMP port unreachable (-2) or socket error
                break;
            }
            if (received < 12) {
                continue; // Ignore undersized stray datagram
            }

            uint16_t rx_txid = static_cast<uint16_t>((static_cast<uint16_t>(recv_buf[0]) << 8) | recv_buf[1]);
            if (rx_txid != txid) {
                continue; // Ignore stray datagram with mismatched TXID
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

            if (attempt == 0 && (rcode == 1 || rcode == 4)) {
                // FORMERR / NOTIMP on EDNS0: immediately retry Attempt 1 without EDNS0
                break;
            }

            auto ips = parse_dns_all_a_responses(recv_buf, static_cast<size_t>(received), txid);
            if (!ips.empty()) {
                closesocket(sock);
                return ips;
            }

            // Valid matching response arrived with NXDOMAIN / SERVFAIL / REFUSED or sinkhole IP
            definitive_negative = true;
            break;
        }

        closesocket(sock);

        if (need_tcp_fallback) {
            auto tcp_ips = query_dns_over_tcp(sa4, domain, total_deadline, cancel_flag);
            if (!tcp_ips.empty()) {
                return tcp_ips;
            }
            return {};
        }

        if (definitive_negative) {
            return {};
        }
    }

    return {};
}

std::string AiChecker::resolve_domain_via_dns(
    const std::string& dns_server_ip,
    const std::string& domain,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag)
{
    auto ips = resolve_domain_all_via_dns(dns_server_ip, domain, timeout_ms, cancel_flag);
    if (ips.empty()) {
        return "";
    }
    return ips.front();
}

AiServiceCheckResult AiChecker::check_single_service(
    const std::string& dns_server_ip,
    AiService service,
    const std::string& client_country_code,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag)
{
    AiServiceCheckResult res;
    res.service = service;
    res.status = AiAccessStatus::Pending;
    res.latency_ms = -1;

    if (is_cancelled(cancel_flag)) {
        res.status = AiAccessStatus::Unreachable;
        res.detail = L"Отменено";
        return res;
    }

    std::string domain;
    switch (service) {
        case AiService::ChatGPT: domain = "chatgpt.com"; break;
        case AiService::Claude:  domain = "claude.ai"; break;
        case AiService::Gemini:  domain = "gemini.google.com"; break;
        case AiService::Copilot: domain = "copilot.microsoft.com"; break;
    }

    int safe_timeout = std::max(300, timeout_ms);
    int dns_timeout = std::min(1200, std::max(400, safe_timeout / 2));
    res.resolved_ip = resolve_domain_via_dns(dns_server_ip, domain, dns_timeout, cancel_flag);
    if (is_cancelled(cancel_flag)) {
        res.status = AiAccessStatus::Unreachable;
        res.detail = L"Отменено";
        return res;
    }
    if (res.resolved_ip.empty()) {
        res.status = AiAccessStatus::DnsFailed;
        res.detail = L"Нет ответа DNS";
        return res;
    }

    std::string client_cc = to_upper_ascii(trim_ascii(client_country_code));

    if (service == AiService::ChatGPT) {
        auto trace_probe = https_probe_ip(res.resolved_ip, "chatgpt.com", "/cdn-cgi/trace", safe_timeout, cancel_flag);
        if (!trace_probe.tcp_connected || !trace_probe.tls_ok) {
            res.status = AiAccessStatus::Unreachable;
            res.detail = trace_probe.error_detail.empty() ? L"Тайм-аут TLS" : trace_probe.error_detail;
            return res;
        }
        res.latency_ms = trace_probe.latency_ms;

        if (trace_probe.http_status == 200) {
            auto trace = parse_cdn_cgi_trace(trace_probe.body);
            if (trace.has_loc) {
                if (is_geo_blocked_country(trace.loc)) {
                    res.status = AiAccessStatus::GeoBlocked;
                    res.detail = L"Гео-блок (" + utf8_to_wide(trace.loc) + L")";
                } else {
                    res.status = AiAccessStatus::Available;
                    res.detail = L"Доступен (" + std::to_wstring(res.latency_ms) + L" мс, " + utf8_to_wide(trace.loc) + L")";
                }
                return res;
            }
        }

        if (is_cancelled(cancel_flag)) {
            res.status = AiAccessStatus::Unreachable;
            res.detail = L"Отменено";
            return res;
        }

        // Fallback for custom SNI proxies that do not expose /cdn-cgi/trace
        auto root_probe = https_probe_ip(res.resolved_ip, "chatgpt.com", "/", safe_timeout, cancel_flag);
        if (!root_probe.tcp_connected || !root_probe.tls_ok) {
            res.status = AiAccessStatus::Unreachable;
            res.detail = root_probe.error_detail.empty() ? L"Тайм-аут TLS" : root_probe.error_detail;
            return res;
        }
        if (root_probe.latency_ms > 0) {
            res.latency_ms = root_probe.latency_ms;
        }

        std::string body_low = to_lower_ascii(root_probe.body);
        std::string loc_low = to_lower_ascii(root_probe.location);
        if (root_probe.http_status == 403 || root_probe.http_status == 451 ||
            body_low.find("unsupported_country") != std::string::npos ||
            body_low.find("unable to load site") != std::string::npos ||
            loc_low.find("unsupported_country") != std::string::npos) {
            res.status = AiAccessStatus::GeoBlocked;
            res.detail = L"Гео-блок";
        } else if (root_probe.http_status == 200 || root_probe.http_status == 301 ||
                   root_probe.http_status == 302 || root_probe.http_status == 303 ||
                   root_probe.http_status == 307 || root_probe.http_status == 308) {
            res.status = AiAccessStatus::Available;
            res.detail = L"Доступен (" + std::to_wstring(res.latency_ms) + L" мс)";
        } else {
            res.status = AiAccessStatus::Unreachable;
            res.detail = (root_probe.http_status > 0)
                ? (L"Ошибка HTTP " + std::to_wstring(root_probe.http_status))
                : L"Недоступен";
        }
        return res;
    }

    if (service == AiService::Claude) {
        auto trace_probe = https_probe_ip(res.resolved_ip, "claude.ai", "/cdn-cgi/trace", safe_timeout, cancel_flag);
        if (!trace_probe.tcp_connected || !trace_probe.tls_ok) {
            res.status = AiAccessStatus::Unreachable;
            res.detail = trace_probe.error_detail.empty() ? L"Тайм-аут TLS" : trace_probe.error_detail;
            return res;
        }
        res.latency_ms = trace_probe.latency_ms;

        if (trace_probe.http_status == 200) {
            auto trace = parse_cdn_cgi_trace(trace_probe.body);
            if (trace.has_loc) {
                if (is_geo_blocked_country(trace.loc)) {
                    res.status = AiAccessStatus::GeoBlocked;
                    res.detail = L"Гео-блок (" + utf8_to_wide(trace.loc) + L")";
                } else {
                    res.status = AiAccessStatus::Available;
                    res.detail = L"Доступен (" + std::to_wstring(res.latency_ms) + L" мс, " + utf8_to_wide(trace.loc) + L")";
                }
                return res;
            }
        }

        if (is_cancelled(cancel_flag)) {
            res.status = AiAccessStatus::Unreachable;
            res.detail = L"Отменено";
            return res;
        }

        // Fallback: GET /login
        auto login_probe = https_probe_ip(res.resolved_ip, "claude.ai", "/login", safe_timeout, cancel_flag);
        if (!login_probe.tcp_connected || !login_probe.tls_ok) {
            res.status = AiAccessStatus::Unreachable;
            res.detail = login_probe.error_detail.empty() ? L"Тайм-аут TLS" : login_probe.error_detail;
            return res;
        }
        if (login_probe.latency_ms > 0) {
            res.latency_ms = login_probe.latency_ms;
        }

        std::string body_low = to_lower_ascii(login_probe.body);
        std::string loc_low = to_lower_ascii(login_probe.location);
        if (login_probe.http_status == 403 || login_probe.http_status == 451 ||
            loc_low.find("app-unavailable-in-region") != std::string::npos ||
            body_low.find("app-unavailable-in-region") != std::string::npos ||
            body_low.find("unavailable in your region") != std::string::npos) {
            res.status = AiAccessStatus::GeoBlocked;
            res.detail = L"Гео-блок";
        } else if (login_probe.http_status == 200 || login_probe.http_status == 301 ||
                   login_probe.http_status == 302 || login_probe.http_status == 303 ||
                   login_probe.http_status == 307 || login_probe.http_status == 308) {
            res.status = AiAccessStatus::Available;
            res.detail = L"Доступен (" + std::to_wstring(res.latency_ms) + L" мс)";
        } else {
            res.status = AiAccessStatus::Unreachable;
            res.detail = (login_probe.http_status > 0)
                ? (L"Ошибка HTTP " + std::to_wstring(login_probe.http_status))
                : L"Недоступен";
        }
        return res;
    }

    if (service == AiService::Gemini) {
        auto probe = https_probe_ip(res.resolved_ip, "gemini.google.com", "/app", safe_timeout, cancel_flag);
        if (!probe.tcp_connected || !probe.tls_ok) {
            res.status = AiAccessStatus::Unreachable;
            res.detail = probe.error_detail.empty() ? L"Тайм-аут TLS" : probe.error_detail;
            return res;
        }
        res.latency_ms = probe.latency_ms;

        std::string loc_low = to_lower_ascii(probe.location);
        std::string body_low = to_lower_ascii(probe.body);

        bool has_faq_redirect =
            (loc_low.find("/faq") != std::string::npos) ||
            (loc_low.find("support.google.com") != std::string::npos) ||
            (body_low.find("gemini.google.com/faq") != std::string::npos) ||
            (body_low.find("\"/faq") != std::string::npos) ||
            (body_low.find("'/faq") != std::string::npos) ||
            (body_low.find("isn't currently supported") != std::string::npos) ||
            (body_low.find("not currently supported") != std::string::npos);

        bool direct_google_from_blocked_region =
            is_geo_blocked_country(client_cc) && is_direct_google_ip(res.resolved_ip);

        if (probe.http_status == 403 || probe.http_status == 451 || has_faq_redirect || direct_google_from_blocked_region) {
            res.status = AiAccessStatus::GeoBlocked;
            if (direct_google_from_blocked_region && !client_cc.empty()) {
                res.detail = L"Гео-блок (" + utf8_to_wide(client_cc) + L")";
            } else {
                res.detail = L"Гео-блок";
            }
        } else if (probe.http_status == 200 || probe.http_status == 301 ||
                   probe.http_status == 302 || probe.http_status == 303 ||
                   probe.http_status == 307 || probe.http_status == 308) {
            res.status = AiAccessStatus::Available;
            res.detail = L"Доступен (" + std::to_wstring(res.latency_ms) + L" мс)";
        } else {
            res.status = AiAccessStatus::Unreachable;
            res.detail = (probe.http_status > 0)
                ? (L"Ошибка HTTP " + std::to_wstring(probe.http_status))
                : L"Недоступен";
        }
        return res;
    }

    // AiService::Copilot
    auto probe = https_probe_ip(res.resolved_ip, "copilot.microsoft.com", "/", safe_timeout, cancel_flag);
    if (!probe.tcp_connected || !probe.tls_ok) {
        res.status = AiAccessStatus::Unreachable;
        res.detail = probe.error_detail.empty() ? L"Тайм-аут TLS" : probe.error_detail;
        return res;
    }
    res.latency_ms = probe.latency_ms;

    std::string loc_low = to_lower_ascii(probe.location);
    std::string body_low = to_lower_ascii(probe.body);

    bool loc_blocked =
        (loc_low.find("unsupported") != std::string::npos) ||
        (loc_low.find("unavailable") != std::string::npos) ||
        (loc_low.find("blocked") != std::string::npos) ||
        (loc_low.find("denied") != std::string::npos) ||
        (loc_low.find("cn.bing.com") != std::string::npos);

    bool body_blocked =
        (body_low.find("not available in your region") != std::string::npos) ||
        (body_low.find("unavailable in your region") != std::string::npos) ||
        (body_low.find("access denied") != std::string::npos) ||
        (probe.body.find("\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x93\xE3\x82\xB9\xE3\x81\xAF\xE3\x81\x94\xE5\x88\xA9\xE7\x94\xA8\xE3\x81\x84\xE3\x81\x9F\xE3\x81\xA0\xE3\x81\x91\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93") != std::string::npos);

    bool direct_ms_from_blocked_region =
        is_geo_blocked_country(client_cc) && is_direct_microsoft_akamai_ip(res.resolved_ip);

    if (probe.http_status == 403 || probe.http_status == 451 || loc_blocked || body_blocked || direct_ms_from_blocked_region) {
        res.status = AiAccessStatus::GeoBlocked;
        if (direct_ms_from_blocked_region && !client_cc.empty()) {
            res.detail = L"Гео-блок (" + utf8_to_wide(client_cc) + L")";
        } else {
            res.detail = L"Гео-блок";
        }
    } else if (probe.http_status == 200 || probe.http_status == 301 ||
               probe.http_status == 302 || probe.http_status == 303 ||
               probe.http_status == 307 || probe.http_status == 308) {
        res.status = AiAccessStatus::Available;
        res.detail = L"Доступен (" + std::to_wstring(res.latency_ms) + L" мс)";
    } else {
        res.status = AiAccessStatus::Unreachable;
        res.detail = (probe.http_status > 0)
            ? (L"Ошибка HTTP " + std::to_wstring(probe.http_status))
            : L"Недоступен";
    }
    return res;
}

AiDnsProfileResult AiChecker::check_profile(
    const DnsProfile& profile,
    const std::string& client_loc,
    int timeout_ms,
    const std::atomic<bool>* cancel_flag)
{
    ProfileAiCheckResult res;
    res.profile_id = profile.id;
    res.profile_name = profile.name;
    res.dns_ip_used = !profile.primary_ipv4.empty() ? profile.primary_ipv4 : profile.secondary_ipv4;

    res.chatgpt.service = AiService::ChatGPT;
    res.claude.service  = AiService::Claude;
    res.gemini.service  = AiService::Gemini;
    res.copilot.service = AiService::Copilot;

    if (res.dns_ip_used.empty()) {
        res.chatgpt.status = AiAccessStatus::DnsFailed;
        res.chatgpt.detail = L"Нет IP DNS";
        res.claude.status  = AiAccessStatus::DnsFailed;
        res.claude.detail  = L"Нет IP DNS";
        res.gemini.status  = AiAccessStatus::DnsFailed;
        res.gemini.detail  = L"Нет IP DNS";
        res.copilot.status = AiAccessStatus::DnsFailed;
        res.copilot.detail = L"Нет IP DNS";
        aggregate_profile_result(res);
        return res;
    }

    if (is_cancelled(cancel_flag)) {
        aggregate_profile_result(res);
        return res;
    }

    auto safe_check = [&](const std::string& dns_ip, AiService svc) -> AiServiceCheckResult {
        try {
            return check_single_service(dns_ip, svc, client_loc, timeout_ms, cancel_flag);
        } catch (...) {
            AiServiceCheckResult fallback;
            fallback.service = svc;
            fallback.status = AiAccessStatus::Unreachable;
            fallback.detail = L"Внутренняя ошибка";
            return fallback;
        }
    };

    auto run_four = [&](const std::string& dns_ip) {
        std::future<AiServiceCheckResult> f_chatgpt;
        std::future<AiServiceCheckResult> f_claude;
        std::future<AiServiceCheckResult> f_gemini;

        try {
            f_chatgpt = std::async(std::launch::async, [&]() {
                return safe_check(dns_ip, AiService::ChatGPT);
            });
            f_claude = std::async(std::launch::async, [&]() {
                return safe_check(dns_ip, AiService::Claude);
            });
            f_gemini = std::async(std::launch::async, [&]() {
                return safe_check(dns_ip, AiService::Gemini);
            });
            res.copilot = safe_check(dns_ip, AiService::Copilot);
        } catch (...) {
            // Fall through and collect any valid futures; run missing ones synchronously
        }

        res.chatgpt = f_chatgpt.valid() ? f_chatgpt.get() : safe_check(dns_ip, AiService::ChatGPT);
        res.claude  = f_claude.valid()  ? f_claude.get()  : safe_check(dns_ip, AiService::Claude);
        res.gemini  = f_gemini.valid()  ? f_gemini.get()  : safe_check(dns_ip, AiService::Gemini);
        if (res.copilot.status == AiAccessStatus::Pending) {
            res.copilot = safe_check(dns_ip, AiService::Copilot);
        }
    };

    run_four(res.dns_ip_used);

    // If primary DNS IP failed to resolve all 4 services and secondary IP is available, try secondary IP
    if (!is_cancelled(cancel_flag) &&
        res.chatgpt.status == AiAccessStatus::DnsFailed &&
        res.claude.status == AiAccessStatus::DnsFailed &&
        res.gemini.status == AiAccessStatus::DnsFailed &&
        res.copilot.status == AiAccessStatus::DnsFailed &&
        !profile.secondary_ipv4.empty() &&
        profile.secondary_ipv4 != res.dns_ip_used) {
        res.dns_ip_used = profile.secondary_ipv4;
        run_four(res.dns_ip_used);
    }

    aggregate_profile_result(res);
    return res;
}

std::vector<ProfileAiCheckResult> AiChecker::check_profiles(
    const std::vector<DnsProfile>& profiles,
    const std::string& client_country_code,
    const std::atomic<bool>* cancel_flag,
    std::function<void(size_t index, const ProfileAiCheckResult& result)> on_profile_done,
    int timeout_ms)
{
    std::vector<ProfileAiCheckResult> results(profiles.size());
    for (size_t i = 0; i < profiles.size(); ++i) {
        results[i].profile_id = profiles[i].id;
        results[i].profile_name = profiles[i].name;
    }

    if (profiles.empty() || is_cancelled(cancel_flag)) {
        return results;
    }

    std::string effective_cc = client_country_code;
    if (effective_cc.empty() && !is_cancelled(cancel_flag)) {
        try {
            auto ip_info = get_client_ip_info(2000, cancel_flag);
            if (ip_info.valid) {
                effective_cc = ip_info.country_code;
            }
        } catch (...) {
        }
    }

    if (is_cancelled(cancel_flag)) {
        return results;
    }

    std::atomic<size_t> next_index{0};
    std::mutex cb_mutex;
    const size_t worker_count = std::min<size_t>(4, profiles.size());

    std::vector<std::thread> workers;
    workers.reserve(worker_count);
    ThreadPoolGuard join_guard(workers);

    auto worker_fn = [&]() {
        while (!is_cancelled(cancel_flag)) {
            size_t idx = next_index.fetch_add(1, std::memory_order_relaxed);
            if (idx >= profiles.size()) {
                break;
            }
            try {
                ProfileAiCheckResult r = check_profile(profiles[idx], effective_cc, timeout_ms, cancel_flag);
                results[idx] = std::move(r);
                if (on_profile_done && !is_cancelled(cancel_flag)) {
                    std::lock_guard<std::mutex> lock(cb_mutex);
                    if (!is_cancelled(cancel_flag)) {
                        on_profile_done(idx, results[idx]);
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

} // namespace quickdns
