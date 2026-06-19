#ifndef ARGUS_CORE_SOURCES_SUBSCRIPTION_PACKET_READER_H
#define ARGUS_CORE_SOURCES_SUBSCRIPTION_PACKET_READER_H

#include "core/sources/ipacket_reader.h"
#include "core/sources/isource_subscription.h"

namespace irs3 {

class SubscriptionPacketReader : public IPacketReader {
public:
    explicit SubscriptionPacketReader(SourceSubscriptionPtr subscription);

    bool Read(SourcePacket *out) override;
    void Close() override;

private:
    SourceSubscriptionPtr subscription_;
};

PacketReaderPtr MakeSubscriptionPacketReader(SourceSubscriptionPtr subscription);

} // namespace irs3

#endif
