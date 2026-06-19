#ifndef ARGUS_CORE_MUX_REMUX_MUXER_H
#define ARGUS_CORE_MUX_REMUX_MUXER_H

#include "core/sources/ipacket_reader.h"
#include "core/sources/source_packet.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace irs3 {

enum class RemuxPayloadMode {
    kFlvTag,
    kPacketized,
};

struct RemuxMuxLane {
    std::string input_id;
    std::string stream_type;
    int output_stream_index = 0;
    PacketReaderPtr reader;
};

struct RemuxMuxPacket {
    std::size_t lane_index = 0;
    SourcePacket packet;
    std::int64_t remapped_timestamp_ms = 0;
};

class RemuxMuxer {
public:
    bool Prepare(std::vector<RemuxMuxLane> lanes, RemuxPayloadMode payload_mode);
    bool NextMuxPacket(RemuxMuxPacket *out);
    void Close();

private:
    struct LaneState {
        RemuxMuxLane lane;
        bool is_main = false;
        bool has_first_timestamp = false;
        std::int64_t first_timestamp_ms = 0;
        std::int64_t timestamp_offset_ms = 0;
        bool has_pending = false;
        SourcePacket pending;
    };

    static std::int64_t packet_timestamp_ms(const SourcePacket &packet);
    static void apply_remapped_timestamp_ms(
        const LaneState &lane,
        std::int64_t remapped_timestamp_ms,
        SourcePacket *packet
    );
    bool ensure_pending(LaneState *lane);
    bool all_lanes_have_first_timestamp() const;
    void update_lane_offset(LaneState *lane);

    RemuxPayloadMode payload_mode_ = RemuxPayloadMode::kFlvTag;
    std::vector<LaneState> lanes_;
    bool main_lane_ready_ = false;
    std::int64_t main_first_timestamp_ms_ = 0;
    bool closed_ = false;
};

} // namespace irs3

#endif
