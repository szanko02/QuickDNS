#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef SECURITY_WIN32
#define SECURITY_WIN32
#endif

#include "ai_checker.h"
#include "encoding_utils.h"
#include "winsock_init.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#include <winhttp.h>
#include <security.h>
#include <schannel.h>

#ifdef _MSC_VER
#pragma comment(lib, "secur32.lib")
#pragma comment(lib, "crypt32.lib")
#endif

#ifndef UNISP_NAME_W
#define UNISP_NAME_W L"Microsoft Unified Security Protocol Provider"
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstring>
#include <future>
#include <mutex>
#include <random>
#include <sstream>
#include <thread>

using QuickDns::Encoding::utf8_to_wide;

namespace {

bool is_valid_public_ipv4_addr(uint8_t a, uint8_t b, uint8_t c, uint8_t d) {
    if (a == 0) return false;
    if (a == 10) return false;
    if (a == 100 && (b >= 64 && b <= 127)) return false;
    if (a == 127) return false;
    if (a == 169 && b == 254) return false;
    if (a == 172 && (b >= 16 && b <= 31)) return false;
    if (a == 192 && b == 0 && c == 0) return false;
    if (a == 192 && b == 0 && c == 2) return false;
    if (a == 192 && b == 168) return false;
    if (a == 198 && (b == 18 || b == 19)) return false;
    if (a == 198 && b == 51 && c == 100) return false;
    if (a == 203 && b == 0 && c == 113) return false;
    if (a >= 224) return false;
    (void)d;
    return true;
}

bool skip_dns_name(const uint8_t* packet, size_t packet_len, size_t& offset) {
    size_t jumps = 0;
    while (offset < packet_len && jumps < 64) {
        uint8_t len = packet[offset];
        if (len == 0) {
            offset += 1;
            return true;
        }
        if ((len & 0xC0) == 0xC0) {
            if (offset + 1 >= packet_len) {
                return false;
            }
            offset += 2;
            return true;
        }
        if ((len & 0xC0) != 0) {
            return false;
        }
        if (offset + 1 + len > packet_len) {
            return false;
        }
        offset += 1 + len;
        ++jumps;
    }
    return false;
}

std::string to_lower_ascii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    }
    return s;
}

