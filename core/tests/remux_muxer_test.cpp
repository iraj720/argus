#include "core/mux/remux_muxer.h"
#include "core/sources/ipacket_reader.h"
#include "core/sources/source_packet.h"

#include <cstdio>
#include <deque>
#include <memory>
#include <vector>

namespace {

int failures = 0;

void expect(bool condition, const char *message) {
    if (!condition) {
        std::fprintf(stderr, "FAIL: %s\n", message);
        ++failures;
    }
}

class VectorPacketReader : public irs3::IPacketReader {
public:
    explicit VectorPacketReader(std::deque<irs3::SourcePacket> packets)
        : packets_(std::move(packets)) {
    }

    bool Read(irs3::SourcePacket *out) override {
        if (out == nullptr || packets_.empty()) {
            return false;
        }
        *out = std::move(packets_.front());
        packets_.pop_front();
        return true;
    }

    void Close() override {
        packets_.clear();
    }

private:
    std::deque<irs3::SourcePacket> packets_;
};

irs3::SourcePacket make_flv_packet(int tag_type, std::uint32_t timestamp_ms) {
    irs3::SourcePacket packet;
    packet.tag_type = tag_type;
    packet.timestamp_ms = timestamp_ms;
    packet.pts = static_cast<std::int64_t>(timestamp_ms);
    packet.dts = packet.pts;
    packet.payload = std::make_shared<std::vector<std::uint8_t>>(std::vector<std::uint8_t>{0x01});
    return packet;
}

} // namespace

int main() {
    std::deque<irs3::SourcePacket> video_packets;
    video_packets.push_back(make_flv_packet(9, 100));
    video_packets.push_back(make_flv_packet(9, 133));

    std::deque<irs3::SourcePacket> voice_packets;
    voice_packets.push_back(make_flv_packet(8, 1000));
    voice_packets.push_back(make_flv_packet(8, 1020));

    irs3::RemuxMuxer muxer;
    std::vector<irs3::RemuxMuxLane> lanes(2);
    lanes[0].input_id = "live/studio";
    lanes[0].stream_type = "video";
    lanes[0].reader = std::make_unique<VectorPacketReader>(std::move(video_packets));
    lanes[1].input_id = "live/guest";
    lanes[1].stream_type = "voice";
    lanes[1].reader = std::make_unique<VectorPacketReader>(std::move(voice_packets));

    expect(muxer.Prepare(std::move(lanes), irs3::RemuxPayloadMode::kFlvTag), "muxer prepare should succeed");

    irs3::RemuxMuxPacket first;
    expect(muxer.NextMuxPacket(&first), "first mux packet");
    expect(first.remapped_timestamp_ms == 100, "main video lane should lead at timestamp 100");
    expect(first.packet.tag_type == 9, "first packet should be video");

    irs3::RemuxMuxPacket second;
    expect(muxer.NextMuxPacket(&second), "second mux packet");
    expect(second.remapped_timestamp_ms == 100, "voice lane should remap 1000 -> 100");
    expect(second.packet.tag_type == 8, "second packet should be audio");

    irs3::RemuxMuxPacket third;
    expect(muxer.NextMuxPacket(&third), "third mux packet");
    expect(third.remapped_timestamp_ms == 120, "next voice packet should remap to 120");

    irs3::RemuxMuxPacket fourth;
    expect(muxer.NextMuxPacket(&fourth), "fourth mux packet");
    expect(fourth.remapped_timestamp_ms == 133, "video should continue on main timeline");

    irs3::RemuxMuxPacket fifth;
    expect(!muxer.NextMuxPacket(&fifth), "muxer should stop when lanes are exhausted");

    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::fprintf(stderr, "remux_muxer_test passed\n");
    return 0;
}
