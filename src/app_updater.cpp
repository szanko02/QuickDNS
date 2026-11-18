#include "app_updater.h"
#include "encoding_utils.h"
#include "../third_party/nlohmann/json.hpp"
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cctype>
#include <cstdint>
#include <climits>

#ifdef _MSC_VER
#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "bcrypt.lib")
#pragma comment(lib, "shell32.lib")
#endif

namespace quickdns {

namespace {

char ascii_tolower(char c) {
    if (c >= 'A' && c <= 'Z') {
        return static_cast<char>(c - 'A' + 'a');
    }
    return c;
}

wchar_t ascii_towlower(wchar_t c) {
    if (c >= L'A' && c <= L'Z') {
        return static_cast<wchar_t>(c - L'A' + L'a');
    }
    return c;
}

bool starts_with_icase_w(std::wstring_view str, std::wstring_view prefix) {
    if (str.size() < prefix.size()) return false;
    for (size_t i = 0; i < prefix.size(); ++i) {
        if (ascii_towlower(str[i]) != ascii_towlower(prefix[i])) {
            return false;
        }
    }
    return true;
}

bool equals_icase_ascii(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
        if (ascii_tolower(a[i]) != ascii_tolower(b[i])) {
            return false;
        }
    }
    return true;
}

bool is_safe_identifier_ascii(std::string_view token) {
    if (token.empty() || token.size() > 128 || token == "." || token.find("..") != std::string_view::npos) {
        return false;
    }
    for (char c : token) {
        bool ok = (c >= 'A' && c <= 'Z') ||
                  (c >= 'a' && c <= 'z') ||
                  (c >= '0' && c <= '9') ||
                  c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool is_safe_tag_wide(std::wstring_view tag) {
    if (tag.empty() || tag.size() > 128 || tag == L"." || tag.find(L"..") != std::wstring_view::npos) {
        return false;
    }
    for (wchar_t c : tag) {
        bool ok = (c >= L'A' && c <= L'Z') ||
                  (c >= L'a' && c <= L'z') ||
                  (c >= L'0' && c <= L'9') ||
                  c == L'.' || c == L'_' || c == L'-';
        if (!ok) return false;
    }
    return true;
}

std::wstring normalize_crlf(const std::wstring& str) {
    std::wstring result;
    result.reserve(str.size() + 32);
    for (size_t i = 0; i < str.size(); ++i) {
        if (str[i] == L'\n') {
            if (i == 0 || str[i - 1] != L'\r') {
                result.push_back(L'\r');
            }
        }
        result.push_back(str[i]);
    }
    return result;
}

struct ParsedSemver {
    bool valid = false;
    std::vector<int> core;
    std::vector<std::string> prerelease;
};

bool is_all_digits(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s) {
        if (c < '0' || c > '9') return false;
    }
    return true;
}

uint64_t parse_u64_clamped(std::string_view digits) {
    uint64_t val = 0;
    for (char c : digits) {
        uint64_t d = static_cast<uint64_t>(c - '0');
        if (val > (UINT64_MAX - d) / 10) {
            return UINT64_MAX;
        }
        val = val * 10 + d;
    }
    return val;
}

int compare_prerelease_identifiers(std::string_view a, std::string_view b) {
    if (a == b) return 0;

    bool a_num = is_all_digits(a);
    bool b_num = is_all_digits(b);

    if (a_num && b_num) {
        // Strip leading zeros for arbitrary-length numeric comparison
        size_t a_nz = a.find_first_not_of('0');
        size_t b_nz = b.find_first_not_of('0');
        std::string_view a_sig = (a_nz == std::string_view::npos) ? std::string_view("0") : a.substr(a_nz);
        std::string_view b_sig = (b_nz == std::string_view::npos) ? std::string_view("0") : b.substr(b_nz);
        if (a_sig.size() != b_sig.size()) {
            return (a_sig.size() < b_sig.size()) ? -1 : 1;
        }
        int cmp = a_sig.compare(b_sig);
        return (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
    }

    // SemVer 2.0.0 section 11.4.1: numeric identifiers have lower precedence than non-numeric
    if (a_num != b_num) {
        return a_num ? -1 : 1;
    }

    // Natural suffix comparison when non-empty alpha prefix matches (e.g. rc2 < rc10)
    size_t a_digits = a.size();
    while (a_digits > 0 && a[a_digits - 1] >= '0' && a[a_digits - 1] <= '9') {
        --a_digits;
    }
    size_t b_digits = b.size();
    while (b_digits > 0 && b[b_digits - 1] >= '0' && b[b_digits - 1] <= '9') {
        --b_digits;
    }
    if (a_digits > 0 && a_digits == b_digits &&
        a_digits < a.size() && b_digits < b.size() &&
        a.substr(0, a_digits) == b.substr(0, b_digits)) {
        return compare_prerelease_identifiers(a.substr(a_digits), b.substr(b_digits));
    }

    int cmp = a.compare(b);
    return (cmp < 0) ? -1 : (cmp > 0 ? 1 : 0);
}

ParsedSemver parse_full_semver(const std::string& ver) {
    ParsedSemver out;
    if (ver.empty()) {
        return out;
    }

    std::string clean = ver;
    if (clean[0] == 'v' || clean[0] == 'V') {
        clean.erase(0, 1);
    }
    if (clean.empty()) {
        return out;
    }

    // Ignore +build metadata per SemVer 2.0.0 section 10
    size_t plus = clean.find('+');
    if (plus != std::string::npos) {
        if (plus + 1 >= clean.size()) {
            return out;
        }
        clean.resize(plus);
    }

    std::string core_part = clean;
    std::string pre_part;
    bool has_pre = false;
    size_t dash = clean.find('-');
    if (dash != std::string::npos) {
        has_pre = true;
        core_part = clean.substr(0, dash);
        pre_part = clean.substr(dash + 1);
        if (pre_part.empty()) {
            return out;
        }
    }

    if (core_part.empty()) {
        return out;
    }

    size_t start = 0;
    while (start <= core_part.size()) {
        size_t dot = core_part.find('.', start);
        std::string_view seg = (dot == std::string::npos)
                                   ? std::string_view(core_part).substr(start)
                                   : std::string_view(core_part).substr(start, dot - start);
        if (!is_all_digits(seg)) {
            return out;
        }
        uint64_t val = parse_u64_clamped(seg);
        out.core.push_back(val > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(val));
        if (dot == std::string::npos) break;
        start = dot + 1;
    }

    while (out.core.size() < 3) {
        out.core.push_back(0);
    }

    if (has_pre) {
        size_t pstart = 0;
        while (pstart <= pre_part.size()) {
            size_t dot = pre_part.find('.', pstart);
            std::string_view seg = (dot == std::string::npos)
                                       ? std::string_view(pre_part).substr(pstart)
                                       : std::string_view(pre_part).substr(pstart, dot - pstart);
            if (seg.empty()) {
                return out;
            }
            for (char c : seg) {
                bool ok = (c >= 'A' && c <= 'Z') ||
                          (c >= 'a' && c <= 'z') ||
                          (c >= '0' && c <= '9') ||
                          c == '-';
                if (!ok) return out;
            }
            out.prerelease.emplace_back(seg);
            if (dot == std::string::npos) break;
            pstart = dot + 1;
        }
    }

    out.valid = true;
    return out;
}

struct CrackedHttpsUrl {
    std::wstring host;
    std::wstring path_and_query;
    INTERNET_PORT port = 0;
};

bool crack_https_url(const std::wstring& url, CrackedHttpsUrl& out) {
    if (url.empty() || url.size() > 8192) {
        return false;
    }

    for (wchar_t ch : url) {
        if (ch == L'\0' || ch <= 0x1F || ch == 0x7F ||
            ch == L' ' || ch == L'\t' || ch == L'\r' || ch == L'\n') {
            return false;
        }
    }

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    urlComp.dwSchemeLength = static_cast<DWORD>(-1);
    urlComp.dwHostNameLength = static_cast<DWORD>(-1);
    urlComp.dwUserNameLength = static_cast<DWORD>(-1);
    urlComp.dwPasswordLength = static_cast<DWORD>(-1);
    urlComp.dwUrlPathLength = static_cast<DWORD>(-1);
    urlComp.dwExtraInfoLength = static_cast<DWORD>(-1);

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.size()), 0, &urlComp)) {
        return false;
    }

