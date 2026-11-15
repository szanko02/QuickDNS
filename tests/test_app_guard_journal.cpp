#include "app_guard_journal.h"
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>

static void require(bool value, const char* message) { if (!value) throw std::runtime_error(message); }
int main() {
    using namespace quickdns;
    const auto path = std::filesystem::temp_directory_path() / L"quickdns_guard_journal_test.json";
    std::filesystem::remove(path);
    try {
        GuardJournal journal(path);
        require(journal.load(), "missing journal must load empty");
        GuardJournalEntry entry{"test-rule", {}, GuardJournalPhase::Prepared};
        entry.keys.sublayer.Data1 = 1;
        for (std::size_t i = 0; i < entry.keys.filters.size(); ++i) entry.keys.filters[i].Data1 = static_cast<unsigned long>(i + 2);
        entry.policy_keys = PolicyKeys{};
        entry.policy_keys->ipv4.Data1 = 20;
        entry.policy_keys->ipv6.Data1 = 21;
        require(journal.replace({entry}) && journal.save(), "cannot journal guard keys before installation");
        GuardJournal restored(path);
        require(restored.load() && restored.entries().size() == 1, "journal was not restored");
        const auto& value = restored.entries()[0];
        require(value.rule_id == "test-rule" && value.phase == GuardJournalPhase::Prepared &&
                IsEqualGUID(value.keys.sublayer, entry.keys.sublayer), "recovered guard differs");
        for (std::size_t i = 0; i < 6; ++i) require(IsEqualGUID(value.keys.filters[i], entry.keys.filters[i]), "recovered filter key differs");
        require(value.policy_keys.has_value() && IsEqualGUID(value.policy_keys->ipv4, entry.policy_keys->ipv4) &&
                IsEqualGUID(value.policy_keys->ipv6, entry.policy_keys->ipv6), "policy keys lost on restart");
        entry.phase = GuardJournalPhase::Removing;
        require(restored.replace({entry}) && restored.save(), "cannot record removal");
        GuardJournal removal(path);
        require(removal.load() && removal.entries()[0].phase == GuardJournalPhase::Removing, "removal phase lost");
        require(!removal.replace({entry, entry}), "duplicate guard identity accepted");
        auto duplicate_policy_key = entry;
        duplicate_policy_key.policy_keys->ipv4 = entry.keys.filters[0];
        require(!removal.replace({duplicate_policy_key}), "policy key aliases guard key");
        auto zero_policy_key = entry;
        zero_policy_key.policy_keys->ipv6 = GUID{};
        require(!removal.replace({zero_policy_key}), "empty policy key accepted");
        const std::string malformed = "{broken";
        { std::ofstream out(path, std::ios::binary | std::ios::trunc); out << malformed; }
        require(!removal.load() && !removal.save(), "malformed journal must not be overwritten");
        std::ifstream in(path, std::ios::binary);
        require(std::string(std::istreambuf_iterator<char>(in), {}) == malformed, "malformed journal was modified");
        in.close();
        std::filesystem::remove(path);
        return 0;
    } catch (const std::exception& e) {
        std::cerr << e.what() << '\n';
        std::filesystem::remove(path);
        return 1;
    }
}
