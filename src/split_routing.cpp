#include "split_routing.h"
#include "encoding_utils.h"
#include <nlohmann/json.hpp>
#include <netioapi.h>
#include <shlobj.h>
#include <objbase.h>
#include <algorithm>
#include <fstream>
#include <string_view>
#include <system_error>

namespace quickdns {
namespace {
using nlohmann::json;
constexpr std::wstring_view tun_alias_prefix = L"QuickDNS TUN";

bool tun_is_up(const std::wstring& alias) {
    MIB_IF_ROW2 adapter{};
    return ConvertInterfaceAliasToLuid(alias.c_str(), &adapter.InterfaceLuid) == NO_ERROR &&
           GetIfEntry2(&adapter) == NO_ERROR && adapter.OperStatus == IfOperStatusUp;
}

bool has_active_quickdns_tun() {
    MIB_IF_TABLE2* table = nullptr;
    if (GetIfTable2(&table) != NO_ERROR) return false;
    const bool active = std::any_of(table->Table, table->Table + table->NumEntries, [](const MIB_IF_ROW2& adapter) {
        const std::wstring_view alias(adapter.Alias);
        return adapter.OperStatus == IfOperStatusUp &&
               (alias == tun_alias_prefix ||
                (alias.starts_with(tun_alias_prefix) && alias[tun_alias_prefix.size()] == L' '));
    });
    FreeMibTable(table);
    return active;
}

HRSRC embedded_engine(HMODULE image = nullptr) {
    return FindResourceW(image, MAKEINTRESOURCEW(4301), RT_RCDATA);
}

std::filesystem::path known_folder(REFKNOWNFOLDERID id) {
    PWSTR raw = nullptr;
    if (FAILED(SHGetKnownFolderPath(id, 0, nullptr, &raw))) return {};
    const std::filesystem::path folder(raw);
    CoTaskMemFree(raw);
    return folder;
}

std::filesystem::path private_engine_directory() {
    // Program Files prevents a non-elevated process from replacing the engine
    // or its configuration before the elevated child reads them.
    const auto root = known_folder(FOLDERID_ProgramFiles);
    if (root.empty()) return {};
    GUID id{};
    wchar_t name[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, 40)) return {};
    const auto directory = root / (std::wstring(L"QuickDNS Routing ") + name);
    if (!CreateDirectoryW(directory.c_str(), nullptr)) return {};
    return directory;
}

bool extract_engine(HRSRC resource, const std::filesystem::path& target) {
    const auto size = SizeofResource(nullptr, resource);
    const auto bytes = LockResource(LoadResource(nullptr, resource));
    if (!size || !bytes) return false;
    const HANDLE file = CreateFileW(target.c_str(), GENERIC_WRITE, 0, nullptr,
                                    CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE) return false;
    DWORD written = 0;
    const bool ok = WriteFile(file, bytes, size, &written, nullptr) && written == size;
    const bool closed = CloseHandle(file) != FALSE;
    return ok && closed;
}

std::string path_regex(const std::filesystem::path& path, bool directory) {
    std::string result = "(?i)^";
    for (char c : QuickDns::Encoding::wide_to_utf8(path.native())) {
        if (c == '/' || c == '\\') {
            result += R"([\\/])";
        } else {
            if (std::string_view(R"(.+*?()[]{}^$|)").find(c) != std::string_view::npos) result += '\\';
            result += c;
        }
    }
    result += directory ? R"([\\/])" : "$";
    return result;
}

std::wstring error_text(DWORD code) {
    return L"Ошибка Windows " + std::to_wstring(code) + L".";
}

std::filesystem::path temp_file() {
    wchar_t base[MAX_PATH]{}, path[MAX_PATH]{};
    DWORD size = GetTempPathW(MAX_PATH, base);
    if (!size || size >= MAX_PATH || !GetTempFileNameW(base, L"qds", 0, path)) return {};
    return path;
}

const std::wstring& adapter_resolver(const RoutingAdapter& adapter) {
    static const std::wstring empty;
    if (!adapter.ipv4_dns.empty()) return adapter.ipv4_dns.front();
    return adapter.ipv6_dns.empty() ? empty : adapter.ipv6_dns.front();
}

json split_dns_config(const RoutingAdapter& primary, const RoutingAdapter& secondary, bool split_dns,
                      std::span<const std::string> domains, bool public_web) {
    const auto server = [](const RoutingAdapter& adapter, const char* tag) {
        return json{{"type", "udp"}, {"tag", tag},
                    {"server", QuickDns::Encoding::wide_to_utf8(adapter_resolver(adapter))},
                    {"bind_interface", QuickDns::Encoding::wide_to_utf8(adapter.name)}};
    };
    // OpenAI network recommendations for ChatGPT on web and apps, article 9247338.
    const json chatgpt = {
        {"domain_suffix", {"chatgpt.com", "openai.com", "oaistatic.com", "oaiusercontent.com",
                           "oaistatsig.com"}},
        {"domain", {"challenges.cloudflare.com", "cdn.openaimerge.com", "cdn.workos.com",
                    "forwarder.workos.com", "images.workoscdn.com", "setup.workos.com",
                    "workos.imgix.net"}},
        {"action", "route"}, {"server", "secondary-dns"}
    };
    auto servers = json::array({server(primary, "primary-dns"), server(secondary, "secondary-dns")});
    auto rules = json::array();
    if (!domains.empty()) {
        // Keep each selected DNS name distinguishable even when sites share an IP
        // or the hostname is absent from the connection's initial packets.
        servers.push_back({{"type", "fakeip"}, {"tag", "selected-domains"},
                           {"inet4_range", "198.18.0.0/15"}, {"inet6_range", "2001:2::/48"}});
        rules.push_back({{"domain_suffix", domains}, {"query_type", {"A", "AAAA"}},
                         {"action", "route"}, {"server", "selected-domains"}, {"rewrite_ttl", 1}});
        rules.push_back({{"domain_suffix", domains}, {"action", "route"}, {"server", "secondary-dns"}});
    }
    if (split_dns) rules.push_back(chatgpt);
    if (public_web) {
        auto local_suffixes = json::array({"local", "in-addr.arpa", "ip6.arpa"});
        if (!primary.dns_suffix.empty()) local_suffixes.push_back(QuickDns::Encoding::wide_to_utf8(primary.dns_suffix));
        rules.push_back({{"domain_suffix", std::move(local_suffixes)}, {"domain_regex", {R"(^[^.]+$)"}},
                         {"action", "route"}, {"server", "primary-dns"}});
    }
    return {{"servers", std::move(servers)},
            {"rules", std::move(rules)}, {"final", public_web ? "secondary-dns" : "primary-dns"}};
}
}

