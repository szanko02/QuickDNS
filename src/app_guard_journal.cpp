#include "app_guard_journal.h"
#include <nlohmann/json.hpp>
#include <objbase.h>
#include <fstream>
#include <set>
#include <stdexcept>
#include <utility>

namespace quickdns {
namespace {
using nlohmann::json;
std::string guid_string(const GUID& id) {
    wchar_t text[39]{};
    if (StringFromGUID2(id, text, 39) != 39) throw std::runtime_error("invalid GUID");
    std::string result;
    for (const wchar_t* p = text; *p; ++p) result.push_back(static_cast<char>(*p));
    return result;
}
GUID parse_guid(const json& value) {
    if (!value.is_string()) throw std::runtime_error("GUID must be text");
    const auto text = value.get<std::string>();
    if (text.size() != 38) throw std::runtime_error("invalid GUID text");
    std::wstring wide(text.begin(), text.end());
    GUID id{};
    if (FAILED(CLSIDFromString(wide.c_str(), &id))) throw std::runtime_error("invalid GUID text");
    return id;
}
const char* phase_name(GuardJournalPhase phase) {
    switch (phase) {
    case GuardJournalPhase::Prepared: return "prepared";
    case GuardJournalPhase::Active: return "active";
    case GuardJournalPhase::Removing: return "removing";
    }
    throw std::runtime_error("invalid phase");
}
GuardJournalPhase parse_phase(const json& value) {
    if (value == "prepared") return GuardJournalPhase::Prepared;
    if (value == "active") return GuardJournalPhase::Active;
    if (value == "removing") return GuardJournalPhase::Removing;
    throw std::runtime_error("invalid phase");
}
void validate(const std::vector<GuardJournalEntry>& entries) {
    std::set<std::string> rule_ids, keys;
    const GUID empty{};
    const auto add_key = [&](const GUID& key) {
        if (IsEqualGUID(key, empty) || !keys.insert(guid_string(key)).second)
            throw std::runtime_error("empty or duplicate guard key");
    };
    for (const auto& e : entries) {
        if (e.rule_id.empty() || !rule_ids.insert(e.rule_id).second) throw std::runtime_error("empty or duplicate rule id");
        (void)phase_name(e.phase);
        add_key(e.keys.sublayer);
        for (const auto& key : e.keys.filters) add_key(key);
        if (e.policy_keys) {
            add_key(e.policy_keys->ipv4);
            add_key(e.policy_keys->ipv6);
        }
    }
}
}

GuardJournal::GuardJournal(std::filesystem::path path) : path_(std::move(path)) {}
bool GuardJournal::load() {
    error_.clear(); loaded_ = false;
    try {
        if (path_.empty()) throw std::runtime_error("journal path unavailable");
        if (!std::filesystem::exists(path_)) { entries_.clear(); loaded_ = true; return true; }
        std::ifstream in(path_, std::ios::binary);
        if (!in) throw std::runtime_error("cannot open journal");
        const json root = json::parse(in);
        if (!root.is_object() || root.value("version", 0) != 1 || !root.contains("guards") || !root["guards"].is_array())
            throw std::runtime_error("unsupported journal format");
        std::vector<GuardJournalEntry> loaded;
        for (const auto& item : root["guards"]) {
            GuardJournalEntry entry{};
            entry.rule_id = item.at("rule_id").get<std::string>();
            entry.phase = parse_phase(item.at("phase"));
            entry.keys.sublayer = parse_guid(item.at("sublayer"));
            const auto& filters = item.at("filters");
            if (!filters.is_array() || filters.size() != entry.keys.filters.size()) throw std::runtime_error("invalid filter list");
            for (std::size_t i = 0; i < entry.keys.filters.size(); ++i) entry.keys.filters[i] = parse_guid(filters[i]);
            if (item.contains("policy_keys")) {
                const auto& keys = item.at("policy_keys");
                if (!keys.is_array() || keys.size() != 2) throw std::runtime_error("invalid policy key list");
                entry.policy_keys = PolicyKeys{parse_guid(keys[0]), parse_guid(keys[1])};
            }
            loaded.push_back(std::move(entry));
        }
        validate(loaded); entries_ = std::move(loaded); loaded_ = true; return true;
    } catch (const std::exception& ex) { error_ = ex.what(); return false; }
}
bool GuardJournal::replace(std::vector<GuardJournalEntry> entries) {
    try {
        if (!loaded_) throw std::runtime_error("journal not loaded");
        validate(entries); entries_ = std::move(entries); error_.clear(); return true;
    } catch (const std::exception& ex) { error_ = ex.what(); return false; }
}
bool GuardJournal::save() {
    error_.clear(); std::filesystem::path temporary;
    try {
        if (!loaded_) throw std::runtime_error("journal not loaded");
        validate(entries_);
        json root{{"version", 1}, {"guards", json::array()}};
        for (const auto& entry : entries_) {
            json item{{"rule_id", entry.rule_id}, {"phase", phase_name(entry.phase)},
                      {"sublayer", guid_string(entry.keys.sublayer)}, {"filters", json::array()}};
            for (const auto& key : entry.keys.filters) item["filters"].push_back(guid_string(key));
            if (entry.policy_keys) item["policy_keys"] = {guid_string(entry.policy_keys->ipv4), guid_string(entry.policy_keys->ipv6)};
            root["guards"].push_back(std::move(item));
        }
        const std::string data = root.dump(2);
        auto directory = path_.parent_path();
        if (directory.empty()) directory = L".";
        std::filesystem::create_directories(directory);
        wchar_t name[MAX_PATH]{};
        if (!GetTempFileNameW(directory.c_str(), L"qgj", 0, name)) throw std::runtime_error("cannot create temporary journal");
        temporary = name;
        HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, TRUNCATE_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (file == INVALID_HANDLE_VALUE) throw std::runtime_error("cannot open temporary journal");
        DWORD written = 0;
        const bool ok = data.size() <= MAXDWORD && WriteFile(file, data.data(), static_cast<DWORD>(data.size()), &written, nullptr) &&
                        written == data.size() && FlushFileBuffers(file);
        const bool closed = CloseHandle(file) != FALSE;
        if (!ok || !closed) throw std::runtime_error("cannot write journal");
        if (!MoveFileExW(temporary.c_str(), path_.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
            throw std::runtime_error("cannot replace journal");
        return true;
    } catch (const std::exception& ex) {
        if (!temporary.empty()) DeleteFileW(temporary.c_str());
        error_ = ex.what(); return false;
    }
}
}
