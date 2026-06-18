#ifndef ARGUS_CORE_SOURCES_ISOURCE_H
#define ARGUS_CORE_SOURCES_ISOURCE_H

#include "core/sources/isource_subscription.h"
#include "core/sources/source_packet.h"

#include <memory>
#include <string>

namespace irs3 {

enum class SourceProtocol {
    kRTMP,
    kWHIP,
};

struct SourceDescriptor {
    std::string id;
    SourceProtocol protocol = SourceProtocol::kRTMP;
    std::string app;
    std::string stream;
    unsigned long session_id = 0;
};

class ISource {
public:
    virtual ~ISource() = default;

    virtual const SourceDescriptor &Descriptor() const = 0;
    virtual bool WaitReady(SourceFormat *out) = 0;
    virtual SourceSubscriptionPtr Subscribe() = 0;
    virtual void Close() = 0;
};

using SourcePtr = std::shared_ptr<ISource>;

} // namespace irs3

#endif