bool has_embedded_routing_engine() { return embedded_engine() != nullptr; }
bool has_embedded_routing_engine(HMODULE image) { return embedded_engine(image) != nullptr; }

std::wstring new_split_routing_tun_alias() {
    // sing-tun derives the Wintun device GUID from its alias. A previous device
    // can still be pending removal after a forced engine exit.
    GUID id{};
    wchar_t name[40]{};
    if (FAILED(CoCreateGuid(&id)) || !StringFromGUID2(id, name, 40)) return {};
    return std::wstring(tun_alias_prefix) + L" " + name;
}

std::string make_split_routing_config(const RoutingAdapter& primary, const RoutingAdapter& secondary,
                                      std::span<const SplitRoutingTarget> targets, const std::filesystem::path& log,
                                      const std::wstring& tun_alias, bool split_dns,
                                      std::span<const std::string> domains, bool public_web) {
    const bool use_split_dns = split_dns || !domains.empty() || public_web;
    const json tun = {
        {"type", "tun"}, {"tag", "quickdns-tun"}, {"interface_name", QuickDns::Encoding::wide_to_utf8(tun_alias)},
        {"address", {"172.19.0.1/30", "fdfe:dcba:9876::1/126"}},
        {"dns_mode", use_split_dns ? "hijack" : "disabled"},
        {"auto_route", true}, {"strict_route", true}, {"stack", "mixed"}
    };
    const json primary_outbound = {{"type", "direct"}, {"tag", "primary"},
        {"bind_interface", QuickDns::Encoding::wide_to_utf8(primary.name)}};
    json secondary_outbound = {{"type", "direct"}, {"tag", "secondary"},
        {"bind_interface", QuickDns::Encoding::wide_to_utf8(secondary.name)}};
    if (!domains.empty()) secondary_outbound["domain_resolver"] = "secondary-dns";
    auto app_paths = json::array();
    for (const auto& target : targets) {
        app_paths.push_back(path_regex(target.path, !target.package_family.empty()));
        if (_wcsicmp(target.package_family.c_str(), L"OpenAI.Codex_2p2nqsd0c76g0") == 0) {
            const auto local_app_data = known_folder(FOLDERID_LocalAppData);
            if (!local_app_data.empty()) {
                // The Codex MSIX launches a versioned backend from outside the package.
                app_paths.push_back(path_regex(local_app_data / L"OpenAI" / L"Codex" / L"bin", true) +
                                    R"([0-9a-f]{16}[\\/]codex\.exe$)");
            }
        }
    }
    const json rule = {{"process_path_regex", std::move(app_paths)},
        {"action", "route"}, {"outbound", "secondary"}};
    const json primary_rule = {{"process_path_regex", json::array({".+"})},
        {"action", "route"}, {"outbound", "primary"}};
    const json reject = {{"action", "reject"}};
    auto rules = json::array();
    if (!targets.empty()) rules.push_back(rule);
    if (!domains.empty() || (public_web && !primary.dns_suffix.empty()))
        rules.push_back({{"action", "sniff"}, {"sniffer", {"http", "tls", "quic"}}});
    if (!domains.empty())
        rules.push_back({{"domain_suffix", domains}, {"action", "route"}, {"outbound", "secondary"}});
    if (public_web) {
        if (!primary.dns_suffix.empty())
            rules.push_back({{"domain_suffix", {QuickDns::Encoding::wide_to_utf8(primary.dns_suffix)}},
                             {"action", "route"}, {"outbound", "primary"}});
        rules.push_back({{"ip_is_private", true}, {"action", "route"}, {"outbound", "primary"}});
        rules.push_back({{"port", {80, 443}}, {"network", {"tcp", "udp"}},
                         {"action", "route"}, {"outbound", "secondary"}});
    }
    if (!use_split_dns) {
        auto secondary_dns = json::array();
        for (const auto& server : secondary.ipv4_dns) {
            if (std::find(primary.ipv4_dns.begin(), primary.ipv4_dns.end(), server) == primary.ipv4_dns.end())
                secondary_dns.push_back(QuickDns::Encoding::wide_to_utf8(server) + "/32");
        }
        if (!secondary_dns.empty()) {
            // Windows can perform DoH on behalf of the application from a system process.
            rules.push_back({{"process_name", {"svchost.exe"}}, {"ip_cidr", secondary_dns},
                             {"port", {443}}, {"action", "route"}, {"outbound", "secondary"}});
            rules.push_back({{"ip_cidr", std::move(secondary_dns)}, {"port", {53}},
                             {"network", {"tcp", "udp"}}, {"action", "route"}, {"outbound", "secondary"}});
        }
        auto primary_dns = json::array();
        for (const auto& server : primary.ipv4_dns)
            primary_dns.push_back(QuickDns::Encoding::wide_to_utf8(server) + "/32");
        if (!primary_dns.empty())
            rules.push_back({{"ip_cidr", std::move(primary_dns)}, {"port", {53}},
                             {"network", {"tcp", "udp"}}, {"action", "route"}, {"outbound", "primary"}});
    }
    rules.push_back(primary_rule);
    rules.push_back(reject);
    json config = {
        {"log", {{"level", "warn"}, {"output", QuickDns::Encoding::wide_to_utf8(log.native())}}},
        {"inbounds", json::array({tun})}, {"outbounds", json::array({primary_outbound, secondary_outbound})},
        {"route", {{"rules", std::move(rules)}, {"final", "primary"},
                   {"default_interface", QuickDns::Encoding::wide_to_utf8(primary.name)}}}
    };
    if (use_split_dns) {
        config["dns"] = split_dns_config(primary, secondary, split_dns, domains, public_web);
        config["route"]["default_domain_resolver"] = public_web ? "secondary-dns" : "primary-dns";
    }
    return config.dump(2, ' ', false, json::error_handler_t::replace);
}

