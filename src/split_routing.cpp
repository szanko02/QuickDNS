#include "split_routing.h"
#include "encoding_utils.h"
#include <nlohmann/json.hpp>
#include <netioapi.h>
#include <fstream>
#include <string_view>
#include <system_error>

namespace quickdns {
namespace {
using nlohmann::json;
constexpr wchar_t tun_alias[] = L"QuickDNS TUN";

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

std::string make_split_routing_config(const RoutingAdapter& primary, const RoutingAdapter& secondary,
                                      const std::filesystem::path& app, bool package_directory,
                                      const std::filesystem::path& log) {
    const json tun = {
        {"type", "tun"}, {"tag", "quickdns-tun"}, {"interface_name", QuickDns::Encoding::wide_to_utf8(tun_alias)},
        {"address", {"172.19.0.1/30", "fdfe:dcba:9876::1/126"}},
        {"auto_route", true}, {"strict_route", true}, {"stack", "mixed"}
    };
    const json primary_outbound = {{"type", "direct"}, {"tag", "primary"},
        {"bind_interface", QuickDns::Encoding::wide_to_utf8(primary.name)}};
    const json secondary_outbound = {{"type", "direct"}, {"tag", "secondary"},
        {"bind_interface", QuickDns::Encoding::wide_to_utf8(secondary.name)}};
    const json rule = {{"process_path_regex", json::array({path_regex(app, package_directory)})},
        {"action", "route"}, {"outbound", "secondary"}};
    const json primary_rule = {{"process_path_regex", json::array({".+"})},
        {"action", "route"}, {"outbound", "primary"}};
    const json reject = {{"action", "reject"}};
    const json config = {
        {"log", {{"level", "warn"}, {"output", QuickDns::Encoding::wide_to_utf8(log.native())}}},
        {"inbounds", json::array({tun})}, {"outbounds", json::array({primary_outbound, secondary_outbound})},
        {"route", {{"rules", json::array({rule, primary_rule, reject})}, {"final", "primary"},
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
        MIB_IF_ROW2 adapter{};
        if (ConvertInterfaceAliasToLuid(tun_alias, &adapter.InterfaceLuid) == NO_ERROR &&
            GetIfEntry2(&adapter) == NO_ERROR && adapter.OperStatus == IfOperStatusUp)
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
}

std::wstring SplitRoutingSession::start(const std::filesystem::path& executable, const RoutingAdapter& primary,
                                        const RoutingAdapter& secondary, const std::filesystem::path& app,
                                        bool package_directory) {
    if (running()) return L"Сначала остановите текущий сеанс.";
    stop();
    if (primary.id == secondary.id || !primary.connected || !secondary.connected ||
        primary.name.empty() || secondary.name.empty())
        return L"Выберите два разных подключённых адаптера.";

    NET_LUID existing_tun{};
    if (ConvertInterfaceAliasToLuid(tun_alias, &existing_tun) == NO_ERROR)
        return L"Адаптер QuickDNS TUN уже существует. Проверьте запущенные сеансы.";

    std::error_code ec;
    if (!executable.is_absolute() || _wcsicmp(executable.filename().c_str(), L"sing-box.exe") != 0 ||
        !std::filesystem::is_regular_file(executable, ec))
        return L"Выберите установленный sing-box.exe.";
    if (!app.is_absolute() || !(package_directory ? std::filesystem::is_directory(app, ec)
                                                   : std::filesystem::is_regular_file(app, ec)))
        return L"Приложение или каталог пакета не найден.";

    config_ = temp_file();
    log_ = temp_file();
    if (config_.empty() || log_.empty()) {
        const auto error = error_text(GetLastError());
        stop();
        return error;
    }

    {
        std::ofstream out(config_, std::ios::binary);
        out << make_split_routing_config(primary, secondary, app, package_directory, log_);
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
    engine = executable;
    primary_id = primary.id;
    return {};
}
}
