#include "primary_adapter_metric_journal.h"
#include <windows.h>
#include <fstream>
#include <iostream>
#include <stdexcept>
using namespace quickdns;
static void require(bool value, const char* error) { if (!value) throw std::runtime_error(error); }
int main() {
    auto path = std::filesystem::temp_directory_path() / (L"quickdns_metric_test_" + std::to_wstring(GetCurrentProcessId()) + L".json");
    try {
        std::filesystem::remove(path);
        std::optional<SavedMetricChange> result;
        SavedMetricChange expected{IpFamily::V6, 42, {true, 33}, {false, 1}};
        {
        FileMetricJournal journal(path);
        require(journal.load(result) && !result, "new journal not empty");
        {
            FileMetricJournal concurrent(path);
            require(!concurrent.load(result), "second writer acquired an in-use journal");
        }
        require(journal.save(expected), "could not save journal");
        }
        FileMetricJournal reader(path);
        require(reader.load(result) && result && result->family == IpFamily::V6 && result->luid == 42 &&
                result->original == expected.original && result->applied == expected.applied, "journal round trip failed");
        { std::ofstream out(path, std::ios::trunc); out << "{malformed"; }
        require(!reader.load(result), "malformed journal accepted");
        require(!reader.save(expected) && !reader.clear(), "corrupt journal was overwritten");
        std::ifstream in(path);
        std::string text((std::istreambuf_iterator<char>(in)), {});
        require(text == "{malformed", "corrupt journal changed");
        in.close();
        std::filesystem::remove(path);
        auto lock_path = path; lock_path += L".lock"; std::filesystem::remove(lock_path);
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << ex.what() << '\n';
        std::error_code ignored; std::filesystem::remove(path, ignored);
        auto lock_path = path; lock_path += L".lock"; std::filesystem::remove(lock_path, ignored);
        return 1;
    }
}
