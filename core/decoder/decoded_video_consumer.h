#ifndef ARGUS_CORE_DECODER_DECODED_VIDEO_CONSUMER_H
#define ARGUS_CORE_DECODER_DECODED_VIDEO_CONSUMER_H

#include "core/decoder/video_frame.h"

namespace irs3 {

class IDecodedVideoConsumer {
public:
    virtual ~IDecodedVideoConsumer() = default;

    virtual void OnVideoFrame(const VideoFrame &frame) = 0;
};

} // namespace irs3

#endif
