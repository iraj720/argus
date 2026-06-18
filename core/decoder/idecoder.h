#ifndef ARGUS_CORE_DECODER_IDECODER_H
#define ARGUS_CORE_DECODER_IDECODER_H

#include "core/decoder/video_frame.h"
#include "core/sources/source_packet.h"

namespace irs3 {

class IDecoder {
public:
    virtual ~IDecoder() = default;

    virtual bool Configure(const SourceFormat &format) = 0;
    virtual bool FeedPacket(const SourcePacket &packet, SourcePayloadMode payload_mode) = 0;
    virtual bool TryDecode(VideoFrame *out) = 0;
    virtual void Reset() = 0;
};

} // namespace irs3

#endif
