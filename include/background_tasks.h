#pragma once

#include <vector>
#include <thread>
#include <mutex>
#include <functional>
#include <atomic>
#include <memory>

namespace quickdns {

class BackgroundTasks {
public:
    BackgroundTasks() : stopping_(false) {}
    ~BackgroundTasks() {
        join_all();
    }

    BackgroundTasks(const BackgroundTasks&) = delete;
    BackgroundTasks& operator=(const BackgroundTasks&) = delete;

    void run(std::function<void()> task) {
        std::lock_guard<std::mutex> lock(mutex_);
        // Clean up completed background threads
        for (auto it = tasks_.begin(); it != tasks_.end(); ) {
            if ((*it)->finished.load(std::memory_order_acquire) && (*it)->worker.joinable()) {
                (*it)->worker.join();
                it = tasks_.erase(it);
            } else {
                ++it;
            }
        }

        auto item = std::make_shared<TaskItem>();
        item->finished.store(false, std::memory_order_release);
        item->worker = std::thread([task = std::move(task), item]() {
            task();
            item->finished.store(true, std::memory_order_release);
        });
        tasks_.push_back(std::move(item));
    }

    void join_all() {
        stopping_.store(true, std::memory_order_release);
        std::vector<std::shared_ptr<TaskItem>> to_join;
        {
            std::lock_guard<std::mutex> lock(mutex_);
            to_join.swap(tasks_);
        }
        for (auto& item : to_join) {
            if (item && item->worker.joinable()) {
                item->worker.join();
            }
        }
    }

    bool is_stopping() const {
        return stopping_.load(std::memory_order_acquire);
    }

private:
    struct TaskItem {
        std::thread worker;
        std::atomic<bool> finished{false};
    };

    std::vector<std::shared_ptr<TaskItem>> tasks_;
    std::mutex mutex_;
    std::atomic<bool> stopping_{false};
};

} // namespace quickdns