    if (urlComp.nScheme != INTERNET_SCHEME_HTTPS ||
        urlComp.nPort != INTERNET_DEFAULT_HTTPS_PORT ||
        urlComp.dwUserNameLength != 0 ||
        urlComp.dwPasswordLength != 0 ||
        urlComp.lpszUserName != nullptr ||
        urlComp.lpszPassword != nullptr ||
        urlComp.lpszHostName == nullptr ||
        urlComp.dwHostNameLength == 0) {
        return false;
    }

    std::wstring hostStr(urlComp.lpszHostName, urlComp.dwHostNameLength);
    for (wchar_t& ch : hostStr) {
        if (ch == L'@' || ch == L':' || ch == L'\\' || ch == L'/') {
            return false;
        }
        ch = ascii_towlower(ch);
    }

    std::wstring pathPart;
    if (urlComp.lpszUrlPath != nullptr && urlComp.dwUrlPathLength > 0) {
        pathPart.assign(urlComp.lpszUrlPath, urlComp.dwUrlPathLength);
    } else {
        pathPart = L"/";
    }
    if (pathPart.empty() || pathPart[0] != L'/') {
        pathPart.insert(pathPart.begin(), L'/');
    }

    if (urlComp.lpszExtraInfo != nullptr && urlComp.dwExtraInfoLength > 0) {
        std::wstring extra(urlComp.lpszExtraInfo, urlComp.dwExtraInfoLength);
        size_t hashPos = extra.find(L'#');
        if (hashPos != std::wstring::npos) {
            extra.resize(hashPos);
        }
        pathPart += extra;
    }

    out.host = std::move(hostStr);
    out.path_and_query = std::move(pathPart);
    out.port = urlComp.nPort;
    return true;
}

bool is_allowed_host(const std::wstring& hostLower) {
    return (hostLower == L"github.com" ||
            hostLower == L"api.github.com" ||
            hostLower == L"objects.githubusercontent.com" ||
            hostLower == L"release-assets.githubusercontent.com");
}

bool is_valid_release_asset_url(
    const std::wstring& url,
    const std::wstring& ownerW,
    const std::wstring& repoW)
{
    CrackedHttpsUrl cracked;
    if (!crack_https_url(url, cracked) || !is_allowed_host(cracked.host)) {
        return false;
    }

    if (cracked.host == L"github.com") {
        std::wstring expectedPrefix = L"/" + ownerW + L"/" + repoW + L"/releases/download/";
        if (!starts_with_icase_w(cracked.path_and_query, expectedPrefix)) {
            return false;
        }
        std::wstring_view remainder = std::wstring_view(cracked.path_and_query).substr(expectedPrefix.size());
        if (remainder.empty() || remainder.find(L"..") != std::wstring_view::npos) {
            return false;
        }
        return true;
    }

    return (cracked.host == L"objects.githubusercontent.com" ||
            cracked.host == L"release-assets.githubusercontent.com");
}

std::wstring trim_http_ows(std::wstring_view s) {
    while (!s.empty() && (s.front() == L' ' || s.front() == L'\t' || s.front() == L'\r' || s.front() == L'\n')) {
        s.remove_prefix(1);
    }
    while (!s.empty() && (s.back() == L' ' || s.back() == L'\t' || s.back() == L'\r' || s.back() == L'\n')) {
        s.remove_suffix(1);
    }
    return std::wstring(s);
}

std::wstring resolve_redirect_url(const CrackedHttpsUrl& base, std::wstring_view rawLocation) {
    std::wstring loc = trim_http_ows(rawLocation);
    if (loc.empty()) {
        return L"";
    }
    if (starts_with_icase_w(loc, L"https://") || starts_with_icase_w(loc, L"http://")) {
        return loc;
    }
    if (loc.rfind(L"//", 0) == 0) {
        return L"https:" + loc;
    }
    if (loc[0] == L'/') {
        return L"https://" + base.host + loc;
    }
    return L"";
}

bool query_header_wstring(HINTERNET hRequest, DWORD infoLevel, std::wstring& outValue) {
    outValue.clear();
    DWORD bytesNeeded = 0;
    WinHttpQueryHeaders(
        hRequest,
        infoLevel,
        WINHTTP_HEADER_NAME_BY_INDEX,
        WINHTTP_NO_OUTPUT_BUFFER,
        &bytesNeeded,
        WINHTTP_NO_HEADER_INDEX
    );
    DWORD err = GetLastError();
    if (err == ERROR_WINHTTP_HEADER_NOT_FOUND) {
        return false;
    }
    if (err != ERROR_INSUFFICIENT_BUFFER || bytesNeeded == 0 || bytesNeeded > 32768) {
        return false;
    }

    std::vector<wchar_t> buf((bytesNeeded + sizeof(wchar_t) - 1) / sizeof(wchar_t) + 1, L'\0');
    DWORD bufBytes = bytesNeeded;
    if (!WinHttpQueryHeaders(
            hRequest,
            infoLevel,
            WINHTTP_HEADER_NAME_BY_INDEX,
            buf.data(),
            &bufBytes,
            WINHTTP_NO_HEADER_INDEX)) {
        return false;
    }

    size_t charLen = bufBytes / sizeof(wchar_t);
    while (charLen > 0 && buf[charLen - 1] == L'\0') {
        --charLen;
    }
    outValue.assign(buf.data(), charLen);
    return true;
}

