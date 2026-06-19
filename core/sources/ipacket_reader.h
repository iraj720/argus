#ifndef ARGUS_CORE_SOURCES_IPACKET_READER_H
#define ARGUS_CORE_SOURCES_IPACKET_READER_H

#include "core/sources/source_packet.h"

#include <memory>

namespace irs3 {

class IPacketReader {
public:
    virtual ~IPacketReader() = default;

    virtual bool Read(SourcePacket *out) = 0;
    virtual void Close() = 0;
};

using PacketReaderPtr = std::unique_ptr<IPacketReader>;

} // namespace irs3

#endif
