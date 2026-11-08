#include "app_updater.h"
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cctype>

#pragma comment(lib, "winhttp.lib")
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "bcrypt.lib")

namespace quickdns {

namespace {

std::string extract_json_field(const std::string& json, const std::string& key) {
    std::string searchKey = "\"" + key + "\"";
    size_t pos = json.find(searchKey);
    if (pos == std::string::npos) return "";

    pos = json.find(':', pos);
    if (pos == std::string::npos) return "";

    pos++;
    while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t' || json[pos] == '\r' || json[pos] == '\n')) {
        pos++;
    }
    if (pos >= json.size()) return "";

    if (json[pos] == '"') {
        size_t endPos = pos + 1;
        while (endPos < json.size()) {
            if (json[endPos] == '"' && json[endPos - 1] != '\\') {
                break;
            }
            endPos++;
        }
        return json.substr(pos + 1, endPos - pos - 1);
    } else {
        size_t endPos = pos;
        while (endPos < json.size() && json[endPos] != ',' && json[endPos] != '}' && json[endPos] != '\n' && json[endPos] != '\r') {
            endPos++;
        }
        std::string val = json.substr(pos, endPos - pos);
        val.erase(std::remove_if(val.begin(), val.end(), ::isspace), val.end());
        return val;
    }
}

std::wstring utf8_to_wide(const std::string& str) {
    if (str.empty()) return L"";
    int sz = MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), nullptr, 0);
    std::wstring out(sz, 0);
    MultiByteToWideChar(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), &out[0], sz);
    return out;
}

std::string wide_to_utf8(const std::wstring& str) {
    if (str.empty()) return "";
    int sz = WideCharToMultiByte(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), nullptr, 0, nullptr, nullptr);
    std::string out(sz, 0);
    WideCharToMultiByte(CP_UTF8, 0, str.c_str(), static_cast<int>(str.length()), &out[0], sz, nullptr, nullptr);
    return out;
}

} // anonymous namespace

std::vector<int> AppUpdater::parse_semver(const std::string& ver) {
    std::vector<int> parts;
    std::string clean = ver;
    if (!clean.empty() && (clean[0] == 'v' || clean[0] == 'V')) {
        clean = clean.substr(1);
    }
    // Remove everything after '-' (e.g. pre-release tag like -rc1)
    size_t dash = clean.find('-');
    if (dash != std::string::npos) {
        clean = clean.substr(0, dash);
    }

    std::stringstream ss(clean);
    std::string token;
    while (std::getline(ss, token, '.')) {
        try {
            parts.push_back(std::stoi(token));
        } catch (...) {
            parts.push_back(0);
        }
    }
    while (parts.size() < 3) {
        parts.push_back(0);
    }
    return parts;
}

bool AppUpdater::is_version_newer(const std::string& currentVer, const std::string& remoteVer) {
    auto cur = parse_semver(currentVer);
    auto rem = parse_semver(remoteVer);

    for (size_t i = 0; i < 3; ++i) {
        if (rem[i] > cur[i]) return true;
        if (rem[i] < cur[i]) return false;
    }
    return false;
}

