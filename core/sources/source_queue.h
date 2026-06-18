#ifndef ARGUS_CORE_SOURCES_SOURCE_QUEUE_H
#define ARGUS_CORE_SOURCES_SOURCE_QUEUE_H

#include "core/sources/isource.h"

#include <condition_variable>
#include <mutex>
#include <queue>

namespace irs3 {

class SourceQueue {
public:
    void Push(SourcePtr source) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return;
        }
        queue_.push(std::move(source));
        cond_.notify_one();
    }

    SourcePtr Pop() {
        std::unique_lock<std::mutex> lock(mutex_);
        cond_.wait(lock, [this]() {
            return closed_ || !queue_.empty();
        });
        if (queue_.empty()) {
            return nullptr;
        }
        SourcePtr source = std::move(queue_.front());
        queue_.pop();
        return source;
    }

    void Close() {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        cond_.notify_all();
    }

private:
    std::mutex mutex_;
    std::condition_variable cond_;
    std::queue<SourcePtr> queue_;
    bool closed_ = false;
};

} // namespace irs3

#endif