std::string to_upper_ascii(std::string s) {
    for (char& ch : s) {
        ch = static_cast<char>(std::toupper(static_cast<unsigned char>(ch)));
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

bool is_geo_blocked_country(const std::string& code) {
    std::string cc = to_upper_ascii(trim_ascii(code));
    return (cc == "RU" || cc == "BY" || cc == "IR" || cc == "CN" ||
            cc == "KP" || cc == "SY" || cc == "CU");
}

bool parse_ipv4_octets(const std::string& ip, unsigned int& a, unsigned int& b, unsigned int& c, unsigned int& d) {
    a = b = c = d = 0;
    char tail = 0;
#ifdef _MSC_VER
    if (sscanf_s(ip.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail, 1) != 4) {
        return false;
    }
#else
    if (std::sscanf(ip.c_str(), "%u.%u.%u.%u%c", &a, &b, &c, &d, &tail) != 4) {
        return false;
    }
#endif
    return (a <= 255 && b <= 255 && c <= 255 && d <= 255);
}

bool is_direct_google_ip(const std::string& ip) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (!parse_ipv4_octets(ip, a, b, c, d)) return false;
    if (a == 142 && (b == 250 || b == 251)) return true;
    if (a == 172 && (b == 217 || b == 253)) return true;
    if (a == 216 && (b == 58 || b == 239)) return true;
    if (a == 74 && b == 125) return true;
    if (a == 64 && b == 233) return true;
    if (a == 108 && b == 177) return true;
    if (a == 209 && b == 85) return true;
    return false;
}

bool is_direct_microsoft_akamai_ip(const std::string& ip) {
    unsigned int a = 0, b = 0, c = 0, d = 0;
    if (!parse_ipv4_octets(ip, a, b, c, d)) return false;
    if (a == 23) return true;
    if (a == 104 && (b >= 64 && b <= 127)) return true;
    if (a == 13 || a == 20 || a == 40 || a == 52) return true;
    if (a == 150 && b == 171) return true;
    if (a == 2 && (b >= 16 && b <= 23)) return true;
    if (a == 184 && ((b >= 24 && b <= 31) || (b >= 50 && b <= 51) || (b >= 84 && b <= 87))) return true;
    if (a == 95 && (b == 100 || b == 101)) return true;
    if (a == 92 && (b == 122 || b == 123)) return true;
    if (a == 88 && b == 221) return true;
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

int remaining_ms(std::chrono::steady_clock::time_point deadline) {
    auto now = std::chrono::steady_clock::now();
    if (now >= deadline) return 0;
    return static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(deadline - now).count());
}

bool wait_socket(SOCKET sock, bool for_write, int wait_ms) {
    if (wait_ms <= 0) return false;
    fd_set fds;
    FD_ZERO(&fds);
    FD_SET(sock, &fds);
    fd_set err_fds;
    FD_ZERO(&err_fds);
    FD_SET(sock, &err_fds);

    timeval tv{};
    tv.tv_sec = wait_ms / 1000;
    tv.tv_usec = (wait_ms % 1000) * 1000;

    int rc = select(0, for_write ? nullptr : &fds, for_write ? &fds : nullptr, &err_fds, &tv);
    if (rc <= 0) return false;
    if (FD_ISSET(sock, &err_fds)) return false;
    return FD_ISSET(sock, &fds) != 0;
}

bool socket_send_all(SOCKET sock, const char* data, int len, std::chrono::steady_clock::time_point deadline) {
    int total_sent = 0;
    while (total_sent < len) {
        int rem = remaining_ms(deadline);
        if (rem <= 0) return false;
        int sent = send(sock, data + total_sent, len - total_sent, 0);
        if (sent > 0) {
            total_sent += sent;
            continue;
        }
        if (sent == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK) {
            if (!wait_socket(sock, true, rem)) return false;
            continue;
        }
        return false;
    }
    return true;
}

int socket_recv_some(SOCKET sock, char* buf, int max_len, int wait_ms) {
    if (wait_ms <= 0) return -1;
    int got = recv(sock, buf, max_len, 0);
    if (got > 0) return got;
    if (got == 0) return 0; // EOF
    if (WSAGetLastError() != WSAEWOULDBLOCK) return -1;
    if (!wait_socket(sock, false, wait_ms)) return -1;
    got = recv(sock, buf, max_len, 0);
    if (got >= 0) return got;
    return -1;
}

void parse_http_response(const std::string& raw, HttpsProbeResult& out) {
    size_t header_end = raw.find("\r\n\r\n");
    std::string header_block = (header_end != std::string::npos) ? raw.substr(0, header_end) : raw;
    if (header_end != std::string::npos) {
        out.body = raw.substr(header_end + 4);
    }
    out.headers = header_block;

    std::istringstream iss(header_block);
    std::string status_line;
    if (std::getline(iss, status_line)) {
        if (status_line.rfind("HTTP/", 0) == 0) {
            size_t sp1 = status_line.find(' ');
            if (sp1 != std::string::npos) {
                while (sp1 < status_line.size() && status_line[sp1] == ' ') ++sp1;
                if (sp1 + 3 <= status_line.size()) {
                    int code = 0;
                    for (size_t i = 0; i < 3; ++i) {
                        char c = status_line[sp1 + i];
                        if (c >= '0' && c <= '9') {
                            code = code * 10 + (c - '0');
                        } else {
                            code = 0;
                            break;
                        }
                    }
                    out.http_status = code;
                }
            }
        }
    }

    std::string line;
    while (std::getline(iss, line)) {
        std::string trimmed = trim_ascii(line);
        std::string lower = to_lower_ascii(trimmed);
        if (lower.rfind("location:", 0) == 0) {
            out.location = trim_ascii(trimmed.substr(9));
        }
    }
}

HttpsProbeResult https_probe_ip(
    const std::string& resolved_ip,
    const std::string& host_sni,
    const std::string& path,
    int timeout_ms)
{
    HttpsProbeResult result;
    if (resolved_ip.empty() || host_sni.empty()) {
        result.error_detail = L"Неверный адрес";
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
            result.error_detail = L"Нет соединения TCP";
            return result;
        }
        if (!wait_socket(sock, true, remaining_ms(deadline))) {
            closesocket(sock);
            result.error_detail = L"Тайм-аут TCP";
            return result;
        }
        int so_err = 0;
        int so_len = sizeof(so_err);
        if (getsockopt(sock, SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&so_err), &so_len) != 0 || so_err != 0) {
            closesocket(sock);
            result.error_detail = L"Отказ соединения TCP";
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

    std::vector<char> in_buf(32768);
    size_t in_used = 0;
    bool need_more_network_data = false;

    while (true) {
        if (remaining_ms(deadline) <= 0) {
            result.error_detail = L"Тайм-аут TLS";
            break;
        }

        if (has_ctx && (in_used == 0 || need_more_network_data)) {
            if (in_used >= in_buf.size()) {
                in_buf.resize(in_buf.size() * 2);
            }
            int got = socket_recv_some(
                sock,
                in_buf.data() + in_used,
                static_cast<int>(in_buf.size() - in_used),
                remaining_ms(deadline)
            );
            if (got <= 0) {
                result.error_detail = (got == 0) ? L"Сброс соединения TLS" : L"Тайм-аут TLS";
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

        if (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED ||
            (FAILED(ss) && (out_attrs & ISC_RET_EXTENDED_ERROR))) {
            if (!has_ctx && (ss == SEC_E_OK || ss == SEC_I_CONTINUE_NEEDED)) {
                has_ctx = true;
            }
            if (out_sec[0].cbBuffer > 0 && out_sec[0].pvBuffer != nullptr) {
                bool sent_ok = socket_send_all(
                    sock,
                    static_cast<const char*>(out_sec[0].pvBuffer),
                    static_cast<int>(out_sec[0].cbBuffer),
                    deadline
                );
                FreeContextBuffer(out_sec[0].pvBuffer);
                if (!sent_ok) {
                    result.error_detail = L"Ошибка отправки TLS";
                    break;
                }
            }
        }

        if (ss == SEC_E_INCOMPLETE_MESSAGE) {
            need_more_network_data = true;
            continue;
        }

        if (has_ctx && in_used > 0) {
            if (in_sec[1].BufferType == SECBUFFER_EXTRA && in_sec[1].cbBuffer > 0 && in_sec[1].cbBuffer <= in_used) {
                size_t extra = in_sec[1].cbBuffer;
                std::memmove(in_buf.data(), in_buf.data() + (in_used - extra), extra);
                in_used = extra;
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

        // Handshake failed (certificate error, alert, etc.)
        result.error_detail = L"Ошибка TLS сертификата";
        break;
    }

    if (!result.tls_ok) {
        if (has_ctx) DeleteSecurityContext(&hCtx);
        FreeCredentialsHandle(&hCred);
        closesocket(sock);
        return result;
    }

    SecPkgContext_StreamSizes sizes{};
    if (QueryContextAttributesW(&hCtx, SECPKG_ATTR_STREAM_SIZES, &sizes) != SEC_E_OK) {
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
            result.error_detail = L"Ошибка шифрования TLS";
            return result;
        }

        int total_enc = static_cast<int>(bufs[0].cbBuffer + bufs[1].cbBuffer + bufs[2].cbBuffer);
        if (!socket_send_all(sock, io_buf.data(), total_enc, deadline)) {
            DeleteSecurityContext(&hCtx);
            FreeCredentialsHandle(&hCred);
            closesocket(sock);
            result.error_detail = L"Ошибка отправки HTTP";
            return result;
        }
        req_offset += chunk_len;
    }

    std::string plaintext;
    plaintext.reserve(16384);
    bool need_more_cipher = (in_used == 0);
    bool latency_recorded = false;

    while (plaintext.size() < 16384) {
        int rem = remaining_ms(deadline);
        if (rem <= 0) break;

        if (need_more_cipher) {
            if (in_used >= in_buf.size()) {
                in_buf.resize(in_buf.size() * 2);
            }
            // Once headers and some body have already been received, use a short linger wait
            int read_wait = rem;
            size_t hdr_pos = plaintext.find("\r\n\r\n");
            if (hdr_pos != std::string::npos && plaintext.size() > hdr_pos + 4) {
                read_wait = std::min(rem, 200);
            }
            int got = socket_recv_some(
                sock,
                in_buf.data() + in_used,
                static_cast<int>(in_buf.size() - in_used),
                read_wait
            );
            if (got <= 0) {
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
            break;
        }

        if (!latency_recorded) {
            auto t_now = std::chrono::steady_clock::now();
            int lat = static_cast<int>(std::chrono::duration_cast<std::chrono::milliseconds>(t_now - start_time).count());
            result.latency_ms = std::max(1, lat);
            latency_recorded = true;
        }

        SecBuffer* extra_buf = nullptr;
        for (int i = 0; i < 4; ++i) {
            if (dec_bufs[i].BufferType == SECBUFFER_DATA && dec_bufs[i].cbBuffer > 0 && dec_bufs[i].pvBuffer != nullptr) {
                plaintext.append(static_cast<const char*>(dec_bufs[i].pvBuffer), dec_bufs[i].cbBuffer);
            } else if (dec_bufs[i].BufferType == SECBUFFER_EXTRA && dec_bufs[i].cbBuffer > 0) {
                extra_buf = &dec_bufs[i];
            }
        }

        if (extra_buf != nullptr && extra_buf->cbBuffer <= in_used) {
            size_t extra_len = extra_buf->cbBuffer;
            std::memmove(in_buf.data(), static_cast<const char*>(extra_buf->pvBuffer), extra_len);
            in_used = extra_len;
            need_more_cipher = false;
        } else {
            in_used = 0;
            need_more_cipher = true;
        }

        if (ss == SEC_I_RENEGOTIATE) {
            break;
        }

        // Fast-exit checks once HTTP headers and needed body are present
        size_t hdr_end = plaintext.find("\r\n\r\n");
        if (hdr_end != std::string::npos) {
            if (path == "/cdn-cgi/trace") {
                size_t loc_pos = plaintext.find("loc=", hdr_end);
                if (loc_pos != std::string::npos && plaintext.find('\n', loc_pos) != std::string::npos) {
                    break;
                }
            }
        }
    }

    DeleteSecurityContext(&hCtx);
    FreeCredentialsHandle(&hCred);
    closesocket(sock);

    if (!plaintext.empty()) {
        parse_http_response(plaintext, result);
    } else if (result.error_detail.empty()) {
        result.error_detail = L"Пустой ответ HTTP";
    }

    return result;
}

std::string winhttp_fetch_text(const wchar_t* host, const wchar_t* path, int timeout_ms) {
    HINTERNET hSession = WinHttpOpen(
        L"Mozilla/5.0 (Windows NT 10.0; Win64; x64) QuickDNS/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!hSession) return "";

    DWORD to = static_cast<DWORD>(timeout_ms);
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

    std::string body;
    if (WinHttpSendRequest(hRequest, WINHTTP_NO_ADDITIONAL_HEADERS, 0, nullptr, 0, 0, 0) &&
        WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD bytes_read = 0;
        char buf[4096];
        while (WinHttpReadData(hRequest, buf, sizeof(buf), &bytes_read) && bytes_read > 0) {
            body.append(buf, bytes_read);
            if (body.size() >= 8192) break;
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return body;
}

} // anonymous namespace

namespace quickdns {

std::vector<uint8_t> AiChecker::build_dns_a_query(uint16_t txid, const std::string& domain) {
    std::vector<uint8_t> packet;
    packet.reserve(64 + domain.size());

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

    size_t start = 0;
    while (start < domain.size()) {
        size_t dot = domain.find('.', start);
        if (dot == std::string::npos) dot = domain.size();
        size_t len = dot - start;
        if (len > 0 && len <= 63) {
            packet.push_back(static_cast<uint8_t>(len));
            for (size_t i = start; i < dot; ++i) {
                packet.push_back(static_cast<uint8_t>(std::tolower(static_cast<unsigned char>(domain[i]))));
            }
        }
        start = dot + 1;
    }
    packet.push_back(0x00); // Root label
    packet.push_back(0x00);
    packet.push_back(0x01); // QTYPE = A (1)
    packet.push_back(0x00);
    packet.push_back(0x01); // QCLASS = IN (1)
    return packet;
}

std::string AiChecker::parse_dns_a_response(const uint8_t* packet, size_t packet_len, uint16_t expected_txid) {
    if (!packet || packet_len < 12) {
        return "";
    }

    uint16_t rx_txid = static_cast<uint16_t>((static_cast<uint16_t>(packet[0]) << 8) | packet[1]);
    if (rx_txid != expected_txid) {
        return "";
    }

    uint8_t flags_hi = packet[2];
    uint8_t flags_lo = packet[3];
    bool is_response = (flags_hi & 0x80) != 0;
    uint8_t rcode = flags_lo & 0x0F;
    if (!is_response || rcode != 0) {
        return "";
    }

    uint16_t qdcount = static_cast<uint16_t>((static_cast<uint16_t>(packet[4]) << 8) | packet[5]);
    uint16_t ancount = static_cast<uint16_t>((static_cast<uint16_t>(packet[6]) << 8) | packet[7]);
    if (ancount == 0) {
        return "";
    }

    size_t offset = 12;
    for (uint16_t q = 0; q < qdcount; ++q) {
        if (!skip_dns_name(packet, packet_len, offset)) {
            return "";
        }
        if (offset + 4 > packet_len) {
            return "";
        }
        offset += 4; // QTYPE + QCLASS
    }

    for (uint16_t a = 0; a < ancount; ++a) {
        if (!skip_dns_name(packet, packet_len, offset)) {
            return "";
        }
        if (offset + 10 > packet_len) {
            return "";
        }

        uint16_t rtype = static_cast<uint16_t>((static_cast<uint16_t>(packet[offset]) << 8) | packet[offset + 1]);
        uint16_t rclass = static_cast<uint16_t>((static_cast<uint16_t>(packet[offset + 2]) << 8) | packet[offset + 3]);
        uint16_t rdlength = static_cast<uint16_t>((static_cast<uint16_t>(packet[offset + 8]) << 8) | packet[offset + 9]);
        offset += 10;

        if (offset + rdlength > packet_len) {
            return "";
        }

        if (rtype == 1 && rclass == 1 && rdlength == 4) {
            uint8_t o1 = packet[offset];
            uint8_t o2 = packet[offset + 1];
            uint8_t o3 = packet[offset + 2];
            uint8_t o4 = packet[offset + 3];
            if (is_valid_public_ipv4_addr(o1, o2, o3, o4)) {
                return std::to_string(o1) + "." +
                       std::to_string(o2) + "." +
                       std::to_string(o3) + "." +
                       std::to_string(o4);
            }
        }

        offset += rdlength;
    }

    return "";
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

ClientIpInfo AiChecker::get_client_ip_info(int timeout_ms) {
    ClientIpInfo info;

    // 1. Direct TLS probe to https://1.1.1.1/cdn-cgi/trace
    auto probe = https_probe_ip("1.1.1.1", "one.one.one.one", "/cdn-cgi/trace", timeout_ms);
    if (probe.tls_ok && probe.http_status == 200 && !probe.body.empty()) {
        auto parsed = parse_cdn_cgi_trace(probe.body);
        if (!parsed.ip.empty() && parsed.has_loc) {
            info.ip = parsed.ip;
            info.country_code = parsed.loc;
            info.valid = true;
            return info;
        }
    }

    // 2. Fallback via WinHTTP to https://1.1.1.1/cdn-cgi/trace
    std::string text = winhttp_fetch_text(L"1.1.1.1", L"/cdn-cgi/trace", timeout_ms);
    if (!text.empty()) {
        auto parsed = parse_cdn_cgi_trace(text);
        if (!parsed.ip.empty() && parsed.has_loc) {
            info.ip = parsed.ip;
            info.country_code = parsed.loc;
            info.valid = true;
            return info;
        }
    }

    // 3. Fallback via WinHTTP to https://cloudflare.com/cdn-cgi/trace
    text = winhttp_fetch_text(L"cloudflare.com", L"/cdn-cgi/trace", timeout_ms);
    if (!text.empty()) {
        auto parsed = parse_cdn_cgi_trace(text);
        if (!parsed.ip.empty() && parsed.has_loc) {
            info.ip = parsed.ip;
            info.country_code = parsed.loc;
            info.valid = true;
            return info;
        }
    }

    return info;
}

std::string AiChecker::resolve_domain_via_dns(const std::string& dns_server_ip, const std::string& domain, int timeout_ms) {
    if (dns_server_ip.empty() || domain.empty()) {
        return "";
    }

    ensure_winsock();

    sockaddr_in sa4{};
    sa4.sin_family = AF_INET;
    sa4.sin_port = htons(53);
    if (inet_pton(AF_INET, dns_server_ip.c_str(), &sa4.sin_addr) != 1) {
        return "";
    }

    SOCKET sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
    if (sock == INVALID_SOCKET) {
        return "";
    }

    DWORD to = static_cast<DWORD>(timeout_ms);
    setsockopt(sock, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));
    setsockopt(sock, SOL_SOCKET, SO_SNDTIMEO, reinterpret_cast<const char*>(&to), sizeof(to));

    static thread_local std::mt19937 rng(std::random_device{}());
    std::uniform_int_distribution<uint16_t> dist(1, 0xFFFE);
    uint16_t txid = dist(rng);

    auto query = build_dns_a_query(txid, domain);
    std::string resolved;

    int sent = sendto(
        sock,
        reinterpret_cast<const char*>(query.data()),
        static_cast<int>(query.size()),
        0,
        reinterpret_cast<const sockaddr*>(&sa4),
        sizeof(sa4)
    );

    if (sent > 0 && wait_socket(sock, false, timeout_ms)) {
        uint8_t recv_buf[1500] = {};
        sockaddr_in from{};
        int from_len = sizeof(from);
        int received = recvfrom(
            sock,
            reinterpret_cast<char*>(recv_buf),
            sizeof(recv_buf),
            0,
            reinterpret_cast<sockaddr*>(&from),
            &from_len
        );
        if (received >= 12 && from.sin_addr.s_addr == sa4.sin_addr.s_addr) {
            resolved = parse_dns_a_response(recv_buf, static_cast<size_t>(received), txid);
        }
    }

    closesocket(sock);
    return resolved;
}

AiServiceCheckResult AiChecker::check_single_service(
    const std::string& dns_server_ip,
    AiService service,
    const std::string& client_country_code,
    int timeout_ms)
{
    AiServiceCheckResult res;
    res.service = service;
    res.status = AiAccessStatus::Pending;
    res.latency_ms = -1;

    std::string domain;
    switch (service) {
        case AiService::ChatGPT: domain = "chatgpt.com"; break;
        case AiService::Claude:  domain = "claude.ai"; break;
        case AiService::Gemini:  domain = "gemini.google.com"; break;
        case AiService::Copilot: domain = "copilot.microsoft.com"; break;
    }

    int dns_timeout = std::min(1200, std::max(400, timeout_ms / 2));
    res.resolved_ip = resolve_domain_via_dns(dns_server_ip, domain, dns_timeout);
    if (res.resolved_ip.empty()) {
        res.status = AiAccessStatus::DnsFailed;
        res.detail = L"Нет ответа DNS";
        return res;
    }

    std::string client_cc = to_upper_ascii(trim_ascii(client_country_code));

    if (service == AiService::ChatGPT) {
        auto trace_probe = https_probe_ip(res.resolved_ip, "chatgpt.com", "/cdn-cgi/trace", timeout_ms);
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

        // Fallback for custom SNI proxies that do not expose /cdn-cgi/trace
        auto root_probe = https_probe_ip(res.resolved_ip, "chatgpt.com", "/", timeout_ms);
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
                   root_probe.http_status == 302 || root_probe.http_status == 307 ||
                   root_probe.http_status == 308) {
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
        auto trace_probe = https_probe_ip(res.resolved_ip, "claude.ai", "/cdn-cgi/trace", timeout_ms);
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

        // Fallback: GET /login
        auto login_probe = https_probe_ip(res.resolved_ip, "claude.ai", "/login", timeout_ms);
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
        auto probe = https_probe_ip(res.resolved_ip, "gemini.google.com", "/app", timeout_ms);
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
    auto probe = https_probe_ip(res.resolved_ip, "copilot.microsoft.com", "/", timeout_ms);
    if (!probe.tcp_connected || !probe.tls_ok) {
        res.status = AiAccessStatus::Unreachable;
        res.detail = probe.error_detail.empty() ? L"Тайм-аут TLS" : probe.error_detail;
        return res;
    }
    res.latency_ms = probe.latency_ms;

    std::string body_low = to_lower_ascii(probe.body);
    bool body_blocked =
        (body_low.find("not available in your region") != std::string::npos) ||
        (body_low.find("access denied") != std::string::npos) ||
        (probe.body.find("\xE3\x82\xB5\xE3\x83\xBC\xE3\x83\x93\xE3\x82\xB9\xE3\x81\xAF\xE3\x81\x94\xE5\x88\xA9\xE7\x94\xA8\xE3\x81\x84\xE3\x81\x9F\xE3\x81\xA0\xE3\x81\x91\xE3\x81\xBE\xE3\x81\x9B\xE3\x82\x93") != std::string::npos);

    bool direct_ms_from_blocked_region =
        is_geo_blocked_country(client_cc) && is_direct_microsoft_akamai_ip(res.resolved_ip);

    if (probe.http_status == 403 || probe.http_status == 451 || body_blocked || direct_ms_from_blocked_region) {
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

ProfileAiCheckResult AiChecker::check_profile(
    const DnsProfile& profile,
    const std::string& client_country_code,
    int timeout_ms)
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

    auto run_four = [&](const std::string& dns_ip) {
        auto f_chatgpt = std::async(std::launch::async, [&]() {
            return check_single_service(dns_ip, AiService::ChatGPT, client_country_code, timeout_ms);
        });
        auto f_claude = std::async(std::launch::async, [&]() {
            return check_single_service(dns_ip, AiService::Claude, client_country_code, timeout_ms);
        });
        auto f_gemini = std::async(std::launch::async, [&]() {
            return check_single_service(dns_ip, AiService::Gemini, client_country_code, timeout_ms);
        });
        auto f_copilot = std::async(std::launch::async, [&]() {
            return check_single_service(dns_ip, AiService::Copilot, client_country_code, timeout_ms);
        });

        res.chatgpt = f_chatgpt.get();
        res.claude  = f_claude.get();
        res.gemini  = f_gemini.get();
        res.copilot = f_copilot.get();
    };

    run_four(res.dns_ip_used);

    // If primary DNS IP failed to resolve all 4 services and secondary IP is available, try secondary IP
    if (res.chatgpt.status == AiAccessStatus::DnsFailed &&
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
    std::atomic<bool>* cancel_flag,
    std::function<void(size_t index, const ProfileAiCheckResult& result)> on_profile_done)
{
    std::vector<ProfileAiCheckResult> results(profiles.size());
    if (profiles.empty()) {
        return results;
    }

    std::string effective_cc = client_country_code;
    if (effective_cc.empty() && (!cancel_flag || !cancel_flag->load())) {
        auto ip_info = get_client_ip_info(2000);
        if (ip_info.valid) {
            effective_cc = ip_info.country_code;
        }
    }

    std::atomic<size_t> next_index{0};
    std::mutex cb_mutex;
    const size_t worker_count = std::min<size_t>(4, profiles.size());

    std::vector<std::thread> workers;
    workers.reserve(worker_count);

    for (size_t w = 0; w < worker_count; ++w) {
        workers.emplace_back([&]() {
            while (true) {
                if (cancel_flag && cancel_flag->load()) {
                    break;
                }
                size_t idx = next_index.fetch_add(1);
                if (idx >= profiles.size()) {
                    break;
                }
                ProfileAiCheckResult r = check_profile(profiles[idx], effective_cc, 3000);
                results[idx] = r;
                if (on_profile_done && (!cancel_flag || !cancel_flag->load())) {
                    std::lock_guard<std::mutex> lock(cb_mutex);
                    on_profile_done(idx, results[idx]);
                }
            }
        });
    }

    for (auto& t : workers) {
        if (t.joinable()) {
            t.join();
        }
    }

    return results;
}

} // namespace quickdns