std::optional<ReleaseInfo> AppUpdater::check_github_update(
    const std::string& currentVersion,
    const std::string& repoOwner,
    const std::string& repoName)
{
    HINTERNET hSession = WinHttpOpen(
        L"QuickDNS-AppUpdater/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0
    );
    if (!hSession) return std::nullopt;

    DWORD to = 10000;
    WinHttpSetTimeouts(hSession, to, to, to, to);

    // Follow redirects automatically, disallowing downgrade to HTTP
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    HINTERNET hConnect = WinHttpConnect(hSession, L"api.github.com", INTERNET_DEFAULT_HTTPS_PORT, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return std::nullopt;
    }

    std::wstring path = L"/repos/" + utf8_to_wide(repoOwner) + L"/" + utf8_to_wide(repoName) + L"/releases/latest";
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return std::nullopt;
    }

    std::wstring headers = L"User-Agent: QuickDNS-AppUpdater\r\nAccept: application/vnd.github+json\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.length()), nullptr, 0, 0, 0);

    std::string responseBody;
    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0;
        DWORD sz = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 200) {
            char buffer[8192];
            DWORD bytesRead = 0;
            while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                responseBody.append(buffer, bytesRead);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    if (responseBody.empty()) return std::nullopt;

    ReleaseInfo info;
    info.tag_name = extract_json_field(responseBody, "tag_name");
    info.title = extract_json_field(responseBody, "name");
    info.notes = extract_json_field(responseBody, "body");

    if (info.tag_name.empty()) return std::nullopt;

    info.semver = info.tag_name;
    if (!info.semver.empty() && (info.semver[0] == 'v' || info.semver[0] == 'V')) {
        info.semver = info.semver.substr(1);
    }

    // Look for QuickDNS.exe asset download URL
    size_t assetPos = responseBody.find("\"name\":\"QuickDNS.exe\"");
    if (assetPos == std::string::npos) {
        assetPos = responseBody.find("\"name\": \"QuickDNS.exe\"");
    }

    if (assetPos != std::string::npos) {
        size_t urlKey = responseBody.find("\"browser_download_url\"", assetPos);
        if (urlKey != std::string::npos) {
            info.download_url = extract_json_field(responseBody.substr(urlKey, 400), "browser_download_url");
        }
        size_t sizeKey = responseBody.find("\"size\"", assetPos);
        if (sizeKey != std::string::npos && (urlKey == std::string::npos || sizeKey < urlKey + 500)) {
            std::string sizeStr = extract_json_field(responseBody.substr(sizeKey, 100), "size");
            if (!sizeStr.empty()) {
                try { info.asset_size = std::stoull(sizeStr); } catch (...) {}
            }
        }
    }

    // Look for QuickDNS.exe.sha256 checksum asset URL
    size_t shaPos = responseBody.find("\"name\":\"QuickDNS.exe.sha256\"");
    if (shaPos == std::string::npos) {
        shaPos = responseBody.find("\"name\": \"QuickDNS.exe.sha256\"");
    }
    if (shaPos != std::string::npos) {
        size_t urlKey = responseBody.find("\"browser_download_url\"", shaPos);
        if (urlKey != std::string::npos) {
            info.sha256_url = extract_json_field(responseBody.substr(urlKey, 400), "browser_download_url");
        }
    }

    info.is_newer = is_version_newer(currentVersion, info.semver);
    return info;
}

bool AppUpdater::is_allowed_update_url(const std::wstring& url) {
    if (url.empty()) return false;
    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = _countof(host);

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.length()), 0, &urlComp)) {
        return false;
    }

    if (urlComp.nScheme != INTERNET_SCHEME_HTTPS) {
        return false;
    }

    std::wstring hostStr(host, urlComp.dwHostNameLength);
    std::transform(hostStr.begin(), hostStr.end(), hostStr.begin(), ::towlower);

    return (hostStr == L"github.com" ||
            hostStr == L"api.github.com" ||
            hostStr == L"objects.githubusercontent.com" ||
            hostStr == L"release-assets.githubusercontent.com");
}

std::string AppUpdater::download_sha256(const std::wstring& sha256Url) {
    if (sha256Url.empty() || !is_allowed_update_url(sha256Url)) {
        return "";
    }

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = _countof(host);
    urlComp.lpszUrlPath = path;
    urlComp.dwUrlPathLength = _countof(path);

    if (!WinHttpCrackUrl(sha256Url.c_str(), static_cast<DWORD>(sha256Url.length()), 0, &urlComp)) {
        return "";
    }

    HINTERNET hSession = WinHttpOpen(
        L"QuickDNS-Downloader/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0
    );
    if (!hSession) return "";

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    DWORD to = 15000;
    WinHttpSetTimeouts(hSession, to, to, to, to);

    HINTERNET hConnect = WinHttpConnect(hSession, host, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return "";
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return "";
    }

    std::wstring headers = L"User-Agent: QuickDNS-Downloader\r\nAccept: text/plain, */*\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.length()), nullptr, 0, 0, 0);

    std::string content;
    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0;
        DWORD sz = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 200) {
            char buf[1024] = {};
            DWORD bytesRead = 0;
            if (WinHttpReadData(hRequest, buf, sizeof(buf), &bytesRead) && bytesRead > 0) {
                content.append(buf, bytesRead);
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);

    // Extract 64-char hex string
    std::string hexHash;
    for (char c : content) {
        if (std::isxdigit(static_cast<unsigned char>(c))) {
            hexHash.push_back(static_cast<char>(std::tolower(static_cast<unsigned char>(c))));
            if (hexHash.length() == 64) break;
        } else if (!hexHash.empty()) {
            if (hexHash.length() == 64) break;
            hexHash.clear();
        }
    }

    return (hexHash.length() == 64) ? hexHash : "";
}

