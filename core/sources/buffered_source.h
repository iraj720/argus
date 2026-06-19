#ifndef ARGUS_CORE_SOURCES_BUFFERED_SOURCE_H
#define ARGUS_CORE_SOURCES_BUFFERED_SOURCE_H

#include "core/sources/isource.h"

#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_map>
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
    explicit BufferedSource(SourceDescriptor descriptor, std::size_t max_packets_per_stream = 4096);

    const SourceDescriptor &Descriptor() const override;
    bool WaitReady(SourceFormat *out) override;
    SourceSubscriptionPtr Subscribe() override;
    SourceSubscriptionPtr Subscribe(const SubscriptionFilter &filter) override;
    void Close() override;

    void SetDescriptor(SourceDescriptor descriptor);
    void SetFormat(SourceFormat format);
    void SetResumePrefixPackets(const std::string &stream_id, std::vector<SourcePacket> packets);
    bool PublishPacket(SourcePacket packet);

private:
    struct StreamRing {
        std::deque<std::shared_ptr<SourcePacket>> packets;
        std::vector<std::shared_ptr<SourcePacket>> prefix_packets;
        std::uint64_t next_sequence = 0;
    };

    bool ReadForSubscription(BufferedSourceSubscription *subscription, SourcePacket *out);
    StreamRing &ring_for_stream(const std::string &stream_id);
    std::uint64_t resume_sequence_locked(const StreamRing &ring) const;
    void trim_ring_locked(StreamRing *ring);

    mutable std::mutex mutex_;
    std::condition_variable data_cond_;
    std::condition_variable ready_cond_;
    SourceDescriptor descriptor_;
    SourceFormat format_{};
    bool ready_ = false;
    bool closed_ = false;
    std::unordered_map<std::string, StreamRing> rings_;
    std::size_t max_packets_per_stream_ = 0;

    friend class BufferedSourceSubscription;
};

} // namespace irs3

#endif
