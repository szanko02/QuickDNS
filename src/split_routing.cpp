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
constexpr wchar_t tun_alias[] = L"QuickDNS TUN";

bool tun_is_up() {
    MIB_IF_ROW2 adapter{};
    return ConvertInterfaceAliasToLuid(tun_alias, &adapter.InterfaceLuid) == NO_ERROR &&
           GetIfEntry2(&adapter) == NO_ERROR && adapter.OperStatus == IfOperStatusUp;
}

HRSRC embedded_engine() {
    return FindResourceW(nullptr, MAKEINTRESOURCEW(4301), RT_RCDATA);
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
}

bool has_embedded_routing_engine() { return embedded_engine() != nullptr; }

std::string make_split_routing_config(const RoutingAdapter& primary, const RoutingAdapter& secondary,
                                      const std::filesystem::path& app, bool package_directory,
                                      const std::filesystem::path& log, const std::wstring& package_family) {
    const json tun = {
        {"type", "tun"}, {"tag", "quickdns-tun"}, {"interface_name", QuickDns::Encoding::wide_to_utf8(tun_alias)},
        {"address", {"172.19.0.1/30", "fdfe:dcba:9876::1/126"}},
        {"dns_mode", "disabled"},
        {"auto_route", true}, {"strict_route", true}, {"stack", "mixed"}
    };
    const json primary_outbound = {{"type", "direct"}, {"tag", "primary"},
        {"bind_interface", QuickDns::Encoding::wide_to_utf8(primary.name)}};
    const json secondary_outbound = {{"type", "direct"}, {"tag", "secondary"},
        {"bind_interface", QuickDns::Encoding::wide_to_utf8(secondary.name)}};
    auto app_paths = json::array({path_regex(app, package_directory)});
    if (package_directory && _wcsicmp(package_family.c_str(), L"OpenAI.Codex_2p2nqsd0c76g0") == 0) {
        const auto local_app_data = known_folder(FOLDERID_LocalAppData);
        if (!local_app_data.empty()) {
            // The Codex MSIX launches a versioned backend from outside the package.
            app_paths.push_back(path_regex(local_app_data / L"OpenAI" / L"Codex" / L"bin", true) +
                                R"([0-9a-f]{16}[\\/]codex\.exe$)");
        }
    }
    const json rule = {{"process_path_regex", std::move(app_paths)},
        {"action", "route"}, {"outbound", "secondary"}};
    const json primary_rule = {{"process_path_regex", json::array({".+"})},
        {"action", "route"}, {"outbound", "primary"}};
    const json reject = {{"action", "reject"}};
    auto rules = json::array({rule});
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
    rules.push_back(primary_rule);
    rules.push_back(reject);
    const json config = {
        {"log", {{"level", "warn"}, {"output", QuickDns::Encoding::wide_to_utf8(log.native())}}},
        {"inbounds", json::array({tun})}, {"outbounds", json::array({primary_outbound, secondary_outbound})},
        {"route", {{"rules", std::move(rules)}, {"final", "primary"},
                   {"default_interface", QuickDns::Encoding::wide_to_utf8(primary.name)}}}
    };
    return config.dump(2, ' ', false, json::error_handler_t::replace);
}

SplitRoutingSession::~SplitRoutingSession() { stop(); }

bool SplitRoutingSession::running() const {
    return process_ && WaitForSingleObject(process_, 0) == WAIT_TIMEOUT;
}

std::wstring SplitRoutingSession::status() const {
    if (running()) {
        if (tun_is_up())
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
                                        const RoutingAdapter& secondary, const std::filesystem::path& app,
                                        bool package_directory, const std::wstring& package_family) {
    if (running()) return L"Сначала остановите текущий сеанс.";
    stop();
    if (primary.id == secondary.id || !primary.connected || !secondary.connected ||
        primary.name.empty() || secondary.name.empty())
        return L"Выберите два разных подключённых адаптера.";

    if (tun_is_up())
        return L"Адаптер QuickDNS TUN активен. Проверьте запущенные сеансы.";

    std::error_code ec;
    if (!app.is_absolute() || !(package_directory ? std::filesystem::is_directory(app, ec)
                                                   : std::filesystem::is_regular_file(app, ec)))
        return L"Приложение или каталог пакета не найден.";

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

    {
        std::ofstream out(config_, std::ios::binary);
        out << make_split_routing_config(primary, secondary, app, package_directory, log_, package_family);
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
