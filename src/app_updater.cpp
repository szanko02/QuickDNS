#include "app_updater.h"
#include "encoding_utils.h"
#include "split_routing.h"
#include "../third_party/nlohmann/json.hpp"
#include <winhttp.h>
#include <shellapi.h>
#include <bcrypt.h>
#include <sstream>
#include <algorithm>
#include <iostream>
#include <cctype>
#include <charconv>
#include <memory>
#include <string_view>

namespace quickdns {

namespace {

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

namespace {

using nlohmann::json;

bool valid_repo_component(std::string_view name) {
    constexpr std::string_view allowed =
        "abcdefghijklmnopqrstuvwxyzABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789-_.";
    return !name.empty() && name != "." && name != ".." &&
           name.find_first_not_of(allowed) == std::string_view::npos;
}

bool valid_release_tag(std::string_view tag) {
    if (!tag.starts_with('v')) return false;
    tag.remove_prefix(1);
    for (int index = 0; index < 3; ++index) {
        const auto dot = tag.find('.');
        if ((dot == std::string_view::npos) != (index == 2)) return false;
        const auto part = tag.substr(0, dot);
        if (part.empty() || (part.length() > 1 && part[0] == '0') ||
            part.find_first_not_of("0123456789") != std::string_view::npos) return false;
        int value = 0;
        if (std::from_chars(part.data(), part.data() + part.size(), value).ec != std::errc{}) return false;
        if (index != 2) tag.remove_prefix(dot + 1);
    }
    return true;
}

std::optional<std::string> fetch_release_response(const std::string& url, bool redirectOnly) {
    constexpr std::string_view scheme = "https://";
    if (!url.starts_with(scheme)) return std::nullopt;
    const auto pathStart = url.find('/', scheme.size());
    if (pathStart == std::string::npos) return std::nullopt;
    const auto host = url.substr(scheme.size(), pathStart - scheme.size());
    if (host != "github.com" && host != "api.github.com") return std::nullopt;

    using HttpHandle = std::unique_ptr<void, decltype(&WinHttpCloseHandle)>;
    HttpHandle session(WinHttpOpen(L"QuickDNS-AppUpdater/2.0", WINHTTP_ACCESS_TYPE_DEFAULT_PROXY,
                                   WINHTTP_NO_PROXY_NAME, WINHTTP_NO_PROXY_BYPASS, 0), WinHttpCloseHandle);
    if (!session) return std::nullopt;
    WinHttpSetTimeouts(session.get(), 10000, 10000, 10000, 10000);
    DWORD redirectPolicy = WINHTTP_OPTION_REDIRECT_POLICY_NEVER;
    if (!WinHttpSetOption(session.get(), WINHTTP_OPTION_REDIRECT_POLICY, &redirectPolicy, sizeof(redirectPolicy)))
        return std::nullopt;

    const auto hostWide = QuickDns::Encoding::utf8_to_wide(host);
    HttpHandle connection(WinHttpConnect(session.get(), hostWide.c_str(), INTERNET_DEFAULT_HTTPS_PORT, 0),
                          WinHttpCloseHandle);
    if (!connection) return std::nullopt;
    const auto pathWide = QuickDns::Encoding::utf8_to_wide(url.substr(pathStart));
    HttpHandle request(WinHttpOpenRequest(connection.get(), redirectOnly ? L"HEAD" : L"GET", pathWide.c_str(),
        nullptr, WINHTTP_NO_REFERER, WINHTTP_DEFAULT_ACCEPT_TYPES, WINHTTP_FLAG_SECURE), WinHttpCloseHandle);
    if (!request) return std::nullopt;
    const wchar_t* headers = host == "api.github.com" ? L"Accept: application/vnd.github+json\r\n" : L"Accept: */*\r\n";
    if (!WinHttpSendRequest(request.get(), headers, static_cast<DWORD>(-1), nullptr, 0, 0, 0) ||
        !WinHttpReceiveResponse(request.get(), nullptr)) return std::nullopt;

    DWORD status = 0;
    DWORD statusSize = sizeof(status);
    if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_STATUS_CODE | WINHTTP_QUERY_FLAG_NUMBER,
                             WINHTTP_HEADER_NAME_BY_INDEX, &status, &statusSize, WINHTTP_NO_HEADER_INDEX))
        return std::nullopt;
    if (redirectOnly) {
        if (status < 300 || status >= 400) return std::nullopt;
        wchar_t location[1024]{};
        DWORD size = sizeof(location);
        if (!WinHttpQueryHeaders(request.get(), WINHTTP_QUERY_LOCATION, WINHTTP_HEADER_NAME_BY_INDEX,
                                location, &size, WINHTTP_NO_HEADER_INDEX)) return std::nullopt;
        return QuickDns::Encoding::wide_to_utf8(location);
    }
    if (status != 200) return std::nullopt;

