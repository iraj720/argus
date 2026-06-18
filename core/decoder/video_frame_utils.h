#ifndef ARGUS_CORE_DECODER_VIDEO_FRAME_UTILS_H
#define ARGUS_CORE_DECODER_VIDEO_FRAME_UTILS_H

#include "core/decoder/video_frame.h"

namespace irs3 {

VideoFrame CloneVideoFrame(const VideoFrame &frame);
void ReleaseVideoFrame(VideoFrame *frame);

} // namespace irs3

#endif
