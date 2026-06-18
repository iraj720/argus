#ifndef ARGUS_CORE_DECODER_PACKET_ADAPTER_H
#define ARGUS_CORE_DECODER_PACKET_ADAPTER_H

#include "core/sources/source_packet.h"

#include <cstddef>
#include <cstdint>
#include <vector>

namespace irs3 {

enum class H264PacketKind {
    kNone,
    kSequenceHeader,
    kMedia,
};

struct H264AdaptedPacket {
    H264PacketKind kind = H264PacketKind::kNone;
    std::vector<std::uint8_t> extradata;
    std::vector<std::uint8_t> payload;
    std::int64_t pts = 0;
    std::int64_t dts = 0;
};

bool AdaptH264SourcePacket(
    const SourcePacket &packet,
    SourcePayloadMode payload_mode,
    const SourceFormat &format,
    H264AdaptedPacket *out
);

} // namespace irs3

#endif
