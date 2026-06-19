#include "core/sources/buffered_source.h"

namespace irs3 {

BufferedSource::BufferedSource(SourceDescriptor descriptor, std::size_t max_packets_per_stream)
    : descriptor_(std::move(descriptor)),
      max_packets_per_stream_(max_packets_per_stream) {
}

const SourceDescriptor &BufferedSource::Descriptor() const {
    return descriptor_;
}

bool BufferedSource::WaitReady(SourceFormat *out) {
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

SourceSubscriptionPtr BufferedSource::Subscribe() {
    return Subscribe(SubscriptionFilter{});
}

SourceSubscriptionPtr BufferedSource::Subscribe(const SubscriptionFilter &filter) {
    return std::make_shared<BufferedSourceSubscription>(shared_from_this(), filter);
}

void BufferedSource::Close() {
    std::lock_guard<std::mutex> lock(mutex_);
    closed_ = true;
    data_cond_.notify_all();
    ready_cond_.notify_all();
}

void BufferedSource::SetDescriptor(SourceDescriptor descriptor) {
    std::lock_guard<std::mutex> lock(mutex_);
    descriptor_ = std::move(descriptor);
}

void BufferedSource::SetFormat(SourceFormat format) {
    std::lock_guard<std::mutex> lock(mutex_);
    format_ = std::move(format);
    ready_ = true;
    ready_cond_.notify_all();
}

void BufferedSource::SetResumePrefixPackets(const std::string &stream_id, std::vector<SourcePacket> packets) {
    std::lock_guard<std::mutex> lock(mutex_);
    StreamRing &ring = ring_for_stream(stream_id);
    ring.prefix_packets.clear();
    ring.prefix_packets.reserve(packets.size());
    for (SourcePacket &packet : packets) {
        ring.prefix_packets.push_back(std::make_shared<SourcePacket>(std::move(packet)));
    }
    data_cond_.notify_all();
}

bool BufferedSource::PublishPacket(SourcePacket packet) {
    if (packet.stream_id.empty()) {
        return false;
    }
    std::lock_guard<std::mutex> lock(mutex_);
    if (closed_) {
        return false;
    }
    StreamRing &ring = ring_for_stream(packet.stream_id);
    packet.sequence = ring.next_sequence++;
    ring.packets.push_back(std::make_shared<SourcePacket>(std::move(packet)));
    trim_ring_locked(&ring);
    data_cond_.notify_all();
    return true;
}

BufferedSource::StreamRing &BufferedSource::ring_for_stream(const std::string &stream_id) {
    return rings_[stream_id];
}

void BufferedSource::trim_ring_locked(StreamRing *ring) {
    if (ring == nullptr) {
        return;
    }
    while (ring->packets.size() > max_packets_per_stream_) {
        ring->packets.pop_front();
    }
}

std::uint64_t BufferedSource::resume_sequence_locked(const StreamRing &ring) const {
    for (auto it = ring.packets.rbegin(); it != ring.packets.rend(); ++it) {
        if ((*it)->gop_start) {
            return (*it)->sequence;
        }
    }
    if (!ring.packets.empty()) {
        return ring.packets.front()->sequence;
    }
    return ring.next_sequence;
}

bool BufferedSource::ReadForSubscription(BufferedSourceSubscription *subscription, SourcePacket *out) {
    if (subscription == nullptr || out == nullptr) {
        return false;
    }
    if (subscription->filter_.stream_id.empty()) {
        return false;
    }

    const std::string &stream_id = subscription->filter_.stream_id;
    std::unique_lock<std::mutex> lock(mutex_);

    auto ring_it = rings_.find(stream_id);
    if (ring_it == rings_.end()) {
        rings_[stream_id] = StreamRing{};
        ring_it = rings_.find(stream_id);
    }
    StreamRing &ring = ring_it->second;

    if (!subscription->next_sequence_initialized_) {
        subscription->next_sequence_ = ring.packets.empty() ? ring.next_sequence : ring.packets.front()->sequence;
        subscription->next_sequence_initialized_ = true;
    }

    for (;;) {
        if (subscription->closed_) {
            return false;
        }

        if (subscription->prefix_index_ < ring.prefix_packets.size()) {
            const SourcePacket &candidate = *ring.prefix_packets[subscription->prefix_index_];
            if (!subscription->filter_.Matches(candidate)) {
                subscription->prefix_index_++;
                continue;
            }
            *out = candidate;
            subscription->prefix_index_++;
            return true;
        }

        if (!ring.packets.empty() && subscription->next_sequence_ < ring.packets.front()->sequence) {
            subscription->next_sequence_ = resume_sequence_locked(ring);
            subscription->prefix_index_ = 0;
            continue;
        }

        if (!ring.packets.empty() &&
            subscription->next_sequence_ >= ring.packets.front()->sequence &&
            subscription->next_sequence_ <= ring.packets.back()->sequence) {
            const std::size_t index =
                static_cast<std::size_t>(subscription->next_sequence_ - ring.packets.front()->sequence);
            const SourcePacket &candidate = *ring.packets[index];
            subscription->next_sequence_ = ring.packets[index]->sequence + 1;
            if (!subscription->filter_.Matches(candidate)) {
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

BufferedSourceSubscription::BufferedSourceSubscription(
    const std::shared_ptr<BufferedSource> &source,
    SubscriptionFilter filter
)
    : source_(source),
      filter_(std::move(filter)) {
}

bool BufferedSourceSubscription::Read(SourcePacket *out) {
    std::shared_ptr<BufferedSource> source = source_.lock();
    if (source == nullptr || out == nullptr) {
        return false;
    }
    return source->ReadForSubscription(this, out);
}

void BufferedSourceSubscription::Close() {
    closed_ = true;
    if (std::shared_ptr<BufferedSource> source = source_.lock()) {
        std::lock_guard<std::mutex> lock(source->mutex_);
        source->data_cond_.notify_all();
    }
}

} // namespace irs3
