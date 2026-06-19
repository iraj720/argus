#ifndef ARGUS_CORE_SOURCES_PACKET_TIMELINE_H
#define ARGUS_CORE_SOURCES_PACKET_TIMELINE_H

#include <cstdint>

namespace irs3 {

// All packets stored in a source buffer use timestamp_ms as the canonical
// presentation timeline (milliseconds from the track/session origin at ingest).
// Ingest servers (RTMP, WHIP, ...) normalize network timestamps before PublishPacket.

inline std::int64_t rtp_ticks_to_ms(std::int64_t ticks, int clock_rate) {
    if (clock_rate <= 0) {
        return ticks;
    }
    return ticks * 1000 / clock_rate;
}

inline std::int64_t rtp_ticks_delta_to_ms(std::int64_t tick_delta, int clock_rate) {
    return rtp_ticks_to_ms(tick_delta, clock_rate);
}

} // namespace irs3

#endif