    constexpr size_t maxResponseSize = 4 * 1024 * 1024;
    std::string body;
    char buffer[8192];
    DWORD bytesRead = 0;
    do {
        if (!WinHttpReadData(request.get(), buffer, sizeof(buffer), &bytesRead) ||
            body.size() + bytesRead > maxResponseSize) return std::nullopt;
        body.append(buffer, bytesRead);
    } while (bytesRead != 0);
    return body;
}

std::string_view release_tag_from_link(std::string_view link, std::string_view tagPath) {
    constexpr std::string_view github = "https://github.com";
    if (link.starts_with(github)) link.remove_prefix(github.size());
    if (!link.starts_with(tagPath)) return {};
    const auto tag = link.substr(tagPath.size());
    return valid_release_tag(tag) ? tag : std::string_view{};
}

ReleaseInfo release_info(const std::string& tag, const std::string& currentVersion,
                         const std::string& releasesUrl, const char* assetName) {
    ReleaseInfo info;
    info.tag_name = tag;
    info.semver = tag.substr(1);
    info.title = "QuickDNS " + tag;
    info.download_url = releasesUrl + "/download/" + tag + "/" + assetName;
    info.sha256_url = info.download_url + ".sha256";
    info.is_newer = AppUpdater::is_version_newer(currentVersion, info.semver);
    return info;
}

std::string json_string(const json& object, const char* key) {
    const auto it = object.find(key);
    return it != object.end() && it->is_string() ? it->get<std::string>() : std::string{};
}

bool release_flag_clear(const json& release, const char* key) {
    const auto it = release.find(key);
    return it == release.end() || (it->is_boolean() && !it->get<bool>());
}

std::optional<ReleaseInfo> compatible_api_release(const json& release, const std::string& currentVersion,
    const std::string& releasesUrl, const char* assetName, UpdateEdition edition) {
    if (!release.is_object() || !release_flag_clear(release, "draft") ||
        (edition == UpdateEdition::Classic && !release_flag_clear(release, "prerelease"))) return std::nullopt;
    const auto tag = json_string(release, "tag_name");
    const auto assets = release.find("assets");
    if (!valid_release_tag(tag) || assets == release.end() || !assets->is_array()) return std::nullopt;

    auto info = release_info(tag, currentVersion, releasesUrl, assetName);
    const std::string checksumName = std::string(assetName) + ".sha256";
    bool executableFound = false;
    bool checksumFound = false;
    for (const auto& asset : *assets) {
        if (!asset.is_object()) continue;
        const auto name = json_string(asset, "name");
        const auto url = json_string(asset, "browser_download_url");
        if (name == assetName && url == info.download_url) {
            executableFound = true;
            const auto size = asset.find("size");
            if (size != asset.end() && size->is_number_unsigned()) info.asset_size = size->get<size_t>();
        } else if (name == checksumName && url == info.sha256_url) {
            checksumFound = true;
        }
    }
    if (!executableFound || !checksumFound) return std::nullopt;
    const auto title = json_string(release, "name");
    if (!title.empty()) info.title = title;
    info.notes = QuickDns::Encoding::wide_to_utf8(normalize_crlf(
        QuickDns::Encoding::utf8_to_wide(json_string(release, "body"))));
    return info;
}

std::vector<std::string> release_tags_from_feed(std::string_view feed, const std::string& tagPath) {
    constexpr std::string_view href = " href=\"";
    std::vector<std::string> tags;
    size_t pos = 0;
    while ((pos = feed.find(href, pos)) != std::string_view::npos) {
        pos += href.size();
        const auto end = feed.find('"', pos);
        if (end == std::string_view::npos) break;
        const auto tag = release_tag_from_link(feed.substr(pos, end - pos), tagPath);
        if (!tag.empty()) tags.emplace_back(tag);
        pos = end + 1;
    }
    std::sort(tags.begin(), tags.end(), [](const auto& a, const auto& b) {
        return AppUpdater::is_version_newer(b, a);
    });
    tags.erase(std::unique(tags.begin(), tags.end()), tags.end());
    return tags;
}

bool contains_asset_link(const std::string& html, const std::string& fullUrl) {
    constexpr std::string_view github = "https://github.com";
    return html.find(" href=\"" + fullUrl.substr(github.size()) + "\"") != std::string::npos ||
           html.find(" href=\"" + fullUrl + "\"") != std::string::npos;
}

} // anonymous namespace

