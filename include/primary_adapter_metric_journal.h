#pragma once
#include "primary_adapter_metric.h"
#include <filesystem>
#include <string>

namespace quickdns {
class FileMetricJournal final : public MetricJournal {
public:
    explicit FileMetricJournal(std::filesystem::path path) : path_(std::move(path)) {}
    ~FileMetricJournal();
    FileMetricJournal(const FileMetricJournal&) = delete;
    FileMetricJournal& operator=(const FileMetricJournal&) = delete;
    bool load(std::optional<SavedMetricChange>&) override;
    bool save(const SavedMetricChange&) override;
    bool clear() override;
    const std::string& error() const { return error_; }
private:
    std::filesystem::path path_;
    std::string error_;
    bool loaded_ = false;
    void* lock_ = nullptr;
};
}
