#pragma once
#include "app_route_guard.h"
#include <filesystem>
#include <optional>
#include <string>
#include <vector>

namespace quickdns {
enum class GuardJournalPhase { Prepared, Active, Removing };
struct PolicyKeys { GUID ipv4{}; GUID ipv6{}; };
struct GuardJournalEntry {
    std::string rule_id;
    GuardKeys keys;
    GuardJournalPhase phase;
    std::optional<PolicyKeys> policy_keys;
};

class GuardJournal {
public:
    explicit GuardJournal(std::filesystem::path path);
    bool load();
    bool replace(std::vector<GuardJournalEntry> entries);
    bool save();
    const std::vector<GuardJournalEntry>& entries() const { return entries_; }
    const std::string& error() const { return error_; }
private:
    std::filesystem::path path_;
    std::vector<GuardJournalEntry> entries_;
    std::string error_;
    bool loaded_ = false;
};
}