SplitRoutingSession::~SplitRoutingSession() { stop(); }

bool SplitRoutingSession::running() const {
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

std::wstring SplitRoutingSession::status() const {
    if (running()) {
        if (tun_is_up(tun_alias_))
            return L"sing-box и TUN запущены. Проверьте маршрут приложения.";
        return L"sing-box запущен. Ожидание адаптера TUN.";
    }
    if (process_) return L"sing-box завершился. Журнал: " + log_.native();
    return L"Маршрутизация приложений остановлена.";
}

void SplitRoutingSession::stop() {
    if (running()) {
        // The child owns a private console and QuickDNS is a GUI process.
        if (AttachConsole(GetProcessId(process_))) {
            SetConsoleCtrlHandler(nullptr, TRUE);
            GenerateConsoleCtrlEvent(CTRL_C_EVENT, 0);
            WaitForSingleObject(process_, 3000);
            FreeConsole();
        }
        if (running()) {
            TerminateJobObject(job_, ERROR_CANCELLED);
            WaitForSingleObject(process_, 3000);
        }
    }
    if (job_) CloseHandle(job_);
    if (process_) CloseHandle(process_);
    job_ = process_ = nullptr;
    tun_alias_.clear();
    if (!config_.empty()) DeleteFileW(config_.c_str());
    if (!log_.empty()) DeleteFileW(log_.c_str());
    config_.clear();
    log_.clear();
    if (!embedded_directory_.empty()) {
        DeleteFileW((embedded_directory_ / L"sing-box.exe").c_str());
        RemoveDirectoryW(embedded_directory_.c_str());
        embedded_directory_.clear();
    }
}

std::wstring SplitRoutingSession::start(const std::filesystem::path& selected_engine, const RoutingAdapter& primary,
                                        const RoutingAdapter& secondary, std::span<const SplitRoutingTarget> targets,
                                        bool split_dns, std::span<const std::string> domains, bool public_web) {
    if (running()) return L"Сначала остановите текущий сеанс.";
    stop();
    if (primary.id == secondary.id || !primary.connected || !secondary.connected ||
        primary.name.empty() || secondary.name.empty())
        return L"Выберите два разных подключённых адаптера.";
    if ((split_dns || !domains.empty() || public_web) &&
        (adapter_resolver(primary).empty() || adapter_resolver(secondary).empty()))
        return L"Для раздельного DNS на обоих адаптерах должны быть указаны DNS-серверы.";

    if (has_active_quickdns_tun())
        return L"Адаптер QuickDNS TUN активен. Проверьте запущенные сеансы.";

    if (targets.empty() && domains.empty() && !public_web) return L"Выберите приложение или укажите домен.";
    std::error_code ec;
    for (const auto& target : targets) {
        if (!target.path.is_absolute() ||
            !(target.package_family.empty() ? std::filesystem::is_regular_file(target.path, ec)
                                            : std::filesystem::is_directory(target.path, ec)))
            return L"Одно из выбранных приложений или каталог пакета не найден.";
    }

    auto executable = selected_engine;
    if (const auto resource = embedded_engine()) {
        embedded_directory_ = private_engine_directory();
        if (embedded_directory_.empty()) return L"Не удалось создать каталог встроенного движка.";
        executable = embedded_directory_ / L"sing-box.exe";
        if (!extract_engine(resource, executable)) {
            stop();
            return L"Не удалось извлечь встроенный движок.";
        }
        config_ = embedded_directory_ / L"routing.json";
        log_ = embedded_directory_ / L"routing.log";
    } else {
        if (!executable.is_absolute() || _wcsicmp(executable.filename().c_str(), L"sing-box.exe") != 0 ||
            !std::filesystem::is_regular_file(executable, ec))
            return L"Выберите установленный sing-box.exe.";
        config_ = temp_file();
        log_ = temp_file();
    }
    if (config_.empty() || log_.empty()) {
        const auto error = error_text(GetLastError());
        stop();
        return error;
    }

    tun_alias_ = new_split_routing_tun_alias();
    if (tun_alias_.empty()) {
        stop();
        return L"Не удалось создать имя адаптера TUN.";
    }
    {
        std::ofstream out(config_, std::ios::binary);
        out << make_split_routing_config(primary, secondary, targets, log_, tun_alias_, split_dns, domains, public_web);
        out.close();
        if (!out) {
            stop();
            return L"Не удалось записать конфигурацию sing-box.";
        }
    }

    STARTUPINFOW startup{};
    startup.cb = sizeof(startup);
    startup.dwFlags = STARTF_USESHOWWINDOW;
    startup.wShowWindow = SW_HIDE;
    PROCESS_INFORMATION info{};
    auto cmd = L"\"" + executable.native() + L"\" check -c \"" + config_.native() + L"\"";
    if (!CreateProcessW(executable.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NO_WINDOW, nullptr, executable.parent_path().c_str(), &startup, &info)) {
        const auto error = error_text(GetLastError());
        stop();
        return error;
    }
    CloseHandle(info.hThread);
    DWORD check_exit = 1;
    if (WaitForSingleObject(info.hProcess, 2500) != WAIT_OBJECT_0) TerminateProcess(info.hProcess, ERROR_TIMEOUT);
    else GetExitCodeProcess(info.hProcess, &check_exit);
    CloseHandle(info.hProcess);
    if (check_exit != 0) {
        stop();
        return L"sing-box не принял конфигурацию. Требуется версия с поддержкой TUN и process_path_regex.";
    }

    job_ = CreateJobObjectW(nullptr, nullptr);
    JOBOBJECT_EXTENDED_LIMIT_INFORMATION limit{};
    limit.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
    if (!job_ || !SetInformationJobObject(job_, JobObjectExtendedLimitInformation, &limit, sizeof(limit))) {
        const auto error = error_text(GetLastError());
        stop();
        return error;
    }

    cmd = L"\"" + executable.native() + L"\" run -c \"" + config_.native() + L"\"";
    if (!CreateProcessW(executable.c_str(), cmd.data(), nullptr, nullptr, FALSE,
                        CREATE_NEW_CONSOLE | CREATE_SUSPENDED, nullptr,
                        executable.parent_path().c_str(), &startup, &info)) {
        const auto error = error_text(GetLastError());
        stop();
        return error;
    }
    process_ = info.hProcess;
    if (!AssignProcessToJobObject(job_, process_) || ResumeThread(info.hThread) == static_cast<DWORD>(-1)) {
        const auto error = error_text(GetLastError());
        TerminateProcess(process_, ERROR_CANCELLED);
        CloseHandle(info.hThread);
        stop();
        return error;
    }
    CloseHandle(info.hThread);
    engine = selected_engine;
    primary_id = primary.id;
    return {};
}
}
