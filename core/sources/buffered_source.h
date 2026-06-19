#ifndef ARGUS_CORE_SOURCES_BUFFERED_SOURCE_H
#define ARGUS_CORE_SOURCES_BUFFERED_SOURCE_H

#include "core/sources/isource.h"

#include <algorithm>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <mutex>
#include <utility>
#include <vector>

namespace irs3 {

class BufferedSource;

class BufferedSourceSubscription : public ISourceSubscription {
public:
    BufferedSourceSubscription(const std::shared_ptr<BufferedSource> &source, SubscriptionFilter filter);
    bool Read(SourcePacket *out) override;
    void Close() override;

private:
    std::weak_ptr<BufferedSource> source_;
    SubscriptionFilter filter_;
    std::uint64_t next_sequence_ = 0;
    bool next_sequence_initialized_ = false;
    bool closed_ = false;
    std::size_t prefix_index_ = 0;

    friend class BufferedSource;
};

class BufferedSource : public ISource, public std::enable_shared_from_this<BufferedSource> {
public:
    explicit BufferedSource(SourceDescriptor descriptor, std::size_t max_packets = 4096)
        : descriptor_(std::move(descriptor)),
          max_packets_(max_packets) {
    }

    const SourceDescriptor &Descriptor() const override {
        return descriptor_;
    }

    bool WaitReady(SourceFormat *out) override {
        std::unique_lock<std::mutex> lock(mutex_);
        ready_cond_.wait(lock, [this]() {
            return closed_ || ready_;
        });
        if (!ready_) {
            return false;
        }
        if (out != nullptr) {
            *out = format_;
        }
        return true;
    }

    SourceSubscriptionPtr Subscribe() override {
        return Subscribe(SubscriptionFilter{});
    }

    SourceSubscriptionPtr Subscribe(const SubscriptionFilter &filter) override {
        return std::make_shared<BufferedSourceSubscription>(shared_from_this(), filter);
    }

    void Close() override {
        std::lock_guard<std::mutex> lock(mutex_);
        closed_ = true;
        data_cond_.notify_all();
        ready_cond_.notify_all();
    }

    void SetDescriptor(SourceDescriptor descriptor) {
        std::lock_guard<std::mutex> lock(mutex_);
        descriptor_ = std::move(descriptor);
    }

    void SetFormat(SourceFormat format) {
        std::lock_guard<std::mutex> lock(mutex_);
        format_ = std::move(format);
        ready_ = true;
        ready_cond_.notify_all();
    }

    void SetResumePrefixPackets(std::vector<SourcePacket> packets) {
        std::lock_guard<std::mutex> lock(mutex_);
        prefix_packets_.clear();
        prefix_packets_.reserve(packets.size());
        for (SourcePacket &packet : packets) {
            prefix_packets_.push_back(std::make_shared<SourcePacket>(std::move(packet)));
        }
        data_cond_.notify_all();
    }

    bool PublishPacket(SourcePacket packet) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (closed_) {
            return false;
        }
        packet.sequence = next_sequence_++;
        packets_.push_back(std::make_shared<SourcePacket>(std::move(packet)));
        while (packets_.size() > max_packets_) {
            packets_.pop_front();
        }
        data_cond_.notify_all();
        return true;
    }

private:
    bool ReadForSubscription(BufferedSourceSubscription *subscription, SourcePacket *out) {
        std::unique_lock<std::mutex> lock(mutex_);
        if (!subscription->next_sequence_initialized_) {
            subscription->next_sequence_ = packets_.empty() ? next_sequence_ : packets_.front()->sequence;
            subscription->next_sequence_initialized_ = true;
        }

        for (;;) {
            if (subscription->closed_) {
                return false;
            }

            if (subscription->prefix_index_ < prefix_packets_.size()) {
                const SourcePacket &candidate = *prefix_packets_[subscription->prefix_index_];
                if (!subscription->filter_.IsEmpty() && !subscription->filter_.Matches(candidate)) {
                    subscription->prefix_index_++;
                    continue;
                }
                *out = candidate;
                subscription->prefix_index_++;
                return true;
            }

            if (!packets_.empty() && subscription->next_sequence_ < packets_.front()->sequence) {
                subscription->next_sequence_ = resume_sequence_locked();
                subscription->prefix_index_ = 0;
                continue;
            }

            if (!packets_.empty() &&
                subscription->next_sequence_ >= packets_.front()->sequence &&
                subscription->next_sequence_ <= packets_.back()->sequence) {
                std::size_t index = static_cast<std::size_t>(subscription->next_sequence_ - packets_.front()->sequence);
                const SourcePacket &candidate = *packets_[index];
                subscription->next_sequence_ = packets_[index]->sequence + 1;
                if (!subscription->filter_.IsEmpty() && !subscription->filter_.Matches(candidate)) {
                    continue;
                }
                *out = candidate;
                return true;
            }

            if (closed_) {
                return false;
            }

            data_cond_.wait(lock);
        }
    }

    std::uint64_t resume_sequence_locked() const {
        for (auto it = packets_.rbegin(); it != packets_.rend(); ++it) {
            if ((*it)->gop_start) {
                return (*it)->sequence;
            }
        }
        if (!packets_.empty()) {
            return packets_.front()->sequence;
        }
        return next_sequence_;
    }

    mutable std::mutex mutex_;
    std::condition_variable data_cond_;
    std::condition_variable ready_cond_;
    SourceDescriptor descriptor_;
    SourceFormat format_{};
    bool ready_ = false;
    bool closed_ = false;
    std::deque<std::shared_ptr<SourcePacket>> packets_;
    std::vector<std::shared_ptr<SourcePacket>> prefix_packets_;
    std::uint64_t next_sequence_ = 0;
    std::size_t max_packets_ = 0;

    friend class BufferedSourceSubscription;
};

inline BufferedSourceSubscription::BufferedSourceSubscription(
    const std::shared_ptr<BufferedSource> &source,
    SubscriptionFilter filter
)
    : source_(source),
      filter_(std::move(filter)) {
}

inline bool BufferedSourceSubscription::Read(SourcePacket *out) {
    std::shared_ptr<BufferedSource> source = source_.lock();
    if (source == nullptr || out == nullptr) {
        return false;
    }
    return source->ReadForSubscription(this, out);
}

inline void BufferedSourceSubscription::Close() {
    closed_ = true;
    if (std::shared_ptr<BufferedSource> source = source_.lock()) {
        std::lock_guard<std::mutex> lock(source->mutex_);
        source->data_cond_.notify_all();
    }
}

} // namespace irs3

#endif