std::optional<ReleaseInfo> AppUpdater::check_github_update(
    const std::string& currentVersion, const std::string& repoOwner, const std::string& repoName,
    UpdateEdition edition, ReleaseFetcher fetch) {
    if (!valid_repo_component(repoOwner) || !valid_repo_component(repoName)) return std::nullopt;
    if (!fetch) fetch = fetch_release_response;

    const std::string releasesPath = "/" + repoOwner + "/" + repoName + "/releases";
    const std::string releasesUrl = "https://github.com" + releasesPath;
    const std::string apiUrl = "https://api.github.com/repos" + releasesPath;
    const auto* assetName = edition == UpdateEdition::Embedded ? "QuickDNS-Embedded.exe" : "QuickDNS.exe";
    if (edition == UpdateEdition::Classic) {
        if (const auto response = fetch(apiUrl + "/latest", false)) {
            const auto release = json::parse(*response, nullptr, false);
            if (auto info = compatible_api_release(release, currentVersion, releasesUrl, assetName, edition)) return info;
        }
    } else {
        std::optional<ReleaseInfo> best;
        for (size_t page = 1; ; ++page) {
            auto url = apiUrl + "?per_page=100";
            if (page > 1) url += "&page=" + std::to_string(page);
            const auto response = fetch(url, false);
            if (!response) break;
            const auto releases = json::parse(*response, nullptr, false);
            if (!releases.is_array()) break;
            for (const auto& release : releases) {
                auto info = compatible_api_release(release, currentVersion, releasesUrl, assetName, edition);
                if (info && (!best || is_version_newer(best->semver, info->semver))) best = std::move(info);
            }
            if (releases.size() < 100) break;
        }
        if (best) return best;
    }

    // Public GitHub pages remain available when the Releases API is rate-limited.
    std::vector<std::string> tags;
    const auto tagPath = releasesPath + "/tag/";
    if (edition == UpdateEdition::Classic) {
        if (const auto location = fetch(releasesUrl + "/latest", true)) {
            const auto tag = release_tag_from_link(*location, tagPath);
            if (!tag.empty()) tags.emplace_back(tag);
        }
    } else if (const auto feed = fetch(releasesUrl + ".atom", false)) {
        tags = release_tags_from_feed(*feed, tagPath);
    }
    for (const auto& tag : tags) {
        auto info = release_info(tag, currentVersion, releasesUrl, assetName);
        const auto assets = fetch(releasesUrl + "/expanded_assets/" + tag, false);
        if (assets && contains_asset_link(*assets, info.download_url) && contains_asset_link(*assets, info.sha256_url))
            return info;
    }
    return std::nullopt;
}