bool query_content_length_64(HINTERNET hRequest, bool& outHasHeader, uint64_t& outLength) {
    outHasHeader = false;
    outLength = 0;

    std::wstring rawHeader;
    if (!query_header_wstring(hRequest, WINHTTP_QUERY_CONTENT_LENGTH, rawHeader)) {
        // Distinguish absent header from oversized/corrupt header
        return (GetLastError() == ERROR_WINHTTP_HEADER_NOT_FOUND);
    }

    outHasHeader = true;
    std::wstring trimmed = trim_http_ows(rawHeader);
    if (trimmed.empty()) {
        return false;
    }

    uint64_t val = 0;
    for (wchar_t ch : trimmed) {
        if (ch < L'0' || ch > L'9') {
            return false;
        }
        uint64_t digit = static_cast<uint64_t>(ch - L'0');
        if (val > (UINT64_MAX - digit) / 10) {
            return false;
        }
        val = val * 10 + digit;
    }

    outLength = val;
    return true;
}

bool is_redirect_status(DWORD statusCode) {
    return (statusCode == 301 || statusCode == 302 || statusCode == 303 ||
            statusCode == 307 || statusCode == 308);
}

struct HttpGetConnection {
    HINTERNET hSession = nullptr;
    HINTERNET hConnect = nullptr;
    HINTERNET hRequest = nullptr;

    HttpGetConnection() = default;
    HttpGetConnection(const HttpGetConnection&) = delete;
    HttpGetConnection& operator=(const HttpGetConnection&) = delete;

    HttpGetConnection(HttpGetConnection&& other) noexcept
        : hSession(other.hSession), hConnect(other.hConnect), hRequest(other.hRequest) {
        other.hSession = nullptr;
        other.hConnect = nullptr;
        other.hRequest = nullptr;
    }

    ~HttpGetConnection() {
        close();
    }

    void close_request_and_connect() {
        if (hRequest) {
            WinHttpCloseHandle(hRequest);
            hRequest = nullptr;
        }
        if (hConnect) {
            WinHttpCloseHandle(hConnect);
            hConnect = nullptr;
        }
    }

    void close() {
        close_request_and_connect();
        if (hSession) {
            WinHttpCloseHandle(hSession);
            hSession = nullptr;
        }
    }
};

bool open_https_get_with_validated_redirects(
    const std::wstring& initialUrl,
    const wchar_t* userAgent,
    const std::wstring& requestHeaders,
    DWORD timeoutMs,
    HttpGetConnection& outConn)
{
    outConn.close();
    if (!AppUpdater::is_allowed_update_url(initialUrl)) {
        return false;
    }

    outConn.hSession = WinHttpOpen(
        userAgent,
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS,
        0
    );
    if (!outConn.hSession) {
        return false;
    }

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(outConn.hSession, WINHTTP_OPTION_REDIRECT_POLICY,
                          &redirectPolicy, sizeof(redirectPolicy))) {
        outConn.close();
        return false;
    }

    WinHttpSetTimeouts(outConn.hSession, timeoutMs, timeoutMs, timeoutMs, timeoutMs);

    std::wstring currentUrl = initialUrl;
    for (int hop = 0; hop <= AppUpdater::MAX_REDIRECT_HOPS; ++hop) {
        CrackedHttpsUrl cracked;
        if (!AppUpdater::is_allowed_update_url(currentUrl) || !crack_https_url(currentUrl, cracked)) {
            outConn.close();
            return false;
        }

        outConn.hConnect = WinHttpConnect(outConn.hSession, cracked.host.c_str(), cracked.port, 0);
        if (!outConn.hConnect) {
            outConn.close();
            return false;
        }

        outConn.hRequest = WinHttpOpenRequest(
            outConn.hConnect,
            L"GET",
            cracked.path_and_query.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE
        );
        if (!outConn.hRequest) {
            outConn.close();
            return false;
        }

        if (!WinHttpSendRequest(
                outConn.hRequest,
                requestHeaders.c_str(),
                static_cast<DWORD>(requestHeaders.size()),
                nullptr,
                0,
                0,
                0) ||
            !WinHttpReceiveResponse(outConn.hRequest, nullptr)) {
            outConn.close();
            return false;
        }

        DWORD statusCode = 0;
        DWORD statusSz = sizeof(statusCode);
        if (!WinHttpQueryHeaders(
                outConn.hRequest,
                WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                WINHTTP_HEADER_NAME_BY_INDEX,
                &statusCode,
                &statusSz,
                WINHTTP_NO_HEADER_INDEX)) {
            outConn.close();
            return false;
        }

        if (statusCode == 200) {
            return true;
        }

        if (is_redirect_status(statusCode)) {
            if (hop == AppUpdater::MAX_REDIRECT_HOPS) {
                outConn.close();
                return false;
            }
            std::wstring locationHeader;
            if (!query_header_wstring(outConn.hRequest, WINHTTP_QUERY_LOCATION, locationHeader)) {
                outConn.close();
                return false;
            }
            std::wstring nextUrl = resolve_redirect_url(cracked, locationHeader);
            outConn.close_request_and_connect();
            if (!AppUpdater::is_allowed_update_url(nextUrl)) {
                outConn.close();
                return false;
            }
            currentUrl = std::move(nextUrl);
            continue;
        }

        outConn.close();
        return false;
    }

    outConn.close();
    return false;
}

bool is_hex_64_lowercase(std::string_view token, std::string& outLower) {
    if (token.size() != 64) {
        return false;
    }
    outLower.clear();
    outLower.reserve(64);
    for (char c : token) {
        unsigned char uc = static_cast<unsigned char>(c);
        if (!std::isxdigit(uc)) {
            return false;
        }
        outLower.push_back(ascii_tolower(c));
    }
    return true;
}

bool line_mentions_quickdns_exe(std::string_view line) {
    constexpr std::string_view target = "QuickDNS.exe";
    size_t pos = 0;
    while (pos + target.size() <= line.size()) {
        bool found = false;
        size_t matchIdx = 0;
        for (size_t i = pos; i + target.size() <= line.size(); ++i) {
            if (equals_icase_ascii(line.substr(i, target.size()), target)) {
                found = true;
                matchIdx = i;
                break;
            }
        }
        if (!found) {
            return false;
        }

        // Check left boundary: start of line, whitespace, '*', '/', '\\', '(', '"'
        bool leftOk = (matchIdx == 0);
        if (!leftOk) {
            char prev = line[matchIdx - 1];
            leftOk = (prev == ' ' || prev == '\t' || prev == '*' ||
                      prev == '/' || prev == '\\' || prev == '(' || prev == '"');
        }

        // Check right boundary: end of line, whitespace, ')', '"' (rejects QuickDNS.exe.sha256 / QuickDNS.exe.zip)
        size_t afterIdx = matchIdx + target.size();
        bool rightOk = (afterIdx == line.size());
        if (!rightOk) {
            char next = line[afterIdx];
            rightOk = (next == ' ' || next == '\t' || next == ')' || next == '"');
        }

        if (leftOk && rightOk) {
            return true;
        }
        pos = matchIdx + 1;
    }
    return false;
}

