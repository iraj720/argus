#include "core/sources/subscription_packet_reader.h"

namespace irs3 {

SubscriptionPacketReader::SubscriptionPacketReader(SourceSubscriptionPtr subscription)
    : subscription_(std::move(subscription)) {
}

bool SubscriptionPacketReader::Read(SourcePacket *out) {
    if (subscription_ == nullptr || out == nullptr) {
        return false;
    }
    return subscription_->Read(out);
}

void SubscriptionPacketReader::Close() {
    if (subscription_ != nullptr) {
        subscription_->Close();
        subscription_.reset();
    }
}

PacketReaderPtr MakeSubscriptionPacketReader(SourceSubscriptionPtr subscription) {
    return std::make_unique<SubscriptionPacketReader>(std::move(subscription));
}

} // namespace irs3