bool AppUpdater::verify_executable_edition(const std::wstring& filePath, UpdateEdition edition) {
    const auto image = LoadLibraryExW(filePath.c_str(), nullptr,
                                      LOAD_LIBRARY_AS_DATAFILE_EXCLUSIVE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
    if (!image) return false;
    const bool embedded = has_embedded_routing_engine(image);
    FreeLibrary(image);
    return embedded == (edition == UpdateEdition::Embedded);
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

    auto retry_op = [](const std::function<bool()>& op, int maxAttempts, DWORD sleepMs) -> bool {
        for (int i = 0; i < maxAttempts; ++i) {
            if (op()) return true;
            Sleep(sleepMs);
        }
        return false;
    };

    // 1. Remove previous old backup file if it exists
    if (GetFileAttributesW(oldExePath.c_str()) != INVALID_FILE_ATTRIBUTES) {
        bool delOldOk = retry_op([&]() {
            return DeleteFileW(oldExePath.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND;
        }, 20, 150);

        if (!delOldOk) {
            DWORD err = GetLastError();
            set_err(L"Не удалось удалить предыдущую резервную копию (.old)", err);
            return false;
        }
    }

    // 2. Rename current target executable -> oldExePath
    bool renameTargetOk = retry_op([&]() {
        return MoveFileExW(targetExePath.c_str(), oldExePath.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }, 20, 150);

    if (!renameTargetOk) {
        DWORD err = GetLastError();
        set_err(L"Не удалось создать резервную копию текущего файла (.old)", err);
        return false;
    }

    // 3. Move downloaded executable -> targetExePath
    bool moveNewOk = retry_op([&]() {
        return MoveFileExW(downloadedExePath.c_str(), targetExePath.c_str(),
                          MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
    }, 20, 150);

    if (!moveNewOk) {
        // Fallback: CopyFileW followed by DeleteFileW
        moveNewOk = retry_op([&]() {
            if (CopyFileW(downloadedExePath.c_str(), targetExePath.c_str(), FALSE)) {
                if (!DeleteFileW(downloadedExePath.c_str())) {
                    MoveFileExW(downloadedExePath.c_str(), nullptr, MOVEFILE_DELAY_UNTIL_REBOOT);
                }
                return true;
            }
            return false;
        }, 10, 150);
    }

    if (!moveNewOk) {
        DWORD err = GetLastError();
        set_err(L"Не удалось установить новый исполняемый файл", err);

        // Rollback: restore oldExePath -> targetExePath
        retry_op([&]() {
            return MoveFileExW(oldExePath.c_str(), targetExePath.c_str(),
                              MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH) != FALSE;
        }, 20, 150);

        return false;
    }

    return true;
}

bool AppUpdater::apply_update_and_restart(const std::wstring& downloadedExePath, std::wstring* outError) {
    wchar_t currentExe[MAX_PATH] = {0};
    DWORD exeLen = GetModuleFileNameW(nullptr, currentExe, MAX_PATH);
    if (exeLen == 0 || exeLen >= MAX_PATH) {
        if (outError) *outError = L"Не удалось определить путь к текущему исполняемому файлу.";
        return false;
    }

    std::wstring exePath(currentExe);
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

    // 3. Launch newly updated application with --updated argument
    std::wstring cmdLine = L"\"" + exePath + L"\" --updated";
    std::vector<wchar_t> cmdBuffer(cmdLine.begin(), cmdLine.end());
    cmdBuffer.push_back(L'\0');

    STARTUPINFOW si = {};
    si.cb = sizeof(si);
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
        // Fallback to ShellExecuteW
        HINSTANCE hInst = ShellExecuteW(
            nullptr,
            L"open",
            currentExe,
            L"--updated",
            exeDir.empty() ? nullptr : exeDir.c_str(),
            SW_SHOWNORMAL);
        if (reinterpret_cast<INT_PTR>(hInst) > 32) {
            launched = true;
        } else {
            launchErr = static_cast<DWORD>(reinterpret_cast<INT_PTR>(hInst));
        }
    }

    if (!launched) {
        if (outError) {
            *outError = L"Не удалось запустить обновленное приложение (код ошибки: " + std::to_wstring(launchErr) + L")";
        }
        // Rollback: restore oldExe -> currentExe
        for (int i = 0; i < 20; ++i) {
            if (MoveFileExW(oldExe.c_str(), currentExe, MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
                break;
            }
            Sleep(150);
        }
        return false;
    }

    // 4. Terminate running process cleanly
    ExitProcess(0);
    return true;
}

void AppUpdater::cleanup_previous_update() {
    wchar_t currentExe[MAX_PATH] = {};
    DWORD exeLen = GetModuleFileNameW(nullptr, currentExe, MAX_PATH);
    if (exeLen > 0 && exeLen < MAX_PATH) {
        std::wstring oldExe = std::wstring(currentExe) + L".old";
        if (GetFileAttributesW(oldExe.c_str()) != INVALID_FILE_ATTRIBUTES) {
            for (int i = 0; i < 20; ++i) {
                if (DeleteFileW(oldExe.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) {
                    break;
                }
                Sleep(100);
            }
        }
    }
}

} // namespace quickdns
