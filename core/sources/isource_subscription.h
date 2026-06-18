#ifndef ARGUS_CORE_SOURCES_ISOURCE_SUBSCRIPTION_H
#define ARGUS_CORE_SOURCES_ISOURCE_SUBSCRIPTION_H

#include "core/sources/source_packet.h"

#include <memory>

namespace irs3 {

class ISourceSubscription {
public:
    virtual ~ISourceSubscription() = default;

    virtual bool Read(SourcePacket *out) = 0;
    virtual void Close() = 0;
};

using SourceSubscriptionPtr = std::shared_ptr<ISourceSubscription>;

} // namespace irs3

#endif