std::string parse_sha256_manifest(std::string_view rawContent) {
    // Strip UTF-8 BOM if present
    if (rawContent.size() >= 3 &&
        static_cast<unsigned char>(rawContent[0]) == 0xEF &&
        static_cast<unsigned char>(rawContent[1]) == 0xBB &&
        static_cast<unsigned char>(rawContent[2]) == 0xBF) {
        rawContent.remove_prefix(3);
    }

    size_t nonEmptyLineCount = 0;
    bool anyLineHasExtraTokens = false;
    std::string singleBareHash;
    std::string matchedNamedHash;

    size_t lineStart = 0;
    while (lineStart <= rawContent.size()) {
        size_t nl = rawContent.find('\n', lineStart);
        std::string_view line = (nl == std::string_view::npos)
                                    ? rawContent.substr(lineStart)
                                    : rawContent.substr(lineStart, nl - lineStart);
        if (!line.empty() && line.back() == '\r') {
            line.remove_suffix(1);
        }
        while (!line.empty() && (line.front() == ' ' || line.front() == '\t')) {
            line.remove_prefix(1);
        }
        while (!line.empty() && (line.back() == ' ' || line.back() == '\t')) {
            line.remove_suffix(1);
        }

        if (!line.empty()) {
            ++nonEmptyLineCount;

            // Tokenize on whitespace and '=' to verify strict 64-hex token boundaries
            std::string lineHex;
            int hexTokenCount = 0;
            int otherTokenCount = 0;

            size_t i = 0;
            while (i < line.size()) {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '=')) {
                    ++i;
                }
                if (i >= line.size()) break;
                size_t tokStart = i;
                while (i < line.size() && line[i] != ' ' && line[i] != '\t' && line[i] != '=') {
                    ++i;
                }
                std::string_view tok = line.substr(tokStart, i - tokStart);
                std::string candidateHex;
                if (is_hex_64_lowercase(tok, candidateHex)) {
                    lineHex = std::move(candidateHex);
                    ++hexTokenCount;
                } else {
                    ++otherTokenCount;
                }
            }

            if (otherTokenCount > 0) {
                anyLineHasExtraTokens = true;
            }

            if (hexTokenCount == 1) {
                if (otherTokenCount == 0) {
                    singleBareHash = lineHex;
                } else if (line_mentions_quickdns_exe(line)) {
                    if (!matchedNamedHash.empty() && matchedNamedHash != lineHex) {
                        return ""; // Conflicting QuickDNS.exe entries
                    }
                    matchedNamedHash = std::move(lineHex);
                }
            }
        }

        if (nl == std::string_view::npos) break;
        lineStart = nl + 1;
    }

    if (!matchedNamedHash.empty()) {
        return matchedNamedHash;
    }
    if (nonEmptyLineCount == 1 && !anyLineHasExtraTokens && singleBareHash.size() == 64) {
        return singleBareHash;
    }
    return "";
}

bool compute_sha256_from_handle(
    HANDLE hFile,
    std::string& outHashHex,
    bool checkMzHeader,
    LONGLONG* outTotalBytesRead)
{
    LARGE_INTEGER zero = {};
    if (!SetFilePointerEx(hFile, zero, nullptr, FILE_BEGIN)) {
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status != 0) {
        return false;
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (status != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    BYTE buffer[32768];
    LONGLONG totalRead = 0;
    bool firstChunk = true;
    bool readOk = true;

    for (;;) {
        DWORD bytesRead = 0;
        if (!ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr)) {
            readOk = false;
            break;
        }
        if (bytesRead == 0) {
            break;
        }
        if (firstChunk && checkMzHeader) {
            if (bytesRead < 2 || buffer[0] != 'M' || buffer[1] != 'Z') {
                readOk = false;
                break;
            }
            firstChunk = false;
        }
        totalRead += static_cast<LONGLONG>(bytesRead);
        status = BCryptHashData(hHash, buffer, bytesRead, 0);
        if (status != 0) {
            readOk = false;
            break;
        }
    }

    if (!readOk || (checkMzHeader && firstChunk)) {
        BCryptDestroyHash(hHash);
        BCryptCloseAlgorithmProvider(hAlg, 0);
        return false;
    }

    BYTE hash[32] = {};
    status = BCryptFinishHash(hHash, hash, sizeof(hash), 0);
    BCryptDestroyHash(hHash);
    BCryptCloseAlgorithmProvider(hAlg, 0);

    if (status != 0) {
        return false;
    }

    static constexpr char kHexDigits[] = "0123456789abcdef";
    outHashHex.resize(64);
    for (size_t i = 0; i < 32; ++i) {
        outHashHex[i * 2] = kHexDigits[(hash[i] >> 4) & 0x0F];
        outHashHex[i * 2 + 1] = kHexDigits[hash[i] & 0x0F];
    }

    if (outTotalBytesRead) {
        *outTotalBytesRead = totalRead;
    }
    return true;
}

void clear_readonly_if_present(const std::wstring& path) {
    DWORD attrs = GetFileAttributesW(path.c_str());
    if (attrs != INVALID_FILE_ATTRIBUTES && (attrs & FILE_ATTRIBUTE_READONLY) != 0) {
        DWORD nextAttrs = attrs & ~FILE_ATTRIBUTE_READONLY;
        if (nextAttrs == 0) nextAttrs = FILE_ATTRIBUTE_NORMAL;
        SetFileAttributesW(path.c_str(), nextAttrs);
    }
}

std::wstring get_current_executable_path() {
    std::wstring buf(MAX_PATH, L'\0');
    while (buf.size() <= 32768) {
        DWORD len = GetModuleFileNameW(nullptr, buf.data(), static_cast<DWORD>(buf.size()));
        if (len == 0) {
            return L"";
        }
        if (len < buf.size()) {
            buf.resize(len);
            return buf;
        }
        buf.resize(buf.size() * 2, L'\0');
    }
    return L"";
}

} // anonymous namespace

std::vector<int> AppUpdater::parse_semver(const std::string& ver) {
    std::vector<int> parts;
    std::string clean = ver;
    if (!clean.empty() && (clean[0] == 'v' || clean[0] == 'V')) {
        clean.erase(0, 1);
    }

    size_t plus = clean.find('+');
    if (plus != std::string::npos) {
        clean.resize(plus);
    }

    size_t dash = clean.find('-');
    if (dash != std::string::npos) {
        clean.resize(dash);
    }

    std::stringstream ss(clean);
    std::string token;
    while (std::getline(ss, token, '.')) {
        if (is_all_digits(token)) {
            uint64_t val = parse_u64_clamped(token);
            parts.push_back(val > static_cast<uint64_t>(INT_MAX) ? INT_MAX : static_cast<int>(val));
        } else {
            parts.push_back(0);
        }
    }
    while (parts.size() < 3) {
        parts.push_back(0);
    }
    return parts;
}

