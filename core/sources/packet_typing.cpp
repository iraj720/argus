#include "core/sources/packet_typing.h"

namespace irs3 {

namespace {

std::string prefix_before_slash(const std::string &value) {
    const std::size_t slash = value.find('/');
    if (slash == std::string::npos) {
        return value;
    }
    return value.substr(0, slash);
}

} // namespace

std::string StreamTypeFromPacketType(const std::string &packet_type) {
    return prefix_before_slash(packet_type);
}

bool PacketTypeMatchesStreamType(const std::string &packet_type, const std::string &stream_type) {
    return StreamTypeFromPacketType(packet_type) == stream_type;
}

bool IsRemuxAllowedStreamType(const std::string &stream_type) {
    return stream_type == "video" || stream_type == "voice";
}

SourceCodec CodecFromPacketType(const std::string &packet_type) {
    if (packet_type == "video/h264") {
        return SourceCodec::kH264;
    }
    if (packet_type == "voice/aac") {
        return SourceCodec::kAAC;
    }
    if (packet_type == "voice/opus") {
        return SourceCodec::kOpus;
    }
    return SourceCodec::kUnknown;
}

int DefaultClockRateForPacketType(const std::string &packet_type) {
    const SourceCodec codec = CodecFromPacketType(packet_type);
    if (codec == SourceCodec::kH264) {
        return 90000;
    }
    if (codec == SourceCodec::kOpus || codec == SourceCodec::kAAC) {
        return 48000;
    }
    return 90000;
}

void AssignRtmpFlvPacketTyping(SourcePacket *packet, int tag_type) {
    if (packet == nullptr) {
        return;
    }
    if (tag_type == 9) {
        packet->stream_type = "video";
        packet->packet_type = "video/h264";
        packet->stream_id = "video/main";
        packet->track_kind = SourceTrackKind::kVideo;
        return;
    }
    if (tag_type == 8) {
        packet->stream_type = "voice";
        packet->packet_type = "voice/aac";
        packet->stream_id = "voice/main";
        packet->track_kind = SourceTrackKind::kAudio;
        return;
    }
    packet->stream_type = "text";
    packet->packet_type = "text/metadata";
    packet->stream_id = "text/metadata";
    packet->track_kind = SourceTrackKind::kData;
}

void AssignWhipVideoPacketTyping(SourcePacket *packet) {
    if (packet == nullptr) {
        return;
    }
    packet->stream_type = "video";
    packet->packet_type = "video/h264";
    packet->stream_id = "video/main";
    packet->track_kind = SourceTrackKind::kVideo;
}

void AssignWhipVoicePacketTyping(SourcePacket *packet, bool opus) {
    if (packet == nullptr) {
        return;
    }
    packet->stream_type = "voice";
    packet->packet_type = opus ? "voice/opus" : "voice/aac";
    packet->stream_id = "voice/main";
    packet->track_kind = SourceTrackKind::kAudio;
}

} // namespace irs3
