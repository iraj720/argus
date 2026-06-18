#include "core/decoder/packet_adapter.h"

namespace irs3 {

namespace {

const std::vector<std::uint8_t> kEmptyPayload;

const std::vector<std::uint8_t> &packet_payload(const SourcePacket &packet) {
    return packet.payload != nullptr ? *packet.payload : kEmptyPayload;
}

bool is_flv_video_config_tag(const std::vector<std::uint8_t> &payload) {
    return payload.size() >= 2 && (payload[0] & 0x0f) == 7 && payload[1] == 0;
}

std::size_t flv_avc_extradata_offset(const std::vector<std::uint8_t> &payload) {
    if (payload.size() >= 6 && payload[1] == 0 && payload[5] == 0x01) {
        return 5;
    }
    return 2;
}

std::size_t flv_avc_media_offset(const std::vector<std::uint8_t> &payload) {
    if (payload.size() >= 6 && payload[1] == 1) {
        return 5;
    }
    return 2;
}

bool is_flv_video_nalu_tag(const std::vector<std::uint8_t> &payload) {
    return payload.size() >= 6 && (payload[0] & 0x0f) == 7 && payload[1] == 1;
}

const SourceTrackConfig *find_h264_video_track(const SourceFormat &format, std::size_t stream_index) {
    if (format.payload_mode == SourcePayloadMode::kPacketized) {
        if (stream_index < format.tracks.size()) {
            const SourceTrackConfig &track = format.tracks[stream_index];
            if (track.kind == SourceTrackKind::kVideo && track.codec == SourceCodec::kH264) {
                return &track;
            }
        }
        for (const SourceTrackConfig &track : format.tracks) {
            if (track.kind == SourceTrackKind::kVideo && track.codec == SourceCodec::kH264) {
                return &track;
            }
        }
        return nullptr;
    }

    for (const SourceTrackConfig &track : format.tracks) {
        if (track.kind == SourceTrackKind::kVideo && track.codec == SourceCodec::kH264) {
            return &track;
        }
    }
    return nullptr;
}

bool adapt_flv_packet(const SourcePacket &packet, H264AdaptedPacket *out) {
    const std::vector<std::uint8_t> &payload = packet_payload(packet);
    const bool is_video_tag = packet.tag_type == 9 || packet.track_kind == SourceTrackKind::kVideo;
    if (!is_video_tag || payload.size() < 2 || (payload[0] & 0x0f) != 7) {
        return false;
    }

    if (is_flv_video_config_tag(payload)) {
        out->kind = H264PacketKind::kSequenceHeader;
        const std::size_t offset = flv_avc_extradata_offset(payload);
        out->extradata.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.end());
        out->payload.clear();
        out->pts = packet.timestamp_ms;
        out->dts = packet.timestamp_ms;
        return !out->extradata.empty();
    }

    if (!is_flv_video_nalu_tag(payload)) {
        return false;
    }

    out->kind = H264PacketKind::kMedia;
    out->extradata.clear();
    const std::size_t offset = flv_avc_media_offset(payload);
    out->payload.assign(payload.begin() + static_cast<std::ptrdiff_t>(offset), payload.end());
    out->pts = packet.timestamp_ms;
    out->dts = packet.timestamp_ms;
    return !out->payload.empty();
}

bool adapt_packetized_packet(
    const SourcePacket &packet,
    const SourceFormat &format,
    H264AdaptedPacket *out
) {
    if (packet.track_kind != SourceTrackKind::kVideo) {
        return false;
    }

    const SourceTrackConfig *track = find_h264_video_track(format, packet.stream_index);
    if (track == nullptr || track->codec != SourceCodec::kH264) {
        return false;
    }

    const std::vector<std::uint8_t> &payload = packet_payload(packet);
    if (payload.empty()) {
        return false;
    }

    out->kind = H264PacketKind::kMedia;
    out->extradata = track->extradata;
    out->payload = payload;
    out->pts = packet.pts;
    out->dts = packet.dts;
    return true;
}

} // namespace

bool AdaptH264SourcePacket(
    const SourcePacket &packet,
    SourcePayloadMode payload_mode,
    const SourceFormat &format,
    H264AdaptedPacket *out
) {
    if (out == nullptr) {
        return false;
    }

    *out = H264AdaptedPacket{};
    switch (payload_mode) {
    case SourcePayloadMode::kFlvTag:
        return adapt_flv_packet(packet, out);
    case SourcePayloadMode::kPacketized:
        return adapt_packetized_packet(packet, format, out);
    }
    return false;
}

} // namespace irs3