bool AppUpdater::is_version_newer(const std::string& currentVer, const std::string& remoteVer) {
    ParsedSemver cur = parse_full_semver(currentVer);
    ParsedSemver rem = parse_full_semver(remoteVer);
    if (!cur.valid || !rem.valid) {
        return false;
    }

    size_t coreLen = std::max(cur.core.size(), rem.core.size());
    for (size_t i = 0; i < coreLen; ++i) {
        int c = (i < cur.core.size()) ? cur.core[i] : 0;
        int r = (i < rem.core.size()) ? rem.core[i] : 0;
        if (r > c) return true;
        if (r < c) return false;
    }

    // Core major.minor.patch are equal: handle pre-release precedence (2.1.0 > 2.1.0-rc1)
    bool curHasPre = !cur.prerelease.empty();
    bool remHasPre = !rem.prerelease.empty();

    if (!curHasPre && !remHasPre) {
        return false;
    }
    if (curHasPre && !remHasPre) {
        return true; // Stable release is newer than pre-release of the same version
    }
    if (!curHasPre && remHasPre) {
        return false; // Pre-release is older than stable release of the same version
    }

    size_t minPre = std::min(cur.prerelease.size(), rem.prerelease.size());
    for (size_t i = 0; i < minPre; ++i) {
        int cmp = compare_prerelease_identifiers(rem.prerelease[i], cur.prerelease[i]);
        if (cmp > 0) return true;
        if (cmp < 0) return false;
    }

    return rem.prerelease.size() > cur.prerelease.size();
}