bool AppUpdater::compute_sha256(const std::wstring& filePath, std::string& outHashHex) {
    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    BCRYPT_ALG_HANDLE hAlg = nullptr;
    NTSTATUS status = BCryptOpenAlgorithmProvider(&hAlg, BCRYPT_SHA256_ALGORITHM, nullptr, 0);
    if (status != 0) {
        CloseHandle(hFile);
        return false;
    }

    BCRYPT_HASH_HANDLE hHash = nullptr;
    status = BCryptCreateHash(hAlg, &hHash, nullptr, 0, nullptr, 0, 0);
    if (status != 0) {
        BCryptCloseAlgorithmProvider(hAlg, 0);
        CloseHandle(hFile);
        return false;
    }

    BYTE buffer[32768];
    DWORD bytesRead = 0;
    bool readOk = true;
    while (ReadFile(hFile, buffer, sizeof(buffer), &bytesRead, nullptr) && bytesRead > 0) {
        status = BCryptHashData(hHash, buffer, bytesRead, 0);
        if (status != 0) {
            readOk = false;
            break;
        }
    }
    CloseHandle(hFile);

    if (!readOk) {
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

    char hexBuf[65] = {};
    for (int i = 0; i < 32; ++i) {
        snprintf(&hexBuf[i * 2], 3, "%02x", hash[i]);
    }
    outHashHex = hexBuf;
    return true;
}

bool AppUpdater::verify_downloaded_executable(const std::wstring& filePath, const std::string& expectedSha256) {
    if (expectedSha256.length() != 64) {
        return false;
    }

    HANDLE hFile = CreateFileW(filePath.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr,
                               OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile == INVALID_HANDLE_VALUE) {
        return false;
    }

    LARGE_INTEGER fileSize = {};
    if (!GetFileSizeEx(hFile, &fileSize)) {
        CloseHandle(hFile);
        return false;
    }

    // Binary must be larger than 100 KB
    if (fileSize.QuadPart <= 102400) {
        CloseHandle(hFile);
        return false;
    }

    // Must start with DOS MZ header
    char magic[2] = {};
    DWORD readBytes = 0;
    if (!ReadFile(hFile, magic, 2, &readBytes, nullptr) || readBytes < 2) {
        CloseHandle(hFile);
        return false;
    }
    CloseHandle(hFile);

    if (magic[0] != 'M' || magic[1] != 'Z') {
        return false;
    }

    std::string actualHash;
    if (!compute_sha256(filePath, actualHash)) {
        return false;
    }

    std::string expectedLower = expectedSha256;
    for (char& c : expectedLower) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (char& c : actualHash) c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));

    return (expectedLower == actualHash);
}

