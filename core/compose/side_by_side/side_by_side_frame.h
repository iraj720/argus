#ifndef ARGUS_CORE_COMPOSE_SIDE_BY_SIDE_SIDE_BY_SIDE_FRAME_H
#define ARGUS_CORE_COMPOSE_SIDE_BY_SIDE_SIDE_BY_SIDE_FRAME_H

#include "core/decoder/video_frame.h"

struct AVFrame;

namespace irs3 {

AVFrame *BuildSideBySideFrame(const VideoFrame &frame);

} // namespace irs3

#endif