std::optional<ReleaseInfo> AppUpdater::check_github_update(
    const std::string& currentVersion,
    const std::string& repoOwner,
    const std::string& repoName)
{
    if (!is_safe_identifier_ascii(repoOwner) || !is_safe_identifier_ascii(repoName)) {
        return std::nullopt;
    }

    std::wstring ownerW = QuickDns::Encoding::utf8_to_wide(repoOwner);
    std::wstring repoW = QuickDns::Encoding::utf8_to_wide(repoName);
    if (ownerW.empty() || repoW.empty()) {
        return std::nullopt;
    }

    std::wstring apiUrl = L"https://api.github.com/repos/" + ownerW + L"/" + repoW + L"/releases/latest";
    std::wstring apiHeaders = L"User-Agent: QuickDNS-AppUpdater\r\nAccept: application/vnd.github+json\r\n";

    std::string responseBody;
    {
        HttpGetConnection apiConn;
        if (open_https_get_with_validated_redirects(apiUrl, L"QuickDNS-AppUpdater/2.1", apiHeaders, 10000, apiConn)) {
            bool hasCl = false;
            uint64_t contentLen = 0;
            bool readOk = query_content_length_64(apiConn.hRequest, hasCl, contentLen) &&
                          (!hasCl || (contentLen > 0 && contentLen <= MAX_API_RESPONSE_BYTES));

            if (readOk) {
                char buffer[8192];
                for (;;) {
                    DWORD bytesRead = 0;
                    if (!WinHttpReadData(apiConn.hRequest, buffer, sizeof(buffer), &bytesRead)) {
                        readOk = false;
                        break;
                    }
                    if (bytesRead == 0) {
                        break;
                    }
                    if (responseBody.size() + bytesRead > MAX_API_RESPONSE_BYTES ||
                        (hasCl && responseBody.size() + bytesRead > contentLen)) {
                        readOk = false;
                        break;
                    }
                    responseBody.append(buffer, bytesRead);
                }
                if (readOk && hasCl && responseBody.size() != static_cast<size_t>(contentLen)) {
                    readOk = false;
                }
            }
            if (!readOk) {
                responseBody.clear();
            }
        }
    }

    if (responseBody.empty()) {
        // Fallback: Query https://github.com/{owner}/{repo}/releases/latest with HEAD request
        // Bypasses api.github.com REST rate limits (e.g. HTTP 403 on shared SmartDNS IPs)
        HttpGetConnection fbConn;
        fbConn.hSession = WinHttpOpen(
            L"QuickDNS-AppUpdater/2.1",
            WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
            WINHTTP_NO_PROXY_NAME,
            WINHTTP_NO_PROXY_BYPASS,
            0
        );
        if (!fbConn.hSession) {
            return std::nullopt;
        }

        DWORD redirectPolicyNever = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
        if (!WinHttpSetOption(fbConn.hSession, WINHTTP_OPTION_REDIRECT_POLICY,
                              &redirectPolicyNever, sizeof(redirectPolicyNever))) {
            return std::nullopt;
        }

        DWORD fbTo = 10000;
        WinHttpSetTimeouts(fbConn.hSession, fbTo, fbTo, fbTo, fbTo);

        fbConn.hConnect = WinHttpConnect(fbConn.hSession, L"github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
        if (!fbConn.hConnect) {
            return std::nullopt;
        }

        std::wstring fbPath = L"/" + ownerW + L"/" + repoW + L"/releases/latest";
        fbConn.hRequest = WinHttpOpenRequest(
            fbConn.hConnect,
            L"HEAD",
            fbPath.c_str(),
            nullptr,
            WINHTTP_NO_REFERER,
            WINHTTP_DEFAULT_ACCEPT_TYPES,
            WINHTTP_FLAG_SECURE
        );
        if (!fbConn.hRequest) {
            return std::nullopt;
        }

        std::wstring fbHeaders = L"User-Agent: QuickDNS-AppUpdater\r\nAccept: */*\r\n";
        std::wstring locHeader;
        if (WinHttpSendRequest(fbConn.hRequest, fbHeaders.c_str(), static_cast<DWORD>(fbHeaders.size()), nullptr, 0, 0, 0) &&
            WinHttpReceiveResponse(fbConn.hRequest, nullptr))
        {
            DWORD statusCode = 0;
            DWORD statusSz = sizeof(statusCode);
            if (WinHttpQueryHeaders(fbConn.hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                                    WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &statusSz, WINHTTP_NO_HEADER_INDEX) &&
                is_redirect_status(statusCode))
            {
                query_header_wstring(fbConn.hRequest, WINHTTP_QUERY_LOCATION, locHeader);
            }
        }
        fbConn.close();

        locHeader = trim_http_ows(locHeader);
        if (!locHeader.empty() && locHeader[0] == L'/') {
            locHeader = L"https://github.com" + locHeader;
        }
        if (locHeader.empty() || !is_allowed_update_url(locHeader)) {
            return std::nullopt;
        }

        std::wstring expectedPrefix = L"https://github.com/" + ownerW + L"/" + repoW + L"/releases/tag/";
        if (!starts_with_icase_w(locHeader, expectedPrefix)) {
            return std::nullopt;
        }

        std::wstring tagW = locHeader.substr(expectedPrefix.size());
        if (!is_safe_tag_wide(tagW)) {
            return std::nullopt;
        }

        std::string tag = QuickDns::Encoding::wide_to_utf8(tagW);
        if (tag.empty()) {
            return std::nullopt;
        }

        std::string semver = tag;
        if (!semver.empty() && (semver[0] == 'v' || semver[0] == 'V')) {
            semver.erase(0, 1);
        }

        ParsedSemver parsed = parse_full_semver(semver);
        if (!parsed.valid || !parsed.prerelease.empty()) {
            return std::nullopt;
        }

        ReleaseInfo fbInfo;
        fbInfo.tag_name = tag;
        fbInfo.title = "QuickDNS " + tag;
        fbInfo.notes = "";
        fbInfo.semver = semver;
        fbInfo.download_url = "https://github.com/" + repoOwner + "/" + repoName + "/releases/download/" + tag + "/QuickDNS.exe";
        fbInfo.sha256_url = "https://github.com/" + repoOwner + "/" + repoName + "/releases/download/" + tag + "/QuickDNS.exe.sha256";
        fbInfo.is_newer = is_version_newer(currentVersion, fbInfo.semver);
        return fbInfo;
    }

    auto j = nlohmann::json::parse(responseBody, nullptr, false);
    if (j.is_discarded() || !j.is_object()) {
        std::wcerr << L"Не удалось разобрать ответ сервера обновлений." << std::endl;
        return std::nullopt;
    }

    // Reject draft or prerelease releases
    if ((j.contains("draft") && (!j["draft"].is_boolean() || j["draft"].get<bool>())) ||
        (j.contains("prerelease") && (!j["prerelease"].is_boolean() || j["prerelease"].get<bool>()))) {
        return std::nullopt;
    }

    ReleaseInfo info;
    std::string tag_name = (j.contains("tag_name") && j["tag_name"].is_string()) ? j["tag_name"].get<std::string>() : "";
    std::string body = (j.contains("body") && j["body"].is_string()) ? j["body"].get<std::string>() : "";
    info.title = (j.contains("name") && j["name"].is_string()) ? j["name"].get<std::string>() : "";

    if (!is_safe_identifier_ascii(tag_name)) {
        return std::nullopt;
    }

    std::string exeUrl;
    std::string shaUrl;

    if (j.contains("assets") && j["assets"].is_array()) {
        for (const auto& asset : j["assets"]) {
            if (!asset.is_object()) continue;
            std::string assetName = (asset.contains("name") && asset["name"].is_string())
                                        ? asset["name"].get<std::string>()
                                        : "";
            std::string dlUrl = (asset.contains("browser_download_url") && asset["browser_download_url"].is_string())
                                    ? asset["browser_download_url"].get<std::string>()
                                    : "";

            if (assetName == "QuickDNS.exe") {
                if (dlUrl.empty()) continue;
                std::wstring dlUrlW = QuickDns::Encoding::utf8_to_wide(dlUrl);
                if (!is_valid_release_asset_url(dlUrlW, ownerW, repoW)) {
                    continue;
                }
                if (asset.contains("size")) {
                    if (!asset["size"].is_number_unsigned()) {
                        continue;
                    }
                    uint64_t rawSize = asset["size"].get<uint64_t>();
                    if (rawSize <= MIN_UPDATE_EXE_BYTES || rawSize > MAX_UPDATE_EXE_BYTES) {
                        continue;
                    }
                    info.asset_size = static_cast<size_t>(rawSize);
                }
                exeUrl = std::move(dlUrl);
            } else if (assetName == "QuickDNS.exe.sha256") {
                if (dlUrl.empty()) continue;
                std::wstring shaUrlW = QuickDns::Encoding::utf8_to_wide(dlUrl);
                if (!is_valid_release_asset_url(shaUrlW, ownerW, repoW)) {
                    continue;
                }
                if (asset.contains("size")) {
                    if (!asset["size"].is_number_unsigned()) {
                        continue;
                    }
                    uint64_t shaSize = asset["size"].get<uint64_t>();
                    if (shaSize == 0 || shaSize > MAX_SHA256_RESPONSE_BYTES) {
                        continue;
                    }
                }
                shaUrl = std::move(dlUrl);
            }
        }
    }

    if (exeUrl.empty() || shaUrl.empty()) {
        return std::nullopt;
    }

    std::wstring tag_name_w = QuickDns::Encoding::utf8_to_wide(tag_name);
    std::wstring body_w = normalize_crlf(QuickDns::Encoding::utf8_to_wide(body));
    std::wstring exeUrl_w = QuickDns::Encoding::utf8_to_wide(exeUrl);
    std::wstring shaUrl_w = QuickDns::Encoding::utf8_to_wide(shaUrl);

    info.tag_name = QuickDns::Encoding::wide_to_utf8(tag_name_w);
    info.notes = QuickDns::Encoding::wide_to_utf8(body_w);
    info.download_url = QuickDns::Encoding::wide_to_utf8(exeUrl_w);
    info.sha256_url = QuickDns::Encoding::wide_to_utf8(shaUrl_w);

    if (info.tag_name.empty()) {
        return std::nullopt;
    }

    info.semver = info.tag_name;
    if (!info.semver.empty() && (info.semver[0] == 'v' || info.semver[0] == 'V')) {
        info.semver.erase(0, 1);
    }

    ParsedSemver parsed = parse_full_semver(info.semver);
    if (!parsed.valid || !parsed.prerelease.empty()) {
        return std::nullopt;
    }

    info.is_newer = is_version_newer(currentVersion, info.semver);
    return info;
}

bool AppUpdater::is_allowed_update_url(const std::wstring& url) {
    CrackedHttpsUrl cracked;
    if (!crack_https_url(url, cracked)) {
        return false;
    }
    return is_allowed_host(cracked.host);
}

std::string AppUpdater::download_sha256(const std::wstring& sha256Url) {
    if (sha256Url.empty() || !is_allowed_update_url(sha256Url)) {
        return "";
    }

    std::wstring headers = L"User-Agent: QuickDNS-Downloader\r\nAccept: text/plain, */*\r\n";
    HttpGetConnection conn;
    if (!open_https_get_with_validated_redirects(sha256Url, L"QuickDNS-Downloader/2.1", headers, 15000, conn)) {
        return "";
    }

    bool hasCl = false;
    uint64_t contentLen = 0;
    if (!query_content_length_64(conn.hRequest, hasCl, contentLen) ||
        (hasCl && (contentLen == 0 || contentLen > MAX_SHA256_RESPONSE_BYTES))) {
        return "";
    }

    std::string content;
    content.reserve(256);
    char buf[1024];
    for (;;) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(conn.hRequest, buf, sizeof(buf), &bytesRead)) {
            return "";
        }
        if (bytesRead == 0) {
            break;
        }
        if (content.size() + bytesRead > MAX_SHA256_RESPONSE_BYTES ||
            (hasCl && content.size() + bytesRead > contentLen)) {
            return "";
        }
        content.append(buf, bytesRead);
    }

    if (content.empty() || (hasCl && content.size() != static_cast<size_t>(contentLen))) {
        return "";
    }

    return parse_sha256_manifest(content);
}