bool AppUpdater::download_file(
    const std::wstring& url,
    const std::wstring& destPath,
    std::function<void(DWORD downloaded, DWORD total)> onProgress)
{
    if (!is_allowed_update_url(url)) {
        return false;
    }

    URL_COMPONENTS urlComp = {};
    urlComp.dwStructSize = sizeof(urlComp);
    wchar_t host[256] = {};
    wchar_t path[2048] = {};
    urlComp.lpszHostName = host;
    urlComp.dwHostNameLength = 256;
    urlComp.lpszUrlPath = path;
    urlComp.dwUrlPathLength = 2048;

    if (!WinHttpCrackUrl(url.c_str(), static_cast<DWORD>(url.length()), 0, &urlComp)) {
        return false;
    }

    HINTERNET hSession = WinHttpOpen(
        L"QuickDNS-Downloader/2.0",
        WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
        WINHTTP_NO_PROXY_NAME,
        WINHTTP_NO_PROXY_BYPASS, 0
    );
    if (!hSession) return false;

    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_DISALLOW_HTTPS_TO_HTTP;
    WinHttpSetOption(hSession, WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy));

    DWORD to = 30000;
    WinHttpSetTimeouts(hSession, to, to, to, to);

    HINTERNET hConnect = WinHttpConnect(hSession, host, urlComp.nPort, 0);
    if (!hConnect) {
        WinHttpCloseHandle(hSession);
        return false;
    }

    DWORD flags = (urlComp.nScheme == INTERNET_SCHEME_HTTPS) ? WINHTTP_FLAG_SECURE : 0;
    HINTERNET hRequest = WinHttpOpenRequest(
        hConnect, L"GET", path, nullptr,
        WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, flags
    );

    if (!hRequest) {
        WinHttpCloseHandle(hConnect);
        WinHttpCloseHandle(hSession);
        return false;
    }

    std::wstring headers = L"User-Agent: QuickDNS-Downloader\r\nAccept: application/octet-stream\r\n";
    BOOL sent = WinHttpSendRequest(hRequest, headers.c_str(), static_cast<DWORD>(headers.length()), nullptr, 0, 0, 0);

    bool success = false;
    if (sent && WinHttpReceiveResponse(hRequest, nullptr)) {
        DWORD statusCode = 0;
        DWORD sz = sizeof(statusCode);
        WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                            WINHTTP_HEADER_NAME_BY_INDEX, &statusCode, &sz, WINHTTP_NO_HEADER_INDEX);

        if (statusCode == 200) {
            DWORD contentLength = 0;
            DWORD clSize = sizeof(contentLength);
            WinHttpQueryHeaders(hRequest, WINHTTP_QUERY_CONTENT_LENGTH | WINHTTP_QUERY_FLAG_NUMBER,
                                WINHTTP_HEADER_NAME_BY_INDEX, &contentLength, &clSize, WINHTTP_NO_HEADER_INDEX);

            HANDLE hFile = CreateFileW(destPath.c_str(), GENERIC_WRITE, 0, nullptr,
                                       CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (hFile != INVALID_HANDLE_VALUE) {
                char buffer[32768];
                DWORD bytesRead = 0;
                DWORD totalDownloaded = 0;

                success = true;
                while (WinHttpReadData(hRequest, buffer, sizeof(buffer), &bytesRead) && bytesRead > 0) {
                    DWORD bytesWritten = 0;
                    if (!WriteFile(hFile, buffer, bytesRead, &bytesWritten, nullptr) || bytesWritten != bytesRead) {
                        success = false;
                        break;
                    }
                    totalDownloaded += bytesWritten;
                    if (onProgress) {
                        onProgress(totalDownloaded, contentLength);
                    }
                }
                CloseHandle(hFile);

                if (!success) {
                    DeleteFileW(destPath.c_str());
                }
            }
        }
    }

    WinHttpCloseHandle(hRequest);
    WinHttpCloseHandle(hConnect);
    WinHttpCloseHandle(hSession);
    return success;
}

bool AppUpdater::apply_update_and_restart(const std::wstring& downloadedExePath) {
    wchar_t currentExe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, currentExe, MAX_PATH) == 0) {
        return false;
    }

    std::wstring oldExe = std::wstring(currentExe) + L".old";

    // 1. Rename running QuickDNS.exe -> QuickDNS.exe.old
    DeleteFileW(oldExe.c_str());
    if (!MoveFileExW(currentExe, oldExe.c_str(), MOVEFILE_REPLACE_EXISTING)) {
        return false;
    }

    // 2. Move downloaded new exe into place
    if (!MoveFileExW(downloadedExePath.c_str(), currentExe, MOVEFILE_REPLACE_EXISTING)) {
        // Rollback
        MoveFileExW(oldExe.c_str(), currentExe, MOVEFILE_REPLACE_EXISTING);
        return false;
    }

    // 3. Launch newly updated application
    HINSTANCE hInst = ShellExecuteW(nullptr, L"open", currentExe, nullptr, nullptr, SW_SHOW);
    if (reinterpret_cast<INT_PTR>(hInst) <= 32) {
        return false;
    }

    // 4. Terminate running process cleanly
    ExitProcess(0);
    return true;
}

void AppUpdater::cleanup_previous_update() {
    wchar_t currentExe[MAX_PATH] = {};
    if (GetModuleFileNameW(nullptr, currentExe, MAX_PATH) > 0) {
        std::wstring oldExe = std::wstring(currentExe) + L".old";
        if (GetFileAttributesW(oldExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            DeleteFileW(oldExe.c_str());
        }
    }
}

} // namespace quickdns
