#include "core/decoder/video_frame_utils.h"

extern "C" {
#include <libavutil/frame.h>
}

namespace irs3 {

VideoFrame CloneVideoFrame(const VideoFrame &frame) {
    VideoFrame cloned;
    cloned.width = frame.width;
    cloned.height = frame.height;
    cloned.pts = frame.pts;
    cloned.native = frame.native != nullptr ? av_frame_clone(frame.native) : nullptr;
    return cloned;
}

void ReleaseVideoFrame(VideoFrame *frame) {
    if (frame == nullptr || frame->native == nullptr) {
        return;
    }
    av_frame_free(&frame->native);
}

} // namespace irs3