bool AppUpdater::compute_sha256(const std::wstring& filePath, std::string& outHashHex) {
    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    bool ok = compute_sha256_from_handle(hFile, outHashHex, false, nullptr);
    CloseHandle(hFile);
    return ok;
}

bool AppUpdater::verify_downloaded_executable(const std::wstring& filePath, const std::string& expectedSha256) {
    std::string expectedLower;
    if (!is_hex_64_lowercase(expectedSha256, expectedLower)) {
        return false;
    }

    HANDLE hFile = CreateFileW(
        filePath.c_str(),
        GENERIC_READ,
        FILE_SHARE_READ,
        nullptr,
        OPEN_EXISTING,
        FILE_ATTRIBUTE_NORMAL | FILE_FLAG_SEQUENTIAL_SCAN,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        return false;
    }

    // Binary must be strictly > 100 KB and <= 25 MB
    if (fileSize.QuadPart <= static_cast<LONGLONG>(MIN_UPDATE_EXE_BYTES) ||
        fileSize.QuadPart > static_cast<LONGLONG>(MAX_UPDATE_EXE_BYTES)) {
        CloseHandle(hFile);
        return false;
    }

    std::string actualHash;
    LONGLONG totalBytesRead = 0;
    bool hashOk = compute_sha256_from_handle(hFile, actualHash, true, &totalBytesRead);
    CloseHandle(hFile);

    if (!hashOk || totalBytesRead != fileSize.QuadPart || actualHash.size() != 64) {
        return false;
    }

    // Constant-time comparison across all 64 lowercase hex characters
    volatile unsigned char diff = 0;
    for (size_t i = 0; i < 64; ++i) {
        unsigned char a = static_cast<unsigned char>(actualHash[i]);
        unsigned char b = static_cast<unsigned char>(expectedLower[i]);
        diff = static_cast<unsigned char>(diff | (a ^ b));
    }
    return (diff == 0);
}

