#ifndef ARGUS_CORE_DECODER_VIDEO_FRAME_H
#define ARGUS_CORE_DECODER_VIDEO_FRAME_H

#include <cstdint>

struct AVFrame;

namespace irs3 {

struct VideoFrame {
    int width = 0;
    int height = 0;
    std::int64_t pts = 0;
    AVFrame *native = nullptr;
};

} // namespace irs3

#endif
