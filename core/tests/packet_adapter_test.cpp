#include "core/decoder/packet_adapter.h"

#include <cstdio>
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

irs3::SourcePacket make_packet(std::vector<std::uint8_t> payload) {
    irs3::SourcePacket packet;
    packet.payload = std::make_shared<std::vector<std::uint8_t>>(std::move(payload));
    return packet;
}

} // namespace

int main() {
    irs3::SourceFormat format;
    format.payload_mode = irs3::SourcePayloadMode::kFlvTag;

    irs3::SourcePacket sequence_header = make_packet({
        0x17, 0x00, 0x01, 0x42, 0x00, 0x1e, 0xff, 0xe1, 0x00, 0x04, 0x67, 0x42, 0x00, 0x1e,
        0x01, 0x00, 0x04, 0x68, 0xce, 0x38, 0x80,
    });
    sequence_header.tag_type = 9;

    irs3::H264AdaptedPacket adapted_sequence{};
    expect(
        irs3::AdaptH264SourcePacket(
            sequence_header,
            irs3::SourcePayloadMode::kFlvTag,
            format,
            &adapted_sequence
        ),
        "flv sequence header should adapt"
    );
    expect(adapted_sequence.kind == irs3::H264PacketKind::kSequenceHeader, "flv sequence header kind");
    expect(!adapted_sequence.extradata.empty(), "flv sequence header extradata");
    expect(adapted_sequence.extradata[0] == 0x01, "flv sequence header extradata starts with avcC version");

    irs3::SourcePacket ffmpeg_sequence_header = make_packet({
        0x17, 0x00, 0x00, 0x00, 0x00, 0x01, 0x64, 0x00, 0x28, 0xff, 0xe1, 0x00, 0x18, 0x67,
    });
    ffmpeg_sequence_header.tag_type = 9;

    irs3::H264AdaptedPacket adapted_ffmpeg_sequence{};
    expect(
        irs3::AdaptH264SourcePacket(
            ffmpeg_sequence_header,
            irs3::SourcePayloadMode::kFlvTag,
            format,
            &adapted_ffmpeg_sequence
        ),
        "ffmpeg-style flv sequence header should adapt"
    );
    expect(adapted_ffmpeg_sequence.extradata[0] == 0x01, "ffmpeg-style extradata skips composition time");

    irs3::SourcePacket media_packet = make_packet({
        0x17, 0x01, 0x00, 0x00, 0x00, 0x00, 0x04, 0x65, 0x88, 0x84, 0x00,
    });
    media_packet.tag_type = 9;

    irs3::H264AdaptedPacket adapted_media{};
    expect(
        irs3::AdaptH264SourcePacket(
            media_packet,
            irs3::SourcePayloadMode::kFlvTag,
            format,
            &adapted_media
        ),
        "flv media packet should adapt"
    );
    expect(adapted_media.kind == irs3::H264PacketKind::kMedia, "flv media packet kind");
    expect(!adapted_media.payload.empty(), "flv media packet payload");

    irs3::SourceFormat packetized_format;
    packetized_format.payload_mode = irs3::SourcePayloadMode::kPacketized;
    packetized_format.tracks.push_back(irs3::SourceTrackConfig{
        irs3::SourceTrackKind::kVideo,
        irs3::SourceCodec::kH264,
        90000,
        0,
        1280,
        720,
        {0x01, 0x42, 0x00, 0x1e},
    });

    irs3::SourcePacket packetized = make_packet({0x00, 0x00, 0x00, 0x04, 0x65, 0x88, 0x84, 0x00});
    packetized.track_kind = irs3::SourceTrackKind::kVideo;
    packetized.stream_index = 0;
    packetized.pts = 9000;
    packetized.dts = 9000;

    irs3::H264AdaptedPacket adapted_packetized{};
    expect(
        irs3::AdaptH264SourcePacket(
            packetized,
            irs3::SourcePayloadMode::kPacketized,
            packetized_format,
            &adapted_packetized
        ),
        "packetized media packet should adapt"
    );
    expect(adapted_packetized.kind == irs3::H264PacketKind::kMedia, "packetized media packet kind");
    expect(!adapted_packetized.payload.empty(), "packetized media packet payload");
    expect(!adapted_packetized.extradata.empty(), "packetized media packet extradata");

    if (failures != 0) {
        std::fprintf(stderr, "%d test(s) failed\n", failures);
        return 1;
    }

    std::fprintf(stderr, "packet_adapter_test passed\n");
    return 0;
}