bool AppUpdater::download_file(
    const std::wstring& url,
    const std::wstring& destPath,
    std::function<void(DWORD downloaded, DWORD total)> onProgress)
{
    if (!is_allowed_update_url(url)) {
        return false;
    }

    std::wstring headers = L"User-Agent: QuickDNS-Downloader\r\nAccept: application/octet-stream\r\n";
    HttpGetConnection conn;
    if (!open_https_get_with_validated_redirects(url, L"QuickDNS-Downloader/2.1", headers, 30000, conn)) {
        return false;
    }

    bool hasContentLength = false;
    uint64_t contentLength64 = 0;
    if (!query_content_length_64(conn.hRequest, hasContentLength, contentLength64)) {
        return false;
    }
    if (hasContentLength && (contentLength64 == 0 || contentLength64 > MAX_UPDATE_EXE_BYTES)) {
        return false;
    }

    clear_readonly_if_present(destPath);
    HANDLE hFile = CreateFileW(
        destPath.c_str(),
        GENERIC_WRITE,
        0,
        nullptr,
        CREATE_ALWAYS,
        FILE_ATTRIBUTE_NORMAL,
        nullptr
    );
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    char buffer[32768];
    uint64_t totalDownloaded = 0;
    DWORD progressTotal = (hasContentLength && contentLength64 <= static_cast<uint64_t>(MAXDWORD))
                              ? static_cast<DWORD>(contentLength64)
                              : 0;

    bool success = true;
    for (;;) {
        DWORD bytesRead = 0;
        if (!WinHttpReadData(conn.hRequest, buffer, sizeof(buffer), &bytesRead)) {
            success = false;
            break;
        }
        if (bytesRead == 0) {
            break;
        }
        if (totalDownloaded + bytesRead > MAX_UPDATE_EXE_BYTES ||
            (hasContentLength && totalDownloaded + bytesRead > contentLength64)) {
            success = false;
            break;
        }

        DWORD bytesWritten = 0;
        if (!WriteFile(hFile, buffer, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead) {
            success = false;
            break;
        }

        totalDownloaded += bytesWritten;
        if (onProgress) {
            onProgress(static_cast<DWORD>(totalDownloaded), progressTotal);
        }
    }

    if (success && totalDownloaded == 0) {
        success = false;
    }
    if (success && hasContentLength && totalDownloaded != contentLength64) {
        success = false;
    }

    if (success) {
        FlushFileBuffers(hFile);
        CloseHandle(hFile);
    } else {
        CloseHandle(hFile);
        DeleteFileW(destPath.c_str());
    }

    return success;
}

bool AppUpdater::perform_file_replacement(
    const std::wstring& targetExePath,
    const std::wstring& downloadedExePath,
    const std::wstring& oldExePath,
    std::wstring* outError)
{
    auto set_err = [&](const std::wstring& actionDesc, DWORD err) {
        if (outError) {
            *outError = actionDesc + L" (код ошибки: " + std::to_wstring(err) + L")";
        }
    };

    auto retry_op = [](const std::function<bool(DWORD& outAttemptErr)>& op,
                       int maxAttempts,
                       DWORD sleepMs,
                       DWORD& outLastErr,
                       bool stopOnMissingSource = false) -> bool {
        outLastErr = ERROR_SUCCESS;
        for (int i = 0; i < maxAttempts; ++i) {
            DWORD attemptErr = ERROR_SUCCESS;
            if (op(attemptErr)) {
                outLastErr = ERROR_SUCCESS;
                return true;
            }
            outLastErr = attemptErr;
            if (stopOnMissingSource &&
                (attemptErr == ERROR_FILE_NOT_FOUND || attemptErr == ERROR_PATH_NOT_FOUND)) {
                break;
            }
            if (i + 1 < maxAttempts) {
                Sleep(sleepMs);
            }
        }
        return false;
    };

    // 1. Remove previous .old backup file if it exists
    if (GetFileAttributesW(oldExePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        DWORD delErr = ERROR_SUCCESS;
        bool delOldOk = retry_op([&](DWORD& attemptErr) {
            clear_readonly_if_present(oldExePath);
            if (DeleteFileW(oldExePath.c_str())) {
                return true;
            }
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                return true;
            }
            attemptErr = err;
            return false;
        }, 20, 150, delErr);

        if (!delOldOk) {
            set_err(L"Не удалось удалить предыдущую резервную копию (.old)", delErr);
            return false;
        }
    }

    // 2. Rename current target executable -> oldExePath
    DWORD renameErr = ERROR_SUCCESS;
    bool renameTargetOk = retry_op([&](DWORD& attemptErr) {
        clear_readonly_if_present(targetExePath);
        clear_readonly_if_present(oldExePath);
        if (MoveFileExW(targetExePath.c_str(), oldExePath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        attemptErr = GetLastError();
        return false;
    }, 20, 150, renameErr, true);

    if (!renameTargetOk) {
        set_err(L"Не удалось создать резервную копию текущего файла (.old)", renameErr);
        return false;
    }

    // 3. Move downloaded executable -> targetExePath
    DWORD moveErr = ERROR_SUCCESS;
    bool moveNewOk = retry_op([&](DWORD& attemptErr) {
        clear_readonly_if_present(downloadedExePath);
        clear_readonly_if_present(targetExePath);
        if (MoveFileExW(downloadedExePath.c_str(), targetExePath.c_str(),
                        MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
            return true;
        }
        attemptErr = GetLastError();
        return false;
    }, 20, 150, moveErr, true);

    if (!moveNewOk &&
        moveErr != ERROR_FILE_NOT_FOUND &&
        moveErr != ERROR_PATH_NOT_FOUND) {
        // Fallback: CopyFileW followed by DeleteFileW
        DWORD copyErr = ERROR_SUCCESS;
        moveNewOk = retry_op([&](DWORD& attemptErr) {
            clear_readonly_if_present(targetExePath);
            if (CopyFileW(downloadedExePath.c_str(), targetExePath.c_str(), FALSE)) {
                clear_readonly_if_present(downloadedExePath);
                if (!DeleteFileW(downloadedExePath.c_str())) {
                    MoveFileExW(downloadedExePath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                }
                return true;
            }
            attemptErr = GetLastError();
            return false;
        }, 10, 150, copyErr, true);
        if (!moveNewOk && copyErr != ERROR_SUCCESS) {
            moveErr = copyErr;
        }
    }

    if (!moveNewOk) {
        set_err(L"Не удалось установить новый исполняемый файл", moveErr);

        // Rollback: restore oldExePath -> targetExePath
        DWORD rollbackErr = ERROR_SUCCESS;
        retry_op([&](DWORD& attemptErr) {
            clear_readonly_if_present(oldExePath);
            clear_readonly_if_present(targetExePath);
            if (MoveFileExW(oldExePath.c_str(), targetExePath.c_str(),
                            MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                return true;
            }
            attemptErr = GetLastError();
            return false;
        }, 20, 150, rollbackErr);

        return false;
    }

    return true;
}

bool AppUpdater::apply_update_and_restart(
    const std::wstring& downloadedExePath,
    std::wstring* outError,
    bool launchMinimized,
    bool noElevate)
{
    std::wstring exePath = get_current_executable_path();
    if (exePath.empty()) {
        if (outError) {
            *outError = L"Не удалось определить путь к текущему исполняемому файлу.";
        }
        return false;
    }

    std::wstring exeDir;
    size_t lastSlash = exePath.find_last_of(L"\\/");
    if (lastSlash != std::wstring::npos) {
        exeDir = exePath.substr(0, lastSlash);
    }

    std::wstring oldExe = exePath + L".old";

    // 1 & 2. Atomically replace file with retries and rollback
    if (!perform_file_replacement(exePath, downloadedExePath, oldExe, outError)) {
        return false;
    }

    // 3. Launch newly updated application with --updated --pid <currentPid> and optional flags
    DWORD currentPid = GetCurrentProcessId();
    std::wstring args = L"--updated --pid " + std::to_wstring(currentPid);
    if (launchMinimized) {
        args += L" --minimized";
    }
    if (noElevate) {
        args += L" --no-elevate";
    }

    std::wstring cmdLine = L"\"" + exePath + L"\" " + args;
    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
    if (launchMinimized) {
        si.dwFlags = STARTF_USESHOWWINDOW;
        si.wShowWindow = SW_HIDE;
    }
    PROCESS_INFORMATION pi = {};

    bool launched = false;
    DWORD launchErr = ERROR_SUCCESS;

    if (CreateProcessW(
            exePath.c_str(),
            cmdBuffer.data(),
            nullptr,
            nullptr,
            FALSE,
            0,
            nullptr,
            exeDir.empty() ? nullptr : exeDir.c_str(),
            &si,
            &pi)) {
        CloseHandle(pi.hProcess);
        CloseHandle(pi.hThread);
        launched = true;
    } else {
        launchErr = GetLastError();
        // Fallback to ShellExecuteExW (handles UAC elevation prompt if needed)
        SHELLEXECUTEINFOW sei = {};
        sei.cbSize = sizeof(sei);
        sei.fMask = SEE_MASK_FLAG_NO_UI;
        sei.lpVerb = L"open";
        sei.lpFile = exePath.c_str();
        sei.lpParameters = args.c_str();
        sei.lpDirectory = exeDir.empty() ? nullptr : exeDir.c_str();
        sei.nShow = launchMinimized ? SW_HIDE : SW_SHOWNORMAL;

        if (ShellExecuteExW(&sei)) {
            launched = true;
        } else {
            DWORD shellErr = GetLastError();
            if (shellErr != ERROR_SUCCESS) {
                launchErr = shellErr;
            }
        }
    }

    if (!launched) {
        if (outError) {
            *outError = L"Не удалось запустить обновленное приложение (код ошибки: " + std::to_wstring(launchErr) + L")";
        }
        // Rollback: restore oldExe -> exePath
        for (int i = 0; i < 20; ++i) {
            clear_readonly_if_present(oldExe);
            clear_readonly_if_present(exePath);
            if (MoveFileExW(oldExe.c_str(), exePath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                break;
            }
            if (i + 1 < 20) {
                Sleep(150);
            }
        }
        return false;
    }

    // Do NOT call ExitProcess(0) on the background thread; return true so MainWindow
    // can remove its tray icon, destroy its window, and exit its message loop cleanly.
    return true;
}

void AppUpdater::cleanup_previous_update() {
    std::wstring exePath = get_current_executable_path();
    if (exePath.empty()) {
        return;
    }

    std::wstring oldExe = exePath + L".old";
    if (GetFileAttributesW(oldExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
        for (int i = 0; i < 20; ++i) {
            clear_readonly_if_present(oldExe);
            if (DeleteFileW(oldExe.c_str())) {
                break;
            }
            DWORD err = GetLastError();
            if (err == ERROR_FILE_NOT_FOUND || err == ERROR_PATH_NOT_FOUND) {
                break;
            }
            if (i + 1 < 20) {
                Sleep(100);
            }
        }
    }
}

} // namespace quickdns
