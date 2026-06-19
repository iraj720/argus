#ifndef ARGUS_CORE_SOURCES_SUBSCRIPTION_FILTER_H
#define ARGUS_CORE_SOURCES_SUBSCRIPTION_FILTER_H

#include "core/sources/source_packet.h"

#include <string>

namespace irs3 {

struct SubscriptionFilter {
    std::string stream_type;
    std::string packet_type;
    std::string stream_id;

    bool IsEmpty() const {
        return stream_type.empty() && packet_type.empty() && stream_id.empty();
    }

    bool Matches(const SourcePacket &packet) const {
        if (IsEmpty()) {
            return true;
        }
        if (!stream_type.empty() && packet.stream_type != stream_type) {
            return false;
        }
        if (!packet_type.empty() && packet.packet_type != packet_type) {
            return false;
        }
        if (!stream_id.empty() && packet.stream_id != stream_id) {
            return false;
        }
        return true;
    }
};

inline SubscriptionFilter MakeSubscriptionFilter(
    const std::string &stream_type,
    const std::string &packet_type,
    const std::string &stream_id
) {
    SubscriptionFilter filter;
    filter.stream_type = stream_type;
    filter.packet_type = packet_type;
    filter.stream_id = stream_id;
    return filter;
}

} // namespace irs3

#endif
