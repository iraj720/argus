#ifndef ARGUS_CORE_SOURCES_PACKET_TYPING_H
#define ARGUS_CORE_SOURCES_PACKET_TYPING_H

#include "core/sources/source_packet.h"

#include <string>

namespace irs3 {

std::string StreamTypeFromPacketType(const std::string &packet_type);
bool PacketTypeMatchesStreamType(const std::string &packet_type, const std::string &stream_type);
bool IsRemuxAllowedStreamType(const std::string &stream_type);

SourceCodec CodecFromPacketType(const std::string &packet_type);
int DefaultClockRateForPacketType(const std::string &packet_type);

void AssignRtmpFlvPacketTyping(SourcePacket *packet, int tag_type);
void AssignWhipVideoPacketTyping(SourcePacket *packet);
void AssignWhipVoicePacketTyping(SourcePacket *packet, bool opus = true);

} // namespace irs3

#endif
