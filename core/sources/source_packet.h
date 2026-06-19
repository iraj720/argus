#ifndef ARGUS_CORE_SOURCES_SOURCE_PACKET_H
#define ARGUS_CORE_SOURCES_SOURCE_PACKET_H

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

namespace irs3 {

enum class SourcePayloadMode {
    kFlvTag,
    kPacketized,
};

enum class SourceTrackKind {
    kVideo,
    kAudio,
    kData,
};

enum class SourceCodec {
    kUnknown,
    kH264,
    kAAC,
    kOpus,
};

struct SourceTrackConfig {
    SourceTrackKind kind = SourceTrackKind::kData;
    SourceCodec codec = SourceCodec::kUnknown;
    int clock_rate = 0;
    int channels = 0;
    int width = 0;
    int height = 0;
    std::vector<std::uint8_t> extradata;
};

struct SourceFormat {
    SourcePayloadMode payload_mode = SourcePayloadMode::kFlvTag;
    std::vector<SourceTrackConfig> tracks;
};

struct SourcePacket {
    std::uint64_t sequence = 0;
    std::size_t stream_index = 0;
    SourceTrackKind track_kind = SourceTrackKind::kData;
    int tag_type = 0;
    std::string stream_type;
    std::string packet_type;
    std::string stream_id;
    std::uint32_t timestamp_ms = 0;
    std::int64_t pts = 0;
    std::int64_t dts = 0;
    std::int64_t duration = 0;
    bool key_frame = false;
    bool gop_start = false;
    std::shared_ptr<std::vector<std::uint8_t>> payload;
};

} // namespace irs3

#endif
