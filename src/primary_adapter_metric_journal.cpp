#include "primary_adapter_metric_journal.h"
#include <windows.h>
#include <nlohmann/json.hpp>
#include <fstream>
#include <limits>
#include <stdexcept>
namespace quickdns {
namespace {
using nlohmann::json;
bool valid(const SavedMetricChange& value) {
    return value.luid != 0 && (value.family == IpFamily::V4 || value.family == IpFamily::V6) &&
           !value.applied.automatic && value.applied.metric == 1;
}
InterfaceMetric parse_metric(const json& value) {
    if (!value.is_object() || !value.contains("automatic") || !value["automatic"].is_boolean() ||
        !value.contains("metric") || !value["metric"].is_number_unsigned()) throw std::runtime_error("invalid metric");
    auto number = value["metric"].get<std::uint64_t>();
    if (number > (std::numeric_limits<std::uint32_t>::max)()) throw std::runtime_error("invalid metric");
    return {value["automatic"].get<bool>(), static_cast<std::uint32_t>(number)};
}
}
FileMetricJournal::~FileMetricJournal() {
    if (lock_) CloseHandle(static_cast<HANDLE>(lock_));
}
bool FileMetricJournal::load(std::optional<SavedMetricChange>& out) {
    loaded_ = false; error_.clear();
    try {
        if (path_.empty()) throw std::runtime_error("journal path unavailable");
        if (!lock_) {
            auto parent = path_.parent_path();
            if (parent.empty()) parent = L".";
            std::filesystem::create_directories(parent);
            auto lock_path = path_; lock_path += L".lock";
            HANDLE lock = CreateFileW(lock_path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
                                      OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (lock == INVALID_HANDLE_VALUE) throw std::runtime_error("journal already in use or lock unavailable");
            lock_ = lock;
        }
        if (!std::filesystem::exists(path_)) { out.reset(); loaded_ = true; return true; }
        std::ifstream input(path_, std::ios::binary);
        if (!input) throw std::runtime_error("cannot read metric journal");
        const auto root = json::parse(input);
        if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("family") ||
            !root["family"].is_number_integer() || !root.contains("luid") || !root["luid"].is_number_unsigned() ||
            !root.contains("original") || !root.contains("applied")) throw std::runtime_error("invalid journal format");
        const int family = root["family"].get<int>();
        if (family != 4 && family != 6) throw std::runtime_error("invalid address family");
        SavedMetricChange change{family == 4 ? IpFamily::V4 : IpFamily::V6, root["luid"].get<std::uint64_t>(),
                                 parse_metric(root["original"]), parse_metric(root["applied"])};
        if (!valid(change)) throw std::runtime_error("invalid metric change");
        out = change; loaded_ = true; return true;
    } catch (const std::exception& ex) { error_ = ex.what(); return false; }
}
bool FileMetricJournal::save(const SavedMetricChange& change) {
    error_.clear(); std::filesystem::path temp;
    try {
        if (!loaded_ || !valid(change)) throw std::runtime_error("journal not loaded or change invalid");
        const json root{{"version", 1}, {"family", change.family == IpFamily::V4 ? 4 : 6}, {"luid", change.luid},
                        {"original", {{"automatic", change.original.automatic}, {"metric", change.original.metric}}},
                        {"applied", {{"automatic", change.applied.automatic}, {"metric", change.applied.metric}}}};
        const auto data = root.dump(2);
        auto directory = path_.parent_path();
        if (directory.empty()) directory = L".";
        std::filesystem::create_directories(directory);
        wchar_t name[MAX_PATH]{};
        if (!GetTempFileNameW(directory.c_str(), L"qdm", 0, name)) throw std::runtime_error("cannot create temporary journal");
        temp = name;
        HANDLE file = CreateFileW(temp.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open temporary journal");
        DWORD written = 0;
        const bool ok = data.size() <= MAXDWORD && WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                        written == data.size() && FlushFileBuffers(file);
        const bool closed = CloseHandle(file) != FALSE;
        if (!ok || !closed) throw std::runtime_error("cannot write temporary journal");
        if (!MoveFileExW(temp.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace journal");
        return true;
    } catch (const std::exception& ex) {
        if (!temp.empty()) DeleteFileW(temp.c_str());
        error_ = ex.what(); return false;
    }
}
bool FileMetricJournal::clear() {
    if (!loaded_) { error_ = "journal not loaded"; return false; }
    if (DeleteFileW(path_.c_str()) || GetLastError() == ERROR_FILE_NOT_FOUND) { error_.clear(); return true; }
    error_ = "cannot delete metric journal"; return false;
}
}
